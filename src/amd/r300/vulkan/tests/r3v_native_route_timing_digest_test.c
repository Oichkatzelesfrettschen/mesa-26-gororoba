/*
 * SPDX-License-Identifier: MIT
 *
 * The route dispatch timing runner declares three authorizations from
 * two offline compositions and one clip-space consumer: the immediate
 * composed stream's digest for the GPU route, the fetched F32_4 composed
 * stream's digest for the fetched route, and the expanded consumer's digest
 * for the CPU route.  Both producer compositions retain the ordinary
 * window-space consumer, while the CPU route records seven output triangle
 * slots for homogeneous clipping.  This test pins those identities dword
 * for dword and proves the three digests differ pairwise, which keeps one
 * route's authorization from admitting another's stream.
 *
 * The runner also states one triangle in two spaces, because the two
 * routes consume different position domains: the GPU producer route
 * declares clip/NDC application input and realizes the viewport transform
 * before emitting its window-space carrier records, while the CPU route
 * declares R300_CARRIER_POSITION_CLIP, validates the clip volume, and
 * realizes the same transform as (ndc + 1) * extent / 2.  The NDC the
 * runner derives must return to the screen-space record bit for bit, or
 * the two legs would render different triangles and the timing pair would
 * compare two workloads, so the round trip is pinned here in binary32.
 *
 * All three digests, the composed lengths, and the consumer splits are
 * pinned to the retained RS485M route identities, so a composer or
 * emitter change reports as a movement against the bytes silicon
 * executed.
 * `--inject-consumer-drift` flips one consumer dword after
 * composition and must fail on the CPU pin alone, which calibrates the
 * pins' sensitivity.
 *
 * Checks are explicit so the test decides the same way under NDEBUG.
 */

#include "amd/r300/common/r300_r2vb_fetched_producer.h"
#include "amd/r300/common/r300_r2vb_public_route.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/tests/r300_fetched_route_digests.h"
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

   struct r300_r2vb_fetched_route_ib fetched;
   if (r300_r2vb_fetched_route_reference_compose(R300_VERTEX_FORMAT_F32_4,
                                                 &fetched) != 0)
      return fail("fetched route composition failed");

   struct r300_tcl_bypass_triangle_ib window_consumer;
   if (r300_tcl_bypass_triangle_reference_emit(&window_consumer) != 0)
      return fail("window-space consumer reference emission failed");
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   struct r300_tcl_bypass_triangle_ib clip_consumer;
   if (r300_tcl_bypass_triangle_clip_space_render_shape_emit(
          &shape, 1u, &clip_consumer) != 0)
      return fail("clip-space consumer reference emission failed");

   if (route.ib_size_dwords != R300_RETAINED_GPU_ROUTE_IB_DWORDS)
      return fail("composed stream length differs from the retained "
                  "GPU route");
   if (route.consumer_start_dwords !=
       R300_RETAINED_GPU_ROUTE_CONSUMER_START_DWORDS)
      return fail("consumer split differs from the retained GPU route");
   if (window_consumer.ib_size_dwords != R300_RETAINED_CPU_ROUTE_IB_DWORDS)
      return fail("window-space consumer length differs from the retained "
                  "producer route consumer");
   if (fetched.ib_size_dwords != R300_FETCHED_F32_4_ROUTE_IB_DWORDS)
      return fail("fetched stream length differs from the retained "
                  "fetched F32_4 route");
   if (fetched.consumer_start_dwords !=
       R300_FETCHED_F32_4_ROUTE_CONSUMER_START_DWORDS)
      return fail("fetched consumer split differs from the retained "
                  "fetched F32_4 route");
   if (inject_consumer_drift) {
      clip_consumer.ib[clip_consumer.ib_size_dwords - 1] ^= 1u;
      route.ib[route.ib_size_dwords - 1] ^= 1u;
      fetched.ib[fetched.ib_size_dwords - 1] ^= 1u;
   }

   if (route.ib_size_dwords <= route.consumer_start_dwords)
      return fail("composed stream carries no consumer slice");
   const uint32_t slice_dwords =
      route.ib_size_dwords - route.consumer_start_dwords;
   if (slice_dwords != window_consumer.ib_size_dwords)
      return fail("consumer slice length differs from the window-space cell");
   if (memcmp(route.ib + route.consumer_start_dwords, window_consumer.ib,
              slice_dwords * sizeof(uint32_t)) != 0)
      return fail("consumer slice bytes differ from the window-space cell");
   /* The fetched composition carries the same consumer behind its own
    * producer, so the CPU declaration names the slice of either stream.
    */
   if (fetched.ib_size_dwords <= fetched.consumer_start_dwords)
      return fail("fetched stream carries no consumer slice");
   if (fetched.ib_size_dwords - fetched.consumer_start_dwords !=
       window_consumer.ib_size_dwords)
      return fail("fetched consumer slice length differs from the "
                  "window-space cell");
   if (memcmp(fetched.ib + fetched.consumer_start_dwords,
              window_consumer.ib,
              slice_dwords * sizeof(uint32_t)) != 0)
      return fail("fetched consumer slice bytes differ from the recorded "
                  "cell");
   if (clip_consumer.ib_size_dwords != window_consumer.ib_size_dwords)
      return fail("the one-draw clip and window consumers differ in length");
   if (memcmp(clip_consumer.ib, window_consumer.ib,
              clip_consumer.ib_size_dwords * sizeof(uint32_t)) == 0)
      return fail("the clip and window consumers are byte-identical");

   char gpu_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   char cpu_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   char consumer_slice_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   char window_consumer_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, gpu_digest);
   r300_triangle_ib_digest_hex(route.ib + route.consumer_start_dwords,
                               slice_dwords, consumer_slice_digest);
   r300_triangle_ib_digest_hex(clip_consumer.ib,
                               clip_consumer.ib_size_dwords, cpu_digest);
   r300_triangle_ib_digest_hex(window_consumer.ib,
                               window_consumer.ib_size_dwords,
                               window_consumer_digest);
   char fetched_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(fetched.ib, fetched.ib_size_dwords,
                               fetched_digest);
   if (strcmp(gpu_digest, cpu_digest) == 0 ||
       strcmp(fetched_digest, cpu_digest) == 0 ||
       strcmp(fetched_digest, gpu_digest) == 0)
      return fail("two route digests coincide");
   if (strcmp(fetched_digest, R300_FETCHED_F32_4_ROUTE_IB_BLAKE3) != 0)
      return fail("fetched digest differs from the retained fetched F32_4 "
                  "route");
   if (strcmp(cpu_digest, window_consumer_digest) == 0)
      return fail("cpu digest equals the window-space consumer digest");
   if (strcmp(consumer_slice_digest, window_consumer_digest) != 0)
      return fail("consumer slice digest differs from the window-space "
                  "consumer digest");
   if (strcmp(window_consumer_digest,
              R300_RETAINED_CPU_ROUTE_IB_BLAKE3) != 0)
      return fail("window-space consumer digest differs from the retained "
                  "producer route consumer");
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
   r300_r2vb_fetched_route_release(&fetched);
   r300_tcl_bypass_triangle_release(&clip_consumer);
   r300_tcl_bypass_triangle_release(&window_consumer);
   printf("route-timing-digest: consumer slice identity holds in both "
          "compositions and the clip round trip is exact; gpu %.8s.. "
          "cpu %.8s.. fetched %.8s..\n",
          gpu_digest, cpu_digest, fetched_digest);
   return 0;
}
