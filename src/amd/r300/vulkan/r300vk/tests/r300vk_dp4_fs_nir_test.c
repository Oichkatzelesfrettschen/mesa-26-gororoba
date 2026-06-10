/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for the DP4 compute-as-raster fragment shader shape.
 *
 * r300vk_synthesize_dp4_fs fed the vec4 texcoord varying straight into nir_tex
 * for a GLSL_SAMPLER_DIM_2D sampler.  nir_build_tex_struct asserts that a tex
 * instruction's coord_components equals the sampler dimension's coordinate
 * count (2 for 2D), so an asserts-enabled build aborted at every DP4 compute
 * pipeline create.  This test builds the same FS NIR via the extracted
 * r300vk_build_dp4_fs_nir for each dot width and pins two invariants: the
 * shader passes nir_validate, and every 2D texture op receives a 2-component
 * coordinate.  A regression to the vec4 coordinate fails this test (or aborts
 * it inside the builder) before it can ship.
 */

#include <stdio.h>

#include "util/macros.h"
#include "util/ralloc.h"
#include "compiler/nir/nir.h"

#include "r300vk_dp4_fs_nir.h"

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

/* Tally the texture instructions and the ones whose coordinate source is not
 * 2-component -- the exact shape the 2D sampler requires and the bug regressed. */
static void
count_tex(nir_shader *s, unsigned *tex_out, unsigned *bad_coord_out)
{
   unsigned tex = 0, bad = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_tex)
               continue;
            nir_tex_instr *t = nir_instr_as_tex(instr);
            tex++;
            int ci = nir_tex_instr_src_index(t, nir_tex_src_coord);
            if (ci < 0 || t->src[ci].src.ssa->num_components != 2)
               bad++;
         }
      }
   }
   *tex_out = tex;
   *bad_coord_out = bad;
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   /* A zero-initialized options block is enough to build this shader; the FS
    * uses only always-available builder primitives. */
   static const nir_shader_compiler_options options = { 0 };

   /* The kernel dot widths the dispatch admits: 0 means full vec4. */
   const unsigned widths[] = { 0, 2, 3, 4 };
   for (unsigned i = 0; i < ARRAY_SIZE(widths); i++) {
      char name[80];
      nir_shader *s = r300vk_build_dp4_fs_nir(&options, widths[i]);

      snprintf(name, sizeof(name), "components=%u builds a shader", widths[i]);
      CHECK(s != NULL, name);
      if (!s)
         continue;

      nir_validate_shader(s, "r300vk dp4 fs"); /* aborts on a malformed shader */

      unsigned tex = 0, bad = 0;
      count_tex(s, &tex, &bad);
      snprintf(name, sizeof(name),
               "components=%u: two 2D tex ops, each a 2-component coord",
               widths[i]);
      CHECK(tex == 2 && bad == 0, name);

      ralloc_free(s);
   }

   glsl_type_singleton_decref();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
