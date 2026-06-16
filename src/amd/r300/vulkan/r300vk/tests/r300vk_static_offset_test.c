/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for r300vk constant-file offset representability.
 */

#include <stdio.h>
#include <stdbool.h>

#include "../r300vk_pipeline.c"

static unsigned failures;

#define CHECK(cond, name)                 \
   do {                                   \
      if (cond) {                         \
         printf("  ok   - %s\n", (name)); \
      } else {                            \
         printf("  FAIL - %s\n", (name)); \
         failures++;                      \
      }                                   \
   } while (0)

static nir_shader *
push_const_load_shader(unsigned offset, unsigned components)
{
   static const nir_shader_compiler_options options = {0};
   nir_builder b =
      nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, &options,
                                     "r300vk static push offset");

   nir_load_push_constant(&b, components, 32, nir_imm_int(&b, offset),
                          .base = 0, .range = 128, .align_mul = 4,
                          .align_offset = offset & 3u);

   return b.shader;
}

static void
check_push_const(unsigned offset, unsigned components, bool expected,
                 const char *name)
{
   nir_shader *shader = push_const_load_shader(offset, components);
   bool ok = r300vk_nir_push_const_shape_ok(NULL, shader);
   CHECK(ok == expected, name);
   ralloc_free(shader);
}

static void
check_straddle_flag_is_explicit(void)
{
   nir_shader *shader = push_const_load_shader(12, 2);
   bool ok = r300vk_nir_offsets_static(NULL, shader,
                                       nir_intrinsic_load_push_constant, 0,
                                       false);
   CHECK(ok, "straddle=false accepts a constant offset crossing a slot");
   ralloc_free(shader);
}

int
main(void)
{
   check_push_const(0, 4, true,
                    "vec4 push load at slot start fits one constant slot");
   check_push_const(4, 3, true,
                    "vec3 push load ending on slot boundary is accepted");
   check_push_const(4, 4, false,
                    "vec4 push load at byte 4 crosses one constant slot");
   check_push_const(12, 1, true,
                    "scalar push load at final slot component is accepted");
   check_push_const(12, 2, false,
                    "vec2 push load at final slot component is rejected");
   check_straddle_flag_is_explicit();

   return failures ? 1 : 0;
}
