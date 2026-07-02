/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "util/ralloc.h"

#include "classic/r300_classic_emit.h"
#include "classic/r300_classic_regalloc.h"
#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_code.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_regalloc.h"

/* Phase-5 exit criterion: zero semantic divergence between the classic front
 * end and nir_to_rc on the corpus.  Each corpus shader compiles through BOTH
 * front ends into two rc_programs, and a CPU evaluator of the RC IR executes
 * both on the same input vectors, the same constant files, and the same
 * texture model; the color outputs must agree within float tolerance.  This
 * is semantic equality, not byte equality -- the two front ends number
 * temporaries and order instructions differently and both are free to. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

/* ---------- RC IR CPU evaluator ---------- */

struct rc_eval {
   float temps[128][4];
   float inputs[8][4];
   float outputs[8][4];
   const struct rc_constant_list *consts;
   const char *error;
};

/* Both front ends see the same deterministic texture: a pure function of
 * the coordinate and the unit. */
static void
eval_tex(unsigned unit, const float *coord, float *out)
{
   out[0] = coord[0];
   out[1] = coord[1];
   out[2] = coord[0] * coord[1] + (float)unit * 0.125f;
   out[3] = 1.0f;
}

static bool
eval_src(struct rc_eval *e, const struct rc_src_register *src, float *out)
{
   float reg[4] = {0, 0, 0, 0};
   switch (src->File) {
   case RC_FILE_TEMPORARY:
      memcpy(reg, e->temps[src->Index & 127], sizeof(reg));
      break;
   case RC_FILE_INPUT:
      memcpy(reg, e->inputs[src->Index & 7], sizeof(reg));
      break;
   case RC_FILE_CONSTANT: {
      if (src->Index >= e->consts->Count) {
         e->error = "constant index out of range";
         return false;
      }
      const struct rc_constant *c = &e->consts->Constants[src->Index];
      if (c->Type != RC_CONSTANT_IMMEDIATE) {
         e->error = "non-immediate constant";
         return false;
      }
      memcpy(reg, c->u.Immediate, sizeof(reg));
      break;
   }
   case RC_FILE_NONE:
      break;
   default:
      e->error = "unhandled source file";
      return false;
   }

   for (unsigned ch = 0; ch < 4; ch++) {
      float v;
      switch (GET_SWZ(src->Swizzle, ch)) {
      case RC_SWIZZLE_X:    v = reg[0]; break;
      case RC_SWIZZLE_Y:    v = reg[1]; break;
      case RC_SWIZZLE_Z:    v = reg[2]; break;
      case RC_SWIZZLE_W:    v = reg[3]; break;
      case RC_SWIZZLE_ZERO: v = 0.0f; break;
      case RC_SWIZZLE_ONE:  v = 1.0f; break;
      case RC_SWIZZLE_HALF: v = 0.5f; break;
      default:              v = 0.0f; break;
      }
      if (src->Abs)
         v = fabsf(v);
      if (src->Negate & (1u << ch))
         v = -v;
      out[ch] = v;
   }
   return true;
}

static bool
eval_program(struct rc_eval *e, struct radeon_compiler *c)
{
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      float s[3][4];
      for (unsigned n = 0; n < info->NumSrcRegs; n++)
         if (!eval_src(e, &inst->U.I.SrcReg[n], s[n]))
            return false;

      float r[4] = {0, 0, 0, 0};
      switch (inst->U.I.Opcode) {
      case RC_OPCODE_MOV:
         memcpy(r, s[0], sizeof(r));
         break;
      case RC_OPCODE_ADD:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = s[0][ch] + s[1][ch];
         break;
      case RC_OPCODE_MUL:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = s[0][ch] * s[1][ch];
         break;
      case RC_OPCODE_MAD:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] * s[1][ch] + s[2][ch];
         break;
      case RC_OPCODE_DP2: {
         const float d = s[0][0] * s[1][0] + s[0][1] * s[1][1];
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = d;
         break;
      }
      case RC_OPCODE_DP3: {
         const float d = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                         s[0][2] * s[1][2];
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = d;
         break;
      }
      case RC_OPCODE_DP4: {
         const float d = s[0][0] * s[1][0] + s[0][1] * s[1][1] +
                         s[0][2] * s[1][2] + s[0][3] * s[1][3];
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = d;
         break;
      }
      case RC_OPCODE_MIN:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = fminf(s[0][ch], s[1][ch]);
         break;
      case RC_OPCODE_MAX:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = fmaxf(s[0][ch], s[1][ch]);
         break;
      case RC_OPCODE_FRC:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] - floorf(s[0][ch]);
         break;
      case RC_OPCODE_RCP:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = 1.0f / s[0][0];
         break;
      case RC_OPCODE_RSQ:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = 1.0f / sqrtf(fabsf(s[0][0]));
         break;
      case RC_OPCODE_ROUND:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = rintf(s[0][ch]);
         break;
      case RC_OPCODE_EX2:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = exp2f(s[0][0]);
         break;
      case RC_OPCODE_LG2:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = log2f(s[0][0]);
         break;
      case RC_OPCODE_SIN:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = sinf(s[0][0]);
         break;
      case RC_OPCODE_COS:
         for (unsigned ch = 0; ch < 4; ch++) r[ch] = cosf(s[0][0]);
         break;
      case RC_OPCODE_POW:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = powf(s[0][0], s[1][0]);
         break;
      case RC_OPCODE_SLT:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] < s[1][ch] ? 1.0f : 0.0f;
         break;
      case RC_OPCODE_SGE:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] >= s[1][ch] ? 1.0f : 0.0f;
         break;
      case RC_OPCODE_SEQ:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] == s[1][ch] ? 1.0f : 0.0f;
         break;
      case RC_OPCODE_SNE:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] != s[1][ch] ? 1.0f : 0.0f;
         break;
      case RC_OPCODE_CMP:
         /* cmp: dst = src0 < 0 ? src1 : src2 (per channel). */
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = s[0][ch] < 0.0f ? s[1][ch] : s[2][ch];
         break;
      case RC_OPCODE_TEX:
         eval_tex(inst->U.I.TexSrcUnit, s[0], r);
         break;
      default:
         e->error = info->Name;
         return false;
      }

      if (!info->HasDstReg)
         continue;

      switch (inst->U.I.SaturateMode) {
      case RC_SATURATE_ZERO_ONE:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = fminf(fmaxf(r[ch], 0.0f), 1.0f);
         break;
      case RC_SATURATE_MINUS_PLUS_ONE:
         for (unsigned ch = 0; ch < 4; ch++)
            r[ch] = fminf(fmaxf(r[ch], -1.0f), 1.0f);
         break;
      default:
         break;
      }

      float *dst;
      if (inst->U.I.DstReg.File == RC_FILE_TEMPORARY)
         dst = e->temps[inst->U.I.DstReg.Index & 127];
      else if (inst->U.I.DstReg.File == RC_FILE_OUTPUT)
         dst = e->outputs[inst->U.I.DstReg.Index & 7];
      else {
         e->error = "unhandled destination file";
         return false;
      }
      for (unsigned ch = 0; ch < 4; ch++)
         if (inst->U.I.DstReg.WriteMask & (1u << ch))
            dst[ch] = r[ch];
   }
   return true;
}

/* ---------- dual-front-end compile ---------- */

static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   return (struct pipe_screen *)s;
}

static void
fs_compiler_init(struct r300_fragment_program_compiler *fc,
                 struct rc_regalloc_state *rs)
{
   rc_init_regalloc_state(rs, RC_FRAGMENT_PROGRAM);
   memset(fc, 0, sizeof(*fc));
   rc_init(&fc->Base, rs);
   fc->Base.type = RC_FRAGMENT_PROGRAM;
   fc->Base.has_half_swizzles = true;
   fc->Base.has_presub = true;
   fc->Base.has_omod = true;
   fc->Base.max_temp_regs = 32;
   fc->Base.max_constants = 32;
   fc->Base.max_alu_insts = 64;
   fc->Base.max_tex_insts = 32;
}

/* Compile through nir_to_rc (consumes s). */
static bool
compile_reference(nir_shader *s, struct r300_fragment_program_compiler *fc,
                  struct rc_regalloc_state *rs,
                  struct r300_fragment_shader_code *fs_code)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   union r300_shader_code code = {.f = fs_code};

   fs_compiler_init(fc, rs);
   r300_optimize_nir(s, r300_screen(ps));
   nir_to_rc(s, ps, ext, code, &fc->Base);
   return !fc->Base.Error;
}

/* Compile through the classic ladder (consumes s). */
static bool
compile_classic(void *ctx, nir_shader *s,
                struct r300_fragment_program_compiler *fc,
                struct rc_regalloc_state *rs, const char **why)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   if (!r300_classic_select(ctx, s, t, 0, NULL, &sel) || !sel.program) {
      *why = sel.reject_reason ? sel.reject_reason : "selection failed";
      return false;
   }
   struct r300_classic_regalloc_result ra;
   if (!r300_classic_regalloc(ctx, sel.program, &ra) || !ra.temp_of_ssa) {
      *why = ra.reject_reason ? ra.reject_reason : "allocation failed";
      return false;
   }
   fs_compiler_init(fc, rs);
   if (!r300_classic_emit(sel.program, &sel.immediates, fc)) {
      *why = "emission failed";
      return false;
   }
   return true;
}

/* Evaluate one front end's program at the standard input set. */
static bool
run_eval(struct r300_fragment_program_compiler *fc, unsigned input_set,
         float *color_out, const char **why)
{
   static const float input_sets[4][4] = {
      {0.25f, 0.5f, 0.75f, 1.0f},
      {1.5f, -0.25f, 0.125f, 2.0f},
      {-1.0f, 0.0f, 3.5f, 0.5f},
      {0.0f, 1.0f, -2.25f, -0.5f},
   };
   struct rc_eval e;
   memset(&e, 0, sizeof(e));
   e.consts = &fc->Base.Program.Constants;
   for (unsigned i = 0; i < 8; i++)
      for (unsigned ch = 0; ch < 4; ch++)
         e.inputs[i][ch] = input_sets[input_set][ch] + (float)i * 0.0625f;

   if (!eval_program(&e, &fc->Base)) {
      *why = e.error;
      return false;
   }
   memcpy(color_out, e.outputs[fc->OutputColor[0] & 7], 4 * sizeof(float));
   return true;
}

/* The parity core: both front ends, four input sets, tolerance compare. */
static void
parity(const char *name, nir_shader *(*build)(void))
{
   void *ctx = ralloc_context(NULL);
   nir_shader *for_ref = build();
   nir_shader *for_classic = build();

   struct r300_fragment_program_compiler ref_fc, cls_fc;
   struct rc_regalloc_state ref_rs, cls_rs;
   struct r300_fragment_shader_code ref_code;
   memset(&ref_code, 0, sizeof(ref_code));

   char what[256];
   snprintf(what, sizeof(what), "%s: reference front end compiles", name);
   const bool ref_ok = compile_reference(for_ref, &ref_fc, &ref_rs, &ref_code);
   CHECK(ref_ok, what);

   const char *why = NULL;
   snprintf(what, sizeof(what), "%s: classic front end compiles", name);
   const bool cls_ok = compile_classic(ctx, for_classic, &cls_fc, &cls_rs,
                                       &why);
   CHECK(cls_ok, what);
   if (!cls_ok && why)
      fprintf(stderr, "  classic: %s\n", why);

   if (ref_ok && cls_ok) {
      for (unsigned set = 0; set < 4; set++) {
         float ref_color[4], cls_color[4];
         const char *ref_why = NULL, *cls_why = NULL;
         snprintf(what, sizeof(what), "%s: set %u evaluates", name, set);
         const bool eval_ok = run_eval(&ref_fc, set, ref_color, &ref_why) &&
                              run_eval(&cls_fc, set, cls_color, &cls_why);
         CHECK(eval_ok, what);
         if (!eval_ok) {
            if (ref_why)
               fprintf(stderr, "  reference eval: %s\n", ref_why);
            if (cls_why)
               fprintf(stderr, "  classic eval: %s\n", cls_why);
            break;
         }
         for (unsigned ch = 0; ch < 4; ch++) {
            const float d = fabsf(ref_color[ch] - cls_color[ch]);
            if (d > 1e-4f) {
               fprintf(stderr,
                       "DIVERGE %s set %u ch %u: reference %g classic %g\n",
                       name, set, ch, ref_color[ch], cls_color[ch]);
               failures++;
            }
         }
      }
   }

   if (ref_ok) {
      rc_destroy(&ref_fc.Base);
      rc_destroy_regalloc_state(&ref_rs);
   }
   if (cls_ok) {
      rc_destroy(&cls_fc.Base);
      rc_destroy_regalloc_state(&cls_rs);
   }
   ralloc_free(ctx);
}

/* ---------- corpus ---------- */

static nir_builder
fs_builder(const char *name)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
      .fdot_replicates = true,
   };
   return nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options, "%s",
                                         name);
}

static nir_variable *
add_varying(nir_builder *b)
{
   nir_variable *in = nir_variable_create(b->shader, nir_var_shader_in,
                                          glsl_vec4_type(), "in0");
   in->data.location = VARYING_SLOT_VAR0;
   in->data.driver_location = 0;
   in->data.interpolation = INTERP_MODE_SMOOTH;
   return in;
}

static nir_variable *
add_color_output(nir_builder *b)
{
   nir_variable *out = nir_variable_create(b->shader, nir_var_shader_out,
                                           glsl_vec4_type(), "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;
   return out;
}

static nir_shader *
build_passthrough(void)
{
   nir_builder b = fs_builder("parity_mov");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_store_var(&b, out, nir_load_var(&b, in), 0xf);
   return b.shader;
}

static nir_shader *
build_fmad(void)
{
   nir_builder b = fs_builder("parity_fmad");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.5f, 0.25f, 2.0f, 1.0f);
   nir_def *c2 = nir_imm_vec4(&b, 0.125f, -0.5f, 0.0f, 0.75f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_fmad, v, c1, c2), 0xf);
   return b.shader;
}

static nir_shader *
build_flrp(void)
{
   nir_builder b = fs_builder("parity_flrp");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c1 = nir_imm_vec4(&b, 0.25f, 0.25f, 0.25f, 0.25f);
   nir_def *c2 = nir_imm_vec4(&b, 0.75f, 0.5f, 1.0f, 0.0f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_flrp, c1, c2, v), 0xf);
   return b.shader;
}

static nir_shader *
build_minmax_chain(void)
{
   nir_builder b = fs_builder("parity_minmax");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *lo = nir_imm_vec4(&b, 0.0f, 0.0f, 0.0f, 0.0f);
   nir_def *hi = nir_imm_vec4(&b, 1.0f, 1.0f, 1.0f, 1.0f);
   nir_def *clamped = nir_fmin(&b, nir_fmax(&b, v, lo), hi);
   nir_def *fr = nir_ffract(&b, nir_fmul_imm(&b, clamped, 3.0f));
   nir_store_var(&b, out, fr, 0xf);
   return b.shader;
}

static nir_shader *
build_swizzle_negate(void)
{
   nir_builder b = fs_builder("parity_swz");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *wzyx = nir_swizzle(&b, v, (unsigned[]){3, 2, 1, 0}, 4);
   nir_def *neg = nir_fneg(&b, wzyx);
   nir_def *sum = nir_fadd(&b, v, neg);
   nir_store_var(&b, out, sum, 0xf);
   return b.shader;
}

static nir_shader *
build_tex_modulate(void)
{
   nir_builder b = fs_builder("parity_tex");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);

   const struct glsl_type *sampler2d =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               sampler2d, "tex0");
   sampler->data.binding = 0;
   nir_deref_instr *deref = nir_build_deref_var(&b, sampler);

   nir_def *v = nir_load_var(&b, in);
   nir_def *uv = nir_trim_vector(&b, v, 2);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 3);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, uv);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_store_var(&b, out, nir_fmul(&b, &tex->def, v), 0xf);
   return b.shader;
}

/* vec4 of four distinct defs: the VEC collect path, plus the fsub fold. */
static nir_shader *
build_vec_compose(void)
{
   nir_builder b = fs_builder("parity_vec");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *x = nir_fmul(&b, nir_channel(&b, v, 0), nir_channel(&b, v, 1));
   nir_def *y = nir_fadd_imm(&b, nir_channel(&b, v, 2), 0.25f);
   nir_def *z = nir_ffract(&b, nir_channel(&b, v, 3));
   nir_def *w = nir_fsub(&b, nir_channel(&b, v, 0), nir_channel(&b, v, 2));
   nir_store_var(&b, out, nir_vec4(&b, x, y, z, w), 0xf);
   return b.shader;
}

/* Replicated dot products of three widths, collected per channel. */
static nir_shader *
build_dots_replicated(void)
{
   nir_builder b = fs_builder("parity_dots");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *c3 = nir_imm_vec3(&b, 0.5f, -0.25f, 1.5f);
   nir_def *c4 = nir_imm_vec4(&b, 0.125f, 2.0f, -1.0f, 0.5f);
   nir_def *d3 = nir_fdot(&b, nir_trim_vector(&b, v, 3), c3);
   nir_def *d4 = nir_fdot(&b, v, c4);
   nir_def *d2 = nir_fdot(&b, nir_trim_vector(&b, v, 2),
                          nir_imm_vec2(&b, 0.75f, 0.25f));
   nir_def *one = nir_imm_float(&b, 1.0f);
   nir_store_var(&b, out, nir_vec4(&b, d3, d4, d2, one), 0xf);
   return b.shader;
}

/* SLT/SGE set-compares steering a per-channel CMP select. */
static nir_shader *
build_setcmp_csel(void)
{
   nir_builder b = fs_builder("parity_setcmp");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *half = nir_imm_vec4(&b, 0.5f, 0.5f, 0.5f, 0.5f);
   nir_def *lt = nir_slt(&b, v, half);
   nir_def *ge = nir_sge(&b, v, half);
   nir_def *sel = nir_build_alu3(&b, nir_op_fcsel, lt,
                                 nir_fmul_imm(&b, v, 0.25f), ge);
   nir_store_var(&b, out, sel, 0xf);
   return b.shader;
}

/* Scalar transcendentals collected per channel; operands are shifted into
 * safe domains so both evaluations stay finite on every input set. */
static nir_shader *
build_transcendentals(void)
{
   nir_builder b = fs_builder("parity_transc");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *x = nir_fexp2(&b, nir_channel(&b, v, 0));
   nir_def *y = nir_flog2(&b, nir_fadd_imm(&b, nir_fabs(&b, nir_channel(&b, v, 1)), 1.5f));
   nir_def *z = nir_fpow(&b, nir_fadd_imm(&b, nir_fabs(&b, nir_channel(&b, v, 2)), 0.5f),
                         nir_imm_float(&b, 2.0f));
   nir_def *w = nir_fmul(&b, nir_fsin(&b, nir_channel(&b, v, 3)),
                         nir_fcos(&b, nir_channel(&b, v, 3)));
   nir_store_var(&b, out, nir_vec4(&b, x, y, z, w), 0xf);
   return b.shader;
}

/* fcsel_gt and fcsel_ge take distinct CMP source folds (negate vs operand
 * swap); both must match ntr_emit_alu_special's mapping per channel. */
static nir_shader *
build_csel_variants(void)
{
   nir_builder b = fs_builder("parity_cselvar");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *cond = nir_fadd_imm(&b, v, -0.4f);
   nir_def *a = nir_fmul_imm(&b, v, 0.5f);
   nir_def *c = nir_imm_vec4(&b, 0.125f, 0.25f, 0.375f, 0.5f);
   nir_def *gt = nir_build_alu3(&b, nir_op_fcsel_gt, cond, a, c);
   nir_def *ge = nir_build_alu3(&b, nir_op_fcsel_ge, cond, c, a);
   nir_store_var(&b, out, nir_fadd(&b, gt, ge), 0xf);
   return b.shader;
}

/* A vec3 collect of distinct defs read back through a four-channel .xyyz
 * swizzle -- the deqp scalar-operator packing shape.  The compose bound is
 * the reading op's source width, not the def's: bounding by the vec3 width
 * drops the w select and reads .xyyy. */
static nir_shader *
build_vec3_wide_read(void)
{
   nir_builder b = fs_builder("parity_vec3wide");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *a = nir_fabs(&b, nir_channel(&b, v, 0));
   nir_def *y = nir_fmul_imm(&b, nir_channel(&b, v, 1), 2.0f);
   nir_def *z = nir_fadd_imm(&b, nir_channel(&b, v, 2), 0.75f);
   nir_def *v3 = nir_vec3(&b, a, y, z);
   nir_def *wide = nir_swizzle(&b, v3, (unsigned[]){0, 1, 1, 2}, 4);
   nir_def *scale = nir_imm_vec4(&b, 0.5f, 0.5f, 0.5f, 0.5f);
   nir_def *bias = nir_imm_vec4(&b, 0.25f, 0.25f, 0.25f, 0.25f);
   nir_store_var(&b, out, nir_build_alu3(&b, nir_op_fmad, wide, scale, bias),
                 0xf);
   return b.shader;
}

/* ffloor's FRC expansion and fround_even's ROUND row. */
static nir_shader *
build_floor_round(void)
{
   nir_builder b = fs_builder("parity_floor");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *fl = nir_ffloor(&b, nir_fmul_imm(&b, v, 1.7f));
   nir_def *rn = nir_fround_even(&b, nir_fmul_imm(&b, v, 2.3f));
   nir_store_var(&b, out, nir_fadd(&b, fl, rn), 0xf);
   return b.shader;
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();
   parity("passthrough", build_passthrough);
   parity("fmad", build_fmad);
   parity("flrp", build_flrp);
   parity("minmax_frc", build_minmax_chain);
   parity("swizzle_negate", build_swizzle_negate);
   parity("tex_modulate", build_tex_modulate);
   parity("vec_compose", build_vec_compose);
   parity("dots_replicated", build_dots_replicated);
   parity("setcmp_csel", build_setcmp_csel);
   parity("csel_variants", build_csel_variants);
   parity("vec3_wide_read", build_vec3_wide_read);
   parity("transcendentals", build_transcendentals);
   parity("floor_round", build_floor_round);
   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_parity_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_parity_test: all checks passed\n");
   return 0;
}
