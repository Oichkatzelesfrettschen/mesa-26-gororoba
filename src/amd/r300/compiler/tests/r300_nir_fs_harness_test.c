/*
 * SPDX-License-Identifier: MIT
 *
 * Fragment-stage NIR-input admission harness for nir_to_rc.
 *
 * The VS harness pins vertex-input aliasing and system-value rejection.  This
 * one pins the fragment ALU opcode map for the multiply-add family.  Two
 * boundaries:
 *
 *   1. nir_op_fmad (unfused multiply-add) must emit RC_OPCODE_MAD.  The r300
 *      fragment MAD is not IEEE single-rounding, so both ffma and fmad target
 *      it.  A fragment whose ALU emits a runtime fmad -- a varying-fed or
 *      uniform-fed x * c1 + c2 -- otherwise falls through to the
 *      unknown-opcode error and the shader fails to link.
 *
 *   2. nir_op_flrp must not reach the emitter as a raw flrp: r300_nir_lower_flrp
 *      rewrites flrp(a, b, c) into a nested fmad chain before emission, and that
 *      chain emits as RC_OPCODE_MAD.  This is the fog / GLSL mix() path.  The
 *      emitter keeps a defensive flrp case that errors only if that lowering
 *      pass did not run; the production lowering always runs it.
 *
 * The shaders build fmad and flrp directly with nir_build_alu3 so the
 * regression pins op_map[nir_op_fmad] (and the flrp lowering) independent of
 * whether the algebraic fuser would have produced fmad from fmul + fadd.  A
 * varying input keeps the multiply-add from constant-folding away.
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "util/u_math.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_screen.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_regalloc.h"

static unsigned g_failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         g_failures++;                    \
      }                                   \
   } while (0)

/* A stack screen whose caps the lowering reads.  is_r500/is_r400 false makes it
 * an R300-class part, the family that uses the PFS_ fragment namespace and the
 * non-IEEE MAD that both ffma and fmad target. */
static struct pipe_screen *
fake_r300_screen(struct r300_screen *s)
{
   memset(s, 0, sizeof(*s));
   s->caps.has_tcl = true;
   s->caps.is_r500 = false;
   s->caps.is_r400 = false;
   return (struct pipe_screen *)s;
}

enum fs_mad_form {
   FS_MAD_FMAD, /* color = fmad(in, c1, c2) -- direct unfused multiply-add */
   FS_MAD_FLRP, /* color = flrp(c1, c2, in) -- lowered to a nested fmad chain */
};

/* Build a fragment shader whose single output is a multiply-add fed by a
 * varying input (so it cannot constant-fold).  FS_MAD_FMAD emits the fmad
 * opcode straight; FS_MAD_FLRP emits a flrp the production lowering must
 * rewrite into fmad before emission. */
static nir_shader *
build_fs(enum fs_mad_form form)
{
   /* The two load-bearing fields from r300_screen.c COMMON_NIR_OPTIONS for this
    * test.  float_mul_add32 advertising has_fmad keeps nir_opt_algebraic from
    * splitting an fmad into fmul + fadd, so the multiply-add reaches the emitter
    * as fmad and exercises op_map[nir_op_fmad].  lower_flrp32 makes the generic
    * flrp lowering rewrite flrp into a fmad chain (which the has_fmad support
    * then preserves).  A zero-init options block omits both and the test
    * silently degrades to MUL + ADD, never touching nir_op_fmad emission. */
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_harness");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_color");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;

   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *in_value = nir_load_var(&b, in0);
   nir_def *c1 = nir_imm_vec4(&b, 7.0f, 7.0f, 7.0f, 7.0f);
   nir_def *c2 = nir_imm_vec4(&b, 1.0f, 1.0f, 1.0f, 1.0f);

   nir_def *value;
   if (form == FS_MAD_FLRP)
      value = nir_build_alu3(&b, nir_op_flrp, c1, c2, in_value);
   else
      value = nir_build_alu3(&b, nir_op_fmad, in_value, c1, c2);

   nir_store_var(&b, out, value, 0xf);
   return b.shader;
}

/* The H.264 deblock edge-strength gate: iand(flt, iand(flt, flt)) over a varying.
 * This is a 1-bit boolean conjunction of comparison results, the shape
 * nir_lower_bool_to_float lowers to fmul/fmin downstream.  b2f the gate so the
 * iand cannot fold away. */
static nir_shader *
build_fs_boolean_iand(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_bool_iand");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_edge");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *x = nir_channel(&b, nir_load_var(&b, in0), 0);
   nir_def *c1 = nir_flt(&b, x, nir_imm_float(&b, 1.0f));
   nir_def *c2 = nir_flt(&b, x, nir_imm_float(&b, 2.0f));
   nir_def *c3 = nir_flt(&b, x, nir_imm_float(&b, 3.0f));
   nir_def *gate = nir_iand(&b, c1, nir_iand(&b, c2, c3));
   nir_def *f = nir_b2f32(&b, gate);
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);
   return b.shader;
}

/* A 32-bit data ixor of two varying-derived integers: a genuine integer bit op
 * with no FP24-exact form, which the lowering MUST still reject to a dummy
 * shader.  Calibrates the boolean skip so it does not over-broaden to real
 * integer bit math. */
static nir_shader *
build_fs_data_ixor(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_data_ixor");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_data");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_f2u32(&b, nir_channel(&b, v, 0));
   nir_def *c = nir_f2u32(&b, nir_channel(&b, v, 1));
   nir_def *f = nir_u2f32(&b, nir_ixor(&b, a, c));
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);
   return b.shader;
}

/* Build a fragment shader that reproduces st_nir_lower_fog's output blend:
 *
 *   f   = fsat(ffma_weak(fogc, params.x, params.y))          (FOG_LINEAR)
 *   fog = ffma_weak(color, f, fog_color * (1 - f))           (vec4)
 *   out = vector_insert(fog, color.w, 3)                     (alpha passthrough)
 *
 * The vector_insert that re-packs the original alpha is the output-packing
 * operation identified by the fog SSA-temp mistranslation; that packing, not
 * the input provenance, is what this test pins.  Production reads the fog
 * coordinate from VARYING_SLOT_FOGC and params/fog_color from state uniforms;
 * the harness instead feeds them from generic varyings so nothing
 * constant-folds and the full packing reaches the emitter.  The substitution
 * is faithful because ntr_fixup_varying_slots remaps only VAR/PNTC/TEX slots,
 * not FOGC, and the vec_to_regs + fsat lowering is downstream of and
 * independent of which input slot the values arrive on.  This reproduces the
 * fog program whose RS485M RC dump read never-written temporaries and wrote the
 * RC sentinel temp[RC_REGISTER_MAX_INDEX - 1]. */
static nir_shader *
build_fs_fog(void)
{
   static const nir_shader_compiler_options options = {
      .float_mul_add32 =
         nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
      .lower_flrp32 = true,
   };
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_harness_fog");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *color_in =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_color");
   color_in->data.location = VARYING_SLOT_VAR0;
   color_in->data.driver_location = 0;
   color_in->data.interpolation = INTERP_MODE_SMOOTH;

   nir_variable *fog_in =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_fog");
   fog_in->data.location = VARYING_SLOT_VAR1;
   fog_in->data.driver_location = 1;
   fog_in->data.interpolation = INTERP_MODE_SMOOTH;

   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *color = nir_load_var(&b, color_in);
   nir_def *fog_vars = nir_load_var(&b, fog_in);
   /* in_fog packs the scalar fog coord (.x), the two FOG_LINEAR params (.y,.z),
    * and a fog_color seed (.w broadcast) into one varying so the harness needs
    * no state-uniform plumbing. */
   nir_def *fogc = nir_channel(&b, fog_vars, 0);
   nir_def *param0 = nir_channel(&b, fog_vars, 1);
   nir_def *param1 = nir_channel(&b, fog_vars, 2);
   nir_def *fog_color = nir_vec4(&b, nir_channel(&b, fog_vars, 3),
                                 nir_channel(&b, fog_vars, 3),
                                 nir_channel(&b, fog_vars, 3),
                                 nir_channel(&b, fog_vars, 3));

   nir_def *f = nir_fsat(&b, nir_ffma_weak(&b, fogc, param0, param1));
   nir_def *one_minus_f = nir_fsub_imm(&b, 1.0, f);
   nir_def *fog = nir_ffma_weak(&b, color, nir_vec4(&b, f, f, f, f),
                                nir_fmul(&b, fog_color, nir_vec4(&b, one_minus_f,
                                                                 one_minus_f,
                                                                 one_minus_f,
                                                                 one_minus_f)));
   nir_def *value = nir_vector_insert_imm(&b, fog, nir_channel(&b, color, 3), 3);

   nir_store_var(&b, out, value, 0xf);
   return b.shader;
}

/* Build a fragment shader that samples a 2D sampler with a projective
 * coordinate, optionally with an LOD bias.  The projector and (when present)
 * the bias arrive in one varying so nothing constant-folds and the projector
 * survives to nir_to_rc.
 *
 * nir_to_rc_lower_txp keeps a plain projective tex as the hardware
 * RC_OPCODE_TXP (the TMU performs the perspective divide at full precision),
 * but a projector combined with a bias makes tex->op == nir_texop_txb, which
 * the gate cannot fit in TXP, so nir_lower_tex divides the coordinate on the
 * FP24 ALU (RCP + MUL) and samples with RC_OPCODE_TXB.  This is the split that
 * decides whether textureProj runs at TMU precision or on the FP24 fragment
 * ALU. */
static nir_shader *
build_fs_texproj(bool with_bias)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "r300_nir_fs_harness_texproj");
   const struct glsl_type *vec4 = glsl_vec4_type();

   const struct glsl_type *sampler2d =
      glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
   nir_variable *sampler =
      nir_variable_create(b.shader, nir_var_uniform, sampler2d, "s");
   sampler->data.binding = 0;

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_coord");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;

   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   /* coord.xy, bias in .z, projector in .w -- all from the varying. */
   nir_def *pq = nir_load_var(&b, in0);
   nir_def *coord = nir_trim_vector(&b, pq, 2);
   nir_def *proj = nir_channel(&b, pq, 3);
   nir_def *bias = nir_channel(&b, pq, 2);

   nir_deref_instr *tex_deref = nir_build_deref_var(&b, sampler);

   const unsigned nsrc = with_bias ? 5 : 4;
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, nsrc);
   tex->op = with_bias ? nir_texop_txb : nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;

   unsigned i = 0;
   tex->src[i++] =
      nir_tex_src_for_ssa(nir_tex_src_texture_deref, &tex_deref->def);
   tex->src[i++] =
      nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &tex_deref->def);
   tex->src[i++] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
   tex->src[i++] = nir_tex_src_for_ssa(nir_tex_src_projector, proj);
   if (with_bias)
      tex->src[i++] = nir_tex_src_for_ssa(nir_tex_src_bias, bias);

   nir_def_init(&tex->instr, &tex->def, nir_tex_instr_dest_size(tex), 32);
   nir_builder_instr_insert(&b, &tex->instr);

   nir_store_var(&b, out, &tex->def, 0xf);
   return b.shader;
}

/* Run the production NIR-to-RC lowering and emitter as a fragment program.
 *
 * Production runs r300_optimize_nir() at shader-state creation
 * (r300_create_fs_state, r300_state.c) before nir_to_rc() translates the NIR
 * into RC instructions.  The fog / mix() flrp lowering runs in that first
 * stage, so the harness runs it before calling nir_to_rc(). */
static void
run_fs(struct r300_fragment_program_compiler *c, struct rc_regalloc_state *rs,
       nir_shader *nir)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   struct r300_fragment_shader_code fs_code = {0};
   union r300_shader_code code = {
      .f = &fs_code,
   };

   rc_init_regalloc_state(rs, RC_FRAGMENT_PROGRAM);
   memset(c, 0, sizeof(*c));
   rc_init(&c->Base, rs);
   c->Base.type = RC_FRAGMENT_PROGRAM;
   c->Base.is_r400 = false;
   c->Base.is_r500 = false;
   c->Base.has_half_swizzles = true;
   c->Base.has_presub = true;
   c->Base.has_omod = true;
   c->Base.max_temp_regs = 32;
   c->Base.max_constants = 32;
   c->Base.max_alu_insts = 64;
   c->Base.max_tex_insts = 32;

   r300_optimize_nir(nir, &r300_screen(ps)->caps);
   nir_to_rc(nir, &r300_screen(ps)->caps, ext, code, &c->Base);
}

static void
teardown_fs(struct r300_fragment_program_compiler *c,
            struct rc_regalloc_state *rs)
{
   rc_destroy(&c->Base);
   rc_destroy_regalloc_state(rs);
}

/* Count total instructions in the compiled program. */
static unsigned
count_total_instructions(struct radeon_compiler *c)
{
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      count++;
   }
   return count;
}

/* Count emitted instructions of a given RC opcode. */
static unsigned
count_opcode(struct radeon_compiler *c, rc_opcode op)
{
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      if (inst->U.I.Opcode == op)
         count++;
   }
   return count;
}

/* Count destination or source register references to the RC sentinel temp
 * (index RC_REGISTER_MAX_INDEX - 1).  A correct program never names it; the fog
 * SSA-temp mistranslation routed the color result through it. */
static unsigned
count_sentinel_temp_refs(struct radeon_compiler *c)
{
   const unsigned sentinel = RC_REGISTER_MAX_INDEX - 1;
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      if (info->HasDstReg && inst->U.I.DstReg.File == RC_FILE_TEMPORARY &&
          inst->U.I.DstReg.Index == sentinel)
         count++;
      for (unsigned s = 0; s < info->NumSrcRegs; s++)
         if (inst->U.I.SrcReg[s].File == RC_FILE_TEMPORARY &&
             inst->U.I.SrcReg[s].Index == sentinel)
            count++;
   }
   return count;
}

/* Count source reads of a temporary register index that no prior instruction
 * wrote.  Read-before-write of a temp is the other symptom of the fog
 * mistranslation (the RC dump reads never-written temp[6]/temp[8]/temp[9]).
 * Index granularity, not per-channel: a write to any channel of temp[i] marks
 * temp[i] as defined, so this under-reports rather than false-positives.  The
 * `Index < RC_REGISTER_MAX_INDEX` guards below bound the written[] access; the
 * RC Index bitfield is RC_REGISTER_INDEX_BITS wide so they hold by construction
 * today, but they keep the array access correct if that width ever grows. */
static unsigned
count_temp_read_before_write(struct radeon_compiler *c)
{
   bool written[RC_REGISTER_MAX_INDEX] = {false};
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      for (unsigned s = 0; s < info->NumSrcRegs; s++)
         if (inst->U.I.SrcReg[s].File == RC_FILE_TEMPORARY &&
             inst->U.I.SrcReg[s].Index < RC_REGISTER_MAX_INDEX &&
             !written[inst->U.I.SrcReg[s].Index])
            count++;
      if (info->HasDstReg && inst->U.I.DstReg.File == RC_FILE_TEMPORARY &&
          inst->U.I.DstReg.Index < RC_REGISTER_MAX_INDEX)
         written[inst->U.I.DstReg.Index] = true;
   }
   return count;
}

/* Count instructions that saturate their result.  The fog factor is
 * f = fsat(ffma_weak(...)); the fsat folds into the MAD, which must then carry
 * RC_SATURATE_ZERO_ONE.  Zero saturating ops means the fold dropped the clamp
 * (f would be unbounded on hardware -- a defect that compiles clean). */
static unsigned
count_saturating_ops(struct radeon_compiler *c)
{
   unsigned count = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next)
      if (inst->U.I.SaturateMode != RC_SATURATE_NONE)
         count++;
   return count;
}

static void
dump_program(struct radeon_compiler *c, const char *label)
{
   printf("    -- %s --\n", label);
   unsigned line = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next, line++) {
      const struct rc_opcode_info *info = rc_get_opcode_info(inst->U.I.Opcode);
      printf("    %2u: %-10s dst t%u.%x", line, info->Name,
             inst->U.I.DstReg.Index, inst->U.I.DstReg.WriteMask);
      for (unsigned s = 0; s < info->NumSrcRegs; s++)
         printf("  src%u=f%u:%u", s, inst->U.I.SrcReg[s].File,
                inst->U.I.SrcReg[s].Index);
      printf("\n");
   }
}

/* A varying-fed fmad must compile cleanly and emit at least one RC_OPCODE_MAD;
 * a regression that drops op_map[nir_op_fmad] re-raises the unknown-opcode
 * error here. */
static void
case_fmad_emits_mad(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;
   run_fs(&c, &rs, build_fs(FS_MAD_FMAD));

   CHECK(!c.Base.Error, "varying-fed fmad fragment compiles without error");
   CHECK(count_opcode(&c.Base, RC_OPCODE_MAD) >= 1,
         "fmad emits at least one RC_OPCODE_MAD");

   teardown_fs(&c, &rs);
}

/* flrp is the fog / mix() path.  r300_nir_lower_flrp rewrites it into a nested
 * fmad chain, so it compiles cleanly, emits MAD, and leaves no raw flrp for the
 * emitter's defensive case to reject.  Silicon evidence lives in the mixflrp
 * rung of the RS485M EGL/GBM fragment ladder (r300_egl_gbm_render_probe):
 * uniform-fed mix(0.25, 0.75, 0.25) renders the FP24-exact 0.375 center pixel,
 * a value a swapped lerp cannot produce, so this harness checks the compile
 * shape only. */
static void
case_flrp_lowers_to_mad(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;
   run_fs(&c, &rs, build_fs(FS_MAD_FLRP));

   CHECK(!c.Base.Error,
         "varying-fed flrp fragment compiles without error after lowering");
   CHECK(count_opcode(&c.Base, RC_OPCODE_MAD) >= 1,
         "lowered flrp emits at least one RC_OPCODE_MAD");

   teardown_fs(&c, &rs);
}

/* The fog blend (st_nir_lower_fog shape) must compile into a well-formed RC
 * program: no read of an unwritten temporary and no reference to the RC
 * sentinel temp.  Both symptoms appear in the RS485M fog RC dump where the
 * lowered-flrp / ffma_weak output packing dropped the color term. */
static void
case_fog_packing_well_formed(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;
   run_fs(&c, &rs, build_fs_fog());

   CHECK(!c.Base.Error, "fog-shaped fragment compiles without error");
   if (getenv("R300_FS_HARNESS_DUMP"))
      dump_program(&c.Base, "fog");
   CHECK(count_sentinel_temp_refs(&c.Base) == 0,
         "fog program names no RC sentinel temp[RC_REGISTER_MAX_INDEX-1]");
   CHECK(count_temp_read_before_write(&c.Base) == 0,
         "fog program reads no never-written temporary");
   CHECK(count_saturating_ops(&c.Base) >= 1,
         "fog factor clamp survives as a saturating instruction");

   teardown_fs(&c, &rs);
}

/* A plain projective sample (textureProj, no bias) fits the nir_to_rc_lower_txp
 * gate, so it stays a hardware RC_OPCODE_TXP: the TMU does the perspective
 * divide at full precision and no FP24 ALU reciprocal is emitted. */
static void
case_textureproj_uses_hw_txp(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;
   run_fs(&c, &rs, build_fs_texproj(false));

   CHECK(!c.Base.Error, "plain textureProj fragment compiles without error");
   if (getenv("R300_FS_HARNESS_DUMP"))
      dump_program(&c.Base, "textureProj (no bias)");
   CHECK(count_opcode(&c.Base, RC_OPCODE_TXP) >= 1,
         "plain textureProj emits hardware RC_OPCODE_TXP");
   CHECK(count_opcode(&c.Base, RC_OPCODE_RCP) == 0,
         "plain textureProj emits no FP24 ALU RCP (divide stays in the TMU)");

   teardown_fs(&c, &rs);
}

/* textureProj with an LOD bias cannot be a single TXP, so nir_lower_tex divides
 * the coordinate on the FP24 fragment ALU (RCP + MUL) and samples with
 * RC_OPCODE_TXB.  This is the path whose FP24 reciprocal precision the
 * textureProj-bias piglit cases exercise -- not a TMU-precision divide. */
static void
case_textureproj_bias_lowers_to_fp24_rcp(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;
   run_fs(&c, &rs, build_fs_texproj(true));

   CHECK(!c.Base.Error, "textureProj+bias fragment compiles without error");
   if (getenv("R300_FS_HARNESS_DUMP"))
      dump_program(&c.Base, "textureProj (bias)");
   CHECK(count_opcode(&c.Base, RC_OPCODE_TXP) == 0,
         "textureProj+bias does not use hardware TXP");
   CHECK(count_opcode(&c.Base, RC_OPCODE_RCP) >= 1,
         "textureProj+bias divides the coordinate with an FP24 ALU RCP");
   CHECK(count_opcode(&c.Base, RC_OPCODE_TXB) >= 1,
         "textureProj+bias samples with RC_OPCODE_TXB after the divide");

   teardown_fs(&c, &rs);
}

/* r300_nir_lower_bitwise_to_arith rejects integer bitwise/shift ops with no
 * FP24-exact form to a dummy shader, but must NOT reject 1-bit boolean
 * iand/ior/ixor -- the logical connectives nir_lower_bool_to_float lowers to
 * fmul/fmin.  The H.264 deblock filter's edge-strength gate is boolean iand;
 * rejecting it substitutes a dummy deblock shader that corrupts reconstructed
 * inter macroblocks.  The data ixor confirms the reject still fires for real
 * integer bit math, so the boolean skip does not over-broaden. */
static void
case_boolean_logic_not_dummy_shaded(void)
{
   bool unsupported = false;
   r300_nir_lower_bitwise_to_arith(build_fs_boolean_iand(), &unsupported);
   CHECK(!unsupported, "boolean iand(flt, iand(flt, flt)) is left for "
                       "bool_to_float, not rejected to a dummy shader");

   unsupported = false;
   r300_nir_lower_bitwise_to_arith(build_fs_data_ixor(), &unsupported);
   CHECK(unsupported,
         "32-bit data ixor with no FP24 form is still rejected to a dummy shader");
}

/* Specimen 2: Single-use distributive factoring
 * y0 = (a & b) | (a & c)
 * Generic nir_opt_algebraic rewrites to a & (b | c).
 */
static nir_shader *
build_fs_distributive_single_use(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_distrib_single");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *b_val = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));
   nir_def *c = nir_flt(&b, nir_channel(&b, v, 2), nir_imm_float(&b, 3.0f));

   nir_def *ab = nir_iand(&b, a, b_val);
   nir_def *ac = nir_iand(&b, a, c);
   nir_def *y0 = nir_ior(&b, ab, ac);
   nir_def *f = nir_b2f32(&b, y0);
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);
   return b.shader;
}

/* Specimen 3: Simple absorption
 * y0 = a | (a & b)
 * Generic nir_opt_algebraic rewrites to a.
 */
static nir_shader *
build_fs_absorption(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_absorption");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *b_val = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));

   nir_def *ab = nir_iand(&b, a, b_val);
   nir_def *y0 = nir_ior(&b, a, ab);
   nir_def *f = nir_b2f32(&b, y0);
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);
   return b.shader;
}

/* Specimen 4 & 5:
 * p  = a & b
 * y0 = p | (a & c)
 * y1 = p | (a & d)
 *
 * If !p_observable (Specimen 4):
 *   p has fanout 2 (used only by y0 and y1).
 *   r300_nir_opt_shared_bool factors both arms:
 *     y0 = a & (b | c)
 *     y1 = a & (b | d)
 *   p, a&c, a&d are removed. Ops drop from 5 to 4.
 *
 * If p_observable (Specimen 5):
 *   y2 = p is also stored to gl_FragColor.z.
 *   p has external fanout.
 *   r300_nir_opt_shared_bool fails closed (preserves 5 operations floor).
 */
static nir_shader *
build_fs_shared_boolean_factoring(bool p_observable)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options,
      p_observable ? "r300_nir_fs_shared_bool_obs" : "r300_nir_fs_shared_bool");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *b_val = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));
   nir_def *c = nir_flt(&b, nir_channel(&b, v, 2), nir_imm_float(&b, 3.0f));
   nir_def *d = nir_flt(&b, nir_channel(&b, v, 3), nir_imm_float(&b, 4.0f));

   nir_def *p = nir_iand(&b, a, b_val);
   nir_def *ac = nir_iand(&b, a, c);
   nir_def *ad = nir_iand(&b, a, d);

   nir_def *y0 = nir_ior(&b, p, ac);
   nir_def *y1 = nir_ior(&b, p, ad);

   nir_def *f0 = nir_b2f32(&b, y0);
   nir_def *f1 = nir_b2f32(&b, y1);
   nir_def *f2 = p_observable ? nir_b2f32(&b, p) : nir_imm_float(&b, 0.0f);
   nir_def *f3 = nir_imm_float(&b, 1.0f);

   nir_store_var(&b, out, nir_vec4(&b, f0, f1, f2, f3), 0xf);
   return b.shader;
}

/* Specimen: Tropical Float shared Boolean factoring with fast math controls
 *   p  = min(a, b)
 *   y0 = max(p, min(a, c))
 *   y1 = max(p, min(a, d))
 */
static nir_shader *
build_fs_float_shared_boolean(uint32_t fp_math_ctrl)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "r300_nir_fs_float_shared_bool");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_channel(&b, v, 0);
   nir_def *b_val = nir_channel(&b, v, 1);
   nir_def *c = nir_channel(&b, v, 2);
   nir_def *d = nir_channel(&b, v, 3);

   b.fp_math_ctrl = fp_math_ctrl;
   nir_def *p = nir_fmin(&b, a, b_val);
   nir_def *ac = nir_fmin(&b, a, c);
   nir_def *ad = nir_fmin(&b, a, d);

   nir_def *y0 = nir_fmax(&b, p, ac);
   nir_def *y1 = nir_fmax(&b, p, ad);

   nir_store_var(&b, out, nir_vec4(&b, y0, y1, nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)), 0xf);
   return b.shader;
}

/* Specimen 6: XOR with both operands available
 * y = x ^ y (boolean)
 */
static nir_shader *
build_fs_xor_available(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_xor_avail");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *b_val = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));
   nir_def *x = nir_ixor(&b, a, b_val);
   nir_def *f = nir_b2f32(&b, x);
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);
   return b.shader;
}

/* Specimen 7: XOR with newly produced operand (scheduling candidate)
 * a = in.x < 1.0
 * b = (in.y < 2.0) & (in.z < 3.0)  -- newly produced immediately before XOR
 * y = a ^ b
 */
static nir_shader *
build_fs_xor_newly_produced(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_xor_new");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *t1 = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));
   nir_def *t2 = nir_flt(&b, nir_channel(&b, v, 2), nir_imm_float(&b, 3.0f));
   nir_def *b_new = nir_iand(&b, t1, t2);
   nir_def *x = nir_ixor(&b, a, b_new);
   nir_def *f = nir_b2f32(&b, x);
   nir_store_var(&b, out, nir_vec4(&b, f, f, f, f), 0xf);
   return b.shader;
}

static void
case_distributive_and_absorption(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;

   run_fs(&c, &rs, build_fs_distributive_single_use());
   CHECK(!c.Base.Error, "single-use distributive fragment compiles without error");
   teardown_fs(&c, &rs);

   run_fs(&c, &rs, build_fs_absorption());
   CHECK(!c.Base.Error, "absorption fragment compiles without error");
   teardown_fs(&c, &rs);
}

struct nir_eval_val {
   float f[4];
   uint32_t u[4];
   bool b[4];
};

static bool
eval_nir_fs_simple(nir_shader *s, const float in_v[4], float out_rgba[4])
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);
   nir_index_ssa_defs(impl);
   unsigned num_defs = impl->ssa_alloc;
   struct nir_eval_val *vals = rzalloc_array(NULL, struct nir_eval_val, num_defs);
   if (!vals)
      return false;

   memset(out_rgba, 0, 4 * sizeof(float));

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_load_const) {
            nir_load_const_instr *lc = nir_instr_as_load_const(instr);
            unsigned idx = lc->def.index;
            for (unsigned c = 0; c < lc->def.num_components; c++) {
               vals[idx].u[c] = lc->value[c].u32;
               vals[idx].f[c] = lc->value[c].f32;
               vals[idx].b[c] = (lc->value[c].u32 != 0);
            }
         } else if (instr->type == nir_instr_type_deref) {
            continue;
         } else if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_load_deref) {
               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
               if (!deref || !deref->var || deref->var->data.mode != nir_var_shader_in) {
                  ralloc_free(vals);
                  return false;
               }
               unsigned idx = intr->def.index;
               for (unsigned c = 0; c < 4; c++) {
                  vals[idx].f[c] = in_v[c];
                  memcpy(&vals[idx].u[c], &in_v[c], sizeof(float));
                  vals[idx].b[c] = (in_v[c] != 0.0f);
               }
            } else if (intr->intrinsic == nir_intrinsic_store_deref) {
               nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
               if (!deref || !deref->var || deref->var->data.mode != nir_var_shader_out) {
                  ralloc_free(vals);
                  return false;
               }
               nir_def *src_def = intr->src[1].ssa;
               unsigned idx = src_def->index;
               for (unsigned c = 0; c < 4; c++) {
                  out_rgba[c] = vals[idx].f[c];
               }
            } else {
               ralloc_free(vals);
               return false;
            }
         } else if (instr->type == nir_instr_type_alu) {
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            unsigned idx = alu->def.index;
            unsigned ncomp = alu->def.num_components;

            struct nir_eval_val s_src[4];
            for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
               unsigned s_idx = alu->src[i].src.ssa->index;
               for (unsigned c = 0; c < ncomp; c++) {
                  uint8_t swiz = alu->src[i].swizzle[c];
                  s_src[i].f[c] = vals[s_idx].f[swiz];
                  s_src[i].u[c] = vals[s_idx].u[swiz];
                  s_src[i].b[c] = vals[s_idx].b[swiz];
               }
            }

            for (unsigned c = 0; c < ncomp; c++) {
               switch (alu->op) {
               case nir_op_mov:
                  vals[idx].f[c] = s_src[0].f[c];
                  memcpy(&vals[idx].u[c], &vals[idx].f[c], sizeof(float));
                  vals[idx].b[c] = s_src[0].b[c];
                  break;
               case nir_op_vec2:
               case nir_op_vec4:
                  vals[idx].f[c] = s_src[c].f[0];
                  memcpy(&vals[idx].u[c], &vals[idx].f[c], sizeof(float));
                  vals[idx].b[c] = s_src[c].b[0];
                  break;
               case nir_op_flt:
                  vals[idx].b[c] = (s_src[0].f[c] < s_src[1].f[c]);
                  vals[idx].u[c] = vals[idx].b[c] ? 0xffffffff : 0;
                  vals[idx].f[c] = vals[idx].b[c] ? 1.0f : 0.0f;
                  break;
               case nir_op_iand:
                  vals[idx].b[c] = s_src[0].b[c] && s_src[1].b[c];
                  vals[idx].u[c] = s_src[0].u[c] & s_src[1].u[c];
                  vals[idx].f[c] = vals[idx].b[c] ? 1.0f : 0.0f;
                  break;
               case nir_op_ior:
                  vals[idx].b[c] = s_src[0].b[c] || s_src[1].b[c];
                  vals[idx].u[c] = s_src[0].u[c] | s_src[1].u[c];
                  vals[idx].f[c] = vals[idx].b[c] ? 1.0f : 0.0f;
                  break;
               case nir_op_fmin:
                  vals[idx].f[c] = (float)util_min_num(s_src[0].f[c], s_src[1].f[c]);
                  memcpy(&vals[idx].u[c], &vals[idx].f[c], sizeof(float));
                  vals[idx].b[c] = (vals[idx].f[c] > 0.0f);
                  break;
               case nir_op_fmax:
                  vals[idx].f[c] = (float)util_max_num(s_src[0].f[c], s_src[1].f[c]);
                  memcpy(&vals[idx].u[c], &vals[idx].f[c], sizeof(float));
                  vals[idx].b[c] = (vals[idx].f[c] > 0.0f);
                  break;
               case nir_op_b2f32:
                  vals[idx].b[c] = s_src[0].b[c];
                  vals[idx].f[c] = s_src[0].b[c] ? 1.0f : 0.0f;
                  memcpy(&vals[idx].u[c], &vals[idx].f[c], sizeof(float));
                  break;
               default:
                  ralloc_free(vals);
                  return false;
               }
            }
         } else {
            ralloc_free(vals);
            return false;
         }
      }
   }

   ralloc_free(vals);
   return true;
}

static unsigned
count_nir_boolean_ops(nir_shader *s, unsigned *out_and, unsigned *out_or)
{
   unsigned num_and = 0, num_or = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (alu->op == nir_op_iand || alu->op == nir_op_fmin)
                  num_and++;
               else if (alu->op == nir_op_ior || alu->op == nir_op_fmax)
                  num_or++;
            }
         }
      }
   }
   if (out_and) *out_and = num_and;
   if (out_or) *out_or = num_or;
   return num_and + num_or;
}

static nir_shader *
build_fs_vector_swizzle_counterexample(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "r300_nir_fs_vec_swizzle_counterexample");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   /* Construct 1-bit boolean predicates with different thresholds */
   nir_def *ax = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *ay = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));
   nir_def *a_vec = nir_vec2(&b, ax, ay); /* 1-bit vec2 */

   nir_def *b_val = nir_flt(&b, nir_channel(&b, v, 2), nir_imm_float(&b, 1.0f));
   nir_def *c_val = nir_flt(&b, nir_channel(&b, v, 3), nir_imm_float(&b, 2.0f));
   nir_def *d_val = nir_flt(&b, nir_channel(&b, v, 3), nir_imm_float(&b, 3.0f));

   /* Build p: iand with src[0] = a_vec.x (swizzle 0) and src[1] = b_val (1-bit) */
   nir_alu_instr *p_alu = nir_alu_instr_create(b.shader, nir_op_iand);
   nir_def_init(&p_alu->instr, &p_alu->def, 1, 1);
   p_alu->src[0].src = nir_src_for_ssa(a_vec);
   p_alu->src[0].swizzle[0] = 0;
   p_alu->src[1].src = nir_src_for_ssa(b_val);
   p_alu->src[1].swizzle[0] = 0;
   nir_builder_instr_insert(&b, &p_alu->instr);

   /* Build ac: iand with src[0] = a_vec.y (swizzle 1) and src[1] = c_val (1-bit) */
   nir_alu_instr *ac_alu = nir_alu_instr_create(b.shader, nir_op_iand);
   nir_def_init(&ac_alu->instr, &ac_alu->def, 1, 1);
   ac_alu->src[0].src = nir_src_for_ssa(a_vec);
   ac_alu->src[0].swizzle[0] = 1;
   ac_alu->src[1].src = nir_src_for_ssa(c_val);
   ac_alu->src[1].swizzle[0] = 0;
   nir_builder_instr_insert(&b, &ac_alu->instr);

   /* Build ad: iand with src[0] = a_vec.y (swizzle 1) and src[1] = d_val (1-bit) */
   nir_alu_instr *ad_alu = nir_alu_instr_create(b.shader, nir_op_iand);
   nir_def_init(&ad_alu->instr, &ad_alu->def, 1, 1);
   ad_alu->src[0].src = nir_src_for_ssa(a_vec);
   ad_alu->src[0].swizzle[0] = 1;
   ad_alu->src[1].src = nir_src_for_ssa(d_val);
   ad_alu->src[1].swizzle[0] = 0;
   nir_builder_instr_insert(&b, &ad_alu->instr);

   nir_def *y0 = nir_ior(&b, &p_alu->def, &ac_alu->def);
   nir_def *y1 = nir_ior(&b, &p_alu->def, &ad_alu->def);

   nir_def *f0 = nir_b2f32(&b, y0);
   nir_def *f1 = nir_b2f32(&b, y1);
   nir_store_var(&b, out, nir_vec4(&b, f0, f1, nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)), 0xf);
   return b.shader;
}

static void
case_nir_shared_bool_pass_direct(void)
{
   /* Test 1: Standard 5-op shared Boolean pattern (Specimen 4) */
   nir_shader *s_orig = build_fs_shared_boolean_factoring(false);
   nir_validate_shader(s_orig, "shared bool initial fixture");

   unsigned and_pre = 0, or_pre = 0;
   unsigned total_pre = count_nir_boolean_ops(s_orig, &and_pre, &or_pre);
   CHECK(total_pre == 5 && and_pre == 3 && or_pre == 2,
         "pre-optimization NIR graph has exactly 5 Boolean ALU ops (3 iand, 2 ior)");

   /* Calibration check: DCE alone without shared factoring preserves all 5 operations */
   nir_shader *s = nir_shader_clone(NULL, s_orig);
   nir_opt_dce(s);
   unsigned total_dce_only = count_nir_boolean_ops(s, NULL, NULL);
   CHECK(total_dce_only == 5,
         "calibration: DCE alone without shared factoring leaves 5 Boolean ops intact");

   /* Run r300_nir_opt_shared_bool: must report progress */
   bool progress = r300_nir_opt_shared_bool(s);
   CHECK(progress, "r300_nir_opt_shared_bool reports progress on 5-op shared Boolean pattern");
   nir_validate_shader(s, "after r300_nir_opt_shared_bool");

   /* After DCE of dead p, ac, ad */
   nir_opt_dce(s);
   unsigned and_post = 0, or_post = 0;
   unsigned total_post = count_nir_boolean_ops(s, &and_post, &or_post);
   CHECK(total_post == 4 && and_post == 2 && or_post == 2,
         "post-optimization NIR graph has exactly 4 Boolean ALU ops (2 iand, 2 ior: 5 -> 4)");

   /* Idempotence: second pass makes no progress */
   bool progress2 = r300_nir_opt_shared_bool(s);
   CHECK(!progress2, "second pass of r300_nir_opt_shared_bool reports no progress (idempotent)");

   /* Semantic evaluation of actual NIR shader graphs across all 16 inputs {0,1}^4 */
   unsigned tt_matches_orig = 0;
   unsigned tt_matches_fact = 0;
   bool eval_ok_all = true;
   for (unsigned i = 0; i < 16; i++) {
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      bool d = (i >> 3) & 1;

      float exp_y0 = (a && (b_val || c)) ? 1.0f : 0.0f;
      float exp_y1 = (a && (b_val || d)) ? 1.0f : 0.0f;

      float v[4] = {
         a ? 0.5f : 1.5f,
         b_val ? 1.5f : 2.5f,
         c ? 2.5f : 3.5f,
         d ? 3.5f : 4.5f,
      };

      float out_orig[4] = {0};
      float out_fact[4] = {0};

      bool ok_orig = eval_nir_fs_simple(s_orig, v, out_orig);
      bool ok_fact = eval_nir_fs_simple(s, v, out_fact);
      eval_ok_all = eval_ok_all && ok_orig && ok_fact;

      if (out_orig[0] == exp_y0 && out_orig[1] == exp_y1)
         tt_matches_orig++;
      if (out_fact[0] == exp_y0 && out_fact[1] == exp_y1 &&
          out_fact[0] == out_orig[0] && out_fact[1] == out_orig[1])
         tt_matches_fact++;
   }
   CHECK(eval_ok_all,
         "eval_nir_fs_simple succeeded across all 16 truth-table assignments");
   CHECK(tt_matches_orig == 16,
         "16/16 actual original NIR graph outputs match independent formula");
   CHECK(tt_matches_fact == 16,
         "16/16 actual factored NIR graph outputs match original graph and formula");

   /* Evaluator Calibration: deliberately mutate shader by swapping output channels y0 and y1.
    * Use nir_alu_src_rewrite_scalar to preserve intrusive use-list integrity.
    * Output swap mismatches if and only if y0 != y1, which occurs precisely when
    * a = 1, b = 0, c != d (i=5: 1,0,1,0 and i=9: 1,0,0,1). The other 14 assignments match.
    */
   nir_shader *s_mut = nir_shader_clone(NULL, s);
   nir_foreach_function_impl(m_impl, s_mut) {
      nir_foreach_block(m_blk, m_impl) {
         nir_foreach_instr(m_ins, m_blk) {
            if (m_ins->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *store = nir_instr_as_intrinsic(m_ins);
               if (store->intrinsic == nir_intrinsic_store_deref) {
                  nir_alu_instr *vec = nir_def_as_alu_or_null(store->src[1].ssa);
                  if (vec && (vec->op == nir_op_vec4 || vec->op == nir_op_vec2)) {
                     nir_scalar s0 = nir_get_scalar(vec->src[0].src.ssa, vec->src[0].swizzle[0]);
                     nir_scalar s1 = nir_get_scalar(vec->src[1].src.ssa, vec->src[1].swizzle[0]);
                     nir_alu_src_rewrite_scalar(&vec->src[0], s1);
                     nir_alu_src_rewrite_scalar(&vec->src[1], s0);
                  }
               }
            }
         }
      }
   }
   nir_validate_shader(s_mut, "after output swap mutation");

   unsigned mut_matches = 0;
   uint32_t mut_mismatch_mask = 0;
   bool mut_eval_ok_all = true;
   for (unsigned i = 0; i < 16; i++) {
      float v[4] = {
         ((i >> 0) & 1) ? 0.5f : 1.5f,
         ((i >> 1) & 1) ? 1.5f : 2.5f,
         ((i >> 2) & 1) ? 2.5f : 3.5f,
         ((i >> 3) & 1) ? 3.5f : 4.5f,
      };
      float out_mut[4] = {0};
      bool ok = eval_nir_fs_simple(s_mut, v, out_mut);
      mut_eval_ok_all = mut_eval_ok_all && ok;
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      bool d = (i >> 3) & 1;
      float exp_y0 = (a && (b_val || c)) ? 1.0f : 0.0f;
      float exp_y1 = (a && (b_val || d)) ? 1.0f : 0.0f;
      if (out_mut[0] == exp_y0 && out_mut[1] == exp_y1) {
         mut_matches++;
      } else {
         mut_mismatch_mask |= (1u << i);
      }
   }
   CHECK(mut_eval_ok_all,
         "eval_nir_fs_simple succeeded for all mutated shader inputs");
   CHECK(mut_matches == 14,
         "calibration: output swap mutation matches exactly 14/16 truth table entries");
   CHECK(mut_mismatch_mask == ((1u << 5) | (1u << 9)),
         "calibration: output swap mismatches precisely on i=5 (1,0,1,0) and i=9 (1,0,0,1)");

   ralloc_free(s_mut);
   ralloc_free(s_orig);
   ralloc_free(s);

   /* Test 2: Observable p (Specimen 5) - must refuse to rewrite */
   nir_shader *s_obs = build_fs_shared_boolean_factoring(true);
   nir_validate_shader(s_obs, "shared bool observable fixture");
   bool progress_obs = r300_nir_opt_shared_bool(s_obs);
   CHECK(!progress_obs, "r300_nir_opt_shared_bool refuses when p has external fanout");
   ralloc_free(s_obs);

   /* Test 3: Vector swizzle counterexample (Specimen 8) - must refuse to rewrite */
   nir_shader *s_vec = build_fs_vector_swizzle_counterexample();
   nir_validate_shader(s_vec, "vector swizzle counterexample before pass");
   bool progress_vec = r300_nir_opt_shared_bool(s_vec);
   CHECK(!progress_vec, "r300_nir_opt_shared_bool refuses vector/swizzled inputs (fail-closed)");
   nir_validate_shader(s_vec, "vector swizzle counterexample after pass");

   /* Concrete counterexample semantic test:
    * For v = (2.0f, 0.0f, 2.0f, 0.0f):
    * ax = (2.0 < 1.0) = 0
    * ay = (0.0 < 2.0) = 1
    * b  = (2.0 < 1.0) = 0
    * c  = (0.0 < 2.0) = 1
    * d  = (0.0 < 3.0) = 1
    * Correct output: p = 0 & 0 = 0, y0 = 0 | (1 & 1) = 1, y1 = 0 | (1 & 1) = 1 -> (1.0, 1.0)
    * Buggy rewrite conflating ax and ay would yield: y0 = 0 & (0 | 1) = 0, y1 = 0 -> (0.0, 0.0)
    */
   float v_counter[4] = { 2.0f, 0.0f, 2.0f, 0.0f };
   float out_vec[4] = {0};
   bool counter_ok = eval_nir_fs_simple(s_vec, v_counter, out_vec);
   CHECK(counter_ok, "eval_nir_fs_simple succeeded for vector swizzle counterexample");
   CHECK(out_vec[0] == 1.0f && out_vec[1] == 1.0f,
         "counterexample preserves correct output (1,1) under fail-closed refusal");
   ralloc_free(s_vec);

   /* Test 4: Floating-point shared Boolean with nir_fp_preserve_nan - must refuse to rewrite.
    * Witness: v = (1.0f, NAN, 0.0f, 0.25f)
    * Baseline:
    *   p  = fmin(1.0, NAN) = 1.0
    *   ac = fmin(1.0, 0.0) = 0.0
    *   ad = fmin(1.0, 0.25) = 0.25
    *   y0 = fmax(p, ac) = fmax(1.0, 0.0) = 1.0
    *   y1 = fmax(p, ad) = fmax(1.0, 0.25) = 1.0  -> output (1.0, 1.0)
    *
    * If factored without NaN guard:
    *   bc = fmax(NAN, 0.0) = 0.0
    *   bd = fmax(NAN, 0.25) = 0.25
    *   y0 = fmin(1.0, 0.0) = 0.0
    *   y1 = fmin(1.0, 0.25) = 0.25               -> output (0.0, 0.25) [CORRUPTED!]
    */
   nir_shader *s_nan = build_fs_float_shared_boolean(nir_fp_preserve_nan | nir_fp_preserve_inf);
   nir_validate_shader(s_nan, "float shared bool with preserve_nan before pass");
   bool progress_nan = r300_nir_opt_shared_bool(s_nan);
   CHECK(!progress_nan,
         "r300_nir_opt_shared_bool refuses float shared factoring when nir_fp_preserve_nan is set");

   float v_nan[4] = { 1.0f, NAN, 0.0f, 0.25f };
   float out_nan[4] = { 0 };
   bool eval_nan_ok = eval_nir_fs_simple(s_nan, v_nan, out_nan);
   CHECK(eval_nan_ok, "eval_nir_fs_simple succeeded for NaN specimen");
   CHECK(out_nan[0] == 1.0f && out_nan[1] == 1.0f,
         "fail-closed NaN refusal preserves correct IEEE 754-2019 output (1.0, 1.0)");
   ralloc_free(s_nan);

   /* Test 5: Floating-point shared Boolean without NaN flags (e.g. nir_fp_preserve_inf)
    * must rewrite and retain valid fp_math_ctrl on replacement instructions.
    */
   nir_shader *s_fp_unflagged = build_fs_float_shared_boolean(nir_fp_preserve_inf);
   nir_validate_shader(s_fp_unflagged, "float shared bool unflagged before pass");
   bool progress_fp = r300_nir_opt_shared_bool(s_fp_unflagged);
   CHECK(progress_fp,
         "r300_nir_opt_shared_bool rewrites float shared factoring when preserve_nan is absent");
   nir_validate_shader(s_fp_unflagged, "float shared bool unflagged after pass");

   /* Verify that replacement ALU instructions retain nir_fp_preserve_inf */
   bool fp_ctrl_retained = false;
   nir_foreach_function_impl(f_impl, s_fp_unflagged) {
      nir_foreach_block(f_blk, f_impl) {
         nir_foreach_instr(f_ins, f_blk) {
            if (f_ins->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(f_ins);
               if (alu->op == nir_op_fmin || alu->op == nir_op_fmax) {
                  if (alu->fp_math_ctrl & nir_fp_preserve_inf)
                     fp_ctrl_retained = true;
               }
            }
         }
      }
   }
   CHECK(fp_ctrl_retained,
         "replacement float ALU instructions preserve inherited fp_math_ctrl (nir_fp_preserve_inf)");
   ralloc_free(s_fp_unflagged);
}

static void
case_shared_boolean_factoring(void)
{
   struct r300_fragment_program_compiler c_factored;
   struct rc_regalloc_state rs_factored;
   run_fs(&c_factored, &rs_factored, build_fs_shared_boolean_factoring(false));
   CHECK(!c_factored.Base.Error, "shared boolean factoring compiles without error");
   unsigned insts_factored = count_total_instructions(&c_factored.Base);

   if (getenv("R300_FS_HARNESS_DUMP"))
      dump_program(&c_factored.Base, "shared boolean factored");

   struct r300_fragment_program_compiler c_obs;
   struct rc_regalloc_state rs_obs;
   run_fs(&c_obs, &rs_obs, build_fs_shared_boolean_factoring(true));
   CHECK(!c_obs.Base.Error, "shared boolean with observable p compiles without error");
   unsigned insts_obs = count_total_instructions(&c_obs.Base);

   if (getenv("R300_FS_HARNESS_DUMP"))
      dump_program(&c_obs.Base, "shared boolean observable (counterexample)");

   printf("    shared boolean factored inst count: %u, observable inst count: %u\n",
          insts_factored, insts_obs);
   CHECK(insts_factored <= insts_obs,
         "joint factoring achieves <= instruction count compared to observable graph");

   teardown_fs(&c_factored, &rs_factored);
   teardown_fs(&c_obs, &rs_obs);
}

static void
case_xor_scheduling(void)
{
   struct r300_fragment_program_compiler c;
   struct rc_regalloc_state rs;

   run_fs(&c, &rs, build_fs_xor_available());
   CHECK(!c.Base.Error, "xor available fragment compiles without error");
   teardown_fs(&c, &rs);

   run_fs(&c, &rs, build_fs_xor_newly_produced());
   CHECK(!c.Base.Error, "xor newly produced fragment compiles without error");
   teardown_fs(&c, &rs);
}

int
main(void)
{
   printf("r300 fragment NIR-to-RC admission harness\n");
   case_fmad_emits_mad();
   case_flrp_lowers_to_mad();
   case_fog_packing_well_formed();
   case_textureproj_uses_hw_txp();
   case_textureproj_bias_lowers_to_fp24_rcp();
   case_boolean_logic_not_dummy_shaded();
   case_distributive_and_absorption();
   case_nir_shared_bool_pass_direct();
   case_shared_boolean_factoring();
   case_xor_scheduling();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
