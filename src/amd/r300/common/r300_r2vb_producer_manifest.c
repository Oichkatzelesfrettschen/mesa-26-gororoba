/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the R2VB producer pass: retained no-submit evidence
 * in the exact form the offline kernel-parser replays consume.
 */

#include "r300_first_draw_state.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"

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

   struct r300_r2vb_producer_ib pass;
   if (r300_r2vb_producer_reference_emit(&pass) != 0) {
      fprintf(stderr, "producer-pass emission failed\n");
      return 1;
   }
   if (r300_r2vb_producer_pass_validate_reloc_sites(&pass) != 0) {
      fprintf(stderr, "producer-pass relocation sites invalid\n");
      r300_r2vb_producer_pass_release(&pass);
      return 1;
   }

   /* ib.bin carries the canonical little-endian encoding
    * (r300_triangle_ib_serialize), so the file matches the digest below
    * and the offline replay reads the same bytes on every host.
    */
   uint8_t *ib_bytes = malloc(pass.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_r2vb_producer_pass_release(&pass);
      return 1;
   }
   r300_triangle_ib_serialize(pass.ib, pass.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       pass.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   /* One carrier BO, read-write: the pass writes it through the color
    * backend and the consuming draw fetches it as the vertex stream.
    */
   char bo_table[256];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"carrier\", \"domain\": \"GTT\","
      " \"size\": 4096}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_R2VB_PRODUCER_SLOT_CARRIER);
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(pass.ib, pass.ib_size_dwords,
                               ib_blake3_hex);

   char sites[128];
   int sites_len = 0;
   for (uint32_t i = 0; i < pass.reloc_site_count; i++) {
      sites_len += snprintf(&sites[sites_len],
                            sizeof(sites) - (size_t)sites_len,
                            "%s{\"slot\": %u, \"ib_index\": %u}",
                            i == 0 ? "" : ", ", pass.reloc_sites[i].slot,
                            pass.reloc_sites[i].ib_index);
   }

   struct r300_r2vb_producer_layout layout;
   if (r300_r2vb_producer_layout_single_row(
          R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout) != 0) {
      r300_r2vb_producer_pass_release(&pass);
      return 1;
   }

   char manifest[1024];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-r2vb-producer-pass/1\",\n"
      "  \"emitter\": \"r300_r2vb_producer_pass\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"vertex_count\": %u,\n"
      "  \"carrier_pitch_pixels\": %u,\n"
      "  \"carrier_height\": %u,\n"
      "  \"carrier_cpp_bytes\": 16\n"
      "}\n",
      pass.ib_size_dwords, ib_blake3_hex, sites, layout.count,
      layout.pitch_pixels, layout.height);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_r2vb_producer_pass_release(&pass);

   if (rc == 0) {
      printf("r300_r2vb_producer_manifest: wrote ib.bin, bo_table.json, "
             "manifest.json to %s\n",
             dir);
   }
   return rc;
}
