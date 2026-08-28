/*
 * SPDX-License-Identifier: MIT
 *
 * Interpolation route selection for the R3V native CPU delivery route.
 */

#include "r3v_interpolation_lowering.h"

#include "r3v_shader_interface.h"

#include <string.h>

enum r3v_interpolation_route
r3v_interpolation_route_select(const struct r3v_interpolation_query *query,
                               const char **reason)
{
   const char *why = NULL;
   if (query == NULL || query->link == NULL) {
      why = "no linked interface";
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
