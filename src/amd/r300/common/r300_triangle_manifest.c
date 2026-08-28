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
#include <stdbool.h>
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
   /* --varying writes the varying cell: position-plus-varying records,
    * the pass-through fragment binary, and the same contract and target;
    * its own ib.bin and digest, a distinct cell from the position-only
    * reference.
    */
   bool varying = false;
   /* --multi-pass writes the two-pass stream: two reference render-shape
    * cells, the second at merged indices 2 and 3, in the bound form the
    * recorder installs, with a four-entry bo_table.
    */
   bool multi_pass = false;
   /* --triangles N writes the cell family member of N triangles: the
    * host expansion of an N-instance draw, differing from the single
    * triangle in the vertex-index bound and the draw count alone. */
   uint32_t triangles = 1;
   bool usage_error = argc < 2;
   for (int a = 2; a < argc && !usage_error; a++) {
      if (strcmp(argv[a], "--multi-pass") == 0) {
         multi_pass = true;
      } else if (strcmp(argv[a], "--varying") == 0) {
         varying = true;
      } else if (strcmp(argv[a], "--triangles") == 0 && a + 1 < argc) {
         char *end = NULL;
         const unsigned long value = strtoul(argv[++a], &end, 10);
         if (end == argv[a] || *end != '\0' || value < 1 ||
             value > R300_TRIANGLE_MAX_TRIANGLES) {
            fprintf(stderr, "--triangles takes 1..%u\n",
                    R300_TRIANGLE_MAX_TRIANGLES);
            return 2;
         }
         triangles = (uint32_t)value;
      } else {
         usage_error = true;
      }
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <output-directory> [--varying] [--triangles N] "
              "[--multi-pass]\n",
              argv[0]);
      return 2;
   }
   const char *dir = argv[1];

   if (multi_pass) {
      struct r300_triangle_multi_pass mp;
      memset(&mp, 0, sizeof(mp));
      r300_tcl_bypass_triangle_render_shape_reference(&mp.pass[0]);
      r300_tcl_bypass_triangle_render_shape_reference(&mp.pass[1]);
      const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
      for (unsigned i = 0; i < 4; i++)
         memcpy(&mp.pass[1].color_bits[i], &green[i], sizeof(float));
      mp.second_vertex_index = 2;
      mp.second_color_index = 3;
      struct r300_tcl_bypass_triangle_ib cell;
      if (r300_tcl_bypass_triangle_multi_pass_emit(&mp, &cell) != 0) {
         fprintf(stderr, "two-pass emission failed\n");
         return 1;
      }
      uint8_t *ib_bytes = malloc(cell.ib_size_dwords * sizeof(uint32_t));
      if (ib_bytes == NULL) {
         r300_tcl_bypass_triangle_release(&cell);
         return 1;
      }
      r300_triangle_ib_serialize(cell.ib, cell.ib_size_dwords, ib_bytes);
      int rc = write_file(dir, "ib.bin", ib_bytes,
                          cell.ib_size_dwords * sizeof(uint32_t));
      free(ib_bytes);
      char bo_table[768];
      int bo_table_len = snprintf(
         bo_table, sizeof(bo_table),
         "{\n"
         "  \"slots\": [\n"
         "    {\"slot\": 0, \"role\": \"vertex\", \"domain\": \"GTT\","
         " \"size\": 4096},\n"
         "    {\"slot\": 1, \"role\": \"color\", \"domain\": \"GTT\","
         " \"size\": %u},\n"
         "    {\"slot\": 2, \"role\": \"vertex\", \"domain\": \"GTT\","
         " \"size\": 4096},\n"
         "    {\"slot\": 3, \"role\": \"color\", \"domain\": \"GTT\","
         " \"size\": %u}\n"
         "  ]\n"
         "}\n",
         (unsigned)R300_TRIANGLE_COLOR_BYTES,
         (unsigned)R300_TRIANGLE_COLOR_BYTES);
      rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);
      char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, ib_blake3_hex);
      char manifest[512];
      int manifest_len = snprintf(
         manifest, sizeof(manifest),
         "{\n"
         "  \"schema\": \"r300-tcl-bypass-cell/1\",\n"
         "  \"cell_kind\": \"two-pass-render-shapes-bound\",\n"
         "  \"emitter\": \"r300_tcl_bypass_triangle\",\n"
         "  \"ib_dwords\": %u,\n"
         "  \"ib_blake3\": \"%s\",\n"
         "  \"second_vertex_index\": 2,\n"
         "  \"second_color_index\": 3\n"
         "}\n",
         cell.ib_size_dwords, ib_blake3_hex);
      rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);
      r300_tcl_bypass_triangle_release(&cell);
      if (rc == 0)
         printf("r300_triangle_manifest: wrote the two-pass ib.bin, "
                "bo_table.json, manifest.json to %s\n",
                dir);
      return rc;
   }

   /* The manifest hashes the reference fragment binary separately, so it
    * resolves the same binary the reference emission bakes into the IB.
    */
   struct r300_fragment_binary fs;
   if ((varying ? r300_tcl_bypass_triangle_varying_fs(&fs)
                : r300_tcl_bypass_triangle_reference_fs(&fs)) != 0) {
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
   if (r300_tcl_bypass_triangle_family_emit(R300_TRIANGLE_TARGET_WIDTH,
                                            R300_TRIANGLE_TARGET_HEIGHT,
                                            varying, triangles, &cell) != 0) {
      fprintf(stderr, "triangle emission failed\n");
      r300_fragment_binary_finish(&fs);
      return 1;
   }

   /* ib.bin carries the canonical little-endian encoding
    * (r300_triangle_ib_serialize), so the file matches the digest below and
    * the offline replay reads the same bytes on every host.
    */
   uint8_t *ib_bytes = malloc(cell.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_tcl_bypass_triangle_release(&cell);
      r300_fragment_binary_finish(&fs);
      return 1;
   }
   r300_triangle_ib_serialize(cell.ib, cell.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       cell.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   char bo_table[512];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"vertex\", \"domain\": \"GTT\","
      " \"size\": 4096},\n"
      "    {\"slot\": %u, \"role\": \"color\", \"domain\": \"GTT\","
      " \"size\": %u}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_TRIANGLE_SLOT_VERTEX,
      (unsigned)R300_TRIANGLE_SLOT_COLOR, (unsigned)R300_TRIANGLE_COLOR_BYTES);
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
      "  \"cell_kind\": \"%s\",\n"
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
      triangles > 1 ? "contract-prefixed-instanced"
      : varying     ? "contract-prefixed-varying"
                    : "contract-prefixed-successor",
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
