/*
 * SPDX-License-Identifier: MIT
 *
 * Byte-exact parity between the fixed re-ingest concatenator and the
 * role-based composer over the same two component emissions.  The
 * concatenator is the golden oracle: the composer must reproduce its
 * stream, its stage boundary, and its relocation sites exactly.
 */

/* The asserts carry the verdicts, so they stay live in NDEBUG builds. */
#undef NDEBUG

#include "r300_pm4_compose.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_r2vb_reingest_pass.h"
#include "r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int
main(void)
{
   /* Golden oracle: the fixed concatenation. */
   struct r300_r2vb_reingest_ib golden;
   assert(r300_r2vb_reingest_pass_emit(&golden) == 0);
   assert(r300_r2vb_reingest_validate_reloc_sites(&golden) == 0);

   /* The same two component emissions become composer fragments. */
   struct r300_r2vb_producer_ib producer;
   assert(r300_r2vb_producer_reference_emit(&producer) == 0);
   struct r300_tcl_bypass_triangle_ib consumer;
   assert(r300_tcl_bypass_triangle_reference_emit(&consumer) == 0);

   /* Component slots map onto composer roles: the carrier crosses both
    * engines (producer color-backend write, consumer vertex-fetch
    * read); the color target takes the consuming draw's write.
    */
   struct r300_pm4_reloc_site producer_sites[1];
   assert(producer.reloc_site_count == 1);
   assert(producer.reloc_sites[0].slot ==
          (uint32_t)R300_R2VB_REINGEST_SLOT_CARRIER);
   producer_sites[0] = (struct r300_pm4_reloc_site){
      .dword_index = producer.reloc_sites[0].ib_index,
      .role = R300_R2VB_BO_CARRIER,
      .read_domains = 0,
      .write_domain = 2,
   };
   struct r300_pm4_reloc_site consumer_sites[2];
   assert(consumer.reloc_site_count == 2);
   for (uint32_t i = 0; i < 2; i++) {
      const bool is_carrier = consumer.reloc_sites[i].slot ==
                              (uint32_t)R300_R2VB_REINGEST_SLOT_CARRIER;
      consumer_sites[i] = (struct r300_pm4_reloc_site){
         .dword_index = consumer.reloc_sites[i].ib_index,
         .role = is_carrier ? R300_R2VB_BO_CARRIER : R300_R2VB_BO_COLOR,
         .read_domains = is_carrier ? 2u : 0u,
         .write_domain = is_carrier ? 0u : 2u,
      };
   }

   const struct r300_pm4_fragment fragments[2] = {
      {.dwords = producer.ib,
       .dword_count = producer.ib_size_dwords,
       .relocs = producer_sites,
       .reloc_count = 1},
      {.dwords = consumer.ib,
       .dword_count = consumer.ib_size_dwords,
       .relocs = consumer_sites,
       .reloc_count = 2},
   };
   /* The re-ingest chunk table: carrier entry 0, color entry 1. */
   const struct r300_pm4_role_map roles = {
      .chunk_index = {[R300_R2VB_BO_SLOT] = -1,
                      [R300_R2VB_BO_MODEL] = -1,
                      [R300_R2VB_BO_CARRIER] = 0,
                      [R300_R2VB_BO_COLOR] = 1},
   };

   uint32_t *composed =
      malloc((size_t)golden.ib_size_dwords * sizeof(uint32_t));
   assert(composed != NULL);
   struct r300_pm4_builder b;
   r300_pm4_builder_init(&b, composed, golden.ib_size_dwords);
   struct r300_pm4_composition comp;
   assert(r300_pm4_compose(&b, fragments, 2, &roles, &comp) == 0);
   uint32_t count = 0;
   assert(r300_pm4_builder_finish(&b, &count) == 0);

   /* Byte-exact stream parity, and the stage boundary agrees. */
   assert(count == golden.ib_size_dwords);
   assert(memcmp(composed, golden.ib, count * 4) == 0);
   assert(comp.fragment_count == 2);
   assert(comp.fragment_start[0] == 0);
   assert(comp.fragment_start[1] == golden.consumer_start_dwords);

   /* Site-for-site relocation parity: same order, same parent index,
    * role matching the golden slot.
    */
   assert(comp.reloc_count == golden.reloc_site_count);
   for (uint32_t i = 0; i < comp.reloc_count; i++) {
      assert(comp.relocs[i].ib_index == golden.reloc_sites[i].ib_index);
      const uint32_t golden_slot = golden.reloc_sites[i].slot;
      if (golden_slot == (uint32_t)R300_R2VB_REINGEST_SLOT_CARRIER)
         assert(comp.relocs[i].role == R300_R2VB_BO_CARRIER);
      else
         assert(comp.relocs[i].role == R300_R2VB_BO_COLOR);
      /* The composed payload equals the golden chunk-index payload. */
      assert(composed[comp.relocs[i].ib_index] ==
             golden.ib[golden.reloc_sites[i].ib_index]);
   }

   /* Composed relocation domains preserve each use-site's declared DRM
    * intent (the DRM UAPI constant RADEON_GEM_DOMAIN_GTT = 0x2): the
    * carrier is a producer color-backend write then a consumer
    * vertex-fetch read as two separate uses, the color target takes
    * the consuming draw's write, and no other domain bit appears.
    */
   assert(comp.relocs[0].role == R300_R2VB_BO_CARRIER);
   assert(comp.relocs[0].write_domain == 2u);
   assert(comp.relocs[0].read_domains == 0u);
   for (uint32_t i = 1; i < comp.reloc_count; i++) {
      if (comp.relocs[i].role == R300_R2VB_BO_CARRIER) {
         assert(comp.relocs[i].read_domains == 2u);
         assert(comp.relocs[i].write_domain == 0u);
      } else {
         assert(comp.relocs[i].role == R300_R2VB_BO_COLOR);
         assert(comp.relocs[i].write_domain == 2u);
         assert(comp.relocs[i].read_domains == 0u);
      }
   }

   free(composed);
   r300_tcl_bypass_triangle_release(&consumer);
   r300_r2vb_producer_pass_release(&producer);
   r300_r2vb_reingest_pass_release(&golden);
   printf("r300_r2vb_reingest_compose_parity_test: composer reproduces "
          "the concatenated stream byte-exactly\n");
   return 0;
}
