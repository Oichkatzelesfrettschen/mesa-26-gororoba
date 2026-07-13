/*
 * SPDX-License-Identifier: MIT
 *
 * NIR-to-RC regression harness for r300 vertex shaders.
 *
 * The translator is shared by g3dvl shaders and the state-tracker path.  These
 * cases pin down three emission paths:
 *
 * 1. Unsupported intrinsics must fail deterministically through Base.Error
 *    instead of printing and continuing with an uninitialized SSA temp.
 * 2. NIR loop continue constructs must be lowered before nir_to_rc emission,
 *    because the RC emitter walks only the loop body list.
 * 3. The special ALU lowering paths must keep emitting the RC opcode/srcmod
 *    patterns that the r300 RC backend expects.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_control_flow.h"

#include "nir_to_rc.h"
#include "r300_screen.h"
#include "r300_shader_semantics.h"
#include "r300_vs.h"
#include "radeon_compiler.h"
#include "radeon_regalloc.h"

static unsigned g_failures;

#define CHECK(cond, name)                                                      \
   do {                                                                        \
      if (cond) {                                                              \
         printf("  ok   - %s\n", (name));                                      \
      } else {                                                                 \
         printf("  FAIL - %s\n", (name));                                      \
         g_failures++;                                                         \
      }                                                                        \
   } while (0)

struct nir_to_rc_vs_test_compiler {
   struct r300_vertex_program_compiler compiler;
   struct rc_regalloc_state regalloc;
   struct r300_vertex_shader_code code;
   struct r300_screen screen;
};

static void
nir_to_rc_vs_test_init(struct nir_to_rc_vs_test_compiler *tc)
{
   memset(tc, 0, sizeof(*tc));

   rc_init_regalloc_state(&tc->regalloc, RC_VERTEX_PROGRAM);
   rc_init(&tc->compiler.Base, &tc->regalloc);

   tc->compiler.code = &tc->code.code;
   tc->compiler.UserData = &tc->code;
   tc->compiler.Base.type = RC_VERTEX_PROGRAM;
   tc->compiler.Base.is_r400 = false;
   tc->compiler.Base.is_r500 = false;
   tc->compiler.Base.has_half_swizzles = true;
   tc->compiler.Base.has_presub = true;
   tc->compiler.Base.has_omod = true;
   tc->compiler.Base.max_temp_regs = 32;
   tc->compiler.Base.max_constants = 256;
   tc->compiler.Base.max_alu_insts = 1024;

   r300_shader_semantics_reset(&tc->code.outputs);
}

static void
nir_to_rc_vs_test_destroy(struct nir_to_rc_vs_test_compiler *tc)
{
   rc_destroy(&tc->compiler.Base);
   rc_destroy_regalloc_state(&tc->regalloc);
}

static nir_builder
vs_builder(const char *name)
{
   static const nir_shader_compiler_options options;
   return nir_builder_init_simple_shader(MESA_SHADER_VERTEX, &options, "%s",
                                         name);
}

static nir_variable *
vs_position_output(nir_builder *b)
{
   nir_variable *pos =
      nir_variable_create(b->shader, nir_var_shader_out, glsl_vec4_type(),
                          "gl_Position");
   pos->data.location = VARYING_SLOT_POS;
   return pos;
}

static nir_shader *
build_vs_with_fsub(void)
{
   nir_builder b = vs_builder("vs_fsub");
   nir_variable *in =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "in0");
   nir_variable *pos = vs_position_output(&b);
   in->data.location = VERT_ATTRIB_GENERIC0;

   nir_def *loaded = nir_load_var(&b, in);
   nir_def *x = nir_channel(&b, loaded, 0);
   nir_def *y = nir_channel(&b, loaded, 1);
   nir_def *diff = nir_fsub(&b, x, y);
   nir_store_var(&b, pos,
                 nir_vec4(&b, diff, nir_imm_float(&b, 0.0f),
                          nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)),
                 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

static nir_shader *
build_vs_with_fcsel_gt(void)
{
   nir_builder b = vs_builder("vs_fcsel_gt");
   nir_variable *in =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "in0");
   nir_variable *pos = vs_position_output(&b);
   in->data.location = VERT_ATTRIB_GENERIC0;

   nir_def *loaded = nir_load_var(&b, in);
   nir_def *x = nir_channel(&b, loaded, 0);
   nir_def *y = nir_channel(&b, loaded, 1);
   nir_def *z = nir_channel(&b, loaded, 2);
   nir_def *selected = nir_build_alu3(&b, nir_op_fcsel_gt, x, y, z);
   nir_store_var(&b, pos,
                 nir_vec4(&b, selected, nir_imm_float(&b, 0.0f),
                          nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)),
                 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

static nir_shader *
build_vs_with_unsupported_intrinsic(void)
{
   nir_builder b = vs_builder("vs_load_vertex_id");
   nir_variable *pos = vs_position_output(&b);
   nir_def *x = nir_i2f32(&b, nir_load_vertex_id(&b));
   nir_store_var(&b, pos,
                 nir_vec4(&b, x, nir_imm_float(&b, 0.0f),
                          nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)),
                 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

static nir_shader *
build_vs_with_narrow_iand(unsigned bit_size, unsigned mask)
{
   nir_builder b = vs_builder("vs_narrow_iand");
   nir_variable *in =
      nir_variable_create(b.shader, nir_var_shader_in, glsl_vec4_type(), "in0");
   nir_variable *pos = vs_position_output(&b);
   in->data.location = VERT_ATTRIB_GENERIC0;

   nir_def *loaded = nir_load_var(&b, in);
   nir_def *integer = nir_f2u32(&b, nir_channel(&b, loaded, 0));
   nir_def *narrow = bit_size == 8 ? nir_u2u8(&b, integer)
                                    : nir_u2u16(&b, integer);
   nir_def *masked = nir_iand_imm(&b, narrow, mask);
   nir_def *x = nir_u2f32(&b, masked);
   nir_store_var(&b, pos,
                 nir_vec4(&b, x, nir_imm_float(&b, 0.0f),
                          nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f)),
                 0xf);
   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

static nir_shader *
build_vs_with_continue_construct(void)
{
   nir_builder b = vs_builder("vs_continue_construct");
   nir_variable *pos = vs_position_output(&b);
   nir_store_var(&b, pos,
                 nir_vec4(&b, nir_imm_float(&b, 0.0f),
                          nir_imm_float(&b, 0.0f), nir_imm_float(&b, 0.0f),
                          nir_imm_float(&b, 1.0f)),
                 0xf);

   nir_loop *loop = nir_push_loop(&b);
   nir_jump(&b, nir_jump_break);
   nir_loop_add_continue_construct(loop);
   nir_push_continue(&b, loop);
   nir_pop_loop(&b, loop);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));
   return b.shader;
}

static void
check_scalar_swizzle(struct rc_src_register src, unsigned chan, const char *name)
{
   CHECK(GET_SWZ(src.Swizzle, 0) == chan &&
            GET_SWZ(src.Swizzle, 1) == chan &&
            GET_SWZ(src.Swizzle, 2) == chan &&
            GET_SWZ(src.Swizzle, 3) == chan,
         name);
}

static struct rc_instruction *
find_first_opcode(struct radeon_compiler *compiler, rc_opcode opcode)
{
   for (struct rc_instruction *inst = compiler->Program.Instructions.Next;
        inst != &compiler->Program.Instructions; inst = inst->Next) {
      if (inst->Type == RC_INSTRUCTION_NORMAL && inst->U.I.Opcode == opcode)
         return inst;
   }

   return NULL;
}

static void
case_unsupported_intrinsic_sets_error(void)
{
   struct nir_to_rc_vs_test_compiler tc = {0};
   nir_to_rc_vs_test_init(&tc);
   union r300_shader_code rc = {.v = &tc.code};

   nir_to_rc(build_vs_with_unsupported_intrinsic(), &tc.screen.screen,
             (struct r300_fragment_program_external_state){0}, rc,
             &tc.compiler.Base);

   CHECK(tc.compiler.Base.Error,
         "unsupported VS intrinsic rejects through compiler.Base.Error");
   CHECK(tc.compiler.Base.ErrorMsg &&
            strstr(tc.compiler.Base.ErrorMsg, "unsupported NIR intrinsic") != NULL,
         "unsupported VS intrinsic records the translator error message");

   nir_to_rc_vs_test_destroy(&tc);
}

static void
case_fsub_emits_add_with_negated_rhs(void)
{
   struct nir_to_rc_vs_test_compiler tc = {0};
   nir_to_rc_vs_test_init(&tc);
   union r300_shader_code rc = {.v = &tc.code};

   nir_to_rc(build_vs_with_fsub(), &tc.screen.screen,
             (struct r300_fragment_program_external_state){0}, rc,
             &tc.compiler.Base);

   struct rc_instruction *add = find_first_opcode(&tc.compiler.Base, RC_OPCODE_ADD);

   CHECK(!tc.compiler.Base.Error, "fsub lowers without a compiler error");
   CHECK(add != NULL, "fsub emits an ADD in RC");
   if (add != NULL) {
      CHECK(add->U.I.SrcReg[0].File == RC_FILE_INPUT &&
               add->U.I.SrcReg[1].File == RC_FILE_INPUT,
            "fsub ADD reads the vertex input file");
      check_scalar_swizzle(add->U.I.SrcReg[0], RC_SWIZZLE_X,
                           "fsub ADD keeps src0 on the X channel");
      check_scalar_swizzle(add->U.I.SrcReg[1], RC_SWIZZLE_Y,
                           "fsub ADD reads src1 from the Y channel");
      CHECK(add->U.I.SrcReg[1].Negate == RC_MASK_XYZW,
            "fsub ADD negates the right-hand source");
   }

   nir_to_rc_vs_test_destroy(&tc);
}

static void
check_narrow_iand_stops_before_int_to_float(unsigned bit_size, unsigned mask)
{
   struct nir_to_rc_vs_test_compiler tc = {0};
   nir_to_rc_vs_test_init(&tc);
   union r300_shader_code rc = {.v = &tc.code};

   nir_to_rc(build_vs_with_narrow_iand(bit_size, mask), &tc.screen.screen,
             (struct r300_fragment_program_external_state){0}, rc,
             &tc.compiler.Base);

   char check_name[96];
   snprintf(check_name, sizeof(check_name),
            "%u-bit iand mask 0x%x rejects through compiler.Base.Error",
            bit_size, mask);
   CHECK(tc.compiler.Base.Error, check_name);
   CHECK(tc.compiler.Base.ErrorMsg &&
            strstr(tc.compiler.Base.ErrorMsg,
                   "integer ALU input or result") != NULL,
         "narrow integer ALU stops at the RC admission boundary");

   nir_to_rc_vs_test_destroy(&tc);
}

static void
case_fcsel_gt_emits_cmp_with_negated_condition(void)
{
   struct nir_to_rc_vs_test_compiler tc = {0};
   nir_to_rc_vs_test_init(&tc);
   union r300_shader_code rc = {.v = &tc.code};

   nir_to_rc(build_vs_with_fcsel_gt(), &tc.screen.screen,
             (struct r300_fragment_program_external_state){0}, rc,
             &tc.compiler.Base);

   struct rc_instruction *cmp = find_first_opcode(&tc.compiler.Base, RC_OPCODE_CMP);

   CHECK(!tc.compiler.Base.Error, "fcsel_gt lowers without a compiler error");
   CHECK(cmp != NULL, "fcsel_gt emits a CMP in RC");
   if (cmp != NULL) {
      CHECK(cmp->U.I.SrcReg[0].File == RC_FILE_INPUT &&
               cmp->U.I.SrcReg[1].File == RC_FILE_INPUT &&
               cmp->U.I.SrcReg[2].File == RC_FILE_INPUT,
            "fcsel_gt CMP reads all operands from the vertex input file");
      check_scalar_swizzle(cmp->U.I.SrcReg[0], RC_SWIZZLE_X,
                           "fcsel_gt CMP keeps the condition on X");
      check_scalar_swizzle(cmp->U.I.SrcReg[1], RC_SWIZZLE_Y,
                           "fcsel_gt CMP keeps the true value on Y");
      check_scalar_swizzle(cmp->U.I.SrcReg[2], RC_SWIZZLE_Z,
                           "fcsel_gt CMP keeps the false value on Z");
      CHECK(cmp->U.I.SrcReg[0].Negate == RC_MASK_XYZW,
            "fcsel_gt CMP negates the condition source");
   }

   nir_to_rc_vs_test_destroy(&tc);
}

static void
case_continue_construct_is_lowered_before_emit(void)
{
   struct nir_to_rc_vs_test_compiler tc = {0};
   nir_to_rc_vs_test_init(&tc);
   union r300_shader_code rc = {.v = &tc.code};

   nir_to_rc(build_vs_with_continue_construct(), &tc.screen.screen,
             (struct r300_fragment_program_external_state){0}, rc,
             &tc.compiler.Base);

   CHECK(!tc.compiler.Base.Error,
         "loop continue constructs lower before nir_to_rc emission");

   nir_to_rc_vs_test_destroy(&tc);
}

int
main(void)
{
   printf("r300 nir_to_rc regression harness\n");
   case_unsupported_intrinsic_sets_error();
   check_narrow_iand_stops_before_int_to_float(8, 0);
   check_narrow_iand_stops_before_int_to_float(16, 0);
   check_narrow_iand_stops_before_int_to_float(8, UINT8_MAX);
   check_narrow_iand_stops_before_int_to_float(16, UINT16_MAX);
   check_narrow_iand_stops_before_int_to_float(8, 3);
   check_narrow_iand_stops_before_int_to_float(16, 3);
   case_fsub_emits_add_with_negated_rhs();
   case_fcsel_gt_emits_cmp_with_negated_condition();
   case_continue_construct_is_lowered_before_emit();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }

   printf("PASSED\n");
   return 0;
}
