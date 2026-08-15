/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 *
 * Calibration of the delivery route resolver: the gate opens on the
 * exact value alone, the format set is closed, and every refused input
 * lands on the CPU route with a naming reason.
 */

/* The route and reason assertions are the calibration verdicts, so they stay
 * live in release builds.
 */
#undef NDEBUG

#include "r300_delivery_route.h"
#include "r300_vertex_format.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
   struct r300_delivery_route_decision d;

   /* Gate calibration: only the exact string "1" opens.  Unset, empty,
    * zero, whitespace-adjacent, numeric-adjacent, and truthy-word
    * values all keep the CPU route -- variable presence is not
    * consent.
    */
   static const char *const closed_gates[] = {
      NULL, "", "0", " 1", "1 ", "01", "true", "yes", "2", "11",
   };
   for (unsigned i = 0; i < sizeof(closed_gates) / sizeof(closed_gates[0]);
        i++) {
      r300_delivery_route_resolve(closed_gates[i], NULL,
                                  R300_VERTEX_FORMAT_F32_4, &d);
      assert(d.route == R300_DELIVERY_ROUTE_CPU);
      assert(d.position_space == R300_CARRIER_POSITION_CLIP);
      assert(d.reason != NULL && strstr(d.reason, "exact value") != NULL);
   }

   /* The open gate delivers exactly the three modeled formats. */
   static const int delivered[] = { R300_VERTEX_FORMAT_F32_4,
                                    R300_VERTEX_FORMAT_F32_3,
                                    R300_VERTEX_FORMAT_F32_2 };
   for (unsigned i = 0; i < 3; i++) {
      r300_delivery_route_resolve("1", NULL, delivered[i], &d);
      assert(d.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL);
      /* Every host-side route copies application values, so the
       * decision declares a clip-space carrier and the consumer owns
       * the single viewport transform.
       */
      assert(d.position_space == R300_CARRIER_POSITION_CLIP);
      assert(d.reason != NULL && strstr(d.reason, "opt-in") != NULL);
   }

   /* Formats outside the model keep the CPU route under the open gate,
    * with the format clause as the reason.
    */
   static const int cpu_formats[] = { R300_VERTEX_FORMAT_F32_1,
                                      R300_VERTEX_FORMAT_INVALID, 99, -1 };
   for (unsigned i = 0; i < sizeof(cpu_formats) / sizeof(cpu_formats[0]);
        i++) {
      r300_delivery_route_resolve("1", "1", cpu_formats[i], &d);
      assert(d.route == R300_DELIVERY_ROUTE_CPU);
      assert(d.position_space == R300_CARRIER_POSITION_CLIP);
      assert(d.reason != NULL && strstr(d.reason, "F32_4") != NULL);
   }

   /* GPU-gate calibration: the producer route takes both gates at the
    * exact value and F32_4 alone.  The GPU gate with the base gate
    * closed selects nothing; a closed GPU-gate spelling stays on the
    * host model; F32_3 and F32_2 stay on the host model under both
    * open gates.  The selected clause declares a WINDOW carrier -- the
    * device pass transforms, so the consumer binds untransformed.
    */
   r300_delivery_route_resolve("1", "1", R300_VERTEX_FORMAT_F32_4, &d);
   assert(d.route == R300_DELIVERY_ROUTE_R2VB_GPU_PRODUCER);
   assert(d.position_space == R300_CARRIER_POSITION_WINDOW);
   assert(d.reason != NULL && strstr(d.reason, "GPU producer") != NULL);

   for (unsigned i = 0; i < sizeof(closed_gates) / sizeof(closed_gates[0]);
        i++) {
      r300_delivery_route_resolve("1", closed_gates[i],
                                  R300_VERTEX_FORMAT_F32_4, &d);
      assert(d.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL);
      assert(d.position_space == R300_CARRIER_POSITION_CLIP);

      r300_delivery_route_resolve(closed_gates[i], "1",
                                  R300_VERTEX_FORMAT_F32_4, &d);
      assert(d.route == R300_DELIVERY_ROUTE_CPU);
   }

   r300_delivery_route_resolve("1", "1", R300_VERTEX_FORMAT_F32_3, &d);
   assert(d.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL);
   r300_delivery_route_resolve("1", "1", R300_VERTEX_FORMAT_F32_2, &d);
   assert(d.route == R300_DELIVERY_ROUTE_R2VB_HOST_MODEL);

   printf("r300_delivery_route_test: all checks passed\n");
   return 0;
}
