/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the 2D solid-fill direct-write control cell:
 * retained no-submit evidence in the exact form the offline kernel-parser
 * replay consumes.
 */

#include "r300_direct_write.h"
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
   fclose(f);
   return written == size ? 0 : 1;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
      return 2;
   }
   const char *dir = argv[1];

   struct r300_direct_write_ib cell;
   if (r300_direct_write_emit(&cell) != 0) {
      fprintf(stderr, "direct-write emission failed\n");
      return 1;
   }
   if (r300_direct_write_validate_reloc_sites(&cell) != 0) {
      fprintf(stderr, "relocation-site validation failed\n");
      r300_direct_write_release(&cell);
      return 1;
   }

   /* ib.bin carries the canonical little-endian encoding
    * (r300_triangle_ib_serialize), so the file matches the digest below
    * and the offline replay reads the same bytes on every host.
    */
   uint8_t *ib_bytes = malloc(cell.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_direct_write_release(&cell);
      return 1;
   }
   r300_triangle_ib_serialize(cell.ib, cell.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       cell.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   char bo_table[256];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"color\", \"domain\": \"GTT\","
      " \"size\": 65536}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_DIRECT_WRITE_SLOT_COLOR);
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, ib_blake3_hex);

   char sites[128];
   int sites_len = 0;
   for (uint32_t i = 0; i < cell.reloc_site_count; i++) {
      sites_len += snprintf(&sites[sites_len],
                            sizeof(sites) - (size_t)sites_len,
                            "%s{\"slot\": %u, \"ib_index\": %u}",
                            i == 0 ? "" : ", ", cell.reloc_sites[i].slot,
                            cell.reloc_sites[i].ib_index);
   }

   char manifest[1024];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-direct-write-cell/1\",\n"
      "  \"cell_kind\": \"2d-solid-fill-control\",\n"
      "  \"emitter\": \"r300_direct_write\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"pixel_a\": {\"x\": %u, \"y\": %u, \"value\": \"0x%08x\"},\n"
      "  \"pixel_b\": {\"x\": %u, \"y\": %u, \"value\": \"0x%08x\"},\n"
      "  \"target_width\": %u,\n"
      "  \"target_height\": %u,\n"
      "  \"target_pitch_pixels\": %u,\n"
      "  \"allocation_rows\": %u\n"
      "}\n",
      cell.ib_size_dwords, ib_blake3_hex, sites,
      R300_DIRECT_WRITE_A_X, R300_DIRECT_WRITE_A_Y,
      R300_DIRECT_WRITE_A_VALUE,
      R300_DIRECT_WRITE_B_X, R300_DIRECT_WRITE_B_Y,
      R300_DIRECT_WRITE_B_VALUE,
      R300_TRIANGLE_TARGET_WIDTH, R300_TRIANGLE_TARGET_HEIGHT,
      R300_TRIANGLE_TARGET_PITCH_PIXELS, R300_TRIANGLE_ALLOCATION_ROWS);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_direct_write_release(&cell);

   if (rc == 0) {
      printf("r300_direct_write_manifest: wrote ib.bin, bo_table.json, "
             "manifest.json to %s\n",
             dir);
   }
   return rc;
}
