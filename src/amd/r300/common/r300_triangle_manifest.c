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

   /* The manifest publishes the contract's clause count, so it resolves the
    * same contract the reference emission prefixes.
    */
   struct r300_first_draw_contract contract;
   if (r300_tcl_bypass_triangle_reference_contract(&contract) != 0) {
      fprintf(stderr, "first-draw contract resolution failed\n");
      r300_fragment_binary_finish(&fs);
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

   /* The digest the arming gate compares against its authorized value, taken
    * over the same bytes ib.bin carries, through the one helper every other
    * site uses.  A manifest without it leaves the gate authorizing a cell it
    * never hashed.
    */
   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, ib_blake3_hex);

   /* The draw is the last packet, and its dword index is what a replay names
    * when it reports which packet a verdict came from.
    */
   const uint32_t draw_dword = r300_triangle_draw_dword(&cell);

   char sites[256];
   int sites_len = 0;
   for (uint32_t i = 0; i < cell.reloc_site_count; i++) {
      sites_len += snprintf(&sites[sites_len],
                            sizeof(sites) - (size_t)sites_len,
                            "%s{\"slot\": %u, \"ib_index\": %u}",
                            i == 0 ? "" : ", ", cell.reloc_sites[i].slot,
                            cell.reloc_sites[i].ib_index);
   }

   char manifest[2048];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-tcl-bypass-cell/1\",\n"
      "  \"cell_kind\": \"contract-prefixed-successor\",\n"
      "  \"emitter\": \"r300_tcl_bypass_triangle\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"draw_dword\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"fragment_binary_blake3\": \"%s\",\n"
      "  \"contract_clause_count\": %u,\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"target_width\": %u,\n"
      "  \"target_height\": %u,\n"
      "  \"target_pitch_pixels\": %u,\n"
      "  \"allocation_rows\": %u\n"
      "}\n",
      cell.ib_size_dwords, draw_dword, ib_blake3_hex, hash_hex,
      contract.count, sites, R300_TRIANGLE_TARGET_WIDTH,
      R300_TRIANGLE_TARGET_HEIGHT, R300_TRIANGLE_TARGET_PITCH_PIXELS,
      R300_TRIANGLE_ALLOCATION_ROWS);
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
