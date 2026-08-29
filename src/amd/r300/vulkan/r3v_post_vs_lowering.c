/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_post_vs_lowering.h"

#include "amd/r300/common/r300_vertex_job.h"

#include <errno.h>
#include <string.h>

void r3v_post_vs_lowering_from_interface(
   const struct r3v_shader_interface_link *link,
   struct r3v_post_vs_lowering *out)
{
   memset(out, 0, sizeof(*out));
   out->flat_mask = link->flat_mask;
   out->provoking_vertex = R3V_POST_VS_PROVOKING_VERTEX_FIRST;
   out->noperspective_mask = link->noperspective_mask;
}

int
r3v_post_vs_pack_noperspective_carrier(
   const struct r3v_post_vs_lowering *lowering, const uint32_t *records,
   uint32_t triangle_count, uint32_t record_dwords, uint32_t *carrier)
{
   if (lowering == NULL || !lowering->reciprocal_carrier ||
       lowering->noperspective_mask != 1u ||
       record_dwords != R300_VERTEX_JOB_POSITION_DWORDS +
                           R300_VERTEX_JOB_VARYING_DWORDS ||
       (triangle_count != 0 && (records == NULL || carrier == NULL)))
      return -EINVAL;
   struct r300_noperspective_reciprocal_plan plan;
   r300_noperspective_reciprocal_plan_tc1(&plan);
   const uint32_t carrier_dwords =
      r300_noperspective_reciprocal_plan_record_dwords(&plan);
   /* Every triangle's envelope is judged before the first write, so a
    * refusal leaves the source list intact. */
   for (uint32_t triangle = 0; triangle < triangle_count; triangle++) {
      float probe[3 * R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS];
      const int rc = r300_noperspective_reciprocal_pack_triangle(
         &plan,
         (const float *)&records[(size_t)triangle * 3u * record_dwords],
         probe);
      if (rc != 0)
         return rc;
   }
   for (uint32_t triangle = triangle_count; triangle-- > 0;) {
      float packed[3 * R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS];
      const int rc = r300_noperspective_reciprocal_pack_triangle(
         &plan,
         (const float *)&records[(size_t)triangle * 3u * record_dwords],
         packed);
      if (rc != 0)
         return rc;
      memcpy(&carrier[(size_t)triangle * 3u * carrier_dwords], packed,
             sizeof(packed));
   }
   return 0;
}

int r3v_post_vs_lower_triangles(const struct r3v_post_vs_lowering *lowering,
                                uint32_t *records, uint32_t triangle_count,
                                uint32_t record_dwords)
{
   if (lowering == NULL || record_dwords < R300_VERTEX_JOB_POSITION_DWORDS ||
       record_dwords % R300_VERTEX_JOB_VARYING_DWORDS != 0 ||
       lowering->provoking_vertex > 2)
      return -EINVAL;
   const uint32_t varying_count =
      (record_dwords - R300_VERTEX_JOB_POSITION_DWORDS) /
      R300_VERTEX_JOB_VARYING_DWORDS;
   if (varying_count < 32 && (lowering->flat_mask >> varying_count) != 0)
      return -EINVAL;
   if (lowering->flat_mask == 0 || triangle_count == 0)
      return 0;
   if (records == NULL)
      return -EINVAL;

   const uint32_t provoking = lowering->provoking_vertex;
   for (uint32_t triangle = 0; triangle < triangle_count; triangle++) {
      uint32_t *vertex0 = &records[(uint64_t)triangle * 3u * record_dwords];
      const uint32_t *source = &vertex0[provoking * record_dwords];
      for (uint32_t location = 0; location < varying_count; location++) {
         if ((lowering->flat_mask & (1u << location)) == 0)
            continue;
         const uint32_t offset = R300_VERTEX_JOB_POSITION_DWORDS +
                                 location * R300_VERTEX_JOB_VARYING_DWORDS;
         for (uint32_t vertex = 0; vertex < 3; vertex++) {
            if (vertex == provoking)
               continue;
            memcpy(&vertex0[vertex * record_dwords + offset],
                   &source[offset],
                   R300_VERTEX_JOB_VARYING_DWORDS * sizeof(uint32_t));
         }
      }
   }
   return 0;
}
