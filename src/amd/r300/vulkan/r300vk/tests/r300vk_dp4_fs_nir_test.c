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
 * r300vk_build_dp4_fs_nir for each dot width and pins the coordinate shape two
 * ways: the explicit per-op check below counts every 2D texture coordinate that
 * is not 2-component, and under an asserts-enabled build nir_build_tex_struct
 * aborts inside the builder the moment a vec4 coordinate reaches the 2D sampler.
 * nir_validate runs too, but as a general well-formedness pass; it does not
 * assert coord_components against the sampler dimension, so it is not the
 * coord-shape net.  A regression to the vec4 coordinate fails this test (or
 * aborts inside the builder) before it can ship.
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

/* Count ALU instructions of a given opcode -- used to pin the Hamilton FS to
 * exactly four DP4s (one fdot4 per output quaternion lane). */
static unsigned
count_alu_op(nir_shader *s, nir_op op)
{
   unsigned n = 0;
   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu &&
                nir_instr_as_alu(instr)->op == op)
               n++;
         }
      }
   }
   return n;
}

static const nir_alu_instr *
find_color_store_vec4(nir_shader *s)
{
   const nir_alu_instr *store_vec = NULL;
   unsigned store_count = 0;

   nir_foreach_function_impl(impl, s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_store_deref ||
                !intr->src[1].ssa)
               continue;

            const nir_alu_instr *alu =
               nir_def_as_alu_or_null(intr->src[1].ssa);
            if (alu && alu->op == nir_op_vec4) {
               store_vec = alu;
               store_count++;
            }
         }
      }
   }

   return store_count == 1 ? store_vec : NULL;
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

      nir_validate_shader(s, "r300vk dp4 fs"); /* general well-formedness, not the coord-shape check */

      unsigned tex = 0, bad = 0;
      count_tex(s, &tex, &bad);
      snprintf(name, sizeof(name),
               "components=%u: two 2D tex ops, each a 2-component coord",
               widths[i]);
      CHECK(tex == 2 && bad == 0, name);

      snprintf(name, sizeof(name),
               "components=%u: dot is truncated before byte-pack",
               widths[i]);
      CHECK(count_alu_op(s, nir_op_ftrunc) == 1, name);

      const nir_alu_instr *color = find_color_store_vec4(s);
      snprintf(name, sizeof(name),
               "components=%u: RGBA8 output carries four computed bytes",
               widths[i]);
      CHECK(color && !nir_src_is_const(color->src[3].src), name);

      ralloc_free(s);
   }

   /* The Hamilton-product FS: two quaternion samplers (2-component coords, the
    * same coord-shape invariant as the DP4 FS) and exactly four DP4s -- one
    * fdot4 per output lane of q1*q2.  A regression that drops a lane or adds a
    * fifth dot (for example from a botched sign-permutation rewrite) trips the
    * fdot4 count. */
   {
      nir_shader *s = r300vk_build_qmul_fs_nir(&options);
      CHECK(s != NULL, "qmul builds a shader");
      if (s) {
         nir_validate_shader(s, "r300vk qmul fs");

         unsigned tex = 0, bad = 0;
         count_tex(s, &tex, &bad);
         CHECK(tex == 2 && bad == 0,
               "qmul: two 2D tex ops, each a 2-component coord");

         unsigned dp4 = count_alu_op(s, nir_op_fdot4);
         CHECK(dp4 == 4, "qmul: exactly four DP4s (one per quaternion lane)");

         ralloc_free(s);
      }
   }

   /* The rotation FS is the sandwich q*embed(v)*conj(q) = two Hamilton products,
    * so it samples two inputs (q, v) with 2-component coords and contains
    * exactly eight DP4s.  A drop to four would mean only one product emitted. */
   {
      nir_shader *s = r300vk_build_qrotate_fs_nir(&options);
      CHECK(s != NULL, "qrotate builds a shader");
      if (s) {
         nir_validate_shader(s, "r300vk qrotate fs");

         unsigned tex = 0, bad = 0;
         count_tex(s, &tex, &bad);
         CHECK(tex == 2 && bad == 0,
               "qrotate: two 2D tex ops, each a 2-component coord");

         unsigned dp4 = count_alu_op(s, nir_op_fdot4);
         CHECK(dp4 == 8, "qrotate: exactly eight DP4s (two Hamilton products)");

         ralloc_free(s);
      }
   }

   /* Each octonion-product pass is two Hamilton products over the four sampled
    * quaternion halves a,b,c,d (bindings 0..3): the lower half a*c - conj(d)*b
    * and the upper half d*a + b*conj(c).  So each pass samples four 2-component-
    * coord inputs and contains exactly eight DP4s; a dropped product would halve
    * the fdot4 count.  Two passes fill the eight-wide octonion result. */
   {
      nir_shader *s = r300vk_build_omul_lo_fs_nir(&options);
      CHECK(s != NULL, "omul_lo builds a shader");
      if (s) {
         nir_validate_shader(s, "r300vk omul_lo fs");
         unsigned tex = 0, bad = 0;
         count_tex(s, &tex, &bad);
         CHECK(tex == 4 && bad == 0,
               "omul_lo: four 2D tex ops, each a 2-component coord");
         unsigned dp4 = count_alu_op(s, nir_op_fdot4);
         CHECK(dp4 == 8, "omul_lo: exactly eight DP4s (two Hamilton products)");
         ralloc_free(s);
      }
   }
   {
      nir_shader *s = r300vk_build_omul_hi_fs_nir(&options);
      CHECK(s != NULL, "omul_hi builds a shader");
      if (s) {
         nir_validate_shader(s, "r300vk omul_hi fs");
         unsigned tex = 0, bad = 0;
         count_tex(s, &tex, &bad);
         CHECK(tex == 4 && bad == 0,
               "omul_hi: four 2D tex ops, each a 2-component coord");
         unsigned dp4 = count_alu_op(s, nir_op_fdot4);
         CHECK(dp4 == 8, "omul_hi: exactly eight DP4s (two Hamilton products)");
         ralloc_free(s);
      }
   }

   /* The MRT octonion FS does both halves in one draw: four sampled inputs, all
    * sixteen DP4s (four Hamilton products), and two color outputs (DATA0 = lower
    * half, DATA1 = upper half).  A regression that dropped a product or an output
    * trips the fdot4 or output count. */
   {
      nir_shader *s = r300vk_build_omul_mrt_fs_nir(&options);
      CHECK(s != NULL, "omul_mrt builds a shader");
      if (s) {
         nir_validate_shader(s, "r300vk omul_mrt fs");
         unsigned tex = 0, bad = 0;
         count_tex(s, &tex, &bad);
         CHECK(tex == 4 && bad == 0,
               "omul_mrt: four 2D tex ops, each a 2-component coord");
         unsigned dp4 = count_alu_op(s, nir_op_fdot4);
         CHECK(dp4 == 16, "omul_mrt: exactly sixteen DP4s (four Hamilton products)");
         unsigned outs = 0;
         nir_foreach_shader_out_variable(var, s)
            outs++;
         CHECK(outs == 2, "omul_mrt: two color outputs (DATA0 + DATA1)");
         ralloc_free(s);
      }
   }

   glsl_type_singleton_decref();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
