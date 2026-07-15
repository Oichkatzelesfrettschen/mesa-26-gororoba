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

/* Front-end parity oracle: zero semantic divergence between the classic front
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
      case RC_OPCODE_TXB:
         /* The model has no mip chain; bias cannot move the result. */
         eval_tex(inst->U.I.TexSrcUnit, s[0], r);
         break;
      case RC_OPCODE_TXP: {
         /* 2D projective: the packed source carries the projector in the
          * lane after the coordinate. */
         const float q = s[0][2] != 0.0f ? s[0][2] : 1.0f;
         const float c[4] = {s[0][0] / q, s[0][1] / q, 0.0f, 1.0f};
         eval_tex(inst->U.I.TexSrcUnit, c, r);
         break;
      }
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

/* Compile through nir_to_rc (consumes s).  ext_in carries per-unit sampler
 * state (shadow compare func, swizzle); a NULL ext_in compiles with the
 * all-zero state every non-shadow corpus shader uses. */
static bool
compile_reference(nir_shader *s, struct r300_fragment_program_compiler *fc,
                  struct rc_regalloc_state *rs,
                  struct r300_fragment_shader_code *fs_code,
                  const struct r300_fragment_program_external_state *ext_in)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state zero_ext = {0};
   const struct r300_fragment_program_external_state ext =
      ext_in ? *ext_in : zero_ext;
   union r300_shader_code code = {.f = fs_code};

   fs_compiler_init(fc, rs);
   r300_optimize_nir(s, r300_screen(ps));
   nir_to_rc(s, ps, ext, code, &fc->Base);
   return !fc->Base.Error;
}

/* Compile through the classic ladder (consumes s).  ext_in mirrors the
 * nir_to_rc argument above; r300_classic_select reads the same
 * sampler_state_count/texture_compare_func fields for its own
 * nir_lower_tex_shadow call. */
static bool
compile_classic(void *ctx, nir_shader *s,
                struct r300_fragment_program_compiler *fc,
                struct rc_regalloc_state *rs, const char **why,
                const struct r300_fragment_program_external_state *ext_in)
{
   static struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   r300_optimize_nir(s, r300_screen(ps));

   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_select_result sel;
   if (!r300_classic_select(ctx, s, t, ext_in, 0, R300_FS_INPUT_INTERPOLATED, NULL, &sel) || !sel.program) {
      *why = sel.reject_reason ? sel.reject_reason : "selection failed";
      return false;
   }
   struct r300_classic_regalloc_result ra;
   if (!r300_classic_regalloc(ctx, sel.program, &ra) || !ra.temp_of_ssa) {
      *why = ra.reject_reason ? ra.reject_reason : "allocation failed";
      return false;
   }
   fs_compiler_init(fc, rs);
   if (!r300_classic_emit(sel.program, &sel.immediates, &sel.states, fc)) {
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

/* The parity core: both front ends, four input sets, tolerance compare.
 * ext carries per-unit sampler state (shadow compare func, swizzle) through
 * both compilers; NULL compiles the plain zero state every non-shadow
 * corpus shader uses.  count_diverge_as_failure gates whether a per-channel
 * mismatch fails the suite; every corpus entry currently runs with it true,
 * so parity_probe is a superset of parity() available for a shader that
 * needs a non-NULL ext.  Returns true if any input set showed a per-channel
 * mismatch. */
static bool
parity_probe(const char *name, nir_shader *(*build)(void),
            const struct r300_fragment_program_external_state *ext,
            bool count_diverge_as_failure)
{
   void *ctx = ralloc_context(NULL);
   nir_shader *for_ref = build();
   nir_shader *for_classic = build();
   bool diverged = false;

   struct r300_fragment_program_compiler ref_fc, cls_fc;
   struct rc_regalloc_state ref_rs, cls_rs;
   struct r300_fragment_shader_code ref_code;
   memset(&ref_code, 0, sizeof(ref_code));

   char what[256];
   snprintf(what, sizeof(what), "%s: reference front end compiles", name);
   const bool ref_ok =
      compile_reference(for_ref, &ref_fc, &ref_rs, &ref_code, ext);
   CHECK(ref_ok, what);

   const char *why = NULL;
   snprintf(what, sizeof(what), "%s: classic front end compiles", name);
   const bool cls_ok = compile_classic(ctx, for_classic, &cls_fc, &cls_rs,
                                       &why, ext);
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
               diverged = true;
               if (count_diverge_as_failure) {
                  fprintf(stderr,
                          "DIVERGE %s set %u ch %u: reference %g classic %g\n",
                          name, set, ch, ref_color[ch], cls_color[ch]);
                  failures++;
               } else {
                  fprintf(stderr,
                          "KNOWN-DIVERGENCE %s set %u ch %u: reference %g "
                          "classic %g\n",
                          name, set, ch, ref_color[ch], cls_color[ch]);
               }
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
   return diverged;
}

/* The regression-gate entry: a channel mismatch fails the suite. */
static void
parity(const char *name, nir_shader *(*build)(void))
{
   parity_probe(name, build, NULL, true);
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

/* An integer-indexed select ladder -- the deqp dynamic-read shape.  The
 * index math is genuinely integer (f2i32, ieq), so selection's entry must
 * run the integer lowering before the bool lowering: bool-to-float over
 * raw integer bits folds the ladder constants to zero. */
static nir_shader *
build_int_index_ladder(void)
{
   nir_builder b = fs_builder("parity_intladder");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *idx = nir_f2i32(&b, nir_fmul_imm(&b, nir_channel(&b, v, 0), 4.0f));
   nir_def *acc = nir_imm_float(&b, 0.0625f);
   for (int step = 1; step <= 3; step++) {
      nir_def *hit = nir_ieq_imm(&b, idx, step);
      acc = nir_bcsel(&b, hit,
                      nir_imm_float(&b, 0.25f * (float)step), acc);
   }
   nir_def *one = nir_imm_float(&b, 1.0f);
   nir_store_var(&b, out, nir_vec4(&b, acc, acc, acc, one), 0xf);
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

/* Projective sampling: the entry packing carries the projector in the
 * lane after the coordinate and selection picks TXP by the width rule. */
static nir_shader *
build_txp_modulate(void)
{
   nir_builder b = fs_builder("parity_txp");
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
   nir_def *proj = nir_fadd_imm(&b, nir_fabs(&b, nir_channel(&b, v, 3)),
                                1.0f);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 4);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, uv);
   tex->src[3] = nir_tex_src_for_ssa(nir_tex_src_projector, proj);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_store_var(&b, out, nir_fmul(&b, &tex->def, v), 0xf);
   return b.shader;
}

/* Vector-width scalar ops: RC RCP/POW replicate one lane, so vec2 frcp
 * must expand per channel -- one RCP writing 1/x to both lanes is the
 * operator.binary_operator.div regression shape. */
static nir_shader *
build_vector_rcp_pow(void)
{
   nir_builder b = fs_builder("parity_vecrcp");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *safe = nir_fadd_imm(&b, nir_fabs(&b, v), 0.5f);
   nir_def *rcp2 = nir_frcp(&b, nir_trim_vector(&b, safe, 2));
   nir_def *powv = nir_fpow(&b, nir_trim_vector(&b, safe, 2),
                            nir_imm_vec2(&b, 2.0f, 3.0f));
   nir_def *x = nir_channel(&b, rcp2, 0);
   nir_def *y = nir_channel(&b, rcp2, 1);
   nir_def *z = nir_channel(&b, powv, 0);
   nir_def *w = nir_channel(&b, powv, 1);
   nir_store_var(&b, out, nir_vec4(&b, x, y, z, w), 0xf);
   return b.shader;
}

/* Two masked stores to the same fragment color: .xy from one value, then
 * .zw from an independent one.  ntr_emit_store_output (nir_to_rc.c) MOVs
 * only the covered channels into RC_FILE_OUTPUT; select_intrinsic's
 * store_output case (r300_classic_select.c) records the same
 * nir_intrinsic_write_mask on the R300C_OP_EXPORT_COLOR instruction, and
 * r300_classic_emit.c propagates it as the export MOV's WriteMask, so the
 * second store's masked export touches only Z/W and cannot stomp the first
 * store's X/Y result -- radeon_pair_translate.c's RGB.OutputWriteMask /
 * Alpha.OutputWriteMask honor an arbitrary per-channel mask on an
 * RC_FILE_OUTPUT dst exactly like a temp register write. */
static nir_shader *
build_masked_output_store(void)
{
   nir_builder b = fs_builder("parity_maskedout");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *x = nir_fmul_imm(&b, nir_channel(&b, v, 0), 0.5f);
   nir_def *y = nir_fadd_imm(&b, nir_channel(&b, v, 1), 0.25f);
   nir_def *z = nir_ffract(&b, nir_channel(&b, v, 2));
   nir_def *w = nir_fmul_imm(&b, nir_channel(&b, v, 3), 2.0f);
   nir_def *stomp = nir_imm_float(&b, -7.0f);
   nir_def *unused = nir_imm_float(&b, 0.0f);

   nir_def *store_xy = nir_vec4(&b, x, y, unused, unused);
   nir_store_var(&b, out, store_xy, 0x3);

   nir_def *store_zw = nir_vec4(&b, stomp, stomp, z, w);
   nir_store_var(&b, out, store_zw, 0xc);

   return b.shader;
}

/* A single-channel store (.x, mask 0x1) followed by a three-channel store
 * (.yzw, mask 0xe starting at bit 1) to the same fragment color: the
 * narrowest and widest non-full masks the write-mask fix has to carry
 * correctly, as distinct from build_masked_output_store's even .xy/.zw
 * split. */
static nir_shader *
build_masked_output_store_single_channel(void)
{
   nir_builder b = fs_builder("parity_maskedout_single");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_def *v = nir_load_var(&b, in);
   nir_def *x = nir_fmul_imm(&b, nir_channel(&b, v, 0), 0.5f);
   nir_def *y = nir_fadd_imm(&b, nir_channel(&b, v, 1), 0.25f);
   nir_def *z = nir_ffract(&b, nir_channel(&b, v, 2));
   nir_def *w = nir_fmul_imm(&b, nir_channel(&b, v, 3), 2.0f);
   nir_def *stomp = nir_imm_float(&b, -3.0f);

   nir_def *store_x = nir_vec4(&b, x, stomp, stomp, stomp);
   nir_store_var(&b, out, store_x, 0x1);

   nir_def *store_yzw = nir_vec4(&b, stomp, y, z, w);
   nir_store_var(&b, out, store_yzw, 0xe);

   return b.shader;
}

/* A masked color store (.xy then .zw, mirroring build_masked_output_store)
 * beside an independent gl_FragDepth store: selection, regalloc, and
 * emission must carry a masked R300C_OP_EXPORT_COLOR and an unmasked
 * R300C_OP_EXPORT_DEPTH through the same program without the depth export's
 * writemask-0 sink (r300_classic_ir.c's has_output_mask split) disturbing
 * the color export's nonzero destination mask, or vice versa. */
static nir_shader *
build_masked_output_store_with_depth(void)
{
   nir_builder b = fs_builder("parity_maskedout_depth");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);
   nir_variable *depth = nir_variable_create(b.shader, nir_var_shader_out,
                                             glsl_float_type(), "gl_FragDepth");
   depth->data.location = FRAG_RESULT_DEPTH;
   depth->data.driver_location = 1;

   nir_def *v = nir_load_var(&b, in);
   nir_def *x = nir_fmul_imm(&b, nir_channel(&b, v, 0), 0.5f);
   nir_def *y = nir_fadd_imm(&b, nir_channel(&b, v, 1), 0.25f);
   nir_def *z = nir_ffract(&b, nir_channel(&b, v, 2));
   nir_def *w = nir_fmul_imm(&b, nir_channel(&b, v, 3), 2.0f);
   nir_def *stomp = nir_imm_float(&b, -7.0f);
   nir_def *unused = nir_imm_float(&b, 0.0f);

   nir_def *store_xy = nir_vec4(&b, x, y, unused, unused);
   nir_store_var(&b, out, store_xy, 0x3);

   nir_def *store_zw = nir_vec4(&b, stomp, stomp, z, w);
   nir_store_var(&b, out, store_zw, 0xc);

   nir_def *depth_val = nir_fadd_imm(&b, nir_fabs(&b, nir_channel(&b, v, 3)),
                                     0.1f);
   nir_store_var(&b, depth, depth_val, 0x1);

   return b.shader;
}

/* texture(s,uv).r beside an independent value.  R300/R400 emit_tex has no
 * writemask field (r300_fragprog_emit.c) and always writes all four TEX
 * destination channels; select_tex declares the full XYZW liveness for
 * TEX/TXB/TXP on non-r500 rather than the narrowed one-component mask
 * tex->def.num_components would give, because the shared register packer
 * (radeon_dataflow.c writes_normal, radeon_optimize.c) reads that declared
 * mask verbatim and can only narrow it, never widen it: a narrow
 * declaration lets the packer place an independent live value in the
 * "unused" G/B/A lanes the hardware write then clobbers.  This shader keeps
 * that shape in the parity corpus as a permanent regression gate on the
 * full-liveness declaration. */
static nir_shader *
build_tex_r_channel_independent(void)
{
   nir_builder b = fs_builder("parity_texr_indep");
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
   /* Narrow the read to .r: nir_opt_shrink_vectors (run inside
    * r300_optimize_nir) turns this into the tex->def.num_components == 1
    * declaration select_tex saw before the #941 fix. */
   nir_def *texr = nir_channel(&b, &tex->def, 0);

   /* An independent value computed the same cycle as the TEX result, with
    * no data dependency on it -- exactly what the packer is free to place
    * in the TEX destination's unused lanes absent full declared liveness. */
   nir_def *indep_g = nir_fmul_imm(&b, nir_channel(&b, v, 1), 3.0f);
   nir_def *indep_b = nir_fadd_imm(&b, nir_channel(&b, v, 2), 0.5f);
   nir_def *indep_a = nir_ffract(&b, nir_channel(&b, v, 3));

   nir_store_var(&b, out, nir_vec4(&b, texr, indep_g, indep_b, indep_a), 0xf);
   return b.shader;
}

/* Shadow compare with COMPARE_FUNC_ALWAYS.  nir_lower_tex_shadow (run
 * inside both nir_to_rc and r300_classic_select) builds the ALWAYS case as
 * nir_b2f32(nir_imm_int(b, ~0)) -- nir_compare_func's own encoding of the
 * trivial case (nir_builder.c) -- and folding that to the float constant
 * 1.0 requires the same nir_opt_algebraic/nir_opt_constant_folding
 * fixed-point loop nir_to_rc runs at its entry to run before int-to-float
 * lowering; running int lowering first turns the ALWAYS case into -1.0
 * instead, the r300_classic_select() ordering this shader locks in as a
 * permanent regression gate.  ext's texture_compare_func =
 * RC_COMPARE_FUNC_ALWAYS drives both front ends down that exact lowering
 * path. */
static nir_shader *
build_shadow_compare_always(void)
{
   nir_builder b = fs_builder("parity_shadow_always");
   nir_variable *in = add_varying(&b);
   nir_variable *out = add_color_output(&b);

   const struct glsl_type *sampler2d_shadow =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, true, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler = nir_variable_create(b.shader, nir_var_uniform,
                                               sampler2d_shadow, "shadow0");
   sampler->data.binding = 0;
   nir_deref_instr *deref = nir_build_deref_var(&b, sampler);

   nir_def *v = nir_load_var(&b, in);
   nir_def *uv = nir_trim_vector(&b, v, 2);
   nir_def *cmp = nir_channel(&b, v, 2);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 4);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->is_shadow = true;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &deref->def);
   tex->src[1] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &deref->def);
   tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_coord, uv);
   tex->src[3] = nir_tex_src_for_ssa(nir_tex_src_comparator, cmp);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_store_var(&b, out, nir_fmul(&b, &tex->def, v), 0xf);
   return b.shader;
}

/* External sampler state for build_shadow_compare_always: unit 0 is a
 * shadow sampler with GL_ALWAYS as its EXT_shadow_func compare mode; the
 * zeroed texture_swizzle replicates the compare result to all four output
 * channels, the same INTENSITY-style default nir_lower_tex_shadow applies
 * when no swizzle override is supplied. */
static const struct r300_fragment_program_external_state
   shadow_always_ext = {
      .unit[0].texture_compare_func = RC_COMPARE_FUNC_ALWAYS,
      .sampler_state_count = 1,
   };

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
   parity("int_index_ladder", build_int_index_ladder);
   parity("txp_modulate", build_txp_modulate);
   parity("vector_rcp_pow", build_vector_rcp_pow);
   parity("transcendentals", build_transcendentals);
   parity("floor_round", build_floor_round);

   /* Regression gates for the fixed TEX-writemask, shadow-compare, and
    * store_output write-mask classic/nir_to_rc divergences: both front ends
    * must agree.  select_intrinsic's store_output case (r300_classic_select.c)
    * records nir_intrinsic_write_mask on the R300C_OP_EXPORT_COLOR
    * instruction and r300_classic_emit.c propagates it as the export MOV's
    * WriteMask, so two differently-masked stores to the same color
    * attachment accumulate the way ntr_emit_store_output's masked MOV does
    * in nir_to_rc.c instead of one clobbering the other. */
   parity("tex_r_channel_independent", build_tex_r_channel_independent);
   parity_probe("shadow_compare_always", build_shadow_compare_always,
               &shadow_always_ext, true);
   parity("masked_output_store", build_masked_output_store);
   parity("masked_output_store_single_channel",
         build_masked_output_store_single_channel);
   parity("masked_output_store_with_depth",
         build_masked_output_store_with_depth);

   glsl_type_singleton_decref();
   if (failures) {
      fprintf(stderr, "r300_classic_parity_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_parity_test: all checks passed\n");
   return 0;
}
