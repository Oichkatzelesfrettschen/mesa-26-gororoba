/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the R2VB producer pass: retained no-submit evidence
 * in the exact form the offline kernel-parser replays consume.
 */

#include "r300_first_draw_state.h"
#include "r300_r2vb_carrier_delivery.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_vertex_format.h"

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
   if (argc != 2 && argc != 3) {
      fprintf(stderr, "usage: %s <output-directory> [fp24-sweep]\n",
              argv[0]);
      return 2;
   }
   const char *dir = argv[1];
   /* The optional stream selector mirrors the attended runners': the
    * sweep embeds the FP24 boundary records under the same layout, so
    * the manifest differs only in the IB bytes and the expected dwords.
    */
   int fp24_sweep = 0;
   if (argc == 3) {
      if (strcmp(argv[2], "fp24-sweep") != 0) {
         fprintf(stderr, "unknown stream selector %s\n", argv[2]);
         return 2;
      }
      fp24_sweep = 1;
   }

   struct r300_r2vb_producer_ib pass;
   int emit_rc = fp24_sweep ? r300_r2vb_producer_fp24_sweep_emit(&pass)
                            : r300_r2vb_producer_reference_emit(&pass);
   if (emit_rc != 0) {
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

   struct r300_r2vb_producer_layout layout;
   if (r300_r2vb_producer_layout_single_row(
          R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout) != 0) {
      r300_r2vb_producer_pass_release(&pass);
      return 1;
   }

   /* The carrier holds exactly the slot row the pass scissors and
    * retargets: pitch pixels of one FP32x4 texel each, one row high.
    * The kernel's color-buffer bound reads the same product, so the BO
    * size derives from the layout rather than from an allocation
    * granularity.
    */
   const uint32_t carrier_size_bytes =
      layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;

   /* One carrier BO, read-write in the GTT: the pass writes it through
    * the color backend and the consuming draw fetches it as the vertex
    * stream, so both relocation domains are RADEON_GEM_DOMAIN_GTT.
    */
   char bo_table[512];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"carrier\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": %u, \"size\": %u}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_R2VB_PRODUCER_SLOT_CARRIER, RADEON_GEM_DOMAIN_GTT,
      RADEON_GEM_DOMAIN_GTT, carrier_size_bytes);
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

   /* Expected carrier content: the delivery identity over the same three
    * FLOAT_4 records the selected emission embeds.  The pass writes one
    * slot per vertex, so the expected extent covers layout.count slots
    * and the odd row's padding slot stays outside it.
    */
   uint32_t expected[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   int expected_rc =
      fp24_sweep
         ? r300_r2vb_producer_fp24_sweep_expected(
              expected, (uint32_t)ARRAY_SIZE(expected))
         : r300_r2vb_producer_reference_expected(
              expected, (uint32_t)ARRAY_SIZE(expected));
   if (expected_rc != 0) {
      fprintf(stderr, "carrier identity delivery failed\n");
      r300_r2vb_producer_pass_release(&pass);
      return 1;
   }

   char carrier[512];
   int carrier_len = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
      carrier_len += snprintf(&carrier[carrier_len],
                              sizeof(carrier) - (size_t)carrier_len,
                              "%s\"0x%08x\"", i == 0 ? "" : ", ",
                              expected[i]);
   }

   char manifest[2048];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-r2vb-producer-pass/1\",\n"
      "  \"emitter\": \"r300_r2vb_producer_pass\",\n"
      "  \"stream\": \"%s\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"vertex_count\": %u,\n"
      "  \"carrier_pitch_pixels\": %u,\n"
      "  \"carrier_height\": %u,\n"
      "  \"carrier_cpp_bytes\": %u,\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"carrier_poison_dword\": \"0x%08x\",\n"
      "  \"expected_carrier_dwords\": [%s]\n"
      "}\n",
      fp24_sweep ? "fp24-sweep" : "reference",
      pass.ib_size_dwords, ib_blake3_hex, sites, layout.count,
      layout.pitch_pixels, layout.height, R300_R2VB_PRODUCER_CPP_BYTES,
      carrier_size_bytes, R300_R2VB_PRODUCER_POISON_DWORD, carrier);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_r2vb_producer_pass_release(&pass);

   if (rc == 0) {
      printf("r300_r2vb_producer_manifest: wrote ib.bin, bo_table.json, "
             "manifest.json to %s\n",
             dir);
   }
   return rc;
}
