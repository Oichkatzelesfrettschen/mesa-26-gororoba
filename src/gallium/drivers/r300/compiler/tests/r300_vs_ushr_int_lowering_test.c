/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Regression guard for the r300 SW-TCL vertex integer lowering that closes the
 * native VertexIndex/InstanceIndex firstInstance path.
 *
 * r300_draw_init_vertex_shader (r300_vs_draw.c) lowers integer vertex shaders
 * to the float domain.  Two passes cooperate, and this test pins both plus
 * their interaction:
 *
 *   1. r300_nir_lower_bitwise_to_arith runs before nir_lower_int_to_float,
 *      matching the fragment path.  Without it a nir_op_ushr from r3v's
 *      Vulkan SPIR-V reaches nir_lower_int_to_float's default arm, which
 *      asserts its operand is not integer-typed and aborts the process.
 *
 *   2. r300_nir_float_encode_synthetic_sysval_index_uses runs after
 *      nir_lower_int_to_float and redirects the synthetic sysval's numeric-index
 *      consumers to an i2f32 clone, leaving the raw-bit equality operands
 *      untouched (the raw-bit sysval compare contract).
 *
 * The firstInstance shape needs both: the shift compiles (1) and the
 * gl_InstanceIndex dynamic-index compare reads a numeric value (2).
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

/* Build a VS reading the synthetic "sys_instance_index" shader_in the default
 * (non-native) VertexIndex/InstanceIndex delivery creates.  When numeric_use is
 * set the value feeds an ordering compare (ilt -> flt, a numeric index use);
 * otherwise it feeds only an equality compare (ieq -> feq, the raw-bit
 * contract).  A ushr on a separate input exercises the prepass in the same
 * shader when with_shift is set -- the firstInstance shape. */
static nir_shader *
build_vs_sysval(bool numeric_use, bool with_shift)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_vs_sysval");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in0");
   in0->data.location = VERT_ATTRIB_GENERIC0;
   in0->data.driver_location = 0;

   nir_variable *sysin =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_int_type(),
                          "sys_instance_index");
   sysin->data.location = VERT_ATTRIB_GENERIC1;
   sysin->data.driver_location = 1;

   nir_variable *pos =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   pos->data.driver_location = 0;

   nir_def *sysval = nir_load_var(&b, sysin);
   nir_def *in = nir_load_var(&b, in0);
   nir_def *ref = nir_f2i32(&b, nir_channel(&b, in, 0));

   nir_def *cond = numeric_use ? nir_ilt(&b, sysval, nir_imm_int(&b, 5))
                               : nir_ieq(&b, sysval, ref);
   nir_def *val = nir_bcsel(&b, cond, nir_imm_float(&b, 1.0),
                            nir_imm_float(&b, 0.0));

   if (with_shift) {
      nir_def *sh = nir_u2f32(
         &b, nir_ushr(&b, nir_f2u32(&b, nir_channel(&b, in, 2)),
                      nir_imm_int(&b, 3)));
      val = nir_fadd(&b, val, sh);
   }

   nir_store_var(&b, pos, nir_vec4(&b, val, val, val, val), 0xf);
   return b.shader;
}

static void
run_ushr_lowering(enum shift_kind kind, bool with_prepass)
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
ushr_survives(enum shift_kind kind, bool with_prepass)
{
   pid_t pid = fork();
   if (pid == 0) {
      run_ushr_lowering(kind, with_prepass);
      _exit(0);
   }
   int status = 0;
   waitpid(pid, &status, 0);
   return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Run the production sequence (prepass, int_to_float, sysval-index encode) in a
 * child and return the outcome through its exit code: 0 no-progress, 1 the
 * numeric use was float-encoded, 2 aborted. */
static int
sysval_encode_result(bool numeric_use, bool with_shift)
{
   pid_t pid = fork();
   if (pid == 0) {
      nir_shader *s = build_vs_sysval(numeric_use, with_shift);
      bool unsupported = false;
      NIR_PASS(_, s, r300_nir_lower_bitwise_to_arith, &unsupported);
      NIR_PASS(_, s, nir_lower_int_to_float);
      bool progress = r300_nir_float_encode_synthetic_sysval_index_uses(s);
      ralloc_free(s);
      _exit(progress ? 1 : 0);
   }
   int status = 0;
   waitpid(pid, &status, 0);
   if (!WIFEXITED(status))
      return 2;
   return WEXITSTATUS(status);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   printf("r300 SW-TCL VS integer lowering (ushr prepass + sysval-index encode):\n");

   /* 1. The prepass sequence lowers ushr without aborting, both amounts. */
   CHECK(ushr_survives(SHIFT_CONST, true), "const ushr survives prepass+int_to_float");
   CHECK(ushr_survives(SHIFT_VARIABLE, true), "variable ushr survives prepass+int_to_float");
   printf("  info - const ushr, no prepass: %s\n",
          ushr_survives(SHIFT_CONST, false) ? "survived" : "aborted (prepass is load-bearing)");

   /* 2. The sysval-index encode fires for a numeric index use and leaves a
    *    raw-bit equality-only use alone (the #942 contract). */
   CHECK(sysval_encode_result(true, false) == 1,
         "synthetic sysval numeric index use is float-encoded");
   CHECK(sysval_encode_result(false, false) == 0,
         "synthetic sysval equality-only use stays raw (mesa #942 contract)");

   /* 3. The firstInstance shape -- a numeric sysval index alongside a shift --
    *    lowers without aborting and is float-encoded. */
   CHECK(sysval_encode_result(true, true) == 1,
         "firstInstance shape (numeric sysval + ushr) compiles and encodes");

   glsl_type_singleton_decref();
   printf("%s\n", g_failures ? "FAILED" : "PASSED");
   return g_failures ? 1 : 0;
}
