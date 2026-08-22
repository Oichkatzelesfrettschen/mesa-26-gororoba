/*
 * SPDX-License-Identifier: MIT
 *
 * Structural proof of the R2VB re-ingest stream: the concatenation is
 * byte-identical to the two reference emissions it composes, the shared
 * relocation table resolves the carrier twice and the color target
 * once, and the delivered carrier the producer stage predicts is the
 * vertex payload the proven triangle cell fetches.
 */

#include "r300_r2vb_producer_pass.h"
#include "r300_r2vb_reingest_pass.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
   do {                                                                \
      if (!(cond)) {                                                   \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                 #cond);                                               \
         failures++;                                                   \
      }                                                                \
   } while (0)

static void
test_composition(void)
{
   struct r300_r2vb_reingest_ib pass;
   CHECK(r300_r2vb_reingest_pass_emit(&pass) == 0);
   CHECK(r300_r2vb_reingest_validate_reloc_sites(&pass) == 0);

   struct r300_r2vb_producer_ib producer;
   CHECK(r300_r2vb_producer_reference_emit(&producer) == 0);
   struct r300_tcl_bypass_triangle_ib consumer;
   CHECK(r300_tcl_bypass_triangle_reference_emit(&consumer) == 0);

   /* The stream is the two reference emissions verbatim, so each half
    * keeps the digest its own qualification proved.
    */
   CHECK(pass.consumer_start_dwords == producer.ib_size_dwords);
   CHECK(pass.ib_size_dwords ==
         producer.ib_size_dwords + consumer.ib_size_dwords);
   CHECK(memcmp(pass.ib, producer.ib,
                producer.ib_size_dwords * sizeof(uint32_t)) == 0);
   CHECK(memcmp(pass.ib + pass.consumer_start_dwords, consumer.ib,
                consumer.ib_size_dwords * sizeof(uint32_t)) == 0);

   /* Three sites over two slots: the producer's carrier write, the
    * consumer's carrier fetch, and the consumer's color target.
    */
   CHECK(pass.reloc_site_count == 3);
   CHECK(pass.reloc_sites[0].slot == R300_R2VB_REINGEST_SLOT_CARRIER);
   CHECK(pass.reloc_sites[0].ib_index ==
         producer.reloc_sites[0].ib_index);
   uint32_t carrier_sites = 0;
   uint32_t color_sites = 0;
   for (uint32_t i = 0; i < pass.reloc_site_count; i++) {
      if (pass.reloc_sites[i].slot == R300_R2VB_REINGEST_SLOT_CARRIER)
         carrier_sites++;
      else if (pass.reloc_sites[i].slot == R300_R2VB_REINGEST_SLOT_COLOR)
         color_sites++;
      if (i > 0)
         CHECK(pass.reloc_sites[i].ib_index >=
               pass.consumer_start_dwords);
   }
   CHECK(carrier_sites == 2 && color_sites == 1);

   /* The producer's expected carrier is the triangle's vertex payload,
    * so the consuming draw fetches the bytes the proven cell renders
    * from.
    */
   uint32_t expected[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   CHECK(r300_r2vb_producer_reference_expected(
            expected, R300_R2VB_PRODUCER_REFERENCE_COUNT * 4) == 0);
   uint32_t vertices[R300_TRIANGLE_VERTEX_DWORDS];
   memcpy(vertices, r300_tcl_bypass_triangle_vertices, sizeof(vertices));
   CHECK(sizeof(expected) == sizeof(vertices));
   CHECK(memcmp(expected, vertices, sizeof(vertices)) == 0);

   /* Determinism: a second emission carries identical bytes. */
   struct r300_r2vb_reingest_ib again;
   CHECK(r300_r2vb_reingest_pass_emit(&again) == 0);
   CHECK(again.ib_size_dwords == pass.ib_size_dwords);
   CHECK(memcmp(again.ib, pass.ib,
                pass.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_r2vb_reingest_pass_release(&again);

   r300_tcl_bypass_triangle_release(&consumer);
   r300_r2vb_producer_pass_release(&producer);
   r300_r2vb_reingest_pass_release(&pass);
}

static void
test_validator_refusals(void)
{
   struct r300_r2vb_reingest_ib pass;
   CHECK(r300_r2vb_reingest_pass_emit(&pass) == 0);

   /* A site count off the contract refuses. */
   struct r300_r2vb_reingest_ib mutated = pass;
   mutated.reloc_site_count = 2;
   CHECK(r300_r2vb_reingest_validate_reloc_sites(&mutated) == -EINVAL);

   /* A site whose payload dword no longer names its slot refuses. */
   mutated = pass;
   const uint32_t site_index = pass.reloc_sites[2].ib_index;
   const uint32_t saved = pass.ib[site_index];
   pass.ib[site_index] = saved ^ 4u;
   CHECK(r300_r2vb_reingest_validate_reloc_sites(&mutated) == -EINVAL);
   pass.ib[site_index] = saved;
   CHECK(r300_r2vb_reingest_validate_reloc_sites(&pass) == 0);

   /* A slot outside the table refuses. */
   mutated = pass;
   mutated.reloc_sites[1].slot = R300_R2VB_REINGEST_SLOT_COUNT;
   CHECK(r300_r2vb_reingest_validate_reloc_sites(&mutated) == -EINVAL);

   CHECK(r300_r2vb_reingest_validate_reloc_sites(NULL) == -EINVAL);

   r300_r2vb_reingest_pass_release(&pass);
}

int
main(void)
{
   test_composition();
   test_validator_refusals();
   if (failures != 0) {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   printf("r300_r2vb_reingest_pass_test: composition and refusals hold\n");
   return 0;
}
