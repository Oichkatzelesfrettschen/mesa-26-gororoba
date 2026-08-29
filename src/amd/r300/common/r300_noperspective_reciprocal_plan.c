/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_noperspective_reciprocal_plan.h"

#include "r300_reg.h"
#include "r300_us_source_read.h"

#include <stdbool.h>

#include <errno.h>
#include <math.h>
#include <string.h>

/* The TCL-bypass output layout places texture coordinate vectors from
 * VAP vector 6 (r300g r300_stream_locations_notcl). */
#define R300_NOPERSPECTIVE_TEX_DST_VEC_BASE 6u

void
r300_noperspective_reciprocal_plan_tc1(
   struct r300_noperspective_reciprocal_plan *out)
{
   memset(out, 0, sizeof(*out));
   out->payload_vectors = 1;
   out->carrier_vector = 1;
}

int
r300_noperspective_reciprocal_plan_validate(
   const struct r300_noperspective_reciprocal_plan *plan)
{
   if (plan == NULL || plan->payload_vectors < 1 ||
       plan->payload_vectors + 1 >
          R300_NOPERSPECTIVE_CARRIER_RS_VECTOR_BUDGET ||
       plan->carrier_vector != plan->payload_vectors)
      return -EINVAL;
   return 0;
}

static uint32_t
vector_count(const struct r300_noperspective_reciprocal_plan *plan)
{
   return plan->payload_vectors + 1;
}

uint32_t
r300_noperspective_reciprocal_plan_record_dwords(
   const struct r300_noperspective_reciprocal_plan *plan)
{
   return R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS +
          vector_count(plan) * R300_NOPERSPECTIVE_CARRIER_VECTOR_DWORDS;
}

uint32_t
r300_noperspective_reciprocal_plan_prog_stream_cntl(
   const struct r300_noperspective_reciprocal_plan *plan, uint32_t index)
{
   /* Element e lands in vector 0 (position) or 6 + (e - 1); each
    * PROG_STREAM_CNTL word holds two elements in its halves. */
   const uint32_t element_count = 1 + vector_count(plan);
   uint32_t word = 0;
   for (uint32_t half = 0; half < 2; half++) {
      const uint32_t element = index * 2 + half;
      if (element >= element_count)
         break;
      const uint32_t dst_vec =
         element == 0 ? 0
                      : R300_NOPERSPECTIVE_TEX_DST_VEC_BASE + (element - 1);
      uint32_t field = R300_DATA_TYPE_FLOAT_4 |
                       (dst_vec << R300_DST_VEC_LOC_SHIFT);
      if (element == element_count - 1)
         field |= R300_LAST_VEC;
      word |= field << (16 * half);
   }
   return word;
}

uint32_t
r300_noperspective_reciprocal_plan_vtx_fmt_1(
   const struct r300_noperspective_reciprocal_plan *plan)
{
   uint32_t word = 0;
   for (uint32_t vector = 0; vector < vector_count(plan); vector++)
      word |= R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS << (3 * vector);
   return word;
}

uint32_t
r300_noperspective_reciprocal_plan_vsm_vtx_assm(
   const struct r300_noperspective_reciprocal_plan *plan)
{
   uint32_t word = R300_INPUT_CNTL_POS;
   for (uint32_t vector = 0; vector < vector_count(plan); vector++)
      word |= R300_INPUT_CNTL_TC0 << vector;
   return word;
}

uint32_t
r300_noperspective_reciprocal_plan_rs_count(
   const struct r300_noperspective_reciprocal_plan *plan)
{
   return R300_IT_COUNT(4 * vector_count(plan)) | R300_IC_COUNT(0) |
          R300_HIRES_EN;
}

uint32_t
r300_noperspective_reciprocal_plan_rs_inst_count(
   const struct r300_noperspective_reciprocal_plan *plan)
{
   return vector_count(plan) - 1;
}

uint32_t
r300_noperspective_reciprocal_plan_rs_ip(
   const struct r300_noperspective_reciprocal_plan *plan, uint32_t index)
{
   (void)plan;
   return R300_RS_TEX_PTR(4 * index) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3);
}

uint32_t
r300_noperspective_reciprocal_plan_rs_inst(
   const struct r300_noperspective_reciprocal_plan *plan, uint32_t index)
{
   (void)plan;
   return R300_RS_INST_TEX_ID(index) | R300_RS_INST_TEX_CN_WRITE |
          R300_RS_INST_TEX_ADDR(index);
}

int
r300_noperspective_reciprocal_pack_triangle(
   const struct r300_noperspective_reciprocal_plan *plan,
   const float *source_records, float *carrier_records)
{
   if (r300_noperspective_reciprocal_plan_validate(plan) != 0 ||
       source_records == NULL || carrier_records == NULL)
      return -EINVAL;
   const uint32_t source_dwords =
      R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS +
      plan->payload_vectors * R300_NOPERSPECTIVE_CARRIER_VECTOR_DWORDS;
   const uint32_t carrier_dwords =
      r300_noperspective_reciprocal_plan_record_dwords(plan);
   double w[3];
   double w_max = 0.0;
   double w_min = INFINITY;
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      const float *record = &source_records[vertex * source_dwords];
      for (uint32_t lane = 0; lane < source_dwords; lane++)
         if (!isfinite(record[lane]))
            return -EDOM;
      w[vertex] = record[3];
      if (!(w[vertex] > 0.0))
         return -EDOM;
      w_max = fmax(w_max, w[vertex]);
      w_min = fmin(w_min, w[vertex]);
   }
   if (w_max / w_min > R300_NOPERSPECTIVE_CARRIER_W_RATIO_MAX)
      return -EDOM;
   const double scale = 1.0 / w_max;
   float packed[3][R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS];
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      const float *record = &source_records[vertex * source_dwords];
      float *out = packed[vertex];
      const double carrier = scale * w[vertex];
      memcpy(out, record,
             R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS * sizeof(float));
      for (uint32_t lane = R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS;
           lane < source_dwords; lane++) {
         const double premultiplied = (double)record[lane] * carrier;
         if (fabs(premultiplied) > R300_NOPERSPECTIVE_CARRIER_LANE_MAX)
            return -EDOM;
         out[lane] = (float)premultiplied;
      }
      float *carrier_lanes = &out[source_dwords];
      carrier_lanes[0] = (float)carrier;
      carrier_lanes[1] = 0.0f;
      carrier_lanes[2] = 0.0f;
      carrier_lanes[3] = 1.0f;
   }
   for (uint32_t vertex = 0; vertex < 3; vertex++)
      memcpy(&carrier_records[vertex * carrier_dwords], packed[vertex],
             carrier_dwords * sizeof(float));
   return 0;
}

int
r300_noperspective_reciprocal_validate_stream(
   const struct r300_noperspective_reciprocal_plan *plan,
   const float *carrier_records, uint32_t triangle_count)
{
   if (r300_noperspective_reciprocal_plan_validate(plan) != 0 ||
       (carrier_records == NULL && triangle_count != 0))
      return -EINVAL;
   const uint32_t carrier_dwords =
      r300_noperspective_reciprocal_plan_record_dwords(plan);
   const uint32_t carrier_lane =
      R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS +
      plan->carrier_vector * R300_NOPERSPECTIVE_CARRIER_VECTOR_DWORDS;
   for (uint64_t vertex = 0; vertex < (uint64_t)triangle_count * 3u;
        vertex++) {
      const float *record = &carrier_records[vertex * carrier_dwords];
      for (uint32_t lane = R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS;
           lane < carrier_lane; lane++)
         if (!isfinite(record[lane]) ||
             fabs(record[lane]) > R300_NOPERSPECTIVE_CARRIER_LANE_MAX)
            return -EDOM;
      const float c = record[carrier_lane];
      if (!(c > 0.0f) || !(c <= 1.0f) || record[carrier_lane + 1] != 0.0f ||
          record[carrier_lane + 2] != 0.0f || record[carrier_lane + 3] != 1.0f)
         return -EDOM;
   }
   return 0;
}

/* The seven R300 draw opcodes r300_cs_parse dispatches through
 * r300_packet3_check (radeon r300.c); the first-draw contract walker
 * names the same set. */
static bool
is_draw_packet(uint32_t header)
{
   if ((header >> 30) != 3)
      return false;
   switch (header & 0xff00) {
   case R300_PACKET3_3D_DRAW_VBUF:
   case R300_PACKET3_3D_DRAW_IMMD:
   case R300_PACKET3_3D_DRAW_INDX:
   case R300_PACKET3_3D_DRAW_VBUF_2:
   case R300_PACKET3_3D_DRAW_IMMD_2:
   case R300_PACKET3_3D_DRAW_INDX_2:
   case R300_PACKET3_3D_DRAW_128:
      return true;
   default:
      return false;
   }
}

struct carrier_register {
   uint32_t reg;
   uint32_t value;
};

/* GB_SELECT, VTX_SIZE, two PSC words, VTX_FMT_1, VSM_VTX_ASSM, RS_COUNT,
 * RS_INST_COUNT, then RS_IP and RS_INST per vector of the budget. */
#define CARRIER_REGISTER_MAX \
   (8u + 2u * R300_NOPERSPECTIVE_CARRIER_RS_VECTOR_BUDGET)

static unsigned
carrier_registers(const struct r300_noperspective_reciprocal_plan *plan,
                  uint32_t gb_select_base,
                  struct carrier_register out[CARRIER_REGISTER_MAX])
{
   unsigned n = 0;
   out[n].reg = R300_GB_SELECT;
   out[n++].value = gb_select_base & ~R300_GB_W_SELECT_1;
   out[n].reg = R300_VAP_VTX_SIZE;
   out[n++].value = r300_noperspective_reciprocal_plan_record_dwords(plan);
   out[n].reg = R300_VAP_PROG_STREAM_CNTL_0;
   out[n++].value =
      r300_noperspective_reciprocal_plan_prog_stream_cntl(plan, 0);
   out[n].reg = R300_VAP_PROG_STREAM_CNTL_1;
   out[n++].value =
      r300_noperspective_reciprocal_plan_prog_stream_cntl(plan, 1);
   out[n].reg = R300_VAP_OUTPUT_VTX_FMT_1;
   out[n++].value = r300_noperspective_reciprocal_plan_vtx_fmt_1(plan);
   out[n].reg = R300_VAP_VSM_VTX_ASSM;
   out[n++].value = r300_noperspective_reciprocal_plan_vsm_vtx_assm(plan);
   out[n].reg = R300_RS_COUNT;
   out[n++].value = r300_noperspective_reciprocal_plan_rs_count(plan);
   out[n].reg = R300_RS_INST_COUNT;
   out[n++].value = r300_noperspective_reciprocal_plan_rs_inst_count(plan);
   for (uint32_t i = 0; i < vector_count(plan); i++) {
      out[n].reg = R300_RS_IP_0 + 4 * i;
      out[n++].value = r300_noperspective_reciprocal_plan_rs_ip(plan, i);
      out[n].reg = R300_RS_INST_0 + 4 * i;
      out[n++].value = r300_noperspective_reciprocal_plan_rs_inst(plan, i);
   }
   return n;
}

int
r300_noperspective_reciprocal_plan_stream_check(
   const struct r300_noperspective_reciprocal_plan *plan,
   uint32_t gb_select_base, const uint32_t *ib, uint32_t ib_dwords)
{
   if (r300_noperspective_reciprocal_plan_validate(plan) != 0 || ib == NULL)
      return -EINVAL;
   struct carrier_register regs[CARRIER_REGISTER_MAX];
   const unsigned n = carrier_registers(plan, gb_select_base, regs);
   bool written[CARRIER_REGISTER_MAX] = { false };
   uint32_t state[CARRIER_REGISTER_MAX] = { 0 };
   int draws = 0;
   bool pass_consumed = false;
   uint32_t i = 0;
   while (i < ib_dwords) {
      const uint32_t header = ib[i];
      const uint32_t kind = header >> 30;
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (kind == 0) {
         if (i + count >= ib_dwords)
            return -EINVAL;
         const uint32_t base = (header & 0x3FFF) * 4;
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = base + (one_reg ? 0 : 4 * k);
            for (unsigned e = 0; e < n; e++) {
               if (regs[e].reg != reg)
                  continue;
               /* The first carrier register written after a draw opens
                * the next pass; the previous pass's words count for
                * nothing from here. */
               if (pass_consumed) {
                  memset(written, 0, sizeof(written));
                  pass_consumed = false;
               }
               state[e] = ib[i + 1 + k];
               written[e] = true;
            }
         }
         i += 1 + count;
      } else if (kind == 3) {
         if (i + count >= ib_dwords)
            return -EINVAL;
         if (is_draw_packet(header)) {
            for (unsigned e = 0; e < n; e++)
               if (!written[e] || state[e] != regs[e].value)
                  return -(1 + draws);
            draws++;
            pass_consumed = true;
         }
         i += 1 + count;
      } else {
         i += 1;
      }
   }
   return draws;
}

float
r300_noperspective_reciprocal_recover(float b, float c)
{
   const float reciprocal = r300_fp24_store_quantize_f32(
      1.0f / r300_fp24_store_quantize_f32(c));
   return r300_fp24_store_quantize_f32(r300_fp24_store_quantize_f32(b) *
                                       reciprocal);
}
