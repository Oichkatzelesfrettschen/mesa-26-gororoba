/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_noperspective_mixed_carrier_plan.h"
#include "r300_noperspective_mixed_carrier_fs_block.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define POSITION_DWORDS R300_NOPERSPECTIVE_CARRIER_POSITION_DWORDS
#define VECTOR_DWORDS R300_NOPERSPECTIVE_CARRIER_VECTOR_DWORDS
#define SOURCE_DWORDS R300_NOPERSPECTIVE_MIXED_CARRIER_SOURCE_DWORDS
#define RECORD_DWORDS R300_NOPERSPECTIVE_MIXED_CARRIER_RECORD_DWORDS
#define CARRIER_LANE (POSITION_DWORDS + \
                      R300_NOPERSPECTIVE_MIXED_CARRIER_VECTOR * VECTOR_DWORDS)

void
r300_noperspective_mixed_carrier_plan_first(
   struct r300_noperspective_mixed_carrier_plan *out)
{
   memset(out, 0, sizeof(*out));
   out->carrier.payload_vectors =
      R300_NOPERSPECTIVE_MIXED_CARRIER_PAYLOAD_VECTORS;
   out->carrier.carrier_vector = R300_NOPERSPECTIVE_MIXED_CARRIER_VECTOR;
   out->noperspective_mask = 0x2u;
   out->us_alu_instructions =
      R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_ALU_INSTRUCTIONS;
   out->us_temporaries = R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_TEMPORARIES;
}

int
r300_noperspective_mixed_carrier_plan_validate(
   const struct r300_noperspective_mixed_carrier_plan *plan)
{
   if (plan == NULL ||
       r300_noperspective_reciprocal_plan_validate(&plan->carrier) != 0 ||
       plan->carrier.payload_vectors !=
          R300_NOPERSPECTIVE_MIXED_CARRIER_PAYLOAD_VECTORS ||
       plan->carrier.carrier_vector !=
          R300_NOPERSPECTIVE_MIXED_CARRIER_VECTOR ||
       plan->noperspective_mask != 0x2u ||
       plan->us_alu_instructions == 0 ||
       plan->us_alu_instructions >
          R300_NOPERSPECTIVE_MIXED_CARRIER_US_ALU_MAX ||
       plan->us_temporaries == 0 ||
       plan->us_temporaries > R300_NOPERSPECTIVE_MIXED_CARRIER_US_TEMP_MAX)
      return -EINVAL;
   return 0;
}

int
r300_noperspective_mixed_carrier_pack_triangle(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   const float *source_records, float *carrier_records)
{
   if (r300_noperspective_mixed_carrier_plan_validate(plan) != 0 ||
       source_records == NULL || carrier_records == NULL)
      return -EINVAL;
   double w[3];
   double w_max = 0.0;
   double w_min = INFINITY;
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      const float *record = &source_records[vertex * SOURCE_DWORDS];
      for (uint32_t lane = 0; lane < SOURCE_DWORDS; lane++)
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
   float packed[3][RECORD_DWORDS];
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      const float *record = &source_records[vertex * SOURCE_DWORDS];
      float *out = packed[vertex];
      const double carrier = scale * w[vertex];
      memcpy(out, record, POSITION_DWORDS * sizeof(float));
      for (uint32_t vector = 0;
           vector < R300_NOPERSPECTIVE_MIXED_CARRIER_PAYLOAD_VECTORS;
           vector++) {
         const uint32_t base = POSITION_DWORDS + vector * VECTOR_DWORDS;
         const bool premultiply =
            (plan->noperspective_mask & (1u << vector)) != 0;
         for (uint32_t lane = 0; lane < VECTOR_DWORDS; lane++) {
            if (!premultiply) {
               out[base + lane] = record[base + lane];
               continue;
            }
            const double premultiplied =
               (double)record[base + lane] * carrier;
            if (fabs(premultiplied) > R300_NOPERSPECTIVE_CARRIER_LANE_MAX)
               return -EDOM;
            out[base + lane] = (float)premultiplied;
         }
      }
      out[CARRIER_LANE] = (float)carrier;
      out[CARRIER_LANE + 1] = 0.0f;
      out[CARRIER_LANE + 2] = 0.0f;
      out[CARRIER_LANE + 3] = 1.0f;
   }
   memcpy(carrier_records, packed, sizeof(packed));
   return 0;
}

static int
validate_record(const struct r300_noperspective_mixed_carrier_plan *plan,
                const float *record)
{
   for (uint32_t vector = 0;
        vector < R300_NOPERSPECTIVE_MIXED_CARRIER_PAYLOAD_VECTORS; vector++) {
      const uint32_t base = POSITION_DWORDS + vector * VECTOR_DWORDS;
      const bool premultiplied =
         (plan->noperspective_mask & (1u << vector)) != 0;
      for (uint32_t lane = 0; lane < VECTOR_DWORDS; lane++) {
         const float value = record[base + lane];
         if (!isfinite(value))
            return -EDOM;
         if (premultiplied &&
             fabs(value) > R300_NOPERSPECTIVE_CARRIER_LANE_MAX)
            return -EDOM;
      }
   }
   const float c = record[CARRIER_LANE];
   if (!(c > 0.0f) || !(c <= 1.0f) || record[CARRIER_LANE + 1] != 0.0f ||
       record[CARRIER_LANE + 2] != 0.0f || record[CARRIER_LANE + 3] != 1.0f)
      return -EDOM;
   return 0;
}

int
r300_noperspective_mixed_carrier_validate_stream(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   const float *carrier_records, uint32_t triangle_count)
{
   if (r300_noperspective_mixed_carrier_plan_validate(plan) != 0 ||
       (carrier_records == NULL && triangle_count != 0))
      return -EINVAL;
   for (uint64_t vertex = 0; vertex < (uint64_t)triangle_count * 3u;
        vertex++) {
      const int rc =
         validate_record(plan, &carrier_records[vertex * RECORD_DWORDS]);
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
   for (uint32_t lane = 0; lane < RECORD_DWORDS; lane++)
      if (lane != 3 && record[lane] != 0.0f)
         return false;
   return true;
}

int
r300_noperspective_mixed_carrier_validate_expanded(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   const float *carrier_records, uint32_t vertex_count)
{
   if (r300_noperspective_mixed_carrier_plan_validate(plan) != 0 ||
       (carrier_records == NULL && vertex_count != 0))
      return -EINVAL;
   int live = 0;
   for (uint32_t vertex = 0; vertex < vertex_count; vertex++) {
      const float *record = &carrier_records[vertex * RECORD_DWORDS];
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

int
r300_noperspective_mixed_carrier_plan_stream_check(
   const struct r300_noperspective_mixed_carrier_plan *plan,
   uint32_t gb_select_base, const uint32_t *ib, uint32_t ib_dwords)
{
   if (r300_noperspective_mixed_carrier_plan_validate(plan) != 0)
      return -EINVAL;
   return r300_noperspective_reciprocal_plan_stream_check(
      &plan->carrier, gb_select_base, ib, ib_dwords);
}

float
r300_noperspective_mixed_carrier_recover(float b, float c)
{
   return r300_noperspective_reciprocal_recover(b, c);
}
