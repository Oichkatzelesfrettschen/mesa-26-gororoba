/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the NoPerspective q-lane plan: the varying cell's
 * register words the kernel width check admits at VAP_VTX_SIZE 8, the
 * per-triangle packing identity payload / q == varying under the
 * 1 / max(w) normalization with lanes past the width zero-filled, the
 * admission envelope refusals ahead of any write, the stream and
 * expanded validators against a wrong carrier and a dirty unused lane,
 * the FP24 recovery model, and the per-draw stream check over the
 * emitted q-lane cell with each register word -- W_SELECT and
 * VTX_SIZE included -- mutated in turn.  The q-lane cell shares the
 * varying cell's register words and differs in its US program, so the
 * emitted cells are compared as bytes: the q-lane block replaces the
 * pass-through block and the TC1 carrier cell stays a different stream.
 */

#include "r300_noperspective_q_lane_plan.h"
#include "r300_noperspective_q_lane_fs_block.h"
#include "r300_noperspective_reciprocal_fs_block.h"
#include "r300_r2vb_producer_fs_block.h"
#include "r300_reg.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

#define GB_SELECT_BASE 0x00000000u

static void
test_register_words(void)
{
   struct r300_noperspective_q_lane_plan plan;
   for (uint32_t width = 1; width <= 3; width++) {
      r300_noperspective_q_lane_plan_init(&plan, width);
      CHECK(r300_noperspective_q_lane_plan_validate(&plan) == 0);
   }
   r300_noperspective_q_lane_plan_init(&plan, 0);
   CHECK(r300_noperspective_q_lane_plan_validate(&plan) == -EINVAL);
   /* Width 4 occupies the q lane: no carrier lane remains. */
   r300_noperspective_q_lane_plan_init(&plan, 4);
   CHECK(r300_noperspective_q_lane_plan_validate(&plan) == -EINVAL);
   CHECK(r300_noperspective_q_lane_plan_validate(NULL) == -EINVAL);
   /* Element 0 FLOAT_4 into vector 0, element 1 FLOAT_4 into vector 6
    * with LAST_VEC: the varying cell's word. */
   CHECK(r300_noperspective_q_lane_plan_prog_stream_cntl_0() == 0x26030003u);
   CHECK(r300_noperspective_q_lane_plan_vtx_fmt_1() ==
         R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS);
   CHECK(r300_noperspective_q_lane_plan_vsm_vtx_assm() ==
         (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0));
   CHECK(r300_noperspective_q_lane_plan_rs_count() ==
         (R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN));
   CHECK(r300_noperspective_q_lane_plan_rs_inst_count() == 0u);
   CHECK(r300_noperspective_q_lane_plan_rs_ip_0() ==
         (R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3)));
   CHECK(r300_noperspective_q_lane_plan_rs_inst_0() ==
         (R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
          R300_RS_INST_TEX_ADDR(0)));
}

static void
fill_source(float source[3][8], const float w[3])
{
   for (unsigned v = 0; v < 3; v++) {
      source[v][0] = 0.5f * (float)v - 0.5f;
      source[v][1] = 0.25f;
      source[v][2] = 0.5f;
      source[v][3] = w[v];
      for (unsigned c = 0; c < 4; c++)
         source[v][4 + c] = 0.125f * (float)(v * 4 + c + 1);
   }
}

static void
test_pack_identity_and_envelope(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][8];
   fill_source(source, w);
   for (uint32_t width = 1; width <= 3; width++) {
      struct r300_noperspective_q_lane_plan plan;
      r300_noperspective_q_lane_plan_init(&plan, width);
      float packed[3][8];
      CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &source[0][0],
                                                     &packed[0][0]) == 0);
      for (unsigned v = 0; v < 3; v++) {
         CHECK(memcmp(packed[v], source[v], 16) == 0);
         CHECK(packed[v][7] == w[v] / 4.0f);
         for (unsigned c = 0; c < 3; c++) {
            if (c < width)
               CHECK(packed[v][4 + c] == source[v][4 + c] * (w[v] / 4.0f));
            else
               CHECK(packed[v][4 + c] == 0.0f);
         }
      }
      CHECK(r300_noperspective_q_lane_validate_stream(&plan, &packed[0][0],
                                                       1) == 0);
      /* In place: the same bytes. */
      float aliased[3][8];
      memcpy(aliased, source, sizeof(aliased));
      CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &aliased[0][0],
                                                     &aliased[0][0]) == 0);
      CHECK(memcmp(aliased, packed, sizeof(packed)) == 0);

      /* A dirty unused lane and a wrong carrier refuse validation. */
      float mutated[3][8];
      if (width < 3) {
         memcpy(mutated, packed, sizeof(mutated));
         mutated[1][4 + width] = 0.5f;
         CHECK(r300_noperspective_q_lane_validate_stream(
                  &plan, &mutated[0][0], 1) == -EDOM);
      }
      memcpy(mutated, packed, sizeof(mutated));
      mutated[0][7] = 0.0f;
      CHECK(r300_noperspective_q_lane_validate_stream(&plan, &mutated[0][0],
                                                       1) == -EDOM);
      memcpy(mutated, packed, sizeof(mutated));
      mutated[2][7] = 1.5f;
      CHECK(r300_noperspective_q_lane_validate_stream(&plan, &mutated[0][0],
                                                       1) == -EDOM);
      memcpy(mutated, packed, sizeof(mutated));
      mutated[2][4] = NAN;
      CHECK(r300_noperspective_q_lane_validate_stream(&plan, &mutated[0][0],
                                                       1) == -EDOM);
   }

   /* Refusals leave the output untouched. */
   struct r300_noperspective_q_lane_plan plan;
   r300_noperspective_q_lane_plan_init(&plan, 3);
   float bad[3][8];
   float untouched[3][8], sentinel[3][8];
   memset(untouched, 0xa5, sizeof(untouched));
   memcpy(sentinel, untouched, sizeof(sentinel));
   memcpy(bad, source, sizeof(bad));
   bad[0][3] = 0.0f;
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &bad[0][0],
                                                  &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[0][3] = -1.0f;
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &bad[0][0],
                                                  &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[1][6] = NAN;
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &bad[0][0],
                                                  &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[1][3] = (float)R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX * 2.0f;
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &bad[0][0],
                                                  &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[2][4] = (float)R300_NOPERSPECTIVE_CARRIER_LANE_MAX * 4.0f;
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &bad[0][0],
                                                  &untouched[0][0]) == -EDOM);
   CHECK(memcmp(untouched, sentinel, sizeof(sentinel)) == 0);
   /* A NaN in a lane past the width is outside the interface and is
    * zero-filled rather than refused. */
   r300_noperspective_q_lane_plan_init(&plan, 1);
   memcpy(bad, source, sizeof(bad));
   bad[1][6] = NAN;
   float narrow[3][8];
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &bad[0][0],
                                                  &narrow[0][0]) == 0);
   CHECK(narrow[1][6] == 0.0f);
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, NULL,
                                                  &narrow[0][0]) == -EINVAL);
   CHECK(r300_noperspective_q_lane_validate_stream(&plan, NULL, 1) ==
         -EINVAL);
}

/* The recovery model: at each vertex b / q is the varying exactly in
 * real arithmetic and the FP24 model lands within one UNORM8 quantum;
 * at a window midpoint the ratio of the perspective-weighted sums is
 * the window-linear midpoint. */
static void
test_recover_model(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][8];
   fill_source(source, w);
   struct r300_noperspective_q_lane_plan plan;
   r300_noperspective_q_lane_plan_init(&plan, 3);
   float packed[3][8];
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &source[0][0],
                                                  &packed[0][0]) == 0);
   for (unsigned v = 0; v < 3; v++)
      for (unsigned c = 0; c < 3; c++)
         CHECK(fabsf(r300_noperspective_q_lane_recover(packed[v][4 + c],
                                                        packed[v][7]) -
                     source[v][4 + c]) <= 1.0f / 255.0f);
   const double W0 = 1.0 / w[0], W1 = 1.0 / w[1];
   const double denominator = 0.5 * W0 + 0.5 * W1;
   const double carrier =
      (0.5 * packed[0][7] * W0 + 0.5 * packed[1][7] * W1) / denominator;
   for (unsigned c = 0; c < 3; c++) {
      const double b = (0.5 * packed[0][4 + c] * W0 +
                        0.5 * packed[1][4 + c] * W1) / denominator;
      const double affine = 0.5 * source[0][4 + c] + 0.5 * source[1][4 + c];
      CHECK(fabs(b / carrier - affine) <= 1e-6);
      CHECK(fabsf(r300_noperspective_q_lane_recover((float)b,
                                                     (float)carrier) -
                  (float)affine) <= 1.0f / 255.0f);
   }
}

static void
test_validate_expanded(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][8];
   fill_source(source, w);
   struct r300_noperspective_q_lane_plan plan;
   r300_noperspective_q_lane_plan_init(&plan, 2);
   float fan[6][8];
   memset(fan, 0, sizeof(fan));
   CHECK(r300_noperspective_q_lane_pack_triangle(&plan, &source[0][0],
                                                  &fan[0][0]) == 0);
   for (unsigned v = 3; v < 6; v++)
      fan[v][3] = 1.0f;
   CHECK(r300_noperspective_q_lane_validate_expanded(&plan, &fan[0][0], 6) ==
         3);
   for (unsigned lane = 0; lane < 8; lane++)
      fan[3][lane] = 0.5f * fan[0][lane] + 0.5f * fan[1][lane];
   CHECK(r300_noperspective_q_lane_validate_expanded(&plan, &fan[0][0], 6) ==
         4);
   fan[3][6] = 0.25f;
   CHECK(r300_noperspective_q_lane_validate_expanded(&plan, &fan[0][0], 6) ==
         -EDOM);
   fan[3][6] = 0.0f;
   fan[3][7] = 0.0f;
   CHECK(r300_noperspective_q_lane_validate_expanded(&plan, &fan[0][0], 6) ==
         -EDOM);
   CHECK(r300_noperspective_q_lane_validate_expanded(&plan, NULL, 0) == 0);
   CHECK(r300_noperspective_q_lane_validate_expanded(&plan, NULL, 1) ==
         -EINVAL);
}

static bool
ib_contains_block(const struct r300_tcl_bypass_triangle_ib *cell,
                  const uint32_t *block, unsigned block_dwords)
{
   for (uint32_t i = 0; i + block_dwords <= cell->ib_size_dwords; i++)
      if (memcmp(&cell->ib[i], block, block_dwords * sizeof(uint32_t)) == 0)
         return true;
   return false;
}

/* The emitted q-lane cell: every register word ahead of its draw, each
 * word mutated in place names draw 0 (VTX_SIZE 4 and W_SELECT 1
 * included), the US program is the q-lane block and not the
 * pass-through or TC1 block, and the TC1 carrier cell is a different
 * stream the q-lane check names as well. */
static void
test_stream_check_and_cell_bytes(void)
{
   struct r300_noperspective_q_lane_plan plan;
   r300_noperspective_q_lane_plan_init(&plan, 3);
   struct r300_tcl_bypass_triangle_ib cell;
   CHECK(r300_tcl_bypass_triangle_noperspective_q_lane_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &plan, &cell) == 0);
   CHECK(r300_noperspective_q_lane_plan_stream_check(
            &plan, GB_SELECT_BASE, cell.ib, cell.ib_size_dwords) == 1);
   static const uint32_t mutated_regs[] = {
      R300_VAP_VTX_SIZE,   R300_VAP_PROG_STREAM_CNTL_0,
      R300_VAP_OUTPUT_VTX_FMT_1, R300_VAP_VSM_VTX_ASSM,
      R300_RS_COUNT,       R300_RS_INST_COUNT,
      R300_RS_IP_0,        R300_RS_INST_0,
      R300_GB_SELECT,
   };
   for (unsigned m = 0; m < sizeof(mutated_regs) / sizeof(mutated_regs[0]);
        m++) {
      uint32_t *copy = malloc(cell.ib_size_dwords * sizeof(uint32_t));
      CHECK(copy != NULL);
      if (copy == NULL)
         break;
      memcpy(copy, cell.ib, cell.ib_size_dwords * sizeof(uint32_t));
      uint32_t last = UINT32_MAX;
      for (uint32_t i = 0; i < cell.ib_size_dwords;) {
         const uint32_t header = copy[i];
         const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
         if ((header >> 30) == 0) {
            const uint32_t base = (header & 0x3FFF) * 4;
            const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
            for (uint32_t k = 0; k < count; k++)
               if (base + (one_reg ? 0 : 4 * k) == mutated_regs[m])
                  last = i + 1 + k;
            i += 1 + count;
         } else if ((header >> 30) == 3) {
            i += 1 + count;
         } else {
            i += 1;
         }
      }
      CHECK(last != UINT32_MAX);
      if (last != UINT32_MAX) {
         /* VTX_SIZE 8 -> 4 and GB_SELECT W_SELECT set are the named
          * mutations; the other words flip one bit. */
         if (mutated_regs[m] == R300_VAP_VTX_SIZE)
            copy[last] = 4u;
         else if (mutated_regs[m] == R300_GB_SELECT)
            copy[last] |= R300_GB_W_SELECT_1;
         else
            copy[last] ^= 0x10u;
         CHECK(r300_noperspective_q_lane_plan_stream_check(
                  &plan, GB_SELECT_BASE, copy, cell.ib_size_dwords) == -1);
      }
      free(copy);
   }
   CHECK(ib_contains_block(
      &cell, r300_noperspective_q_lane_fs_block,
      sizeof(r300_noperspective_q_lane_fs_block) / sizeof(uint32_t)));
   CHECK(!ib_contains_block(
      &cell, r300_r2vb_producer_fs_block,
      sizeof(r300_r2vb_producer_fs_block) / sizeof(uint32_t)));
   CHECK(!ib_contains_block(
      &cell, r300_noperspective_reciprocal_fs_block,
      sizeof(r300_noperspective_reciprocal_fs_block) / sizeof(uint32_t)));

   /* The varying cell carries the same register words under the
    * pass-through block: register words pass, US words differ. */
   struct r300_tcl_bypass_triangle_ib legacy;
   CHECK(r300_tcl_bypass_triangle_clip_space_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &legacy) == 0);
   CHECK(r300_noperspective_q_lane_plan_stream_check(
            &plan, GB_SELECT_BASE, legacy.ib, legacy.ib_size_dwords) == 1);
   CHECK(memcmp(legacy.ib, cell.ib, cell.ib_size_dwords * 4u) != 0);
   CHECK(ib_contains_block(
      &legacy, r300_r2vb_producer_fs_block,
      sizeof(r300_r2vb_producer_fs_block) / sizeof(uint32_t)));
   r300_tcl_bypass_triangle_release(&legacy);

   /* The TC1 carrier cell is the other record shape. */
   struct r300_noperspective_reciprocal_plan tc1;
   r300_noperspective_reciprocal_plan_tc1(&tc1);
   struct r300_tcl_bypass_triangle_ib carrier;
   CHECK(r300_tcl_bypass_triangle_noperspective_carrier_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &tc1, &carrier) == 0);
   CHECK(r300_noperspective_q_lane_plan_stream_check(
            &plan, GB_SELECT_BASE, carrier.ib, carrier.ib_size_dwords) == -1);
   r300_tcl_bypass_triangle_release(&carrier);
   r300_tcl_bypass_triangle_release(&cell);

   CHECK(r300_noperspective_q_lane_plan_stream_check(&plan, GB_SELECT_BASE,
                                                      NULL, 0) == -EINVAL);
   /* The emitter refuses a width the q lane cannot carry. */
   struct r300_noperspective_q_lane_plan wide;
   r300_noperspective_q_lane_plan_init(&wide, 4);
   CHECK(r300_tcl_bypass_triangle_noperspective_q_lane_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &wide, &cell) == -EINVAL);
}

int
main(void)
{
   test_register_words();
   test_pack_identity_and_envelope();
   test_recover_model();
   test_validate_expanded();
   test_stream_check_and_cell_bytes();
   if (failures != 0) {
      fprintf(stderr, "r300_noperspective_q_lane_plan_test: %d failures\n",
              failures);
      return 1;
   }
   printf("r300_noperspective_q_lane_plan_test: all checks pass\n");
   return 0;
}
