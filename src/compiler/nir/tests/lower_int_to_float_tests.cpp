/*
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "nir_test.h"

namespace {

class nir_lower_int_to_float_test : public nir_test {
protected:
   nir_lower_int_to_float_test()
      : nir_test::nir_test("nir_lower_int_to_float_test")
   {
   }

   nir_intrinsic_instr *store_result(nir_def *value);
   void lower_and_validate();
   void lower_fold_and_validate();
   unsigned count_alu(nir_op op);
   unsigned count_zero_load_const(unsigned bit_size);
};

static enum glsl_base_type
float_base_type_for_bit_size(unsigned bit_size)
{
   switch (bit_size) {
   case 16:
      return GLSL_TYPE_FLOAT16;
   case 32:
      return GLSL_TYPE_FLOAT;
   case 64:
      return GLSL_TYPE_DOUBLE;
   default:
      UNREACHABLE("Invalid float bit size");
   }
}

static const glsl_type *
float_type_for_def(nir_def *value)
{
   if (value->num_components == 1)
      return glsl_floatN_t_type(value->bit_size);

   return glsl_vector_type(float_base_type_for_bit_size(value->bit_size),
                           value->num_components);
}

nir_intrinsic_instr *
nir_lower_int_to_float_test::store_result(nir_def *value)
{
   nir_variable *res_var =
      nir_local_variable_create(b->impl, float_type_for_def(value),
                                "res");

   return nir_build_store_deref(b, &nir_build_deref_var(b, res_var)->def,
                                value, 0x1);
}

void
nir_lower_int_to_float_test::lower_and_validate()
{
   EXPECT_TRUE(nir_lower_int_to_float(b->shader));
   nir_validate_shader(b->shader, NULL);
}

void
nir_lower_int_to_float_test::lower_fold_and_validate()
{
   lower_and_validate();

   bool progress;
   do {
      progress = false;
      progress |= nir_opt_constant_folding(b->shader);
      progress |= nir_opt_dce(b->shader);
   } while (progress);

   nir_validate_shader(b->shader, NULL);
}

unsigned
nir_lower_int_to_float_test::count_alu(nir_op op)
{
   unsigned count = 0;

   nir_foreach_function_impl(impl, b->shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               count++;
         }
      }
   }

   return count;
}

unsigned
nir_lower_int_to_float_test::count_zero_load_const(unsigned bit_size)
{
   unsigned count = 0;

   nir_foreach_function_impl(impl, b->shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_load_const)
               continue;

            nir_load_const_instr *load = nir_instr_as_load_const(instr);
            if (load->def.bit_size != bit_size)
               continue;

            for (unsigned i = 0; i < load->def.num_components; i++) {
               if (nir_const_value_as_float(load->value[i], bit_size) == 0.0)
                  count++;
            }
         }
      }
   }

   return count;
}

TEST_F(nir_lower_int_to_float_test, udiv_by_zero_with_lower_fdiv_returns_zero)
{
   options.lower_fdiv = true;

   nir_intrinsic_instr *store =
      store_result(nir_udiv(b, nir_imm_int(b, 7), nir_imm_int(b, 0)));

   lower_fold_and_validate();

   ASSERT_TRUE(nir_src_is_const(store->src[1]));
   EXPECT_EQ(nir_src_as_float(store->src[1]), 0.0);
   EXPECT_EQ(count_alu(nir_op_frcp), 0u);
}

TEST_F(nir_lower_int_to_float_test, umod_by_zero_without_lower_fdiv_returns_zero)
{
   options.lower_fdiv = false;

   nir_intrinsic_instr *store =
      store_result(nir_umod(b, nir_imm_int(b, 7), nir_imm_int(b, 0)));

   lower_fold_and_validate();

   ASSERT_TRUE(nir_src_is_const(store->src[1]));
   EXPECT_EQ(nir_src_as_float(store->src[1]), 0.0);
   EXPECT_EQ(count_alu(nir_op_fdiv), 0u);
}

TEST_F(nir_lower_int_to_float_test, udiv_vec2_by_zero_with_lower_fdiv_returns_zero)
{
   options.lower_fdiv = true;

   nir_intrinsic_instr *store =
      store_result(nir_udiv(b, nir_imm_ivec2(b, 7, 9),
                            nir_imm_ivec2(b, 0, 0)));

   lower_fold_and_validate();

   ASSERT_TRUE(nir_src_is_const(store->src[1]));
   EXPECT_EQ(nir_src_comp_as_float(store->src[1], 0), 0.0);
   EXPECT_EQ(nir_src_comp_as_float(store->src[1], 1), 0.0);
   EXPECT_EQ(count_alu(nir_op_frcp), 0u);
}

TEST_F(nir_lower_int_to_float_test, signed_division_overflow_returns_zero)
{
   nir_intrinsic_instr *store =
      store_result(nir_idiv(b, nir_imm_int(b, INT32_MIN),
                            nir_imm_int(b, -1)));

   lower_fold_and_validate();

   ASSERT_TRUE(nir_src_is_const(store->src[1]));
   EXPECT_EQ(nir_src_as_float(store->src[1]), 0.0);
}

TEST_F(nir_lower_int_to_float_test, imod_16_bit_zero_matches_result_size)
{
   nir_def *value =
      nir_imod(b, nir_imm_intN_t(b, -3, 16), nir_imm_intN_t(b, 2, 16));

   store_result(value);
   lower_and_validate();

   EXPECT_GT(count_zero_load_const(16), 0u);
   EXPECT_EQ(count_zero_load_const(32), 0u);
}

}
