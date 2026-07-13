/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Build-time guard for the DP4 compute-as-raster fragment shader shape.
 *
 * r3v_synthesize_dp4_fs fed the vec4 texcoord varying straight into nir_tex
 * for a GLSL_SAMPLER_DIM_2D sampler.  nir_build_tex_struct asserts that a tex
 * instruction's coord_components equals the sampler dimension's coordinate
 * count (2 for 2D), so an asserts-enabled build aborted at every DP4 compute
 * pipeline create.  This test builds the same FS NIR via the extracted
 * r3v_build_dp4_fs_nir for each dot width and pins the coordinate shape two
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

#include "r3v_dp4_fs_nir.h"

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

static bool
has_single_tex0_input(nir_shader *s)
{
   unsigned tex0 = 0, other = 0;

   nir_foreach_shader_in_variable(var, s) {
      if (var->data.location == VARYING_SLOT_TEX0)
         tex0++;
      else
         other++;
   }

   return tex0 == 1 && other == 0;
}

static void
check_texcoord_shape(nir_shader *s, const char *label, unsigned expected_tex)
{
   char name[96];

   snprintf(name, sizeof(name), "%s: texcoord input uses TEX0 for generic0",
            label);
   CHECK(has_single_tex0_input(s), name);

   unsigned tex = 0, bad = 0;
   count_tex(s, &tex, &bad);
   snprintf(name, sizeof(name),
            "%s: %u 2D tex ops, each a 2-component coord",
            label, expected_tex);
   CHECK(tex == expected_tex && bad == 0, name);
}

static void
check_dp4_widths(const nir_shader_compiler_options *options)
{
   /* The kernel dot widths the dispatch admits: 0 means full vec4. */
   const unsigned widths[] = { 0, 2, 3, 4 };
   for (unsigned i = 0; i < ARRAY_SIZE(widths); i++) {
      char name[80];
      nir_shader *s = r3v_build_dp4_fs_nir(options, widths[i]);

      snprintf(name, sizeof(name), "components=%u builds a shader", widths[i]);
      CHECK(s != NULL, name);
      if (!s)
         continue;

      nir_validate_shader(s, "r3v dp4 fs"); /* general well-formedness, not the coord-shape check */

      snprintf(name, sizeof(name),
               "components=%u", widths[i]);
      check_texcoord_shape(s, name, 2);

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
}

typedef nir_shader *
(*fs_builder)(const nir_shader_compiler_options *options);

static void
check_hamilton_fs(const nir_shader_compiler_options *options,
                  const char *label, fs_builder builder,
                  unsigned expected_tex, unsigned expected_dp4)
{
   char name[96];
   nir_shader *s = builder(options);

   snprintf(name, sizeof(name), "%s builds a shader", label);
   CHECK(s != NULL, name);
   if (!s)
      return;

   snprintf(name, sizeof(name), "r3v %s fs", label);
   nir_validate_shader(s, name);
   check_texcoord_shape(s, label, expected_tex);

   snprintf(name, sizeof(name), "%s: exactly %u DP4s",
            label, expected_dp4);
   CHECK(count_alu_op(s, nir_op_fdot4) == expected_dp4, name);
   ralloc_free(s);
}

static void
check_omul_mrt_fs(const nir_shader_compiler_options *options)
{
   nir_shader *s = r3v_build_omul_mrt_fs_nir(options);
   CHECK(s != NULL, "omul_mrt builds a shader");
   if (!s)
      return;

   nir_validate_shader(s, "r3v omul_mrt fs");
   check_texcoord_shape(s, "omul_mrt", 4);
   CHECK(count_alu_op(s, nir_op_fdot4) == 16,
         "omul_mrt: exactly sixteen DP4s");

   unsigned outs = 0;
   nir_foreach_shader_out_variable(var, s)
      outs++;
   CHECK(outs == 2, "omul_mrt: two color outputs (DATA0 + DATA1)");
   ralloc_free(s);
}

static void
check_qfmaddsub_fs(const nir_shader_compiler_options *options, bool is_sub)
{
   const char *label = is_sub ? "qfmsub" : "qfmadd";
   nir_shader *s = r3v_build_qfmadd_fs_nir(options, is_sub);
   char name[96];

   snprintf(name, sizeof(name), "%s builds a shader", label);
   CHECK(s != NULL, name);
   if (!s)
      return;

   snprintf(name, sizeof(name), "r3v %s fs", label);
   nir_validate_shader(s, name);
   check_texcoord_shape(s, label, 3);

   snprintf(name, sizeof(name), "%s: exactly four DP4s", label);
   CHECK(count_alu_op(s, nir_op_fdot4) == 4, name);
   snprintf(name, sizeof(name), "%s: final combine uses %s", label,
            is_sub ? "fsub" : "fadd");
   CHECK(count_alu_op(s, is_sub ? nir_op_fsub : nir_op_fadd) == 1, name);
   snprintf(name, sizeof(name), "%s: opposite combine opcode is absent", label);
   CHECK(count_alu_op(s, is_sub ? nir_op_fadd : nir_op_fsub) == 0, name);

   ralloc_free(s);
}

int
main(void)
{
   glsl_type_singleton_init_or_ref();

   /* A zero-initialized options block is enough to build this shader; the FS
    * uses only always-available builder primitives. */
   static const nir_shader_compiler_options options = { 0 };

   check_dp4_widths(&options);
   check_hamilton_fs(&options, "qmul", r3v_build_qmul_fs_nir, 2, 4);
   check_hamilton_fs(&options, "qrotate", r3v_build_qrotate_fs_nir, 2, 8);
   check_hamilton_fs(&options, "omul_lo", r3v_build_omul_lo_fs_nir, 4, 8);
   check_hamilton_fs(&options, "omul_hi", r3v_build_omul_hi_fs_nir, 4, 8);
   check_omul_mrt_fs(&options);
   check_qfmaddsub_fs(&options, false);
   check_qfmaddsub_fs(&options, true);

   glsl_type_singleton_decref();

   if (g_failures) {
      printf("FAILED: %u check(s)\n", g_failures);
      return 1;
   }
   printf("PASSED\n");
   return 0;
}
