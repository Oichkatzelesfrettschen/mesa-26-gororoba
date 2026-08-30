/*
 * SPDX-License-Identifier: MIT
 *
 * Interpolation route selection for the R3V native CPU delivery route.
 */

#include "r3v_interpolation_lowering.h"

#include "r3v_shader_interface.h"

#include "amd/r300/common/r300_noperspective_mixed_carrier_plan.h"
#include "amd/r300/common/r300_noperspective_reciprocal_plan.h"

#include <string.h>

uint32_t
r3v_interpolation_published_record_dwords(enum r3v_interpolation_route route,
                                          uint32_t job_record_dwords)
{
   switch (route) {
   case R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER:
      return R300_NOPERSPECTIVE_CARRIER_RECORD_DWORDS;
   case R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER:
      return R300_NOPERSPECTIVE_MIXED_CARRIER_RECORD_DWORDS;
   default:
      return job_record_dwords;
   }
}

/* The mixed reciprocal carrier route: exactly location 0 Smooth float
 * vec4 and location 1 NoPerspective float vec4, no Flat location, the
 * mixed carrier fragment program, CPU delivery over a triangle list,
 * and the RS destinations the program reads.  The stage packs ahead of
 * the clipper, so the clipping class is not judged.  The three payload
 * and carrier vectors fit the RS budget of four and the baked US block
 * fits the R300 budget, both judged by the plan's validate; every other
 * mixed shape is UNSUPPORTED. */
static enum r3v_interpolation_route
r3v_interpolation_route_select_mixed(
   const struct r3v_interpolation_query *query, const char **reason)
{
   const struct r3v_shader_interface_link *link = query->link;
   const struct r3v_shader_interface_varying *smooth = &link->varyings[0];
   const struct r3v_shader_interface_varying *noperspective =
      &link->varyings[1];
   const char *why = NULL;
   if (!query->cpu_delivery) {
      why = "delivery route is not CPU";
   } else if (!query->triangle_list) {
      why = "primitive is not a triangle list";
   } else if (link->varying_mask != 0x3u || link->flat_mask != 0 ||
              link->noperspective_mask != 0x2u) {
      why = "mixed carrier program outside the Smooth location 0 plus "
            "NoPerspective location 1 interface";
   } else if (!smooth->present ||
              smooth->scalar != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 ||
              smooth->width != 4 || smooth->component_mask != 0xf ||
              smooth->interpolation != R3V_SHADER_INTERFACE_SMOOTH ||
              !noperspective->present ||
              noperspective->scalar != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 ||
              noperspective->width != 4 ||
              noperspective->component_mask != 0xf ||
              noperspective->interpolation !=
                 R3V_SHADER_INTERFACE_NOPERSPECTIVE) {
      why = "mixed carrier locations are not full float vec4s";
   } else if (!query->rs_destination_available) {
      why = "RS destination unavailable";
   } else if (!query->fragment_consumes_destination) {
      why = "fragment program does not consume the RS destination";
   } else {
      struct r300_noperspective_mixed_carrier_plan plan;
      r300_noperspective_mixed_carrier_plan_first(&plan);
      if (r300_noperspective_mixed_carrier_plan_validate(&plan) != 0)
         why = "mixed carrier plan outside the RS vector or US budget";
   }
   if (why != NULL) {
      if (reason != NULL)
         *reason = why;
      return R3V_INTERPOLATION_ROUTE_UNSUPPORTED;
   }
   if (reason != NULL)
      *reason = "mixed reciprocal carrier: TC0 Smooth, TC1 = a * c, "
                "TC2.x = c";
   return R3V_INTERPOLATION_ROUTE_MIXED_RECIPROCAL_CARRIER;
}

/* The direct GB W_SELECT route: one full float vec4 NoPerspective
 * varying at location 0 rides TEX0 under GB_SELECT.W_SELECT = 1 on the
 * CPU delivery route over a triangle list whose clipping class is
 * ACCEPT.  A clipped vertex's NoPerspective value is the
 * screen-space-parameterized combination the Vulkan specification
 * defines under Clipping Shader Outputs, which the clip-space linear
 * clipper produces only when the edge's endpoint W are equal, so the
 * partial class refuses at execution (r3v_native_cell.c).  Every other
 * refused predicate is UNSUPPORTED: replication interpolates the
 * varying with perspective, a wrong value rather than a fallback. */
static enum r3v_interpolation_route
r3v_interpolation_route_select_noperspective(
   const struct r3v_interpolation_query *query, const char **reason)
{
   const char *why = NULL;
   if (!query->cpu_delivery) {
      why = "delivery route is not CPU";
   } else if (!query->triangle_list) {
      why = "primitive is not a triangle list";
   } else if (query->clip_class != R3V_INTERPOLATION_CLIP_ACCEPT &&
              query->narrow_passthrough_width == 0) {
      /* The direct W_SELECT route acts on the emitted fan; the q-lane
       * route packs ahead of the clipper and admits every class. */
      why = "clipping class is not ACCEPT";
   } else if (query->link->varying_mask != 1u ||
              query->link->noperspective_mask != 1u) {
      /* W_SELECT is one word for the whole draw: a Smooth location
       * beside the NoPerspective one would lose its perspective. */
      why = "NoPerspective location does not map completely to TEX0";
   } else {
      const struct r3v_shader_interface_varying *v =
         &query->link->varyings[0];
      const bool float_noperspective =
         v->present && v->scalar == R3V_SHADER_INTERFACE_SCALAR_FLOAT32 &&
         v->interpolation == R3V_SHADER_INTERFACE_NOPERSPECTIVE;
      /* The q-lane conjunction: width 1..3 with the components
       * starting at x and contiguous (the mask is the width's low
       * bits), under the narrow pass-through fragment program of the
       * same width. */
      const bool q_lane_shape =
         float_noperspective && v->width >= 1 && v->width <= 3 &&
         v->component_mask == ((1u << v->width) - 1u) &&
         query->narrow_passthrough_width == v->width;
      if (q_lane_shape) {
         if (!query->rs_destination_available)
            why = "RS destination unavailable";
         else if (!query->fragment_consumes_destination)
            why = "fragment program does not consume the RS destination";
         if (why != NULL) {
            if (reason != NULL)
               *reason = why;
            return R3V_INTERPOLATION_ROUTE_UNSUPPORTED;
         }
         if (reason != NULL)
            *reason = "reciprocal q-lane carrier: TEX0.xyz = a * c, "
                      "TEX0.w = c";
         return R3V_INTERPOLATION_ROUTE_RECIPROCAL_Q_LANE;
      }
      if (!float_noperspective || v->width != 4 || v->component_mask != 0xf) {
         why = v->width < 4
                  ? "NoPerspective location is not a full float vec4 and "
                    "lies outside the q-lane shape"
                  : "NoPerspective location is not a full float vec4";
      } else if (query->narrow_passthrough_width != 0) {
         why = "narrow pass-through fragment program on a vec4 varying";
      } else if (!query->rs_destination_available) {
         why = "RS destination unavailable";
      } else if (!query->fragment_consumes_destination) {
         why = "fragment program does not consume the RS destination";
      }
   }
   if (why != NULL) {
      if (reason != NULL)
         *reason = why;
      return R3V_INTERPOLATION_ROUTE_UNSUPPORTED;
   }
   if (query->carrier_forced) {
      if (reason != NULL)
         *reason = "forced reciprocal carrier: TEX0 = a * w, TEX1.x = w";
      return R3V_INTERPOLATION_ROUTE_RECIPROCAL_CARRIER;
   }
   if (reason != NULL)
      *reason = "direct GB W_SELECT NoPerspective through TEX0";
   return R3V_INTERPOLATION_ROUTE_DIRECT_GB_W_SELECT;
}

enum r3v_interpolation_route
r3v_interpolation_route_select(const struct r3v_interpolation_query *query,
                               const char **reason)
{
   const char *why = NULL;
   if (query == NULL || query->link == NULL) {
      why = "no linked interface";
   } else if (query->mixed_carrier_fragment) {
      /* The mixed carrier binary is the (TC0.xy, TC1.xy * rcp(TC2.x))
       * recovery, which only the mixed interface feeds. */
      return r3v_interpolation_route_select_mixed(query, reason);
   } else if (query->link->noperspective_mask == 0 &&
              query->narrow_passthrough_width != 0) {
      /* The narrow pass-through fragment binary is the q-lane
       * recovery, which only a NoPerspective q-lane varying feeds. */
      if (reason != NULL)
         *reason = "narrow pass-through fragment program outside a "
                   "NoPerspective q-lane varying";
      return R3V_INTERPOLATION_ROUTE_UNSUPPORTED;
   } else if (query->link->noperspective_mask != 0) {
      if (query->link->flat_mask == 0)
         return r3v_interpolation_route_select_noperspective(query, reason);
      /* One draw carries one W_SELECT word and one provoking
       * selection; the Flat route replicates the NoPerspective
       * location with perspective, so the mix refuses. */
      if (reason != NULL)
         *reason = "Flat and NoPerspective locations mixed in one interface";
      return R3V_INTERPOLATION_ROUTE_UNSUPPORTED;
   } else if (query->link->flat_mask == 0) {
      why = "no Flat location";
   } else if (!query->cpu_delivery) {
      why = "delivery route is not CPU";
   } else if (!query->triangle_list) {
      why = "primitive is not a triangle list";
   } else if (query->clip_class != R3V_INTERPOLATION_CLIP_ACCEPT) {
      why = "clipping class is not ACCEPT";
   } else if (query->link->varying_mask != 1u ||
              query->link->flat_mask != 1u) {
      /* The cell carries one varying at location 0, and color 0 is one
       * lane: a second location or a Smooth location beside the Flat
       * one has no lane of its own. */
      why = "Flat location does not map completely to color 0";
   } else {
      const struct r3v_shader_interface_varying *v =
         &query->link->varyings[0];
      if (!v->present || v->scalar != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 ||
          v->width != 4 || v->component_mask != 0xf ||
          v->interpolation != R3V_SHADER_INTERFACE_FLAT) {
         why = "Flat location is not a full float vec4";
      } else if (!query->rs_destination_available) {
         why = "RS destination unavailable";
      } else if (!query->fragment_consumes_destination) {
         why = "fragment program does not consume the RS destination";
      } else if (!query->provoking_first_representable) {
         why = "provoking FIRST not representable";
      }
   }
   if (reason != NULL)
      *reason = why != NULL ? why : "direct GA Flat through color 0";
   return why != NULL ? R3V_INTERPOLATION_ROUTE_REPLICATE
                      : R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0;
}

enum r3v_interpolation_clip_class
r3v_interpolation_clip_class_of_triangle(const uint32_t *records,
                                         uint32_t record_dwords)
{
   if (records == NULL || record_dwords < 4)
      return R3V_INTERPOLATION_CLIP_PARTIAL;
   for (uint32_t vertex = 0; vertex < 3; vertex++) {
      float p[4];
      memcpy(p, &records[vertex * record_dwords], sizeof(p));
      const float w = p[3];
      /* A NaN coordinate fails every comparison below, so it lands in
       * PARTIAL with the vertices outside the volume. */
      if (!(w > 0.0f) || !(p[0] >= -w) || !(p[0] <= w) || !(p[1] >= -w) ||
          !(p[1] <= w) || !(p[2] >= 0.0f) || !(p[2] <= w))
         return R3V_INTERPOLATION_CLIP_PARTIAL;
   }
   return R3V_INTERPOLATION_CLIP_ACCEPT;
}

enum r3v_rs_probe_candidate
r3v_rs_probe_candidate_select(const struct r3v_rs_probe_query *query,
                              const char **reason)
{
   const char *why = NULL;
   enum r3v_rs_probe_candidate candidate = R3V_RS_PROBE_NONE;
   if (query == NULL || query->link == NULL) {
      why = "no linked interface";
   } else if (!query->tex_adj_gate && !query->w_select_gate) {
      why = "no probe gate open";
   } else if (query->tex_adj_gate && query->w_select_gate) {
      why = "both probe gates open";
   } else if (query->link->noperspective_mask == 0) {
      why = "no NoPerspective location";
   } else if (!query->cpu_delivery) {
      why = "delivery route is not CPU";
   } else if (!query->triangle_list) {
      why = "primitive is not a triangle list";
   } else if (query->link->varying_mask != 1u ||
              query->link->noperspective_mask != 1u) {
      why = "NoPerspective location does not map completely to TEX0";
   } else {
      const struct r3v_shader_interface_varying *v =
         &query->link->varyings[0];
      if (!v->present || v->scalar != R3V_SHADER_INTERFACE_SCALAR_FLOAT32 ||
          v->width != 4 || v->component_mask != 0xf ||
          v->interpolation != R3V_SHADER_INTERFACE_NOPERSPECTIVE) {
         why = "NoPerspective location is not a full float vec4";
      } else if (!query->rs_destination_available) {
         why = "RS destination unavailable";
      } else if (!query->fragment_consumes_destination) {
         why = "fragment program does not consume the RS destination";
      } else {
         candidate = query->tex_adj_gate ? R3V_RS_PROBE_TEX_ADJ
                                         : R3V_RS_PROBE_W_SELECT_ONE;
      }
   }
   if (reason != NULL)
      *reason = why;
   return candidate;
}
