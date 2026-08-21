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
 * the other's stream.  Checks are explicit so the test decides the
 * same way under NDEBUG.
 */

#include "amd/r300/common/r300_r2vb_public_route.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <stdio.h>
#include <string.h>

static int
fail(const char *what)
{
   fprintf(stderr, "route-timing-digest: %s\n", what);
   return 1;
}

int
main(void)
{
   struct r300_r2vb_public_route_ib route;
   if (r300_r2vb_public_route_reference_compose(&route) != 0)
      return fail("route composition failed");

   struct r300_tcl_bypass_triangle_ib consumer;
   if (r300_tcl_bypass_triangle_reference_emit(&consumer) != 0)
      return fail("consumer reference emission failed");

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

   r300_r2vb_public_route_release(&route);
   r300_tcl_bypass_triangle_release(&consumer);
   printf("route-timing-digest: consumer slice identity holds; "
          "gpu %.8s.. cpu %.8s..\n",
          gpu_digest, cpu_digest);
   return 0;
}
