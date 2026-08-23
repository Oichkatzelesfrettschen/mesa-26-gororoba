/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_delivery_route.h"
#include "r300_vertex_format.h"

#include <string.h>

void
r300_delivery_route_resolve(const char *gate_value,
                            const char *gpu_gate_value,
                            const char *fetched_gate_value, int format_id,
                            struct r300_delivery_route_decision *out)
{
   /* The host-side routes copy application values into the carrier, so
    * the decision declares clip space here; the GPU producer clause
    * declares WINDOW because the device pass rasterizes transformed
    * slot positions and the consumer binds the carrier untransformed.
    */
   out->position_space = R300_CARRIER_POSITION_CLIP;
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
   if (gpu_gate_value != NULL && strcmp(gpu_gate_value, "1") == 0) {
      /* The fetched producer reads its records through the VAP, so one
       * emitter covers the three admitted widths and the route opens
       * per width under the third gate; the immediate producer embeds
       * gathered F32_4 dwords and stays F32_4 alone.
       */
      if (fetched_gate_value != NULL && strcmp(fetched_gate_value, "1") == 0) {
         out->route = R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER_FETCHED;
         out->position_space = R300_CARRIER_POSITION_WINDOW;
         out->reason =
            format_id == R300_VERTEX_FORMAT_F32_4
               ? "R2VB fetched GPU producer by exact experimental "
                 "opt-in; F32_4 source fetched from the bound vertex BO"
            : format_id == R300_VERTEX_FORMAT_F32_3
               ? "R2VB fetched GPU producer by exact experimental "
                 "opt-in; F32_3 source fetched from the bound vertex BO, "
                 "w filled by the fetch swizzle"
               : "R2VB fetched GPU producer by exact experimental "
                 "opt-in; F32_2 source fetched from the bound vertex BO, "
                 "z and w filled by the fetch swizzle";
         return;
      }
      if (format_id == R300_VERTEX_FORMAT_F32_4) {
         out->route = R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER;
         out->position_space = R300_CARRIER_POSITION_WINDOW;
         out->reason = "R2VB GPU producer by exact experimental opt-in; "
                       "F32_4 identity delivery holds on RS482 silicon";
         return;
      }
   }
   out->route = R300_DELIVERY_ROUTE_R2VB_HOST_MODEL;
   out->reason = "R2VB host model by exact experimental opt-in";
}
