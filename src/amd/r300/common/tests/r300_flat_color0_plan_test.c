/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the direct GA Flat plan ahead of silicon: the
 * canonical plan's register words, the refusal and the localized byte
 * deviation of every mutation, the per-draw stream check over a
 * two-pass stream, and the expected-target generator judged by the
 * coverage oracle the CPU replication receipt qualified.
 */

#include "r300_flat_color0_plan.h"
#include "r300_first_draw_state.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
   do {                                                                    \
      if (!(cond)) {                                                       \
         fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                 #cond);                                                   \
         failures++;                                                       \
      }                                                                    \
   } while (0)

/* The contract's GA_COLOR_CONTROL word: every lane Gouraud, provoking
 * LAST (r300_first_draw_state.c). */
#define GA_BASE 0x0003aaaau

static void
test_canonical_words(void)
{
   struct r300_flat_color0_plan plan;
   r300_flat_color0_plan_direct_first(&plan);
   CHECK(r300_flat_color0_plan_validate(&plan) == 0);
   CHECK(r300_flat_color0_plan_ga_color_control(&plan, GA_BASE) ==
         0x0000aaa5u);
   CHECK(r300_flat_color0_plan_rs_count(&plan) ==
         (R300_IC_COUNT(1) | R300_HIRES_EN));
   CHECK(r300_flat_color0_plan_rs_ip_0(&plan) == 0u);
   CHECK(r300_flat_color0_plan_rs_inst_0(&plan) ==
         (R300_RS_INST_COL_CN_WRITE | R300_RS_INST_COL_ADDR(0)));
   CHECK(r300_flat_color0_plan_vsm_vtx_assm(&plan) ==
         (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_COLOR));
   CHECK(r300_flat_color0_plan_validate(NULL) != 0);
}

/* Registers whose PACKET0 payload differs between two streams of the
 * same packet structure; returns the count and fills regs. */
static unsigned
differing_registers(const uint32_t *a, const uint32_t *b, uint32_t dwords,
                    uint32_t regs[16])
{
   unsigned n = 0;
   uint32_t i = 0;
   while (i < dwords) {
      const uint32_t header = a[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (header != b[i]) {
         if (n < 16)
            regs[n] = 0xffffffffu;
         n++;
      }
      if (kind == 0) {
         if (i + count >= dwords)
            break;
         const uint32_t base = (header & 0x3FFF) * 4;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         for (uint32_t k = 0; k < count; k++) {
            if (a[i + 1 + k] != b[i + 1 + k]) {
               if (n < 16)
                  regs[n] = base + (one_reg ? 0 : 4 * k);
               n++;
            }
         }
         i += 1 + count;
      } else if (kind == 3) {
         if (i + count >= dwords)
            break;
         for (uint32_t k = 0; k < count; k++) {
            if (a[i + 1 + k] != b[i + 1 + k]) {
               if (n < 16)
                  regs[n] = 0xfffffffeu;
               n++;
            }
         }
         i += 1 + count;
      } else {
         i += 1;
      }
   }
   return n;
}

static bool
same_register_set(const uint32_t *regs, unsigned n, const uint32_t *want,
                  unsigned want_n)
{
   if (n != want_n)
      return false;
   for (unsigned i = 0; i < want_n; i++) {
      bool found = false;
      for (unsigned j = 0; j < n; j++)
         found |= regs[j] == want[i];
      if (!found)
         return false;
   }
   return true;
}

static void
expect_mutation(const struct r300_flat_color0_plan *mutated,
                const struct r300_tcl_bypass_triangle_ib *canonical,
                const uint32_t *want, unsigned want_n, const char *name)
{
   CHECK(r300_flat_color0_plan_validate(mutated) == -EINVAL);
   struct r300_tcl_bypass_triangle_ib refused;
   CHECK(r300_tcl_bypass_triangle_flat_color0_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, mutated, &refused) == -EINVAL);
   CHECK(refused.ib == NULL);
   struct r300_tcl_bypass_triangle_ib realized;
   CHECK(r300_tcl_bypass_triangle_flat_color0_plan_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, mutated, &realized) == 0);
   CHECK(realized.ib_size_dwords == canonical->ib_size_dwords);
   uint32_t regs[16];
   const unsigned n = differing_registers(canonical->ib, realized.ib,
                                          canonical->ib_size_dwords, regs);
   if (!same_register_set(regs, n, want, want_n)) {
      fprintf(stderr, "%s: deviates at %u register(s):", name, n);
      for (unsigned i = 0; i < n && i < 16; i++)
         fprintf(stderr, " 0x%04x", regs[i]);
      fprintf(stderr, "\n");
      failures++;
   }
   /* The mutation's own stream fails the canonical plan's per-draw
    * check, and passes its own. */
   struct r300_flat_color0_plan canonical_plan;
   r300_flat_color0_plan_direct_first(&canonical_plan);
   CHECK(r300_flat_color0_plan_stream_check(&canonical_plan, GA_BASE,
                                            realized.ib,
                                            realized.ib_size_dwords) == -1);
   CHECK(r300_flat_color0_plan_stream_check(mutated, GA_BASE, realized.ib,
                                            realized.ib_size_dwords) == 1);
   r300_tcl_bypass_triangle_release(&realized);
}

static void
test_mutations_refuse_and_localize(void)
{
   struct r300_flat_color0_plan plan;
   r300_flat_color0_plan_direct_first(&plan);
   struct r300_tcl_bypass_triangle_ib canonical;
   CHECK(r300_tcl_bypass_triangle_flat_color0_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &plan, &canonical) == 0);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, canonical.ib,
                                            canonical.ib_size_dwords) == 1);

   struct r300_flat_color0_plan m;
   const uint32_t ga[1] = { R300_GA_COLOR_CONTROL };
   const uint32_t rs_inst[1] = { R300_RS_INST_0 };
   const uint32_t rs_source[4] = { R300_RS_COUNT, R300_RS_IP_0,
                                   R300_RS_INST_0, R300_VAP_VSM_VTX_ASSM };

   m = plan;
   m.rgb0_shading = R300_GA_COLOR_CONTROL_RGB0_SHADING_GOURAUD;
   m.alpha0_shading = R300_GA_COLOR_CONTROL_ALPHA0_SHADING_GOURAUD;
   expect_mutation(&m, &canonical, ga, 1, "GA Flat -> Gouraud");

   m = plan;
   m.provoking = R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_LAST;
   expect_mutation(&m, &canonical, ga, 1, "provoking FIRST -> LAST");

   m = plan;
   m.alpha0_shading = R300_GA_COLOR_CONTROL_ALPHA0_SHADING_GOURAUD;
   expect_mutation(&m, &canonical, ga, 1, "RGB Flat, alpha Gouraud");

   m = plan;
   m.rs_source = R300_FLAT_COLOR0_RS_SOURCE_TEX0;
   expect_mutation(&m, &canonical, rs_source, 4, "RS source COLOR0 -> TEX0");

   m = plan;
   m.us_input = 1;
   expect_mutation(&m, &canonical, rs_inst, 1,
                   "RS destination away from the fragment input");

   /* The direct stream and the replication stream necessarily differ:
    * the varying moves from TEX0 to color 0 and the interpolation
    * state moves into the contract. */
   struct r300_tcl_bypass_triangle_ib replication;
   CHECK(r300_tcl_bypass_triangle_clip_space_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &replication) == 0);
   CHECK(replication.ib_size_dwords != canonical.ib_size_dwords ||
         memcmp(replication.ib, canonical.ib,
                canonical.ib_size_dwords * 4u) != 0);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, replication.ib,
                                            replication.ib_size_dwords) ==
         -1);
   r300_tcl_bypass_triangle_release(&replication);
   r300_tcl_bypass_triangle_release(&canonical);
}

static void
test_contract_carries_the_plan(void)
{
   struct r300_flat_color0_plan plan;
   r300_flat_color0_plan_direct_first(&plan);
   struct r300_first_draw_contract contract;
   CHECK(r300_tcl_bypass_triangle_reference_contract(&contract) == 0);
   CHECK(r300_flat_color0_plan_apply_contract(&plan, &contract) == 0);
   struct r300_tcl_bypass_triangle_ib cell;
   CHECK(r300_tcl_bypass_triangle_flat_color0_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &plan, &cell) == 0);
   /* The poison-model checker over the plan-applied contract: the
    * stream establishes every clause itself, the six plan registers
    * included. */
   struct r300_first_draw_check_report report;
   CHECK(r300_first_draw_state_check(&contract, cell.ib, cell.ib_size_dwords,
                                     0xdeadbeefu, &report) == 0);
   /* The cell writes no RS block of its own: each plan register occurs
    * exactly once, in the contract prefix. */
   uint32_t regs[16];
   uint32_t zeroed[512];
   memcpy(zeroed, cell.ib, cell.ib_size_dwords * 4u);
   struct r300_flat_color0_plan gouraud = plan;
   gouraud.rgb0_shading = R300_GA_COLOR_CONTROL_RGB0_SHADING_GOURAUD;
   struct r300_tcl_bypass_triangle_ib other;
   CHECK(r300_tcl_bypass_triangle_flat_color0_plan_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, false,
            1u, &gouraud, &other) == 0);
   CHECK(differing_registers(cell.ib, other.ib, cell.ib_size_dwords, regs) ==
         1);
   r300_tcl_bypass_triangle_release(&other);
   r300_tcl_bypass_triangle_release(&cell);
   CHECK(r300_flat_color0_plan_apply_contract(&plan, NULL) == -EINVAL);
}

static void
test_second_pass_must_establish_its_own_state(void)
{
   struct r300_triangle_multi_pass mp;
   memset(&mp, 0, sizeof(mp));
   for (unsigned p = 0; p < 2; p++) {
      r300_tcl_bypass_triangle_render_shape_reference(&mp.pass[p]);
      mp.pass[p].varying = true;
      mp.pass[p].flat_color0 = true;
   }
   mp.second_vertex_index = 2;
   mp.second_color_index = 3;
   struct r300_tcl_bypass_triangle_ib stream;
   CHECK(r300_tcl_bypass_triangle_clip_space_multi_pass_emit(&mp, &stream) ==
         0);
   struct r300_flat_color0_plan plan;
   r300_flat_color0_plan_direct_first(&plan);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, stream.ib,
                                            stream.ib_size_dwords) == 2);

   /* Second pass retaining the first pass's GA state: its own
    * GA_COLOR_CONTROL write carries the contract's Gouraud word, as a
    * pass that inherits rather than establishes would; the check
    * names the second draw. */
   const uint32_t ga_word = r300_flat_color0_plan_ga_color_control(&plan,
                                                                   GA_BASE);
   unsigned occurrences = 0;
   for (uint32_t i = 0; i + 1 < stream.ib_size_dwords; i++) {
      if (stream.ib[i] == CP_PACKET0(R300_GA_COLOR_CONTROL, 0) &&
          stream.ib[i + 1] == ga_word) {
         occurrences++;
         if (occurrences == 2)
            stream.ib[i + 1] = GA_BASE;
      }
   }
   CHECK(occurrences == 2);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, stream.ib,
                                            stream.ib_size_dwords) == -2);
   /* Second pass retaining the first pass's RS state, likewise. */
   const uint32_t rs_word = r300_flat_color0_plan_rs_inst_0(&plan);
   occurrences = 0;
   for (uint32_t i = 0; i + 1 < stream.ib_size_dwords; i++) {
      if (stream.ib[i] == CP_PACKET0(R300_GA_COLOR_CONTROL, 0))
         stream.ib[i + 1] = ga_word;
      if (stream.ib[i] == CP_PACKET0(R300_RS_INST_0, 0) &&
          stream.ib[i + 1] == rs_word) {
         occurrences++;
         if (occurrences == 2)
            stream.ib[i + 1] = 0;
      }
   }
   CHECK(occurrences == 2);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, stream.ib,
                                            stream.ib_size_dwords) == -2);
   r300_tcl_bypass_triangle_release(&stream);

   /* A pass with varying alone keeps the TEX0 family; flat_color0
    * without varying is the constant-color shape. */
   mp.pass[1].flat_color0 = false;
   CHECK(r300_tcl_bypass_triangle_clip_space_multi_pass_emit(&mp, &stream) ==
         0);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, stream.ib,
                                            stream.ib_size_dwords) == -2);
   r300_tcl_bypass_triangle_release(&stream);
}

/* One cell above the per-draw triangle ceiling emits several segment
 * draws behind one contract prefix; the check counts every draw, since
 * the pass boundary is the VAP_PROG_STREAM_CNTL_0 rewrite a new cell
 * makes, not the draw packet.  A truncated stream refuses.
 */
static void
test_segment_draws_share_one_pass(void)
{
   struct r300_flat_color0_plan plan;
   r300_flat_color0_plan_direct_first(&plan);
   struct r300_tcl_bypass_triangle_ib cell;
   /* The clipper's output count for this source count crosses the
    * per-draw ceiling, so the one cell emits two segment draws. */
   const uint32_t source_count =
      R300_TRIANGLE_MAX_TRIANGLES /
         R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT +
      1u;
   CHECK(r300_tcl_bypass_triangle_flat_color0_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            source_count, &plan, &cell) == 0);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, cell.ib,
                                            cell.ib_size_dwords) == 2);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, cell.ib,
                                            cell.ib_size_dwords - 1u) ==
         -EINVAL);
   CHECK(r300_flat_color0_plan_stream_check(&plan, GA_BASE, NULL, 0) ==
         -EINVAL);
   r300_tcl_bypass_triangle_release(&cell);
}

static void
test_expected_target_and_carrier_mutation(void)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const uint32_t footprint =
      shape.pitch_pixels * (shape.height + R300_TRIANGLE_CANARY_ROWS) * 4u;
   uint32_t *expected = calloc(1, footprint);
   uint32_t *wrong = calloc(1, footprint);
   const uint32_t provoking = 0x8cff0000u, other = 0xf200ff00u;
   const uint32_t sentinel = R300_TRIANGLE_COLOR_SENTINEL;
   CHECK(r300_tcl_bypass_triangle_expected_target(&shape, provoking, sentinel,
                                                  expected, footprint) == 0);
   struct r300_triangle_coverage_verdict v;
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &provoking, 1, sentinel,
                                            expected, footprint, &v);
   CHECK(v.judged && v.coverage_exact && v.canary_pass);
   CHECK(v.interior_pixels == v.analytic_pixels && v.mismatch_pixels == 0);
   bool judged = false;
   CHECK(r300_tcl_bypass_triangle_target_compare(&shape, expected, expected,
                                                 footprint, &judged) == 0);
   CHECK(judged);

   /* COLOR0 carrier replaced with one vertex's wrong value: the
    * replication oracle's provoking value changes, so a target drawn
    * from the wrong record differs at every interior pixel. */
   CHECK(r300_tcl_bypass_triangle_expected_target(&shape, other, sentinel,
                                                  wrong, footprint) == 0);
   CHECK(r300_tcl_bypass_triangle_target_compare(&shape, expected, wrong,
                                                 footprint, &judged) ==
         (int)v.analytic_pixels);
   r300_tcl_bypass_triangle_coverage_oracle(&shape, &provoking, 1, sentinel,
                                            wrong, footprint, &v);
   CHECK(v.judged && !v.coverage_exact && v.mismatch_pixels ==
                                             v.analytic_pixels);
   /* A single exterior write is one differing dword. */
   memcpy(wrong, expected, footprint);
   wrong[0] ^= 1u;
   CHECK(r300_tcl_bypass_triangle_target_compare(&shape, expected, wrong,
                                                 footprint, &judged) == 1);
   CHECK(r300_tcl_bypass_triangle_target_compare(&shape, expected, wrong,
                                                 footprint - 4u, &judged) ==
         -EINVAL);
   CHECK(!judged);
   CHECK(r300_tcl_bypass_triangle_expected_target(&shape, provoking, sentinel,
                                                  expected, footprint - 4u) ==
         -EINVAL);
   free(expected);
   free(wrong);
}

int
main(void)
{
   test_canonical_words();
   test_mutations_refuse_and_localize();
   test_contract_carries_the_plan();
   test_second_pass_must_establish_its_own_state();
   test_segment_draws_share_one_pass();
   test_expected_target_and_carrier_mutation();
   if (failures != 0) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return EXIT_FAILURE;
   }
   printf("r300-flat-color0-plan: ok\n");
   return EXIT_SUCCESS;
}
