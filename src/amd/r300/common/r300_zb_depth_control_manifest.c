/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the depth control cell: retained no-submit
 * evidence in the exact form the offline kernel-parser replay consumes.
 */

#include "r300_first_draw_state.h"
#include "r300_fragment_binary.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_zb_depth_control_cell.h"

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
   const size_t written = fwrite(data, 1, size, f);
   /* The flush runs at close, so the close status is where a full or
    * over-quota directory reports itself; a manifest digest computed
    * from memory describes bytes that never reached the file otherwise.
    */
   const int close_rc = fclose(f);
   return (written == size && close_rc == 0) ? 0 : 1;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
      return 2;
   }
   const char *dir = argv[1];

   /* The manifest hashes the fragment binary separately, so it resolves
    * the same binary the reference emission bakes into the IB.
    */
   struct r300_fragment_binary fs;
   if (r300_tcl_bypass_triangle_reference_fs(&fs) != 0) {
      fprintf(stderr, "fragment binary construction failed\n");
      return 1;
   }

   struct r300_first_draw_contract contract;
   if (r300_zb_depth_control_reference_contract(&contract) != 0) {
      fprintf(stderr, "first-draw contract resolution failed\n");
      r300_fragment_binary_finish(&fs);
      return 1;
   }

   struct r300_zb_depth_control_ib cell;
   if (r300_zb_depth_control_reference_emit(&cell) != 0) {
      fprintf(stderr, "depth control emission failed\n");
      r300_fragment_binary_finish(&fs);
      return 1;
   }
   if (r300_zb_depth_control_validate_reloc_sites(&cell) != 0) {
      fprintf(stderr, "relocation sites failed validation\n");
      r300_zb_depth_control_release(&cell);
      r300_fragment_binary_finish(&fs);
      return 1;
   }

   /* ib.bin carries the canonical little-endian encoding, so the file
    * matches the digest below and the offline replay reads the same
    * bytes on every host.
    */
   uint8_t *ib_bytes = malloc(cell.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_zb_depth_control_release(&cell);
      r300_fragment_binary_finish(&fs);
      return 1;
   }
   r300_triangle_ib_serialize(cell.ib, cell.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       cell.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   /* The depth surface carries both domains: the host fills it with the
    * sentinel before the draw and the device writes passing fragments
    * into it.  The color target is written alone and the vertex array
    * read alone.
    */
   char bo_table[768];
   const int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"vertex\", \"domain\": \"GTT\","
      " \"size\": 4096},\n"
      "    {\"slot\": %u, \"role\": \"color\", \"domain\": \"GTT\","
      " \"size\": %u},\n"
      "    {\"slot\": %u, \"role\": \"depth\", \"domain\": \"GTT\","
      " \"size\": %u}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_ZB_DEPTH_CONTROL_SLOT_VERTEX,
      (unsigned)R300_ZB_DEPTH_CONTROL_SLOT_COLOR,
      (unsigned)R300_ZB_DEPTH_CONTROL_COLOR_BYTES,
      (unsigned)R300_ZB_DEPTH_CONTROL_SLOT_DEPTH,
      (unsigned)R300_ZB_DEPTH_CONTROL_DEPTH_BYTES);
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char hash_hex[2 * R300_FRAGMENT_BINARY_HASH_SIZE + 1];
   for (unsigned i = 0; i < R300_FRAGMENT_BINARY_HASH_SIZE; i++)
      snprintf(&hash_hex[2 * i], 3, "%02x", fs.hash[i]);

   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, ib_blake3_hex);

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
   const int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-zb-depth-control-cell/1\",\n"
      "  \"cell_kind\": \"depth-control\",\n"
      "  \"emitter\": \"r300_zb_depth_control\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"draw_dword\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"fragment_binary_blake3\": \"%s\",\n"
      "  \"contract_clause_count\": %u,\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"target_width\": %u,\n"
      "  \"target_height\": %u,\n"
      "  \"target_pitch_pixels\": %u,\n"
      "  \"allocation_rows\": %u,\n"
      "  \"depth_cpp\": %u,\n"
      "  \"depth_sentinel\": %u,\n"
      "  \"near_z\": %.6f,\n"
      "  \"far_z\": %.6f\n"
      "}\n",
      cell.ib_size_dwords, r300_zb_depth_control_draw_dword(&cell),
      ib_blake3_hex, hash_hex, contract.count, sites,
      (unsigned)R300_ZB_DEPTH_CONTROL_TARGET_WIDTH,
      (unsigned)R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT,
      (unsigned)R300_ZB_DEPTH_CONTROL_PITCH_PIXELS,
      (unsigned)R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS,
      (unsigned)R300_ZB_DEPTH_CONTROL_DEPTH_CPP,
      (unsigned)R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL,
      (double)R300_ZB_DEPTH_CONTROL_NEAR_Z,
      (double)R300_ZB_DEPTH_CONTROL_FAR_Z);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_zb_depth_control_release(&cell);
   r300_fragment_binary_finish(&fs);

   if (rc == 0)
      printf("r300_zb_depth_control_manifest: wrote ib.bin, bo_table.json, "
             "manifest.json to %s\n",
             dir);
   return rc;
}
