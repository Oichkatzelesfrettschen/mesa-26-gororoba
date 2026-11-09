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
#include "r300_pair_eval.h"
#include "r300_screen.h"
#include "radeon_compiler.h"
#include "radeon_program.h"
#include "radeon_program_constants.h"
#include "radeon_program_pair.h"
#include "radeon_regalloc.h"
#include "classic/r300_classic_validate_schedule.h"

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

/* Specimen 1: Independent 3-predicate conjunction g(a, b, c) = a & b & c
 * over three distinct channels (v.x < 1.0 && v.y < 2.0 && v.z < 3.0).
 * Serves as the neutral control: none of the predicates can fold into one
 * another, r300_nir_opt_shared_bool leaves it untouched, and the full
 * pipeline evaluates all 8 truth-table assignments {0,1}^3 accurately. */
static nir_shader *
build_fs_independent_abc(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                                  "r300_nir_fs_indep_abc");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v = nir_load_var(&b, in0);
   nir_def *a = nir_flt(&b, nir_channel(&b, v, 0), nir_imm_float(&b, 1.0f));
   nir_def *b_val = nir_flt(&b, nir_channel(&b, v, 1), nir_imm_float(&b, 2.0f));
   nir_def *c = nir_flt(&b, nir_channel(&b, v, 2), nir_imm_float(&b, 3.0f));
   nir_def *gate = nir_iand(&b, a, nir_iand(&b, b_val, c));
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

static void
allocate_identity_inputs(struct r300_fragment_program_compiler *c,
                         void (*allocate)(void *data, unsigned input,
                                          unsigned hwreg),
                         void *mydata)
{
   (void)c;
   for (unsigned i = 0; i < 8; i++)
      allocate(mydata, i, i);
}

/* Run the production NIR-to-RC lowering and emitter as a fragment program.
 *
 * Production runs r300_optimize_nir() at shader-state creation
 * (r300_create_fs_state, r300_state.c) before nir_to_rc() translates the NIR
 * into RC instructions.  The fog / mix() flrp lowering runs in that first
 * stage, so the harness runs it before calling nir_to_rc(). */
static void
run_fs_with_code(struct r300_fragment_program_compiler *c,
                 struct rc_regalloc_state *rs,
                 struct r300_fragment_shader_code *fs_code,
                 nir_shader *nir)
{
   struct r300_screen screen = {0};
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};
   memset(fs_code, 0, sizeof(*fs_code));
   union r300_shader_code code = {
      .f = fs_code,
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
   c->code = &fs_code->code;
   c->AllocateHwInputs = allocate_identity_inputs;

   r300_optimize_nir(nir, &r300_screen(ps)->caps);
   nir_to_rc(nir, &r300_screen(ps)->caps, ext, code, &c->Base);
}

static void
run_fs(struct r300_fragment_program_compiler *c, struct rc_regalloc_state *rs,
       nir_shader *nir)
{
   struct r300_fragment_shader_code fs_code = {0};
   run_fs_with_code(c, rs, &fs_code, nir);
}

/* Count active written scalar channels across normal-form RC Boolean-lowered
 * arithmetic instructions (MUL, MIN, MAX).  Because nir_opt_vectorize packs
 * independent scalar Boolean ops into vec2/vec3/vec4 instructions before
 * nir_to_rc, counting instruction nodes alone can read 1 vec3 MUL vs 1 vec2
 * MAX while the active written component count drops from 5 to 4. */
static unsigned
count_active_bool_components(struct radeon_compiler *c)
{
   unsigned comps = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      if (inst->Type == RC_INSTRUCTION_NORMAL &&
          (inst->U.I.Opcode == RC_OPCODE_MUL ||
           inst->U.I.Opcode == RC_OPCODE_MIN ||
           inst->U.I.Opcode == RC_OPCODE_MAX)) {
         comps += util_bitcount(inst->U.I.DstReg.WriteMask);
      }
   }
   return comps;
}

static unsigned
count_pair_nops(struct radeon_compiler *c)
{
   unsigned nops = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      if (inst->Type == RC_INSTRUCTION_PAIR && inst->U.P.Nop)
         nops++;
   }
   return nops;
}

static unsigned
count_pair_presubs(struct radeon_compiler *c)
{
   unsigned presubs = 0;
   for (struct rc_instruction *inst = c->Program.Instructions.Next;
        inst != &c->Program.Instructions; inst = inst->Next) {
      if (inst->Type == RC_INSTRUCTION_PAIR) {
         if (inst->U.P.RGB.Src[RC_PAIR_PRESUB_SRC].Used)
            presubs++;
         if (inst->U.P.Alpha.Src[RC_PAIR_PRESUB_SRC].Used)
            presubs++;
      }
   }
   return presubs;
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

static bool
eval_nir_fs_simple(nir_shader *s, const float in_v[4], float out_rgba[4]);

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

   /* Specimen 1: Independent 3-predicate conjunction g(a, b, c) = a & b & c
    * over three distinct channels (v.x < 1.0 && v.y < 2.0 && v.z < 3.0).
    * Neutral control: r300_nir_opt_shared_bool must return false, and both
    * NIR evaluation and full backend paired evaluation must match all 8/8
    * truth-table assignments {0,1}^3. */
   nir_shader *s_indep = build_fs_independent_abc();
   nir_validate_shader(s_indep, "independent abc initial");
   bool indep_progress = r300_nir_opt_shared_bool(s_indep);
   CHECK(!indep_progress,
         "neutral control: r300_nir_opt_shared_bool leaves independent a & b & c untouched");

   unsigned indep_nir_matches = 0;
   for (unsigned i = 0; i < 8; i++) {
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      float exp_g = (a && b_val && c) ? 1.0f : 0.0f;
      float v[4] = {
         a ? 0.5f : 1.5f,
         b_val ? 1.5f : 2.5f,
         c ? 2.5f : 3.5f,
         0.0f,
      };
      float out_rgba[4] = {0};
      if (eval_nir_fs_simple(s_indep, v, out_rgba) && out_rgba[0] == exp_g)
         indep_nir_matches++;
   }
   CHECK(indep_nir_matches == 8,
         "neutral control: 8/8 NIR truth-table evaluations match a & b & c");

   struct r300_fragment_program_compiler c_indep;
   struct rc_regalloc_state rs_indep;
   struct r300_fragment_shader_code code_indep;
   run_fs_with_code(&c_indep, &rs_indep, &code_indep, s_indep);
   CHECK(!c_indep.Base.Error,
         "neutral control: independent a & b & c lowers to RC without error");
   r3xx_compile_fragment_program(&c_indep);
   CHECK(!c_indep.Base.Error,
         "neutral control: independent a & b & c compiles through full R300 backend");

   const char *reject_indep = NULL;
   CHECK(r300_classic_validate_schedule(&c_indep.Base, &reject_indep),
         "neutral control: independent a & b & c paired schedule is legal");

   unsigned indep_pair_matches = 0;
   for (unsigned i = 0; i < 8; i++) {
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      float exp_g = (a && b_val && c) ? 1.0f : 0.0f;

      struct r300_pair_eval ev;
      memset(&ev, 0, sizeof(ev));
      ev.consts = &c_indep.Base.Program.Constants;
      ev.inputs[0][0] = a ? 0.5f : 1.5f;
      ev.inputs[0][1] = b_val ? 1.5f : 2.5f;
      ev.inputs[0][2] = c ? 2.5f : 3.5f;
      ev.inputs[0][3] = 0.0f;
      if (r300_pair_eval_program(&ev, &c_indep.Base) &&
          ev.outputs[0][0] == exp_g && ev.outputs[0][1] == exp_g &&
          ev.outputs[0][2] == exp_g && ev.outputs[0][3] == exp_g) {
         indep_pair_matches++;
      }
   }
   CHECK(indep_pair_matches == 8,
         "neutral control: 8/8 paired backend evaluations match a & b & c");
   teardown_fs(&c_indep, &rs_indep);
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

   /* Clean dead original instructions (p, ac, ad) */
   nir_opt_dce(s_fp_unflagged);
   nir_validate_shader(s_fp_unflagged, "after clean float unflagged shared bool");

   /* Require exactly 4 remaining min/max instructions: 2 minima and 2 maxima */
   unsigned fp_min_count = 0, fp_max_count = 0;
   unsigned fp_total = count_nir_boolean_ops(s_fp_unflagged, &fp_min_count, &fp_max_count);
   CHECK(fp_total == 4 && fp_min_count == 2 && fp_max_count == 2,
         "clean post-optimization float NIR graph has exactly 4 min/max ALU ops (2 fmin, 2 fmax)");

   /* Require EVERY one of those 4 live replacement instructions to retain the expected controls */
   unsigned flagged_min_max_count = 0;
   bool all_replacements_retain_ctrl = true;
   nir_foreach_function_impl(f_impl, s_fp_unflagged) {
      nir_foreach_block(f_blk, f_impl) {
         nir_foreach_instr(f_ins, f_blk) {
            if (f_ins->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(f_ins);
               if (alu->op == nir_op_fmin || alu->op == nir_op_fmax) {
                  flagged_min_max_count++;
                  if ((alu->fp_math_ctrl & nir_fp_preserve_inf) != nir_fp_preserve_inf)
                     all_replacements_retain_ctrl = false;
               }
            }
         }
      }
   }
   CHECK(flagged_min_max_count == 4 && all_replacements_retain_ctrl,
         "all 4/4 live replacement float ALU instructions retain inherited fp_math_ctrl (nir_fp_preserve_inf)");

   /* Negative calibration: verify that if a replacement instruction loses its controls,
    * the universal retention check detects it. */
   nir_shader *s_fp_calib = nir_shader_clone(NULL, s_fp_unflagged);
   bool stripped_one = false;
   nir_foreach_function_impl(c_impl, s_fp_calib) {
      nir_foreach_block(c_blk, c_impl) {
         nir_foreach_instr(c_ins, c_blk) {
            if (c_ins->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(c_ins);
               if ((alu->op == nir_op_fmin || alu->op == nir_op_fmax) && !stripped_one) {
                  alu->fp_math_ctrl = 0;
                  stripped_one = true;
               }
            }
         }
      }
   }
   bool calib_all_retain = true;
   nir_foreach_function_impl(c_impl, s_fp_calib) {
      nir_foreach_block(c_blk, c_impl) {
         nir_foreach_instr(c_ins, c_blk) {
            if (c_ins->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(c_ins);
               if (alu->op == nir_op_fmin || alu->op == nir_op_fmax) {
                  if ((alu->fp_math_ctrl & nir_fp_preserve_inf) != nir_fp_preserve_inf)
                     calib_all_retain = false;
               }
            }
         }
      }
   }
   CHECK(stripped_one && !calib_all_retain,
         "calibration: universal control check detects instruction with stripped fp_math_ctrl");
   ralloc_free(s_fp_calib);
   ralloc_free(s_fp_unflagged);
}

/* Dual-clause 4-output shared Boolean specimen:
 *   Clause 0 (from in_v0):
 *     p0 = a0 & b0
 *     y0 = p0 | (a0 & c0)
 *     y1 = p0 | (a0 & d0)
 *   Clause 1 (from in_v1):
 *     p1 = a1 & b1
 *     y2 = p1 | (a1 & c1)
 *     y3 = p1 | (a1 & d1)
 *   Output: gl_FragColor = vec4(y0, y1, y2, y3)
 *
 * Without r300_nir_opt_shared_bool:
 *   6 iand (fmul) operations + 4 ior (fmax) operations = 10 active Boolean
 *   channels.  Because 6 fmul channels exceed the 4-channel width of a single
 *   R300 vector instruction, nir_opt_vectorize / nir_to_rc must emit two MUL
 *   instructions (vec4 + vec2) plus one vec4 MAX instruction.
 *
 * With r300_nir_opt_shared_bool:
 *   4 ior (fmax) operations + 4 iand (fmul) operations = 8 active Boolean
 *   channels, which pack into a single vec4 MAX instruction and a single vec4
 *   MUL instruction, reducing both normal-form RC instructions and final
 *   encoded hardware paired ALU slots. */
static nir_shader *
build_fs_dual_shared_boolean_factoring(void)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, &options, "r300_nir_fs_dual_shared_bool");
   const struct glsl_type *vec4 = glsl_vec4_type();
   nir_variable *in0 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v0");
   in0->data.location = VARYING_SLOT_VAR0;
   in0->data.driver_location = 0;
   in0->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *in1 = nir_variable_create(b.shader, nir_var_shader_in, vec4, "in_v1");
   in1->data.location = VARYING_SLOT_VAR1;
   in1->data.driver_location = 1;
   in1->data.interpolation = INTERP_MODE_SMOOTH;
   nir_variable *out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_FragColor");
   out->data.location = FRAG_RESULT_COLOR;
   out->data.driver_location = 0;

   nir_def *v0 = nir_load_var(&b, in0);
   nir_def *a0 = nir_flt(&b, nir_channel(&b, v0, 0), nir_imm_float(&b, 1.0f));
   nir_def *b0 = nir_flt(&b, nir_channel(&b, v0, 1), nir_imm_float(&b, 2.0f));
   nir_def *c0 = nir_flt(&b, nir_channel(&b, v0, 2), nir_imm_float(&b, 3.0f));
   nir_def *d0 = nir_flt(&b, nir_channel(&b, v0, 3), nir_imm_float(&b, 4.0f));

   nir_def *p0 = nir_iand(&b, a0, b0);
   nir_def *ac0 = nir_iand(&b, a0, c0);
   nir_def *ad0 = nir_iand(&b, a0, d0);
   nir_def *y0 = nir_ior(&b, p0, ac0);
   nir_def *y1 = nir_ior(&b, p0, ad0);

   nir_def *v1 = nir_load_var(&b, in1);
   nir_def *a1 = nir_flt(&b, nir_channel(&b, v1, 0), nir_imm_float(&b, 1.0f));
   nir_def *b1 = nir_flt(&b, nir_channel(&b, v1, 1), nir_imm_float(&b, 2.0f));
   nir_def *c1 = nir_flt(&b, nir_channel(&b, v1, 2), nir_imm_float(&b, 3.0f));
   nir_def *d1 = nir_flt(&b, nir_channel(&b, v1, 3), nir_imm_float(&b, 4.0f));

   nir_def *p1 = nir_iand(&b, a1, b1);
   nir_def *ac1 = nir_iand(&b, a1, c1);
   nir_def *ad1 = nir_iand(&b, a1, d1);
   nir_def *y2 = nir_ior(&b, p1, ac1);
   nir_def *y3 = nir_ior(&b, p1, ad1);

   nir_store_var(&b, out,
                 nir_vec4(&b, nir_b2f32(&b, y0), nir_b2f32(&b, y1),
                          nir_b2f32(&b, y2), nir_b2f32(&b, y3)),
                 0xf);
   return b.shader;
}

static void
case_shared_boolean_factoring(void)
{
   /* 1. Same-shader comparison on Specimen 4 (2-output shared Boolean) with
    *    and without r300_nir_opt_shared_bool. */
   struct r300_fragment_program_compiler c_unfact;
   struct rc_regalloc_state rs_unfact;
   struct r300_fragment_shader_code code_unfact;
   setenv("R300_DISABLE_SHARED_BOOL", "1", 1);
   run_fs_with_code(&c_unfact, &rs_unfact, &code_unfact,
                    build_fs_shared_boolean_factoring(false));
   unsetenv("R300_DISABLE_SHARED_BOOL");
   CHECK(!c_unfact.Base.Error,
         "same-shader unfactored baseline lowers to RC without error");
   unsigned rc_insts_unfact = count_total_instructions(&c_unfact.Base);
   unsigned bool_comps_unfact = count_active_bool_components(&c_unfact.Base);

   struct r300_fragment_program_compiler c_factored;
   struct rc_regalloc_state rs_factored;
   struct r300_fragment_shader_code code_factored;
   run_fs_with_code(&c_factored, &rs_factored, &code_factored,
                    build_fs_shared_boolean_factoring(false));
   CHECK(!c_factored.Base.Error,
         "shared boolean factoring lowers to RC without error");
   unsigned rc_insts_factored = count_total_instructions(&c_factored.Base);
   unsigned bool_comps_factored = count_active_bool_components(&c_factored.Base);

   if (getenv("R300_FS_HARNESS_DUMP")) {
      dump_program(&c_unfact.Base, "shared boolean unfactored (same shader)");
      dump_program(&c_factored.Base, "shared boolean factored (same shader)");
   }

   CHECK(bool_comps_unfact == 5 && bool_comps_factored == 4,
         "same-shader active RC Boolean channel operations drop from 5 to 4 (-20%)");

   r3xx_compile_fragment_program(&c_unfact);
   r3xx_compile_fragment_program(&c_factored);
   CHECK(!c_unfact.Base.Error && !c_factored.Base.Error,
         "same-shader unfactored and factored programs compile through full R300 backend");

   const char *reject = NULL;
   CHECK(r300_classic_validate_schedule(&c_unfact.Base, &reject),
         "same-shader unfactored paired schedule passes legality validator");
   CHECK(r300_classic_validate_schedule(&c_factored.Base, &reject),
         "same-shader factored paired schedule passes legality validator");

   unsigned alu_slots_unfact = code_unfact.code.code.r300.alu.length;
   unsigned alu_slots_fact = code_factored.code.code.r300.alu.length;
   unsigned pixsize_unfact = code_unfact.code.code.r300.pixsize;
   unsigned pixsize_fact = code_factored.code.code.r300.pixsize;
   unsigned nops_unfact = count_pair_nops(&c_unfact.Base);
   unsigned nops_fact = count_pair_nops(&c_factored.Base);

   printf("    [single-clause same-shader] unfactored: rc_insts=%u bool_comps=%u "
          "alu_slots=%u pixsize=%u nops=%u | factored: rc_insts=%u bool_comps=%u "
          "alu_slots=%u pixsize=%u nops=%u\n",
          rc_insts_unfact, bool_comps_unfact, alu_slots_unfact, pixsize_unfact,
          nops_unfact, rc_insts_factored, bool_comps_factored, alu_slots_fact,
          pixsize_fact, nops_fact);

   CHECK(alu_slots_fact <= alu_slots_unfact,
         "same-shader factored encoded paired ALU slots <= unfactored baseline");

   unsigned pair_tt_matches = 0;
   for (unsigned i = 0; i < 16; i++) {
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      bool d = (i >> 3) & 1;
      float exp_y0 = (a && (b_val || c)) ? 1.0f : 0.0f;
      float exp_y1 = (a && (b_val || d)) ? 1.0f : 0.0f;

      struct r300_pair_eval ev_unfact, ev_fact;
      memset(&ev_unfact, 0, sizeof(ev_unfact));
      memset(&ev_fact, 0, sizeof(ev_fact));
      ev_unfact.consts = &c_unfact.Base.Program.Constants;
      ev_fact.consts = &c_factored.Base.Program.Constants;

      float v[4] = {
         a ? 0.5f : 1.5f,
         b_val ? 1.5f : 2.5f,
         c ? 2.5f : 3.5f,
         d ? 3.5f : 4.5f,
      };
      memcpy(ev_unfact.inputs[0], v, sizeof(v));
      memcpy(ev_fact.inputs[0], v, sizeof(v));

      if (r300_pair_eval_program(&ev_unfact, &c_unfact.Base) &&
          r300_pair_eval_program(&ev_fact, &c_factored.Base) &&
          ev_fact.outputs[0][0] == exp_y0 &&
          ev_fact.outputs[0][1] == exp_y1 &&
          ev_fact.outputs[0][2] == 0.0f &&
          ev_fact.outputs[0][3] == 1.0f &&
          ev_unfact.outputs[0][0] == ev_fact.outputs[0][0] &&
          ev_unfact.outputs[0][1] == ev_fact.outputs[0][1]) {
         pair_tt_matches++;
      }
   }
   CHECK(pair_tt_matches == 16,
         "16/16 paired backend evaluations match across unfactored and factored schedules");

   /* 2. Specimen 5: Observable p (3-output counterexample) */
   struct r300_fragment_program_compiler c_obs;
   struct rc_regalloc_state rs_obs;
   struct r300_fragment_shader_code code_obs;
   run_fs_with_code(&c_obs, &rs_obs, &code_obs,
                    build_fs_shared_boolean_factoring(true));
   CHECK(!c_obs.Base.Error, "shared boolean with observable p compiles without error");
   unsigned rc_insts_obs = count_total_instructions(&c_obs.Base);
   unsigned bool_comps_obs = count_active_bool_components(&c_obs.Base);

   if (getenv("R300_FS_HARNESS_DUMP"))
      dump_program(&c_obs.Base, "shared boolean observable (counterexample)");

   CHECK(bool_comps_obs == 5,
         "observable p retains 5 active RC Boolean channel operations");
   r3xx_compile_fragment_program(&c_obs);
   CHECK(!c_obs.Base.Error && r300_classic_validate_schedule(&c_obs.Base, &reject),
         "observable p compiles through full backend with legal paired schedule");

   unsigned obs_tt_matches = 0;
   for (unsigned i = 0; i < 16; i++) {
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      bool c = (i >> 2) & 1;
      bool d = (i >> 3) & 1;
      float exp_y0 = (a && (b_val || c)) ? 1.0f : 0.0f;
      float exp_y1 = (a && (b_val || d)) ? 1.0f : 0.0f;
      float exp_p = (a && b_val) ? 1.0f : 0.0f;

      struct r300_pair_eval ev_obs;
      memset(&ev_obs, 0, sizeof(ev_obs));
      ev_obs.consts = &c_obs.Base.Program.Constants;
      ev_obs.inputs[0][0] = a ? 0.5f : 1.5f;
      ev_obs.inputs[0][1] = b_val ? 1.5f : 2.5f;
      ev_obs.inputs[0][2] = c ? 2.5f : 3.5f;
      ev_obs.inputs[0][3] = d ? 3.5f : 4.5f;

      if (r300_pair_eval_program(&ev_obs, &c_obs.Base) &&
          ev_obs.outputs[0][0] == exp_y0 &&
          ev_obs.outputs[0][1] == exp_y1 &&
          ev_obs.outputs[0][2] == exp_p &&
          ev_obs.outputs[0][3] == 1.0f) {
         obs_tt_matches++;
      }
   }
   CHECK(obs_tt_matches == 16,
         "16/16 paired backend evaluations match for observable p (including channel z = p)");
   printf("    [observable p] rc_insts=%u bool_comps=%u alu_slots=%u pixsize=%u nops=%u\n",
          rc_insts_obs, bool_comps_obs, code_obs.code.code.r300.alu.length,
          code_obs.code.code.r300.pixsize, count_pair_nops(&c_obs.Base));

   /* 3. Dual-clause 4-output shared Boolean specimen: factoring drops active
    *    Boolean channel operations from 10 to 8, fitting into two vec4
    *    instructions (MAX + MUL) and reducing both normal-form RC instruction
    *    count and encoded hardware paired ALU slots. */
   struct r300_fragment_program_compiler c_dual_unfact, c_dual_fact;
   struct rc_regalloc_state rs_dual_unfact, rs_dual_fact;
   struct r300_fragment_shader_code code_dual_unfact, code_dual_fact;

   setenv("R300_DISABLE_SHARED_BOOL", "1", 1);
   run_fs_with_code(&c_dual_unfact, &rs_dual_unfact, &code_dual_unfact,
                    build_fs_dual_shared_boolean_factoring());
   unsetenv("R300_DISABLE_SHARED_BOOL");
   run_fs_with_code(&c_dual_fact, &rs_dual_fact, &code_dual_fact,
                    build_fs_dual_shared_boolean_factoring());

   unsigned rc_dual_unfact = count_total_instructions(&c_dual_unfact.Base);
   unsigned rc_dual_fact = count_total_instructions(&c_dual_fact.Base);
   unsigned comps_dual_unfact = count_active_bool_components(&c_dual_unfact.Base);
   unsigned comps_dual_fact = count_active_bool_components(&c_dual_fact.Base);

   CHECK(comps_dual_unfact == 10 && comps_dual_fact == 8,
         "dual-clause active RC Boolean channel operations drop from 10 to 8 (-20%)");
   CHECK(rc_dual_fact <= rc_dual_unfact,
         "dual-clause normal-form RC instruction count does not increase after factoring");

   r3xx_compile_fragment_program(&c_dual_unfact);
   r3xx_compile_fragment_program(&c_dual_fact);
   CHECK(!c_dual_unfact.Base.Error && !c_dual_fact.Base.Error,
         "dual-clause unfactored and factored programs compile through full R300 backend");
   CHECK(r300_classic_validate_schedule(&c_dual_unfact.Base, &reject) &&
         r300_classic_validate_schedule(&c_dual_fact.Base, &reject),
         "dual-clause unfactored and factored paired schedules pass legality validator");

   unsigned slots_dual_unfact = code_dual_unfact.code.code.r300.alu.length;
   unsigned slots_dual_fact = code_dual_fact.code.code.r300.alu.length;
   printf("    [dual-clause same-shader] unfactored: rc_insts=%u bool_comps=%u "
          "alu_slots=%u pixsize=%u nops=%u | factored: rc_insts=%u bool_comps=%u "
          "alu_slots=%u pixsize=%u nops=%u\n",
          rc_dual_unfact, comps_dual_unfact, slots_dual_unfact,
          code_dual_unfact.code.code.r300.pixsize, count_pair_nops(&c_dual_unfact.Base),
          rc_dual_fact, comps_dual_fact, slots_dual_fact,
          code_dual_fact.code.code.r300.pixsize, count_pair_nops(&c_dual_fact.Base));
   CHECK(slots_dual_fact < slots_dual_unfact,
         "dual-clause encoded hardware paired ALU slots strictly decrease after factoring");

   unsigned dual_tt_matches = 0;
   for (unsigned i = 0; i < 16; i++) {
      unsigned j = (15u - i);
      bool a0 = (i >> 0) & 1, b0 = (i >> 1) & 1, c0 = (i >> 2) & 1, d0 = (i >> 3) & 1;
      bool a1 = (j >> 0) & 1, b1 = (j >> 1) & 1, c1 = (j >> 2) & 1, d1 = (j >> 3) & 1;
      float exp0 = (a0 && (b0 || c0)) ? 1.0f : 0.0f;
      float exp1 = (a0 && (b0 || d0)) ? 1.0f : 0.0f;
      float exp2 = (a1 && (b1 || c1)) ? 1.0f : 0.0f;
      float exp3 = (a1 && (b1 || d1)) ? 1.0f : 0.0f;

      struct r300_pair_eval ev_du, ev_df;
      memset(&ev_du, 0, sizeof(ev_du));
      memset(&ev_df, 0, sizeof(ev_df));
      ev_du.consts = &c_dual_unfact.Base.Program.Constants;
      ev_df.consts = &c_dual_fact.Base.Program.Constants;

      ev_du.inputs[0][0] = ev_df.inputs[0][0] = a0 ? 0.5f : 1.5f;
      ev_du.inputs[0][1] = ev_df.inputs[0][1] = b0 ? 1.5f : 2.5f;
      ev_du.inputs[0][2] = ev_df.inputs[0][2] = c0 ? 2.5f : 3.5f;
      ev_du.inputs[0][3] = ev_df.inputs[0][3] = d0 ? 3.5f : 4.5f;
      ev_du.inputs[1][0] = ev_df.inputs[1][0] = a1 ? 0.5f : 1.5f;
      ev_du.inputs[1][1] = ev_df.inputs[1][1] = b1 ? 1.5f : 2.5f;
      ev_du.inputs[1][2] = ev_df.inputs[1][2] = c1 ? 2.5f : 3.5f;
      ev_du.inputs[1][3] = ev_df.inputs[1][3] = d1 ? 3.5f : 4.5f;

      if (r300_pair_eval_program(&ev_du, &c_dual_unfact.Base) &&
          r300_pair_eval_program(&ev_df, &c_dual_fact.Base) &&
          ev_df.outputs[0][0] == exp0 && ev_df.outputs[0][1] == exp1 &&
          ev_df.outputs[0][2] == exp2 && ev_df.outputs[0][3] == exp3 &&
          memcmp(ev_du.outputs[0], ev_df.outputs[0], sizeof(ev_df.outputs[0])) == 0) {
         dual_tt_matches++;
      }
   }
   CHECK(dual_tt_matches == 16,
         "dual-clause 16/16 paired backend evaluations match across all 4 outputs");

   teardown_fs(&c_unfact, &rs_unfact);
   teardown_fs(&c_factored, &rs_factored);
   teardown_fs(&c_obs, &rs_obs);
   teardown_fs(&c_dual_unfact, &rs_dual_unfact);
   teardown_fs(&c_dual_fact, &rs_dual_fact);
}

static void
case_xor_scheduling(void)
{
   const char *reject = NULL;

   /* Specimen 6: XOR with both operands available (a ^ b) */
   struct r300_fragment_program_compiler c_avail;
   struct rc_regalloc_state rs_avail;
   struct r300_fragment_shader_code code_avail;
   run_fs_with_code(&c_avail, &rs_avail, &code_avail, build_fs_xor_available());
   CHECK(!c_avail.Base.Error, "xor available fragment lowers to RC without error");
   unsigned rc_avail = count_total_instructions(&c_avail.Base);
   r3xx_compile_fragment_program(&c_avail);
   CHECK(!c_avail.Base.Error, "xor available compiles through full R300 backend");
   CHECK(r300_classic_validate_schedule(&c_avail.Base, &reject),
         "xor available paired schedule passes legality validator (no presub hazard violation)");

   unsigned avail_matches = 0;
   for (unsigned i = 0; i < 4; i++) {
      bool a = (i >> 0) & 1;
      bool b_val = (i >> 1) & 1;
      float exp_xor = (a ^ b_val) ? 1.0f : 0.0f;

      struct r300_pair_eval ev;
      memset(&ev, 0, sizeof(ev));
      ev.consts = &c_avail.Base.Program.Constants;
      ev.inputs[0][0] = a ? 0.5f : 1.5f;
      ev.inputs[0][1] = b_val ? 1.5f : 2.5f;
      if (r300_pair_eval_program(&ev, &c_avail.Base) &&
          ev.outputs[0][0] == exp_xor && ev.outputs[0][1] == exp_xor &&
          ev.outputs[0][2] == exp_xor && ev.outputs[0][3] == exp_xor) {
         avail_matches++;
      }
   }
   CHECK(avail_matches == 4,
         "xor available: 4/4 paired backend evaluations match a ^ b");
   printf("    [xor available] rc_insts=%u alu_slots=%u pixsize=%u presubs=%u nops=%u\n",
          rc_avail, code_avail.code.code.r300.alu.length,
          code_avail.code.code.r300.pixsize, count_pair_presubs(&c_avail.Base),
          count_pair_nops(&c_avail.Base));
   teardown_fs(&c_avail, &rs_avail);

   /* Specimen 7: XOR with newly produced operand: a ^ (t1 & t2) */
   struct r300_fragment_program_compiler c_new;
   struct rc_regalloc_state rs_new;
   struct r300_fragment_shader_code code_new;
   run_fs_with_code(&c_new, &rs_new, &code_new, build_fs_xor_newly_produced());
   CHECK(!c_new.Base.Error, "xor newly produced fragment lowers to RC without error");
   unsigned rc_new = count_total_instructions(&c_new.Base);
   r3xx_compile_fragment_program(&c_new);
   CHECK(!c_new.Base.Error, "xor newly produced compiles through full R300 backend");
   CHECK(r300_classic_validate_schedule(&c_new.Base, &reject),
         "xor newly produced paired schedule passes legality validator (presub hazard guarded)");

   unsigned new_matches = 0;
   for (unsigned i = 0; i < 8; i++) {
      bool a = (i >> 0) & 1;
      bool t1 = (i >> 1) & 1;
      bool t2 = (i >> 2) & 1;
      float exp_xor = (a ^ (t1 && t2)) ? 1.0f : 0.0f;

      struct r300_pair_eval ev;
      memset(&ev, 0, sizeof(ev));
      ev.consts = &c_new.Base.Program.Constants;
      ev.inputs[0][0] = a ? 0.5f : 1.5f;
      ev.inputs[0][1] = t1 ? 1.5f : 2.5f;
      ev.inputs[0][2] = t2 ? 2.5f : 3.5f;
      if (r300_pair_eval_program(&ev, &c_new.Base) &&
          ev.outputs[0][0] == exp_xor && ev.outputs[0][1] == exp_xor &&
          ev.outputs[0][2] == exp_xor && ev.outputs[0][3] == exp_xor) {
         new_matches++;
      }
   }
   CHECK(new_matches == 8,
         "xor newly produced: 8/8 paired backend evaluations match a ^ (t1 & t2)");
   printf("    [xor newly produced] rc_insts=%u alu_slots=%u pixsize=%u presubs=%u nops=%u\n",
          rc_new, code_new.code.code.r300.alu.length,
          code_new.code.code.r300.pixsize, count_pair_presubs(&c_new.Base),
          count_pair_nops(&c_new.Base));
   teardown_fs(&c_new, &rs_new);
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
