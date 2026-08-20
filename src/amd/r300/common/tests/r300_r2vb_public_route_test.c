/*
 * SPDX-License-Identifier: MIT
 *
 * Composition contract for the public GPU-producer route: the composed
 * stream is the producer emission over the records followed by the
 * consumer cell at the extent, its relocation sites index the halves
 * they came from, and every refusal the two emitters own reaches the
 * caller before an allocation exists.
 */

#include "r300_r2vb_public_route.h"

#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Explicit check: the release profile compiles with NDEBUG, and a
 * verdict this test reports rests on the comparison actually running.
 */
static void
check(bool condition, const char *message)
{
   if (!condition) {
      fprintf(stderr, "r300_r2vb_public_route_test: %s\n", message);
      exit(1);
   }
}

static void
test_reference_composition(void)
{
   struct r300_r2vb_producer_ib producer;
   check(r300_r2vb_producer_reference_emit(&producer) == 0,
         "reference producer emission failed");
   struct r300_tcl_bypass_triangle_ib consumer;
   check(r300_tcl_bypass_triangle_extent_emit(R300_TRIANGLE_TARGET_WIDTH,
                                              R300_TRIANGLE_TARGET_HEIGHT,
                                              &consumer) == 0,
         "consumer cell emission failed");

   struct r300_r2vb_public_route_ib route;
   check(r300_r2vb_public_route_reference_compose(&route) == 0,
         "reference composition failed");

   check(route.consumer_start_dwords == producer.ib_size_dwords,
         "the split does not land at the producer length");
   check(route.ib_size_dwords ==
            producer.ib_size_dwords + consumer.ib_size_dwords,
         "the composed length is not the sum of the halves");
   check(memcmp(route.ib, producer.ib,
                producer.ib_size_dwords * sizeof(uint32_t)) == 0,
         "the prefix is not the producer emission");
   check(memcmp(route.ib + route.consumer_start_dwords, consumer.ib,
                consumer.ib_size_dwords * sizeof(uint32_t)) == 0,
         "the suffix is not the consumer emission");

   check(r300_r2vb_public_route_validate_reloc_sites(&route) == 0,
         "the composed relocation sites do not validate");
   check(route.reloc_site_count == 3, "the composed site count is not three");
   check(route.reloc_sites[0].slot == R300_R2VB_PUBLIC_ROUTE_SLOT_CARRIER &&
            route.reloc_sites[1].slot == R300_R2VB_PUBLIC_ROUTE_SLOT_COLOR &&
            route.reloc_sites[2].slot ==
               R300_R2VB_PUBLIC_ROUTE_SLOT_CARRIER,
         "the composed sites do not carry the stream's slot order");
   check(route.reloc_sites[0].ib_index == producer.reloc_sites[0].ib_index,
         "the producer site moved");
   for (uint32_t i = 0; i < consumer.reloc_site_count; i++) {
      check(route.reloc_sites[1 + i].ib_index ==
               consumer.reloc_sites[i].ib_index + producer.ib_size_dwords,
            "a consumer site did not shift by the producer length");
   }

   r300_r2vb_public_route_release(&route);
   check(route.ib == NULL && route.ib_size_dwords == 0 &&
            route.reloc_site_count == 0,
         "release left state behind");
   r300_r2vb_public_route_release(&route);

   r300_tcl_bypass_triangle_release(&consumer);
   r300_r2vb_producer_pass_release(&producer);
}

/* The route carries the application's records, so a stream differing
 * from the reference in its records alone composes with the same
 * consumer and the same site geometry; only the producer prefix moves.
 */
static void
test_records_composition(void)
{
   static const float records[3][4] = {
      { 4.0f, 4.0f, 0.0f, 1.0f },
      { 28.0f, 4.0f, 0.0f, 1.0f },
      { 16.0f, 28.0f, 0.0f, 1.0f },
   };

   struct r300_r2vb_producer_ib producer;
   check(r300_r2vb_producer_records_emit(records, &producer) == 0,
         "records producer emission failed");

   struct r300_r2vb_public_route_ib route;
   check(r300_r2vb_public_route_compose(records, R300_TRIANGLE_TARGET_WIDTH,
                                        R300_TRIANGLE_TARGET_HEIGHT,
                                        &route) == 0,
         "records composition failed");
   check(memcmp(route.ib, producer.ib,
                producer.ib_size_dwords * sizeof(uint32_t)) == 0,
         "the prefix is not the records emission");
   check(r300_r2vb_public_route_validate_reloc_sites(&route) == 0,
         "the records composition's sites do not validate");

   struct r300_r2vb_public_route_ib reference;
   check(r300_r2vb_public_route_reference_compose(&reference) == 0,
         "reference composition failed");
   check(reference.ib_size_dwords == route.ib_size_dwords &&
            reference.consumer_start_dwords == route.consumer_start_dwords,
         "a records-only difference changed the stream geometry");
   check(memcmp(reference.ib + reference.consumer_start_dwords,
                route.ib + route.consumer_start_dwords,
                (route.ib_size_dwords - route.consumer_start_dwords) *
                   sizeof(uint32_t)) == 0,
         "a records-only difference reached the consumer half");
   check(memcmp(reference.ib, route.ib,
                route.consumer_start_dwords * sizeof(uint32_t)) != 0,
         "two different record sets composed identical producer halves");

   r300_r2vb_public_route_release(&reference);
   r300_r2vb_public_route_release(&route);
   r300_r2vb_producer_pass_release(&producer);
}

/* A composition that refuses leaves the caller with no allocation to
 * free and no stream to submit, so each refusal is checked on a
 * destination poisoned beforehand.
 */
static void
test_refusals(void)
{
   struct r300_r2vb_public_route_ib route;
   memset(&route, 0xa5, sizeof(route));

   check(r300_r2vb_public_route_compose(NULL, R300_TRIANGLE_TARGET_WIDTH,
                                        R300_TRIANGLE_TARGET_HEIGHT,
                                        &route) == -EINVAL,
         "a null record array composed");
   check(r300_r2vb_public_route_compose(
            (const float(*)[4])r300_tcl_bypass_triangle_vertices,
            R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
            NULL) == -EINVAL,
         "a null destination composed");

   /* 0.1f is not a fixed point of the s1e7m16 round trip, so the
    * producer refuses it by the delivery-admission window.
    */
   float off_domain[3][4];
   memcpy(off_domain, r300_tcl_bypass_triangle_vertices,
          sizeof(off_domain));
   off_domain[0][0] = 0.1f;
   check(r300_r2vb_public_route_compose(off_domain,
                                        R300_TRIANGLE_TARGET_WIDTH,
                                        R300_TRIANGLE_TARGET_HEIGHT,
                                        &route) == -EDOM,
         "an off-domain record composed");

   check(r300_r2vb_public_route_compose(
            (const float(*)[4])r300_tcl_bypass_triangle_vertices, 0,
            R300_TRIANGLE_TARGET_HEIGHT, &route) == -EINVAL,
         "a zero-width extent composed");
   check(r300_r2vb_public_route_compose(
            (const float(*)[4])r300_tcl_bypass_triangle_vertices,
            R300_TRIANGLE_TARGET_WIDTH + 1u, R300_TRIANGLE_TARGET_HEIGHT,
            &route) == -EINVAL,
         "an oversize extent composed");

   check(r300_r2vb_public_route_validate_reloc_sites(NULL) == -EINVAL,
         "a null stream validated");
}

/* The site validator decides on the stream, so each mutation of an
 * otherwise valid composition refuses by its own class.
 */
static void
test_site_validation(void)
{
   struct r300_r2vb_public_route_ib route;
   check(r300_r2vb_public_route_reference_compose(&route) == 0,
         "reference composition failed");

   struct r300_r2vb_public_route_ib mutated = route;
   mutated.reloc_site_count = 2;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -EINVAL,
         "a short site list validated");

   mutated = route;
   mutated.reloc_sites[1].slot = R300_R2VB_PUBLIC_ROUTE_SLOT_CARRIER;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -EINVAL,
         "a site out of the stream's slot order validated");

   mutated = route;
   mutated.reloc_sites[2].ib_index = route.reloc_sites[1].ib_index;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -EINVAL,
         "non-increasing site indices validated");

   mutated = route;
   mutated.consumer_start_dwords = route.reloc_sites[0].ib_index;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -ERANGE,
         "a split leaving the producer's site in the consumer half "
         "validated");

   mutated = route;
   mutated.consumer_start_dwords = route.reloc_sites[1].ib_index + 1u;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -ERANGE,
         "a split leaving a consumer site in the producer half validated");

   mutated = route;
   mutated.consumer_start_dwords = 0;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -ERANGE,
         "a split at the stream start validated");

   mutated = route;
   mutated.consumer_start_dwords = route.ib_size_dwords;
   check(r300_r2vb_public_route_validate_reloc_sites(&mutated) == -ERANGE,
         "a split at the stream end validated");

   r300_r2vb_public_route_release(&route);
}

int
main(void)
{
   test_reference_composition();
   test_records_composition();
   test_refusals();
   test_site_validation();
   printf("r300_r2vb_public_route_test: composition contract holds\n");
   return 0;
}
