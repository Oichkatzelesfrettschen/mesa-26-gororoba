/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the NoPerspective reciprocal carrier plan: the TC1
 * register words the kernel width check admits at VAP_VTX_SIZE 12, the
 * per-triangle packing identity payload / carrier == varying under the
 * 1 / max(w) normalization, the admission envelope refusals ahead of any
 * write, the stream validator, the FP24 recovery model against the
 * exact UNORM8 witness, and the per-draw stream check over the emitted
 * carrier cell with each register word mutated in turn.
 */

#include "r300_noperspective_reciprocal_plan.h"
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

/* The contract's GB_SELECT word (r300_first_draw_state.c). */
#define GB_SELECT_BASE 0x00000000u

static void
test_register_words(void)
{
   struct r300_noperspective_reciprocal_plan plan;
   r300_noperspective_reciprocal_plan_tc1(&plan);
   CHECK(r300_noperspective_reciprocal_plan_validate(&plan) == 0);
   CHECK(r300_noperspective_reciprocal_plan_record_dwords(&plan) == 12u);
   /* Element 0 FLOAT_4 into vector 0, element 1 FLOAT_4 into vector 6;
    * element 2 FLOAT_4 into vector 7 with LAST_VEC. */
   CHECK(r300_noperspective_reciprocal_plan_prog_stream_cntl(&plan, 0) ==
         0x06030003u);
   CHECK(r300_noperspective_reciprocal_plan_prog_stream_cntl(&plan, 1) ==
         0x00002703u);
   /* TEX_0_COMP_CNT = TEX_1_COMP_CNT = 4: the kernel check's required
    * width 4 + 4 + 4 = 12. */
   CHECK(r300_noperspective_reciprocal_plan_vtx_fmt_1(&plan) == 0x24u);
   CHECK(r300_noperspective_reciprocal_plan_vsm_vtx_assm(&plan) ==
         (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0 | R300_INPUT_CNTL_TC1));
   CHECK(r300_noperspective_reciprocal_plan_rs_count(&plan) ==
         (R300_IT_COUNT(8) | R300_IC_COUNT(0) | R300_HIRES_EN));
   CHECK(r300_noperspective_reciprocal_plan_rs_inst_count(&plan) == 1u);
   CHECK(r300_noperspective_reciprocal_plan_rs_ip(&plan, 0) ==
         (R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3)));
   CHECK(r300_noperspective_reciprocal_plan_rs_ip(&plan, 1) ==
         (R300_RS_TEX_PTR(4) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3)));
   CHECK(r300_noperspective_reciprocal_plan_rs_inst(&plan, 1) ==
         (R300_RS_INST_TEX_ID(1) | R300_RS_INST_TEX_CN_WRITE |
          R300_RS_INST_TEX_ADDR(1)));

   struct r300_noperspective_reciprocal_plan bad = plan;
   bad.payload_vectors = 0;
   CHECK(r300_noperspective_reciprocal_plan_validate(&bad) == -EINVAL);
   bad = plan;
   bad.payload_vectors = R300_NOPERSPECTIVE_CARRIER_RS_VECTOR_BUDGET;
   bad.carrier_vector = bad.payload_vectors;
   CHECK(r300_noperspective_reciprocal_plan_validate(&bad) == -EINVAL);
   bad = plan;
   bad.carrier_vector = 0;
   CHECK(r300_noperspective_reciprocal_plan_validate(&bad) == -EINVAL);
   CHECK(r300_noperspective_reciprocal_plan_validate(NULL) == -EINVAL);
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
   struct r300_noperspective_reciprocal_plan plan;
   r300_noperspective_reciprocal_plan_tc1(&plan);
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][8];
   fill_source(source, w);
   float packed[3][12];
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &source[0][0], &packed[0][0]) == 0);
   for (unsigned v = 0; v < 3; v++) {
      CHECK(memcmp(packed[v], source[v], 16) == 0);
      CHECK(packed[v][8] == w[v] / 4.0f && packed[v][9] == 0.0f &&
            packed[v][10] == 0.0f && packed[v][11] == 1.0f);
      for (unsigned c = 0; c < 4; c++)
         CHECK(packed[v][4 + c] == source[v][4 + c] * (w[v] / 4.0f));
   }
   CHECK(r300_noperspective_reciprocal_validate_stream(&plan, &packed[0][0],
                                                       1) == 0);

   /* Refusals leave the output untouched. */
   float bad[3][8];
   float untouched[3][12];
   memset(untouched, 0xa5, sizeof(untouched));
   memcpy(bad, source, sizeof(bad));
   bad[0][3] = 0.0f;
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[0][3] = -1.0f;
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[1][6] = NAN;
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[1][3] = 1.0f * (float)R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX * 2.0f;
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[2][4] = (float)R300_NOPERSPECTIVE_CARRIER_LANE_MAX * 4.0f;
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   uint8_t sentinel[sizeof(untouched)];
   memset(sentinel, 0xa5, sizeof(sentinel));
   CHECK(memcmp(untouched, sentinel, sizeof(sentinel)) == 0);
   CHECK(r300_noperspective_reciprocal_pack_triangle(NULL, &source[0][0],
                                                     &packed[0][0]) ==
         -EINVAL);

   /* The stream validator refuses a carrier lane outside (0, 1] and a
    * fixed lane that moved. */
   float stream[3][12];
   memcpy(stream, packed, sizeof(stream));
   stream[1][8] = 1.5f;
   CHECK(r300_noperspective_reciprocal_validate_stream(&plan, &stream[0][0],
                                                       1) == -EDOM);
   memcpy(stream, packed, sizeof(stream));
   stream[2][11] = 0.0f;
   CHECK(r300_noperspective_reciprocal_validate_stream(&plan, &stream[0][0],
                                                       1) == -EDOM);
   CHECK(r300_noperspective_reciprocal_validate_stream(&plan, NULL, 0) == 0);
   CHECK(r300_noperspective_reciprocal_validate_stream(&plan, NULL, 1) ==
         -EINVAL);
}

/* The recovery model against the probe's own payload: at each vertex
 * b / c is the varying exactly in real arithmetic, and the FP24 model
 * lands within one UNORM8 quantum of it. */
static void
test_recover_model(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][8];
   fill_source(source, w);
   struct r300_noperspective_reciprocal_plan plan;
   r300_noperspective_reciprocal_plan_tc1(&plan);
   float packed[3][12];
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &source[0][0], &packed[0][0]) == 0);
   for (unsigned v = 0; v < 3; v++) {
      for (unsigned c = 0; c < 4; c++) {
         const float recovered = r300_noperspective_reciprocal_recover(
            packed[v][4 + c], packed[v][8]);
         CHECK(fabsf(recovered - source[v][4 + c]) <= 1.0f / 255.0f);
      }
   }
   /* Midpoint of vertices 0 and 1 in window space: interp(b) and
    * interp(c) are the perspective-weighted sums, and their ratio is
    * the window-linear midpoint of the varying. */
   const double W0 = 1.0 / w[0], W1 = 1.0 / w[1];
   const double denominator = 0.5 * W0 + 0.5 * W1;
   for (unsigned c = 0; c < 4; c++) {
      const double b = (0.5 * packed[0][4 + c] * W0 +
                        0.5 * packed[1][4 + c] * W1) / denominator;
      const double carrier =
         (0.5 * packed[0][8] * W0 + 0.5 * packed[1][8] * W1) / denominator;
      const double affine = 0.5 * source[0][4 + c] + 0.5 * source[1][4 + c];
      CHECK(fabs(b / carrier - affine) <= 1e-6);
      CHECK(fabsf(r300_noperspective_reciprocal_recover((float)b,
                                                        (float)carrier) -
                  (float)affine) <= 1.0f / 255.0f);
   }
}

/* The expanded-stream validator over a clipper fan: padding records
 * (position 0, 0, 0, 1, every other lane 0) are skipped and counted
 * out, a live record with a carrier lane outside (0, 1] refuses, and
 * the clipped-edge oracle is the perspective-weighted blend. */
static void
test_validate_expanded(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][8];
   fill_source(source, w);
   struct r300_noperspective_reciprocal_plan plan;
   r300_noperspective_reciprocal_plan_tc1(&plan);
   float fan[6][12];
   memset(fan, 0, sizeof(fan));
   CHECK(r300_noperspective_reciprocal_pack_triangle(
            &plan, &source[0][0], &fan[0][0]) == 0);
   for (unsigned v = 3; v < 6; v++)
      fan[v][3] = 1.0f;
   CHECK(r300_noperspective_reciprocal_validate_expanded(&plan, &fan[0][0],
                                                         6) == 3);
   /* A midpoint of records 0 and 1 is a live convex combination. */
   for (unsigned lane = 0; lane < 12; lane++)
      fan[3][lane] = 0.5f * fan[0][lane] + 0.5f * fan[1][lane];
   CHECK(r300_noperspective_reciprocal_validate_expanded(&plan, &fan[0][0],
                                                         6) == 4);
   fan[3][8] = 0.0f;
   CHECK(r300_noperspective_reciprocal_validate_expanded(&plan, &fan[0][0],
                                                         6) == -EDOM);
   CHECK(r300_noperspective_reciprocal_validate_expanded(&plan, NULL, 0) ==
         0);
   CHECK(r300_noperspective_reciprocal_validate_expanded(&plan, NULL, 1) ==
         -EINVAL);
   /* t = 1/2 between w 1 and w 4: weights 1 and 4 of 5. */
   CHECK(fabs(r300_noperspective_reciprocal_clipped_edge_value(
                 0.0, 1.0, 1.0, 4.0, 0.5) - 0.8) <= 1e-12);
   CHECK(r300_noperspective_reciprocal_clipped_edge_value(
            3.0, 1.0, 7.0, 4.0, 0.0) == 3.0);
   CHECK(r300_noperspective_reciprocal_clipped_edge_value(
            3.0, 1.0, 7.0, 4.0, 1.0) == 7.0);
}

/* The per-draw stream check over the emitted carrier cell: the cell
 * establishes every word ahead of its draw; each word mutated in place
 * names draw 0; the legacy varying cell (VTX_SIZE 8, one interpolator)
 * names draw 0 as well. */
static void
test_stream_check(void)
{
   struct r300_noperspective_reciprocal_plan plan;
   r300_noperspective_reciprocal_plan_tc1(&plan);
   struct r300_tcl_bypass_triangle_ib cell;
   CHECK(r300_tcl_bypass_triangle_noperspective_carrier_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &plan, &cell) == 0);
   CHECK(r300_noperspective_reciprocal_plan_stream_check(
            &plan, GB_SELECT_BASE, cell.ib, cell.ib_size_dwords) == 1);
   static const uint32_t mutated_regs[] = {
      R300_VAP_VTX_SIZE, R300_VAP_PROG_STREAM_CNTL_0,
      R300_VAP_PROG_STREAM_CNTL_1, R300_VAP_OUTPUT_VTX_FMT_1,
      R300_VAP_VSM_VTX_ASSM, R300_RS_COUNT, R300_RS_INST_COUNT,
      R300_RS_IP_0, R300_RS_IP_1, R300_RS_INST_0, R300_RS_INST_1,
      R300_GB_SELECT,
   };
   for (unsigned m = 0; m < sizeof(mutated_regs) / sizeof(mutated_regs[0]);
        m++) {
      uint32_t *copy = malloc(cell.ib_size_dwords * sizeof(uint32_t));
      CHECK(copy != NULL);
      if (copy == NULL)
         break;
      memcpy(copy, cell.ib, cell.ib_size_dwords * sizeof(uint32_t));
      /* Flip one bit of the last payload the register receives ahead
       * of the draw: the stream check reads the final write. */
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
         copy[last] ^= 0x10u;
         CHECK(r300_noperspective_reciprocal_plan_stream_check(
                  &plan, GB_SELECT_BASE, copy, cell.ib_size_dwords) == -1);
      }
      free(copy);
   }
   r300_tcl_bypass_triangle_release(&cell);

   struct r300_tcl_bypass_triangle_ib legacy;
   CHECK(r300_tcl_bypass_triangle_clip_space_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &legacy) == 0);
   CHECK(r300_noperspective_reciprocal_plan_stream_check(
            &plan, GB_SELECT_BASE, legacy.ib, legacy.ib_size_dwords) == -1);
   r300_tcl_bypass_triangle_release(&legacy);
   CHECK(r300_noperspective_reciprocal_plan_stream_check(
            &plan, GB_SELECT_BASE, NULL, 0) == -EINVAL);
}

int
main(void)
{
   test_register_words();
   test_pack_identity_and_envelope();
   test_recover_model();
   test_validate_expanded();
   test_stream_check();
   if (failures != 0) {
      fprintf(stderr, "r300_noperspective_reciprocal_plan_test: %d "
              "failures\n", failures);
      return 1;
   }
   printf("r300_noperspective_reciprocal_plan_test: all checks pass\n");
   return 0;
}
