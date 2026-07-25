/*
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
#include <stdint.h>
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

static nir_def *
imm_uvec4(nir_builder *b, const uint32_t values[4])
{
   nir_const_value constants[4];
   for (unsigned component = 0; component < 4; component++)
      constants[component] = nir_const_value_for_uint(values[component], 32);
   return nir_build_imm(b, 4, 32, constants);
}

static nir_shader *
build_vs_vector_bitwise(nir_op op, const uint32_t constants[4],
                        const uint32_t bounds[4], bool constant_first)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_vs_vector_bitwise");
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *in0 =
      nir_variable_create(b.shader, nir_var_shader_in, vec4, "in0");
   in0->data.location = VERT_ATTRIB_GENERIC0;
   in0->data.driver_location = 0;

   nir_variable *pos =
      nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   pos->data.driver_location = 0;

   nir_def *input = nir_f2u32(&b, nir_load_var(&b, in0));
   nir_def *bounded = nir_umin(&b, input, imm_uvec4(&b, bounds));
   nir_def *constant = imm_uvec4(&b, constants);
   nir_def *result = constant_first
                        ? nir_build_alu2(&b, op, constant, bounded)
                        : nir_build_alu2(&b, op, bounded, constant);
   nir_store_var(&b, pos, nir_u2f32(&b, result), 0xf);
   return b.shader;
}

static unsigned
count_alu_op(nir_shader *s, nir_op op)
{
   unsigned count = 0;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               count++;
         }
      }
   }
   return count;
}

static unsigned
count_alu_const_divisor(nir_shader *s, nir_op op, uint32_t divisor)
{
   unsigned count = 0;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;

            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op == op && nir_src_is_const(alu->src[1].src) &&
                nir_src_comp_as_uint(alu->src[1].src,
                                     alu->src[1].swizzle[0]) == divisor)
               count++;
         }
      }
   }
   return count;
}

static void
check_vector_bitwise_lowering(const char *name, nir_op source_op,
                              const uint32_t constants[4],
                              const uint32_t bounds[4], bool constant_first,
                              bool expect_supported, nir_op lowered_op,
                              const uint32_t expected_divisors[4])
{
   nir_shader *s = build_vs_vector_bitwise(source_op, constants, bounds,
                                           constant_first);
   bool unsupported = false;
   bool progress = r300_nir_lower_bitwise_to_arith(s, &unsupported);

   printf("  case - %s\n", name);
   CHECK(progress, "bitwise pass makes progress");
   CHECK(unsupported != expect_supported,
         expect_supported ? "vector operation remains supported"
                          : "invalid vector lane rejects the operation");
   CHECK(count_alu_op(s, source_op) == 0,
         "source bitwise operation is removed");

   if (expect_supported) {
      unsigned expected_arithmetic_count = 0;
      for (unsigned component = 0; component < 4; component++) {
         /* A zero divisor marks a zero-mask lane that lowers directly to a
          * constant zero and emits no arithmetic instruction. */
         if (expected_divisors[component] != 0)
            expected_arithmetic_count++;
      }
      CHECK(count_alu_op(s, lowered_op) == expected_arithmetic_count,
            "arithmetic is emitted only for nonzero-mask lanes");
      for (unsigned component = 0; component < 4; component++) {
         if (expected_divisors[component] == 0)
            continue;

         unsigned expected_count = 0;
         for (unsigned other = 0; other < 4; other++) {
            if (expected_divisors[other] == expected_divisors[component])
               expected_count++;
         }
         CHECK(count_alu_const_divisor(s, lowered_op,
                                      expected_divisors[component]) ==
                  expected_count,
               "arithmetic divisor matches the constant lane");
      }
   } else {
      CHECK(count_alu_op(s, lowered_op) == 0,
            "rejected operation emits no arithmetic approximation");
   }

   nir_validate_shader(s, "after r300 vector bitwise lowering test");
   ralloc_free(s);
}

static void
check_scalar_iand_mask(unsigned bit_size, uint32_t mask,
                       bool expect_supported, bool expect_umod)
{
   static const nir_shader_compiler_options options;
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options,
                                                  "r300_iand_bit_size");
   bool fold_alu = b.constant_fold_alu;
   b.constant_fold_alu = false;
   nir_iand(&b, nir_imm_intN_t(&b, 7, bit_size),
            nir_imm_intN_t(&b, mask, bit_size));
   b.constant_fold_alu = fold_alu;

   bool unsupported = false;
   bool progress = r300_nir_lower_bitwise_to_arith(b.shader, &unsupported);
   unsigned umod_count = 0;
   nir_foreach_function_impl (impl, b.shader) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op == nir_op_umod)
               umod_count++;
         }
      }
   }

   char check_name[96];
   snprintf(check_name, sizeof(check_name),
            "%u-bit mask 0x%x is %s", bit_size, (unsigned)mask,
            expect_supported ? "admitted" : "rejected");
   CHECK(unsupported != expect_supported && (!expect_supported || progress),
         check_name);
   CHECK(umod_count == (expect_umod ? 1u : 0u),
         expect_umod ? "admitted mask emits one modulo"
                     : "identity or rejected mask emits no modulo");
   nir_validate_shader(b.shader, "after r300 iand bit-size lowering test");
   ralloc_free(b.shader);
}

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

   /* 4. Constant masks and shifts are proven and lowered per result lane. */
   static const uint32_t exact_bounds[4] = { 17, 100, 1024, 131072 };
   static const uint32_t splat_masks[4] = { 3, 3, 3, 3 };
   static const uint32_t splat_moduli[4] = { 4, 4, 4, 4 };
   static const uint32_t vector_masks[4] = { 1, 3, 7, 15 };
   static const uint32_t vector_moduli[4] = { 2, 4, 8, 16 };
   static const uint32_t zero_masks[4] = { 0, 0, 0, 0 };
   static const uint32_t zero_moduli[4] = { 0, 0, 0, 0 };
   static const uint32_t mixed_masks[4] = { 0, 3, 0, 15 };
   static const uint32_t mixed_bounds[4] = {
      UINT32_MAX, 100, UINT32_MAX, 1024
   };
   static const uint32_t mixed_moduli[4] = { 0, 4, 0, 16 };
   static const uint32_t vector_shifts[4] = { 0, 1, 2, 17 };
   static const uint32_t vector_divisors[4] = { 1, 2, 4, 131072 };
   static const uint32_t invalid_masks[4] = { 1, 3, 5, 7 };
   static const uint32_t identity_lane_masks[4] = {1, 3, UINT32_MAX, 7};
   static const uint32_t identity_lane_moduli[4] = {2, 4, 0, 8};
   static const uint32_t invalid_shifts[4] = { 0, 1, 18, 3 };
   static const uint32_t out_of_range_bounds[4] = { 17, 100, 131073, 1024 };

   check_vector_bitwise_lowering("splat low-bit mask", nir_op_iand,
                                 splat_masks, exact_bounds, false, true,
                                 nir_op_umod, splat_moduli);
   check_vector_bitwise_lowering("distinct low-bit masks with constant first",
                                 nir_op_iand, vector_masks, exact_bounds, true,
                                 true, nir_op_umod, vector_moduli);
   check_vector_bitwise_lowering("all-zero masks with unbounded values",
                                 nir_op_iand, zero_masks, mixed_bounds, false,
                                 true, nir_op_umod, zero_moduli);
   check_vector_bitwise_lowering("mixed zero and low-bit masks",
                                 nir_op_iand, mixed_masks, mixed_bounds, false,
                                 true, nir_op_umod, mixed_moduli);
   check_vector_bitwise_lowering("distinct constant shifts", nir_op_ushr,
                                 vector_shifts, exact_bounds, false, true,
                                 nir_op_udiv, vector_divisors);
   check_vector_bitwise_lowering("non-low-bit mask lane", nir_op_iand,
                                 invalid_masks, exact_bounds, false, false,
                                 nir_op_umod, vector_moduli);
   check_vector_bitwise_lowering("full-width identity mask lane", nir_op_iand,
                                 identity_lane_masks, exact_bounds, false, true,
                                 nir_op_umod, identity_lane_moduli);
   check_vector_bitwise_lowering("out-of-window shift lane", nir_op_ushr,
                                 invalid_shifts, exact_bounds, false, false,
                                 nir_op_udiv, vector_divisors);
   check_vector_bitwise_lowering("out-of-window value lane", nir_op_iand,
                                 vector_masks, out_of_range_bounds, false,
                                 false, nir_op_umod, vector_moduli);
   check_scalar_iand_mask(8, 0, false, false);
   check_scalar_iand_mask(16, 0, false, false);
   check_scalar_iand_mask(8, UINT8_MAX, false, false);
   check_scalar_iand_mask(16, UINT16_MAX, false, false);
   check_scalar_iand_mask(8, 3, false, false);
   check_scalar_iand_mask(16, 3, false, false);

   glsl_type_singleton_decref();
   printf("%s\n", g_failures ? "FAILED" : "PASSED");
   return g_failures ? 1 : 0;
}
