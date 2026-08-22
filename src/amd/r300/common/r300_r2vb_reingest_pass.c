/*
 * SPDX-License-Identifier: MIT
 *
 * R2VB producer-plus-re-ingest emitter: concatenates the reference
 * producer and reference triangle emissions into one stream over a
 * shared relocation table.
 */

#include "r300_r2vb_reingest_pass.h"

#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"

#include "r300_pm4_builder.h"
#include "r300_reg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Both component emitters encode a relocation NOP payload of slot times
 * four -- the dword index of the slot's relocation-chunk entry -- so the
 * concatenated stream resolves against the re-ingest slot table exactly
 * when the component slots map onto it without renumbering: producer
 * carrier (0) and triangle vertex (0) share the carrier entry, and
 * triangle color (1) is the color entry.
 */
_Static_assert((int)R300_R2VB_PRODUCER_SLOT_CARRIER ==
                  (int)R300_R2VB_REINGEST_SLOT_CARRIER,
               "the producer's carrier payload must index the re-ingest "
               "carrier entry");
_Static_assert((int)R300_TRIANGLE_SLOT_VERTEX ==
                  (int)R300_R2VB_REINGEST_SLOT_CARRIER,
               "the triangle's vertex payload must index the re-ingest "
               "carrier entry");
_Static_assert((int)R300_TRIANGLE_SLOT_COLOR ==
                  (int)R300_R2VB_REINGEST_SLOT_COLOR,
               "the triangle's color payload must index the re-ingest "
               "color entry");

int
r300_r2vb_reingest_pass_emit(struct r300_r2vb_reingest_ib *out)
{
   memset(out, 0, sizeof(*out));

   struct r300_r2vb_producer_ib producer;
   int rc = r300_r2vb_producer_reference_emit(&producer);
   if (rc != 0)
      return rc;
   rc = r300_r2vb_producer_pass_validate_reloc_sites(&producer);
   if (rc != 0) {
      r300_r2vb_producer_pass_release(&producer);
      return rc;
   }

   struct r300_tcl_bypass_triangle_ib consumer;
   rc = r300_tcl_bypass_triangle_reference_emit(&consumer);
   if (rc != 0) {
      r300_r2vb_producer_pass_release(&producer);
      return rc;
   }
   rc = r300_tcl_bypass_triangle_validate_reloc_sites(&consumer);
   if (rc != 0) {
      r300_tcl_bypass_triangle_release(&consumer);
      r300_r2vb_producer_pass_release(&producer);
      return rc;
   }

   const uint32_t total_dwords =
      producer.ib_size_dwords + consumer.ib_size_dwords;
   uint32_t *ib = malloc((size_t)total_dwords * sizeof(uint32_t));
   if (ib == NULL) {
      r300_tcl_bypass_triangle_release(&consumer);
      r300_r2vb_producer_pass_release(&producer);
      return -ENOMEM;
   }
   memcpy(ib, producer.ib, producer.ib_size_dwords * sizeof(uint32_t));
   memcpy(ib + producer.ib_size_dwords, consumer.ib,
          consumer.ib_size_dwords * sizeof(uint32_t));

   /* Producer sites keep their indices; consumer sites shift by the
    * producer extent and keep their slots, which the static asserts
    * above bind to the re-ingest table.
    */
   uint32_t site_count = 0;
   for (uint32_t i = 0; i < producer.reloc_site_count; i++) {
      out->reloc_sites[site_count++] = (struct r300_r2vb_reingest_reloc_site){
         .ib_index = producer.reloc_sites[i].ib_index,
         .slot = producer.reloc_sites[i].slot,
      };
   }
   for (uint32_t i = 0; i < consumer.reloc_site_count; i++) {
      out->reloc_sites[site_count++] = (struct r300_r2vb_reingest_reloc_site){
         .ib_index =
            producer.ib_size_dwords + consumer.reloc_sites[i].ib_index,
         .slot = consumer.reloc_sites[i].slot,
      };
   }

   out->ib = ib;
   out->ib_size_dwords = total_dwords;
   out->reloc_site_count = site_count;
   out->consumer_start_dwords = producer.ib_size_dwords;
   out->owns_ib = true;

   r300_tcl_bypass_triangle_release(&consumer);
   r300_r2vb_producer_pass_release(&producer);
   return 0;
}

void
r300_r2vb_reingest_pass_release(struct r300_r2vb_reingest_ib *ib)
{
   if (ib == NULL)
      return;
   if (ib->owns_ib)
      free(ib->ib);
   memset(ib, 0, sizeof(*ib));
}

int
r300_r2vb_reingest_validate_reloc_sites(
   const struct r300_r2vb_reingest_ib *ib)
{
   if (ib == NULL || ib->ib == NULL)
      return -EINVAL;
   if (ib->reloc_site_count != R300_R2VB_REINGEST_MAX_RELOC_SITES)
      return -EINVAL;

   uint32_t carrier_sites = 0;
   uint32_t color_sites = 0;
   for (uint32_t i = 0; i < ib->reloc_site_count; i++) {
      const struct r300_r2vb_reingest_reloc_site *site =
         &ib->reloc_sites[i];
      if (site->slot >= R300_R2VB_REINGEST_SLOT_COUNT)
         return -EINVAL;
      if (site->ib_index == 0 || site->ib_index >= ib->ib_size_dwords)
         return -EINVAL;
      /* The site indexes the payload dword of a one-payload NOP whose
       * payload is the slot's relocation-chunk dword index.
       */
      if (ib->ib[site->ib_index - 1] != CP_PACKET3(R300_PM4_PACKET3_NOP, 0))
         return -EINVAL;
      if (ib->ib[site->ib_index] != site->slot * 4)
         return -EINVAL;
      if (site->slot == R300_R2VB_REINGEST_SLOT_CARRIER)
         carrier_sites++;
      else
         color_sites++;
   }
   if (carrier_sites != 2 || color_sites != 1)
      return -EINVAL;
   return 0;
}
