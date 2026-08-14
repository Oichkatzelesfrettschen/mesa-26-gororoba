/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the R2VB producer-plus-re-ingest pass: retained
 * no-submit evidence in the exact form the offline kernel-parser
 * replays consume.
 */

#include "r300_r2vb_producer_pass.h"
#include "r300_r2vb_reingest_pass.h"
#include "r300_tcl_bypass_triangle.h"

#include "drm-uapi/radeon_drm.h"
#include "util/macros.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
write_file(const char *dir, const char *name, const void *data, size_t size)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "wb");
   if (f == NULL) {
      fprintf(stderr, "open %s: %s\n", path, strerror(errno));
      return 1;
   }
   size_t written = fwrite(data, 1, size, f);
   const int close_rc = fclose(f);
   if (written != size) {
      fprintf(stderr, "write %s: short write (%zu/%zu)\n", path, written,
              size);
      return 1;
   }
   if (close_rc != 0) {
      fprintf(stderr, "close %s: %s\n", path, strerror(errno));
      return 1;
   }
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
      return 2;
   }
   const char *dir = argv[1];

   struct r300_r2vb_reingest_ib pass;
   if (r300_r2vb_reingest_reference_emit(&pass) != 0) {
      fprintf(stderr, "re-ingest emission failed\n");
      return 1;
   }
   if (r300_r2vb_reingest_validate_reloc_sites(&pass) != 0) {
      fprintf(stderr, "re-ingest relocation sites invalid\n");
      r300_r2vb_reingest_pass_release(&pass);
      return 1;
   }

   /* ib.bin carries the canonical little-endian encoding
    * (r300_triangle_ib_serialize), matching the digest below.
    */
   uint8_t *ib_bytes = malloc(pass.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_r2vb_reingest_pass_release(&pass);
      return 1;
   }
   r300_triangle_ib_serialize(pass.ib, pass.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       pass.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   /* The two allocations the attended transport makes: the producer
    * carrier row (pitch pixels of one FP32x4 texel, one row) and the
    * triangle's 64-pixel-pitch B8G8R8A8 target with its canary row.
    */
   struct r300_r2vb_producer_layout layout;
   if (r300_r2vb_producer_layout_single_row(
          R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout) != 0) {
      r300_r2vb_reingest_pass_release(&pass);
      return 1;
   }
   const uint32_t carrier_size_bytes =
      layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;
   const uint32_t color_size_bytes = 64u * 65u * 4u;

   char bo_table[768];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"carrier\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": %u, \"size\": %u},\n"
      "    {\"slot\": %u, \"role\": \"color\", \"domain\": \"GTT\","
      " \"read_domains\": 0, \"write_domain\": %u, \"size\": %u}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_R2VB_REINGEST_SLOT_CARRIER, RADEON_GEM_DOMAIN_GTT,
      RADEON_GEM_DOMAIN_GTT, carrier_size_bytes,
      (unsigned)R300_R2VB_REINGEST_SLOT_COLOR, RADEON_GEM_DOMAIN_GTT,
      color_size_bytes);
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(pass.ib, pass.ib_size_dwords,
                               ib_blake3_hex);

   char sites[256];
   int sites_len = 0;
   for (uint32_t i = 0; i < pass.reloc_site_count; i++) {
      sites_len += snprintf(&sites[sites_len],
                            sizeof(sites) - (size_t)sites_len,
                            "%s{\"slot\": %u, \"ib_index\": %u}",
                            i == 0 ? "" : ", ", pass.reloc_sites[i].slot,
                            pass.reloc_sites[i].ib_index);
   }

   char manifest[1024];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-r2vb-reingest-pass/1\",\n"
      "  \"emitter\": \"r300_r2vb_reingest_pass\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"consumer_start_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"carrier_poison_dword\": \"0x%08x\",\n"
      "  \"color_size_bytes\": %u,\n"
      "  \"color_sentinel_dword\": \"0x%08x\"\n"
      "}\n",
      pass.ib_size_dwords, pass.consumer_start_dwords, ib_blake3_hex,
      sites, carrier_size_bytes, R300_R2VB_PRODUCER_POISON_DWORD,
      color_size_bytes, R300_TRIANGLE_COLOR_SENTINEL);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_r2vb_reingest_pass_release(&pass);

   if (rc == 0) {
      printf("r300_r2vb_reingest_manifest: wrote ib.bin, bo_table.json, "
             "manifest.json to %s\n",
             dir);
   }
   return rc;
}
