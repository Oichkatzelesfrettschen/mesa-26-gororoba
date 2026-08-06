/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the fixed TCL-bypass triangle cell: retained
 * no-submit evidence in the exact form the offline kernel-parser replay
 * consumes.
 */

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
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

   /* The manifest hashes the reference fragment binary separately, so it
    * resolves the same binary the reference emission bakes into the IB.
    */
   struct r300_fragment_binary fs;
   if (r300_tcl_bypass_triangle_reference_fs(&fs) != 0) {
      fprintf(stderr, "fragment binary construction failed\n");
      return 1;
   }

   struct r300_tcl_bypass_triangle_ib cell;
   if (r300_tcl_bypass_triangle_reference_emit(&cell) != 0) {
      fprintf(stderr, "triangle emission failed\n");
      r300_fragment_binary_finish(&fs);
      return 1;
   }

   int rc = write_file(dir, "ib.bin", cell.ib,
                       cell.ib_size_dwords * sizeof(uint32_t));

   char bo_table[512];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"vertex\", \"domain\": \"GTT\","
      " \"size\": 4096},\n"
      "    {\"slot\": %u, \"role\": \"color\", \"domain\": \"GTT\","
      " \"size\": 65536}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_TRIANGLE_SLOT_VERTEX,
      (unsigned)R300_TRIANGLE_SLOT_COLOR);
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char hash_hex[2 * R300_FRAGMENT_BINARY_HASH_SIZE + 1];
   for (unsigned i = 0; i < R300_FRAGMENT_BINARY_HASH_SIZE; i++) {
      snprintf(&hash_hex[2 * i], 3, "%02x", fs.hash[i]);
   }

   char manifest[1024];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"ib_dwords\": %u,\n"
      "  \"reloc_sites\": %u,\n"
      "  \"emitter\": \"r300_tcl_bypass_triangle\",\n"
      "  \"fragment_binary_blake3\": \"%s\"\n"
      "}\n",
      cell.ib_size_dwords, cell.reloc_site_count, hash_hex);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_tcl_bypass_triangle_release(&cell);
   r300_fragment_binary_finish(&fs);

   if (rc == 0) {
      printf("r300_triangle_manifest: wrote ib.bin, bo_table.json, "
             "manifest.json to %s\n",
             dir);
   }
   return rc;
}
