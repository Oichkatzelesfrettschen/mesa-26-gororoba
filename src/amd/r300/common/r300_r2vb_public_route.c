/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_r2vb_public_route.h"

#include "r300_pm4_builder.h"
#include "r300_r2vb_producer_pass.h"

#include "r300_reg.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Both halves encode a relocation payload as the slot's chunk index,
 * four dwords per entry, so one expression decodes either half's sites.
 */
#define R300_R2VB_PUBLIC_ROUTE_RELOC_PAYLOAD(slot) ((slot) * 4)

static int
compose(const struct r300_r2vb_producer_ib *producer,
        const struct r300_tcl_bypass_triangle_ib *consumer,
        struct r300_r2vb_public_route_ib *out)
{
   const uint32_t total = producer->ib_size_dwords + consumer->ib_size_dwords;
   uint32_t *words = malloc(total * sizeof(uint32_t));
   if (words == NULL)
      return -ENOMEM;

   memcpy(words, producer->ib, producer->ib_size_dwords * sizeof(uint32_t));
   memcpy(words + producer->ib_size_dwords, consumer->ib,
          consumer->ib_size_dwords * sizeof(uint32_t));

   memset(out, 0, sizeof(*out));
   out->ib = words;
   out->ib_size_dwords = total;
   out->consumer_start_dwords = producer->ib_size_dwords;
   out->owns_ib = true;

   /* The producer's sites keep their indices; the consumer's shift by
    * the producer length, which is exactly what appending did to the
    * dwords they name.
    */
   for (uint32_t i = 0; i < producer->reloc_site_count; i++) {
      out->reloc_sites[out->reloc_site_count++] =
         (struct r300_r2vb_public_route_reloc_site){
            .ib_index = producer->reloc_sites[i].ib_index,
            .slot = producer->reloc_sites[i].slot,
         };
   }
   for (uint32_t i = 0; i < consumer->reloc_site_count; i++) {
      out->reloc_sites[out->reloc_site_count++] =
         (struct r300_r2vb_public_route_reloc_site){
            .ib_index = consumer->reloc_sites[i].ib_index +
                        producer->ib_size_dwords,
            .slot = consumer->reloc_sites[i].slot,
         };
   }
   return 0;
}

int
r300_r2vb_public_route_compose(const float (*records)[4], uint32_t width,
                               uint32_t height,
                               struct r300_r2vb_public_route_ib *out)
{
   if (records == NULL || out == NULL)
      return -EINVAL;

   struct r300_r2vb_producer_ib producer;
   int rc = r300_r2vb_producer_records_emit(records, &producer);
   if (rc != 0)
      return rc;

   struct r300_tcl_bypass_triangle_ib consumer;
   rc = r300_tcl_bypass_triangle_extent_emit(width, height, &consumer);
   if (rc != 0) {
      r300_r2vb_producer_pass_release(&producer);
      return rc;
   }

   /* The composed site array is fixed at the two emitters' site counts,
    * so a grammar change that adds a relocation surfaces here rather
    * than writing past the array.
    */
   if (producer.reloc_site_count + consumer.reloc_site_count >
       R300_R2VB_PUBLIC_ROUTE_MAX_RELOC_SITES)
      rc = -EOVERFLOW;
   else
      rc = compose(&producer, &consumer, out);

   r300_tcl_bypass_triangle_release(&consumer);
   r300_r2vb_producer_pass_release(&producer);
   return rc;
}

int
r300_r2vb_public_route_reference_compose(
   struct r300_r2vb_public_route_ib *out)
{
   return r300_r2vb_public_route_compose(
      (const float(*)[4])r300_tcl_bypass_triangle_vertices,
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT, out);
}

void
r300_r2vb_public_route_release(struct r300_r2vb_public_route_ib *ib)
{
   if (ib == NULL)
      return;
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_public_route_validate_reloc_sites(
   const struct r300_r2vb_public_route_ib *ib)
{
   if (ib == NULL || ib->ib == NULL)
      return -EINVAL;

   /* The carrier is referenced twice -- written by the producer, fetched
    * by the consumer -- so the composed list carries three sites over
    * two slots and slot uniqueness is the wrong predicate.  Stream order
    * is the invariant: the producer's carrier retarget, then the
    * consumer's color target, then the consumer's vertex array.
    */
   static const uint32_t expected_slots[] = {
      R300_R2VB_PUBLIC_ROUTE_SLOT_CARRIER,
      R300_R2VB_PUBLIC_ROUTE_SLOT_COLOR,
      R300_R2VB_PUBLIC_ROUTE_SLOT_CARRIER,
   };
   static_assert(ARRAY_SIZE(expected_slots) ==
                    R300_R2VB_PUBLIC_ROUTE_MAX_RELOC_SITES,
                 "every composed site has a place in the stream order");

   if (ib->reloc_site_count != R300_R2VB_PUBLIC_ROUTE_MAX_RELOC_SITES)
      return -EINVAL;
   if (ib->consumer_start_dwords == 0 ||
       ib->consumer_start_dwords >= ib->ib_size_dwords)
      return -ERANGE;

   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_r2vb_public_route_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot != expected_slots[i])
         return -EINVAL;
      if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
         return -ERANGE;
      if (i > 0 && ib->reloc_sites[i - 1].ib_index >= site->ib_index)
         return -EINVAL;
      if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
         return -EINVAL;
      if (ib->ib[site->ib_index] !=
          R300_R2VB_PUBLIC_ROUTE_RELOC_PAYLOAD(site->slot))
         return -EINVAL;
   }

   /* The producer half owns exactly the first site: a consumer site
    * landing inside the producer, or the producer's site landing past
    * the split, would resolve a buffer object into the other half's
    * stream.
    */
   if (ib->reloc_sites[0].ib_index >= ib->consumer_start_dwords)
      return -ERANGE;
   if (ib->reloc_sites[1].ib_index < ib->consumer_start_dwords)
      return -ERANGE;

   return 0;
}
