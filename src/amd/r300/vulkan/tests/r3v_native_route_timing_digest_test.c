/*
 * SPDX-License-Identifier: MIT
 *
 * The route dispatch timing runner declares two authorizations from
 * one offline composition: the composed stream's digest for the GPU
 * route and the consumer slice's digest for the CPU route.  The CPU
 * declaration holds only while the consumer slice of the composed
 * reference is the recorded triangle consumer verbatim, so this test
 * pins that identity dword for dword and proves the two digests
 * differ, which is what keeps one route's authorization from admitting
 * the other's stream.
 *
 * The runner also states one triangle in two spaces, because the two
 * routes admit different ones: the GPU producer route declares
 * R300_CARRIER_POSITION_WINDOW and takes the screen-space records
 * directly, while the CPU route declares R300_CARRIER_POSITION_CLIP,
 * validates the clip volume, and realizes the viewport transform as
 * (ndc + 1) * extent / 2.  The NDC the runner derives must return to
 * the screen-space record bit for bit, or the two legs would render
 * different triangles and the timing pair would compare two workloads,
 * so the round trip is pinned here in binary32.
 *
 * Both digests, the composed length, and the consumer split are pinned
 * to the retained RS482 route identities, so a composer or emitter
 * change reports as a movement against the bytes silicon executed.
 * `--inject-consumer-drift` flips one consumer dword after
 * composition and must fail on the CPU pin alone, which calibrates the
 * pins' sensitivity.
 *
 * Checks are explicit so the test decides the same way under NDEBUG.
 */

#include "amd/r300/common/r300_r2vb_public_route.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/tests/r300_retained_route_digests.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int
fail(const char *what)
{
   fprintf(stderr, "route-timing-digest: %s\n", what);
   return 1;
}

int
main(int argc, char **argv)
{
   const bool inject_consumer_drift =
      argc == 2 && strcmp(argv[1], "--inject-consumer-drift") == 0;
   if (argc > 1 && !inject_consumer_drift)
      return fail("usage: [--inject-consumer-drift]");

   struct r300_r2vb_public_route_ib route;
   if (r300_r2vb_public_route_reference_compose(&route) != 0)
      return fail("route composition failed");

   struct r300_tcl_bypass_triangle_ib consumer;
   if (r300_tcl_bypass_triangle_reference_emit(&consumer) != 0)
      return fail("consumer reference emission failed");

   if (route.ib_size_dwords != R300_RETAINED_GPU_ROUTE_IB_DWORDS)
      return fail("composed stream length differs from the retained "
                  "GPU route");
   if (route.consumer_start_dwords !=
       R300_RETAINED_GPU_ROUTE_CONSUMER_START_DWORDS)
      return fail("consumer split differs from the retained GPU route");
   if (consumer.ib_size_dwords != R300_RETAINED_CPU_ROUTE_IB_DWORDS)
      return fail("consumer cell length differs from the retained "
                  "CPU route");
   if (inject_consumer_drift) {
      consumer.ib[consumer.ib_size_dwords - 1] ^= 1u;
      route.ib[route.ib_size_dwords - 1] ^= 1u;
   }

   if (route.ib_size_dwords <= route.consumer_start_dwords)
      return fail("composed stream carries no consumer slice");
   const uint32_t slice_dwords =
      route.ib_size_dwords - route.consumer_start_dwords;
   if (slice_dwords != consumer.ib_size_dwords)
      return fail("consumer slice length differs from the recorded cell");
   if (memcmp(route.ib + route.consumer_start_dwords, consumer.ib,
              slice_dwords * sizeof(uint32_t)) != 0)
      return fail("consumer slice bytes differ from the recorded cell");

   char gpu_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   char cpu_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   char consumer_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, gpu_digest);
   r300_triangle_ib_digest_hex(route.ib + route.consumer_start_dwords,
                               slice_dwords, cpu_digest);
   r300_triangle_ib_digest_hex(consumer.ib, consumer.ib_size_dwords,
                               consumer_digest);
   if (strcmp(gpu_digest, cpu_digest) == 0)
      return fail("the two route digests coincide");
   if (strcmp(cpu_digest, consumer_digest) != 0)
      return fail("cpu digest differs from the recorded consumer digest");
   if (strcmp(cpu_digest, R300_RETAINED_CPU_ROUTE_IB_BLAKE3) != 0)
      return fail("cpu digest differs from the retained CPU route");
   if (strcmp(gpu_digest, R300_RETAINED_GPU_ROUTE_IB_BLAKE3) != 0)
      return fail("gpu digest differs from the retained GPU route");

   /* The CPU leg's derivation and the driver's transform, in the order
    * the two run, over the exact extent the cell declares.
    */
   for (unsigned v = 0; v < R300_TRIANGLE_VERTEX_DWORDS / 4; v++) {
      const float *window = &r300_tcl_bypass_triangle_vertices[v * 4];
      const float ndc_x =
         window[0] * 2.0f / (float)R300_TRIANGLE_TARGET_WIDTH - 1.0f;
      const float ndc_y =
         window[1] * 2.0f / (float)R300_TRIANGLE_TARGET_HEIGHT - 1.0f;
      if (!(ndc_x >= -1.0f && ndc_x <= 1.0f) ||
          !(ndc_y >= -1.0f && ndc_y <= 1.0f))
         return fail("a derived record lies outside the clip volume");
      if (!(window[2] >= 0.0f && window[2] <= 1.0f) || window[3] != 1.0f)
         return fail("a record's depth or w leaves the admitted domain");
      const float back_x =
         (ndc_x + 1.0f) * ((float)R300_TRIANGLE_TARGET_WIDTH / 2.0f);
      const float back_y =
         (ndc_y + 1.0f) * ((float)R300_TRIANGLE_TARGET_HEIGHT / 2.0f);
      if (back_x != window[0] || back_y != window[1])
         return fail("the clip-space round trip does not return the "
                     "screen-space record");
   }

   r300_r2vb_public_route_release(&route);
   r300_tcl_bypass_triangle_release(&consumer);
   printf("route-timing-digest: consumer slice identity holds and the "
          "clip round trip is exact; gpu %.8s.. cpu %.8s..\n",
          gpu_digest, cpu_digest);
   return 0;
}
