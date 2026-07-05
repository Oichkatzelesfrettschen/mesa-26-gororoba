/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Regression guard for the r300 SW-TCL vertex integer-shift lowering.
 *
 * A vertex shader lowered through r300_draw_init_vertex_shader (r300_vs_draw.c)
 * runs r300_nir_lower_bitwise_to_arith before nir_lower_int_to_float, matching
 * the fragment path in nir_to_rc.c.  Without that prepass a nir_op_ushr reaches
 * nir_lower_int_to_float's default arm, which asserts its operand is not
 * integer-typed and aborts the process -- the crash that HW-confirmation-blocks
 * the native VertexIndex/InstanceIndex firstInstance path, reachable because
 * r300vk feeds Vulkan SPIR-V (which has shift ops) through this GL-shaped path.
 *
 * The asserted invariant is the production one: the prepass-then-int_to_float
 * sequence lowers an unsigned-shift vertex shader without aborting, for both a
 * constant and a variable shift amount.  Each case runs in a forked child so a
 * regression (a surviving abort) is observed as a signal rather than ending the
 * test.  The no-prepass abort is reported for context only, not asserted, so
 * the test does not couple to nir_lower_int_to_float's internal assert.
 */

#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nir.h"
#include "nir_builder.h"
#include "r300_nir.h"

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

enum shift_kind { SHIFT_CONST, SHIFT_VARIABLE };

/* gl_Position = vec4(float(ushr(f2u32(in0.x), amt))).  The shift operand is an
 * f2u32 of an input -- an exact integer for in-range values -- so the exact
 * window of r300_nir_lower_bitwise_to_arith is reachable for the constant amount. */
static nir_shader *
build_vs_ushr(enum shift_kind kind)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_vs_ushr");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in0");
   in0->data.location = VERT_ATTRIB_GENERIC0;
   in0->data.driver_location = 0;

   nir_variable *pos =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   pos->data.driver_location = 0;

   nir_def *in = nir_load_var(&b, in0);
   nir_def *x = nir_f2u32(&b, nir_channel(&b, in, 0));
   nir_def *amt = (kind == SHIFT_CONST)
                     ? nir_imm_int(&b, 2)
                     : nir_f2u32(&b, nir_channel(&b, in, 1));
   nir_def *fval = nir_u2f32(&b, nir_ushr(&b, x, amt));

   nir_store_var(&b, pos, nir_vec4(&b, fval, fval, fval, fval), 0xf);
   return b.shader;
}

/* Mirror r300_vs_draw.c: optionally the FS-path prepass, then int_to_float.
 * Returns cleanly, or the child never returns if int_to_float aborts. */
static void
run_vs_lowering(enum shift_kind kind, bool with_prepass)
{
   nir_shader *s = build_vs_ushr(kind);
   if (with_prepass) {
      bool unsupported = false;
      NIR_PASS(_, s, r300_nir_lower_bitwise_to_arith, &unsupported);
   }
   NIR_PASS(_, s, nir_lower_int_to_float);
   ralloc_free(s);
}

/* true == the child returned (no abort). */
static bool
survives(enum shift_kind kind, bool with_prepass)
{
   pid_t pid = fork();
   if (pid == 0) {
      run_vs_lowering(kind, with_prepass);
      _exit(0);
   }
   int status = 0;
   waitpid(pid, &status, 0);
   return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   printf("r300 SW-TCL VS unsigned-shift lowering:\n");
   /* Production invariant: the prepass sequence lowers ushr without aborting. */
   CHECK(survives(SHIFT_CONST, true), "const ushr survives prepass+int_to_float");
   CHECK(survives(SHIFT_VARIABLE, true), "variable ushr survives prepass+int_to_float");

   /* Context (not asserted): without the prepass the ushr aborts int_to_float. */
   printf("  info - const ushr, no prepass: %s\n",
          survives(SHIFT_CONST, false) ? "survived" : "aborted (prepass is load-bearing)");
   printf("  info - variable ushr, no prepass: %s\n",
          survives(SHIFT_VARIABLE, false) ? "survived" : "aborted (prepass is load-bearing)");

   glsl_type_singleton_decref();
   printf("%s\n", g_failures ? "FAILED" : "PASSED");
   return g_failures ? 1 : 0;
}
