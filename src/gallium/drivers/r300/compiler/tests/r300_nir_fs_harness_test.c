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

int
main(void)
{
   printf("r300 fragment NIR-to-RC admission harness\n");
   case_fmad_emits_mad();
   case_flrp_lowers_to_mad();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
