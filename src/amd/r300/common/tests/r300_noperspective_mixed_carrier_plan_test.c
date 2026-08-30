/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the mixed Smooth/NoPerspective carrier plan: the
 * three-vector register words at VAP_VTX_SIZE 16, the packing identity
 * (vector 0 verbatim, vector 1 premultiplied, TC2 the normalized w) with
 * the envelope refusals ahead of any write, the plan refusals the first
 * shape names (RS budget, carrier alias, premultiplied set, US budget),
 * the stream and expanded validators, the recovery model, and the
 * per-draw stream check over the emitted cell with each register word
 * -- W_SELECT and VTX_SIZE 12 included -- mutated in turn.  The US
 * program is the mixed block and no other cell's.
 */

#include "r300_noperspective_mixed_carrier_plan.h"
#include "r300_noperspective_mixed_carrier_fs_block.h"
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
#define SRC R300_NOPERSPECTIVE_MIXED_CARRIER_SOURCE_DWORDS
#define REC R300_NOPERSPECTIVE_MIXED_CARRIER_RECORD_DWORDS

static void
test_plan_words_and_refusals(void)
{
   struct r300_noperspective_mixed_carrier_plan plan;
   r300_noperspective_mixed_carrier_plan_first(&plan);
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&plan) == 0);
   CHECK(r300_noperspective_reciprocal_plan_record_dwords(&plan.carrier) ==
         REC);
   /* Elements: position into vector 0, TC0 into 6 (LAST_VEC clear:
    * the varying cell's 0x26030003 minus that bit), TC1 into 7, TC2
    * into 8 with LAST_VEC. */
   CHECK(r300_noperspective_reciprocal_plan_prog_stream_cntl(&plan.carrier,
                                                              0) ==
         0x06030003u);
   CHECK(r300_noperspective_reciprocal_plan_prog_stream_cntl(&plan.carrier,
                                                              1) ==
         ((R300_DATA_TYPE_FLOAT_4 | (7u << R300_DST_VEC_LOC_SHIFT)) |
          ((R300_DATA_TYPE_FLOAT_4 | (8u << R300_DST_VEC_LOC_SHIFT) |
            R300_LAST_VEC)
           << 16)));
   CHECK(r300_noperspective_reciprocal_plan_vtx_fmt_1(&plan.carrier) ==
         (R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS |
          (R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS << 3) |
          (R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS << 6)));
   CHECK(r300_noperspective_reciprocal_plan_vsm_vtx_assm(&plan.carrier) ==
         (R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0 | R300_INPUT_CNTL_TC1 |
          R300_INPUT_CNTL_TC2));
   CHECK(r300_noperspective_reciprocal_plan_rs_count(&plan.carrier) ==
         (R300_IT_COUNT(12) | R300_IC_COUNT(0) | R300_HIRES_EN));
   CHECK(r300_noperspective_reciprocal_plan_rs_inst_count(&plan.carrier) ==
         2u);
   for (uint32_t i = 0; i < 3; i++) {
      CHECK(r300_noperspective_reciprocal_plan_rs_ip(&plan.carrier, i) ==
            (R300_RS_TEX_PTR(4 * i) | R300_RS_SEL_S(R300_RS_SEL_C0) |
             R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
             R300_RS_SEL_Q(R300_RS_SEL_C3)));
      CHECK(r300_noperspective_reciprocal_plan_rs_inst(&plan.carrier, i) ==
            (R300_RS_INST_TEX_ID(i) | R300_RS_INST_TEX_CN_WRITE |
             R300_RS_INST_TEX_ADDR(i)));
   }
   /* The baked block fits the R300 US budget. */
   CHECK(plan.us_alu_instructions >= 1 &&
         plan.us_alu_instructions <=
            R300_NOPERSPECTIVE_MIXED_CARRIER_US_ALU_MAX);
   CHECK(plan.us_temporaries >= 1 &&
         plan.us_temporaries <= R300_NOPERSPECTIVE_MIXED_CARRIER_US_TEMP_MAX);

   struct r300_noperspective_mixed_carrier_plan bad;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(NULL) == -EINVAL);
   /* More than four RS vectors including the carrier. */
   bad = plan;
   bad.carrier.payload_vectors = 4;
   bad.carrier.carrier_vector = 4;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   /* Three payloads plus the carrier fit the RS budget but are not
    * the first shape. */
   bad = plan;
   bad.carrier.payload_vectors = 3;
   bad.carrier.carrier_vector = 3;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   /* The carrier aliasing a payload vector. */
   bad = plan;
   bad.carrier.carrier_vector = 1;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   /* A missing TC2: one payload vector, the TC1 shape. */
   bad = plan;
   bad.carrier.payload_vectors = 1;
   bad.carrier.carrier_vector = 1;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   /* The Smooth vector premultiplied, the NoPerspective one left
    * unmultiplied, or both premultiplied. */
   bad = plan;
   bad.noperspective_mask = 0x1u;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   bad.noperspective_mask = 0x0u;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   bad.noperspective_mask = 0x3u;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   /* The US program past the admitted budget. */
   bad = plan;
   bad.us_alu_instructions = R300_NOPERSPECTIVE_MIXED_CARRIER_US_ALU_MAX + 1;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   bad = plan;
   bad.us_temporaries = R300_NOPERSPECTIVE_MIXED_CARRIER_US_TEMP_MAX + 1;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   bad = plan;
   bad.us_alu_instructions = 0;
   CHECK(r300_noperspective_mixed_carrier_plan_validate(&bad) == -EINVAL);
   /* Each refused plan refuses the emitter and the packer too. */
   struct r300_tcl_bypass_triangle_ib cell;
   bad = plan;
   bad.us_alu_instructions = R300_NOPERSPECTIVE_MIXED_CARRIER_US_ALU_MAX + 1;
   CHECK(r300_tcl_bypass_triangle_noperspective_mixed_carrier_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &bad, &cell) == -EINVAL);
   float scratch[3 * REC];
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(&bad, scratch,
                                                         scratch) == -EINVAL);
}

static void
fill_source(float source[3][SRC], const float w[3])
{
   for (unsigned v = 0; v < 3; v++) {
      source[v][0] = 0.5f * (float)v - 0.5f;
      source[v][1] = 0.25f;
      source[v][2] = 0.5f;
      source[v][3] = w[v];
      for (unsigned c = 0; c < 8; c++)
         source[v][4 + c] = 0.0625f * (float)(v * 8 + c + 1);
   }
}

static void
test_pack_identity_and_envelope(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][SRC];
   fill_source(source, w);
   struct r300_noperspective_mixed_carrier_plan plan;
   r300_noperspective_mixed_carrier_plan_first(&plan);
   float packed[3][REC];
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &source[0][0], &packed[0][0]) == 0);
   for (unsigned v = 0; v < 3; v++) {
      const float c = w[v] / 4.0f;
      CHECK(memcmp(packed[v], source[v], 16) == 0);
      /* Vector 0 verbatim, vector 1 premultiplied, the carrier
       * (c, 0, 0, 1). */
      CHECK(memcmp(&packed[v][4], &source[v][4], 16) == 0);
      for (unsigned lane = 0; lane < 4; lane++)
         CHECK(packed[v][8 + lane] == source[v][8 + lane] * c);
      CHECK(packed[v][12] == c && packed[v][13] == 0.0f &&
            packed[v][14] == 0.0f && packed[v][15] == 1.0f);
   }
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &packed[0][0], 1) == 0);
   /* In place over a wider destination: the source triangle sits at
    * the start of the output buffer and is read whole first. */
   float aliased[3 * REC];
   memset(aliased, 0x5a, sizeof(aliased));
   memcpy(aliased, source, sizeof(source));
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(&plan, aliased,
                                                         aliased) == 0);
   CHECK(memcmp(aliased, packed, sizeof(packed)) == 0);

   /* Validation refuses a wrong carrier, a dirty carrier lane, a
    * non-finite Smooth lane, and a premultiplied lane past the
    * envelope; a Smooth lane past the envelope is admitted. */
   float mutated[3][REC];
   memcpy(mutated, packed, sizeof(mutated));
   mutated[0][12] = 0.0f;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == -EDOM);
   memcpy(mutated, packed, sizeof(mutated));
   mutated[2][12] = 1.5f;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == -EDOM);
   memcpy(mutated, packed, sizeof(mutated));
   mutated[1][15] = 0.0f;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == -EDOM);
   memcpy(mutated, packed, sizeof(mutated));
   mutated[1][13] = 0.5f;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == -EDOM);
   memcpy(mutated, packed, sizeof(mutated));
   mutated[1][5] = NAN;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == -EDOM);
   memcpy(mutated, packed, sizeof(mutated));
   mutated[1][9] = (float)R300_NOPERSPECTIVE_CARRIER_LANE_MAX * 4.0f;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == -EDOM);
   memcpy(mutated, packed, sizeof(mutated));
   mutated[1][5] = (float)R300_NOPERSPECTIVE_CARRIER_LANE_MAX * 4.0f;
   CHECK(r300_noperspective_mixed_carrier_validate_stream(
            &plan, &mutated[0][0], 1) == 0);

   /* Refusals leave the output untouched. */
   float bad[3][SRC];
   float untouched[3][REC], sentinel[3][REC];
   memset(untouched, 0xa5, sizeof(untouched));
   memcpy(sentinel, untouched, sizeof(sentinel));
   memcpy(bad, source, sizeof(bad));
   bad[0][3] = 0.0f;
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[0][3] = -1.0f;
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[1][6] = NAN;
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[1][3] = (float)R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX * 2.0f;
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   memcpy(bad, source, sizeof(bad));
   bad[2][8] = (float)R300_NOPERSPECTIVE_CARRIER_LANE_MAX * 4.0f;
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &bad[0][0], &untouched[0][0]) == -EDOM);
   CHECK(memcmp(untouched, sentinel, sizeof(sentinel)) == 0);
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, NULL, &untouched[0][0]) == -EINVAL);
   CHECK(r300_noperspective_mixed_carrier_validate_stream(&plan, NULL, 1) ==
         -EINVAL);
}

/* The recovery model: at each vertex b / c is the NoPerspective value
 * exactly and the FP24 model lands within one UNORM8 quantum; at a
 * window midpoint the ratio of the perspective-weighted sums is the
 * window-linear midpoint while the Smooth vector's weighted sum is the
 * perspective value. */
static void
test_recover_model(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][SRC];
   fill_source(source, w);
   struct r300_noperspective_mixed_carrier_plan plan;
   r300_noperspective_mixed_carrier_plan_first(&plan);
   float packed[3][REC];
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &source[0][0], &packed[0][0]) == 0);
   for (unsigned v = 0; v < 3; v++)
      for (unsigned c = 0; c < 4; c++)
         CHECK(fabsf(r300_noperspective_mixed_carrier_recover(
                        packed[v][8 + c], packed[v][12]) -
                     source[v][8 + c]) <= 1.0f / 255.0f);
   const double W0 = 1.0 / w[0], W1 = 1.0 / w[1];
   const double denominator = 0.5 * W0 + 0.5 * W1;
   const double carrier =
      (0.5 * packed[0][12] * W0 + 0.5 * packed[1][12] * W1) / denominator;
   for (unsigned c = 0; c < 2; c++) {
      const double b = (0.5 * packed[0][8 + c] * W0 +
                        0.5 * packed[1][8 + c] * W1) / denominator;
      const double affine = 0.5 * source[0][8 + c] + 0.5 * source[1][8 + c];
      CHECK(fabs(b / carrier - affine) <= 1e-6);
      const double smooth = (0.5 * packed[0][4 + c] * W0 +
                             0.5 * packed[1][4 + c] * W1) / denominator;
      const double perspective =
         (0.5 * source[0][4 + c] * W0 + 0.5 * source[1][4 + c] * W1) /
         denominator;
      CHECK(fabs(smooth - perspective) <= 1e-9);
      /* The unequal w separates the two models at this midpoint. */
      const double smooth_affine =
         0.5 * source[0][4 + c] + 0.5 * source[1][4 + c];
      CHECK(fabs(smooth - smooth_affine) > 1.0 / 255.0);
   }
}

static void
test_validate_expanded(void)
{
   const float w[3] = { 1.0f, 4.0f, 2.0f };
   float source[3][SRC];
   fill_source(source, w);
   struct r300_noperspective_mixed_carrier_plan plan;
   r300_noperspective_mixed_carrier_plan_first(&plan);
   float fan[6][REC];
   memset(fan, 0, sizeof(fan));
   CHECK(r300_noperspective_mixed_carrier_pack_triangle(
            &plan, &source[0][0], &fan[0][0]) == 0);
   for (unsigned v = 3; v < 6; v++)
      fan[v][3] = 1.0f;
   CHECK(r300_noperspective_mixed_carrier_validate_expanded(
            &plan, &fan[0][0], 6) == 3);
   for (unsigned lane = 0; lane < REC; lane++)
      fan[3][lane] = 0.5f * fan[0][lane] + 0.5f * fan[1][lane];
   CHECK(r300_noperspective_mixed_carrier_validate_expanded(
            &plan, &fan[0][0], 6) == 4);
   fan[3][13] = 0.25f;
   CHECK(r300_noperspective_mixed_carrier_validate_expanded(
            &plan, &fan[0][0], 6) == -EDOM);
   fan[3][13] = 0.0f;
   fan[3][12] = 0.0f;
   CHECK(r300_noperspective_mixed_carrier_validate_expanded(
            &plan, &fan[0][0], 6) == -EDOM);
   CHECK(r300_noperspective_mixed_carrier_validate_expanded(&plan, NULL,
                                                             0) == 0);
   CHECK(r300_noperspective_mixed_carrier_validate_expanded(&plan, NULL,
                                                             1) == -EINVAL);
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

/* The emitted mixed cell: every register word ahead of its draw, each
 * word mutated in place names draw 0 (VTX_SIZE 12, W_SELECT 1, and a
 * wrong RS_IP/RS_INST of every vector included), the US program is the
 * mixed block and no other cell's, and the TC1 carrier and varying
 * cells are different streams the mixed check names. */
static void
test_stream_check_and_cell_bytes(void)
{
   struct r300_noperspective_mixed_carrier_plan plan;
   r300_noperspective_mixed_carrier_plan_first(&plan);
   struct r300_tcl_bypass_triangle_ib cell;
   CHECK(r300_tcl_bypass_triangle_noperspective_mixed_carrier_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &plan, &cell) == 0);
   CHECK(r300_noperspective_mixed_carrier_plan_stream_check(
            &plan, GB_SELECT_BASE, cell.ib, cell.ib_size_dwords) == 1);
   static const uint32_t mutated_regs[] = {
      R300_VAP_VTX_SIZE,         R300_VAP_PROG_STREAM_CNTL_0,
      R300_VAP_PROG_STREAM_CNTL_1, R300_VAP_OUTPUT_VTX_FMT_1,
      R300_VAP_VSM_VTX_ASSM,     R300_RS_COUNT,
      R300_RS_INST_COUNT,        R300_RS_IP_0,
      R300_RS_INST_0,            R300_RS_IP_1,
      R300_RS_INST_1,            R300_RS_IP_2,
      R300_RS_INST_2,            R300_GB_SELECT,
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
         if (mutated_regs[m] == R300_VAP_VTX_SIZE)
            copy[last] = 12u;
         else if (mutated_regs[m] == R300_GB_SELECT)
            copy[last] |= R300_GB_W_SELECT_1;
         else
            copy[last] ^= 0x10u;
         CHECK(r300_noperspective_mixed_carrier_plan_stream_check(
                  &plan, GB_SELECT_BASE, copy, cell.ib_size_dwords) == -1);
      }
      free(copy);
   }
   CHECK(ib_contains_block(
      &cell, r300_noperspective_mixed_carrier_fs_block,
      sizeof(r300_noperspective_mixed_carrier_fs_block) / sizeof(uint32_t)));
   CHECK(!ib_contains_block(
      &cell, r300_r2vb_producer_fs_block,
      sizeof(r300_r2vb_producer_fs_block) / sizeof(uint32_t)));
   CHECK(!ib_contains_block(
      &cell, r300_noperspective_reciprocal_fs_block,
      sizeof(r300_noperspective_reciprocal_fs_block) / sizeof(uint32_t)));
   CHECK(!ib_contains_block(
      &cell, r300_noperspective_q_lane_fs_block,
      sizeof(r300_noperspective_q_lane_fs_block) / sizeof(uint32_t)));

   /* The TC1 carrier cell lacks the third vector; the varying cell
    * lacks two. */
   struct r300_noperspective_reciprocal_plan tc1;
   r300_noperspective_reciprocal_plan_tc1(&tc1);
   struct r300_tcl_bypass_triangle_ib other;
   CHECK(r300_tcl_bypass_triangle_noperspective_carrier_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &tc1, &other) == 0);
   CHECK(r300_noperspective_mixed_carrier_plan_stream_check(
            &plan, GB_SELECT_BASE, other.ib, other.ib_size_dwords) == -1);
   r300_tcl_bypass_triangle_release(&other);
   CHECK(r300_tcl_bypass_triangle_clip_space_family_emit(
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, true,
            1u, &other) == 0);
   CHECK(r300_noperspective_mixed_carrier_plan_stream_check(
            &plan, GB_SELECT_BASE, other.ib, other.ib_size_dwords) == -1);
   r300_tcl_bypass_triangle_release(&other);
   r300_tcl_bypass_triangle_release(&cell);
   CHECK(r300_noperspective_mixed_carrier_plan_stream_check(
            &plan, GB_SELECT_BASE, NULL, 0) == -EINVAL);
}

int
main(void)
{
   test_plan_words_and_refusals();
   test_pack_identity_and_envelope();
   test_recover_model();
   test_validate_expanded();
   test_stream_check_and_cell_bytes();
   if (failures != 0) {
      fprintf(stderr, "r300_noperspective_mixed_carrier_plan_test: %d "
                      "checks failed\n", failures);
      return 1;
   }
   printf("r300_noperspective_mixed_carrier_plan_test: all checks pass\n");
   return 0;
}
