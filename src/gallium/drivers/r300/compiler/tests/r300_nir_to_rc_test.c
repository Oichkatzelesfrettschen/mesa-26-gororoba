/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Direct NIR-to-RC regression harness for r300 vertex shaders.
 *
 * The translator is shared by direct-NIR g3dvl shaders and the classic state
 * tracker path.  These cases pin down two robustness edges:
 *
 * 1. Unsupported intrinsics must fail deterministically through Base.Error
 *    instead of printing and continuing with an uninitialized SSA temp.
 * 2. NIR loop continue constructs must be lowered before nir_to_rc emission,
 *    because the RC emitter walks only the loop body list.
 */

#include <stdbool.h>
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
case_unsupported_intrinsic_sets_error(void)
{
   struct nir_to_rc_vs_test_compiler tc;
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
case_continue_construct_is_lowered_before_emit(void)
{
   struct nir_to_rc_vs_test_compiler tc;
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
   case_continue_construct_is_lowered_before_emit();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }

   printf("PASSED\n");
   return 0;
}
