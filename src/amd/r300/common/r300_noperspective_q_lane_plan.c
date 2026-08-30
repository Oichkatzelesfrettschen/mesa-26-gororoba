/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_noperspective_q_lane_plan.h"

#include "r300_reg.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

/* The TCL-bypass output layout places texture coordinate 0 in VAP
 * vector 6 (r300g r300_stream_locations_notcl). */
#define Q_LANE_TEX0_DST_VEC 6u
#define POSITION_DWORDS 4u

void
r300_noperspective_q_lane_plan_init(
   struct r300_noperspective_q_lane_plan *out, uint32_t width)
{
   memset(out, 0, sizeof(*out));
   out->width = width;
}

int
r300_noperspective_q_lane_plan_validate(
   const struct r300_noperspective_q_lane_plan *plan)
{
   if (plan == NULL || plan->width < 1 ||
       plan->width > R300_NOPERSPECTIVE_Q_LANE_WIDTH_MAX)
      return -EINVAL;
   return 0;
}

uint32_t
r300_noperspective_q_lane_plan_prog_stream_cntl_0(void)
{
   return (R300_DATA_TYPE_FLOAT_4 | (0 << R300_DST_VEC_LOC_SHIFT)) |
          ((R300_DATA_TYPE_FLOAT_4 |
            (Q_LANE_TEX0_DST_VEC << R300_DST_VEC_LOC_SHIFT) | R300_LAST_VEC)
           << 16);
}

uint32_t
r300_noperspective_q_lane_plan_vtx_fmt_1(void)
{
   return R300_VAP_OUTPUT_VTX_FMT_1__4_COMPONENTS;
}

uint32_t
r300_noperspective_q_lane_plan_vsm_vtx_assm(void)
{
   return R300_INPUT_CNTL_POS | R300_INPUT_CNTL_TC0;
}

uint32_t
r300_noperspective_q_lane_plan_rs_count(void)
{
   return R300_IT_COUNT(4) | R300_IC_COUNT(0) | R300_HIRES_EN;
}

uint32_t
r300_noperspective_q_lane_plan_rs_inst_count(void)
{
   return 0;
}

uint32_t
r300_noperspective_q_lane_plan_rs_ip_0(void)
{
   return R300_RS_TEX_PTR(0) | R300_RS_SEL_S(R300_RS_SEL_C0) |
          R300_RS_SEL_T(R300_RS_SEL_C1) | R300_RS_SEL_R(R300_RS_SEL_C2) |
          R300_RS_SEL_Q(R300_RS_SEL_C3);
}

uint32_t
r300_noperspective_q_lane_plan_rs_inst_0(void)
{
   return R300_RS_INST_TEX_ID(0) | R300_RS_INST_TEX_CN_WRITE |
          R300_RS_INST_TEX_ADDR(0);
}

int
r300_noperspective_q_lane_pack_triangle(
   const struct r300_noperspective_q_lane_plan *plan,
   const float *source_records, float *q_lane_records)
{
   if (r300_noperspective_q_lane_plan_validate(plan) != 0 ||
       source_records == NULL || q_lane_records == NULL)
      return -EINVAL;
   const uint32_t dwords = R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS;
   double w[3];
   double w_max = 0.0;
   double w_min = INFINITY;
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      const float *record = &source_records[vertex * dwords];
      for (uint32_t lane = 0; lane < POSITION_DWORDS + plan->width; lane++)
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
   float packed[3][R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS];
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      const float *record = &source_records[vertex * dwords];
      float *out = packed[vertex];
      const double carrier = scale * w[vertex];
      memcpy(out, record, POSITION_DWORDS * sizeof(float));
      for (uint32_t lane = 0; lane < R300_NOPERSPECTIVE_Q_LANE_WIDTH_MAX;
           lane++) {
         if (lane >= plan->width) {
            out[POSITION_DWORDS + lane] = 0.0f;
            continue;
         }
         const double premultiplied =
            (double)record[POSITION_DWORDS + lane] * carrier;
         if (fabs(premultiplied) > R300_NOPERSPECTIVE_CARRIER_LANE_MAX)
            return -EDOM;
         out[POSITION_DWORDS + lane] = (float)premultiplied;
      }
      out[R300_NOPERSPECTIVE_Q_LANE_CARRIER_LANE] = (float)carrier;
   }
   memcpy(q_lane_records, packed, sizeof(packed));
   return 0;
}

static int
validate_record(const struct r300_noperspective_q_lane_plan *plan,
                const float *record)
{
   for (uint32_t lane = 0; lane < R300_NOPERSPECTIVE_Q_LANE_WIDTH_MAX;
        lane++) {
      const float value = record[POSITION_DWORDS + lane];
      if (lane >= plan->width) {
         if (value != 0.0f)
            return -EDOM;
      } else if (!isfinite(value) ||
                 fabs(value) > R300_NOPERSPECTIVE_CARRIER_LANE_MAX) {
         return -EDOM;
      }
   }
   const float c = record[R300_NOPERSPECTIVE_Q_LANE_CARRIER_LANE];
   if (!(c > 0.0f) || !(c <= 1.0f))
      return -EDOM;
   return 0;
}

int
r300_noperspective_q_lane_validate_stream(
   const struct r300_noperspective_q_lane_plan *plan,
   const float *q_lane_records, uint32_t triangle_count)
{
   if (r300_noperspective_q_lane_plan_validate(plan) != 0 ||
       (q_lane_records == NULL && triangle_count != 0))
      return -EINVAL;
   for (uint64_t vertex = 0; vertex < (uint64_t)triangle_count * 3u;
        vertex++) {
      const int rc = validate_record(
         plan, &q_lane_records[vertex * R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS]);
      if (rc != 0)
         return rc;
   }
   return 0;
}

static bool
is_padding_record(const float *record)
{
   if (record[3] != 1.0f)
      return false;
   for (uint32_t lane = 0; lane < R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS;
        lane++)
      if (lane != 3 && record[lane] != 0.0f)
         return false;
   return true;
}

int
r300_noperspective_q_lane_validate_expanded(
   const struct r300_noperspective_q_lane_plan *plan,
   const float *q_lane_records, uint32_t vertex_count)
{
   if (r300_noperspective_q_lane_plan_validate(plan) != 0 ||
       (q_lane_records == NULL && vertex_count != 0))
      return -EINVAL;
   int live = 0;
   for (uint32_t vertex = 0; vertex < vertex_count; vertex++) {
      const float *record =
         &q_lane_records[vertex * R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS];
      if (is_padding_record(record))
         continue;
      for (uint32_t lane = 0; lane < POSITION_DWORDS; lane++)
         if (!isfinite(record[lane]))
            return -EDOM;
      const int rc = validate_record(plan, record);
      if (rc != 0)
         return rc;
      live++;
   }
   return live;
}

/* The seven R300 draw opcodes r300_cs_parse dispatches through
 * r300_packet3_check (radeon r300.c). */
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

struct q_lane_register {
   uint32_t reg;
   uint32_t value;
};

#define Q_LANE_REGISTER_COUNT 9u

static void
q_lane_registers(uint32_t gb_select_base,
                 struct q_lane_register out[Q_LANE_REGISTER_COUNT])
{
   const struct q_lane_register regs[Q_LANE_REGISTER_COUNT] = {
      { R300_GB_SELECT, gb_select_base & ~R300_GB_W_SELECT_1 },
      { R300_VAP_VTX_SIZE, R300_NOPERSPECTIVE_Q_LANE_RECORD_DWORDS },
      { R300_VAP_PROG_STREAM_CNTL_0,
        r300_noperspective_q_lane_plan_prog_stream_cntl_0() },
      { R300_VAP_OUTPUT_VTX_FMT_1, r300_noperspective_q_lane_plan_vtx_fmt_1() },
      { R300_VAP_VSM_VTX_ASSM, r300_noperspective_q_lane_plan_vsm_vtx_assm() },
      { R300_RS_COUNT, r300_noperspective_q_lane_plan_rs_count() },
      { R300_RS_INST_COUNT, r300_noperspective_q_lane_plan_rs_inst_count() },
      { R300_RS_IP_0, r300_noperspective_q_lane_plan_rs_ip_0() },
      { R300_RS_INST_0, r300_noperspective_q_lane_plan_rs_inst_0() },
   };
   memcpy(out, regs, sizeof(regs));
}

int
r300_noperspective_q_lane_plan_stream_check(
   const struct r300_noperspective_q_lane_plan *plan,
   uint32_t gb_select_base, const uint32_t *ib, uint32_t ib_dwords)
{
   if (r300_noperspective_q_lane_plan_validate(plan) != 0 || ib == NULL)
      return -EINVAL;
   struct q_lane_register regs[Q_LANE_REGISTER_COUNT];
   q_lane_registers(gb_select_base, regs);
   bool written[Q_LANE_REGISTER_COUNT] = { false };
   uint32_t state[Q_LANE_REGISTER_COUNT] = { 0 };
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
            for (unsigned e = 0; e < Q_LANE_REGISTER_COUNT; e++) {
               if (regs[e].reg != reg)
                  continue;
               /* The first tracked register written after a draw
                * opens the next pass. */
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
            for (unsigned e = 0; e < Q_LANE_REGISTER_COUNT; e++)
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
r300_noperspective_q_lane_recover(float b, float c)
{
   return r300_noperspective_reciprocal_recover(b, c);
}
