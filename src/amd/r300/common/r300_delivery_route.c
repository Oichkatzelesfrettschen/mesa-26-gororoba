/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 */

#include "r300_delivery_route.h"
#include "r300_vertex_format.h"

#include <string.h>

void
r300_delivery_route_resolve(const char *gate_value, int format_id,
                            struct r300_delivery_route_decision *out)
{
   if (gate_value == NULL || strcmp(gate_value, "1") != 0) {
      out->route = R300_DELIVERY_ROUTE_CPU;
      out->reason = "CPU gather default; the R2VB gate takes the exact "
                    "value 1";
      return;
   }
   if (format_id != R300_VERTEX_FORMAT_F32_4 &&
       format_id != R300_VERTEX_FORMAT_F32_3 &&
       format_id != R300_VERTEX_FORMAT_F32_2) {
      out->route = R300_DELIVERY_ROUTE_CPU;
      out->reason = "CPU gather; the R2VB host model delivers F32_4, "
                    "F32_3, and F32_2 alone";
      return;
   }
   out->route = R300_DELIVERY_ROUTE_R2VB_HOST_MODEL;
   out->reason = "R2VB host model by exact experimental opt-in";
}
