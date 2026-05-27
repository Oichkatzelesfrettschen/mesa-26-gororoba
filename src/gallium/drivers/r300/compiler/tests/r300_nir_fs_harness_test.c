/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Fragment-stage NIR-input admission harness for r300_nir_to_rc_direct.
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

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"

#include "nir_to_rc.h"
#include "r300_fs.h"
#include "r300_nir.h"
#include "r300_nir_to_rc_direct.h"
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
    * silently degrades to MUL + ADD, never touching the path #249 fixed. */
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

/* Build a fragment shader that reproduces st_nir_lower_fog's output blend:
 *
 *   f   = fsat(ffma_weak(fogc, params.x, params.y))          (FOG_LINEAR)
 *   fog = ffma_weak(color, f, fog_color * (1 - f))           (vec4)
 *   out = vector_insert(fog, color.w, 3)                     (alpha passthrough)
 *
 * The vector_insert that re-packs the original alpha is the "output packing"
 * the fog SSA-temp-mistranslation finding fingers; that packing, not the input
 * provenance, is what this test pins.  Production reads the fog coordinate from
 * VARYING_SLOT_FOGC and params/fog_color from state uniforms; the harness
 * instead feeds them from generic varyings (so nothing constant-folds and the
 * full packing reaches the emitter).  The substitution is faithful because
 * ntr_fixup_varying_slots remaps only the VAR/PNTC/TEX slots, not FOGC, and the
 * r300 vec_to_regs + fsat lowering the bug lives in is downstream of and
 * independent of which input slot the values arrive on.  This reproduces the
 * fog program whose RS482 RC dump read never-written temporaries and wrote the
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

/* Run the production NIR pipeline + the direct emitter as a fragment program.
 *
 * Production lowers NIR in two stages: r300_optimize_nir() at shader-state
 * creation (r300_create_fs_state, r300_state.c) and r300_nir_lower_for_rc() at
 * compile time (r300_fs.c).  The fog / mix() flrp lowering (r300_nir_lower_flrp)
 * runs in the first stage for every stage; lower_for_rc only re-runs it for
 * vertex shaders.  A faithful fragment harness must run both, or a flrp never
 * gets lowered and trips the emitter's defensive raw-flrp guard. */
static void
run_fs(struct r300_fragment_program_compiler *c, struct rc_regalloc_state *rs,
       nir_shader *nir)
{
   struct r300_screen screen;
   struct pipe_screen *ps = fake_r300_screen(&screen);
   const struct r300_fragment_program_external_state ext = {0};

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

   r300_optimize_nir(nir, r300_screen(ps));
   r300_nir_lower_for_rc(nir, ps, ext);
   r300_nir_to_rc_direct(&c->Base, nir, ps, ext);
}

static void
teardown_fs(struct r300_fragment_program_compiler *c,
            struct rc_regalloc_state *rs)
{
   rc_destroy(&c->Base);
   rc_destroy_regalloc_state(rs);
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
 * SSA-temp mistranslation routed the colour result through it. */
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
 * emitter's defensive case to reject. */
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
 * sentinel temp.  Both symptoms appear in the RS482 fog RC dump where the
 * lowered-flrp / ffma_weak output packing dropped the colour term. */
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

int
main(void)
{
   printf("r300 fragment NIR-to-RC admission harness\n");
   case_fmad_emits_mad();
   case_flrp_lowers_to_mad();
   case_fog_packing_well_formed();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
