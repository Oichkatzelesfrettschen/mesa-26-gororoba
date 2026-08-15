/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the R2VB FLOAT_4 + FLOAT_2 tuple pass: retained
 * no-submit evidence in the exact form the offline kernel-parser
 * replays consume.
 */

#include "r300_first_draw_state.h"
#include "r300_r2vb_carrier_delivery.h"
#include "r300_r2vb_float2_tuple_pass.h"
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
   if (argc != 2) {
      fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
      return 2;
   }
   const char *dir = argv[1];

   struct r300_r2vb_float2_tuple_ib pass;
   if (r300_r2vb_float2_tuple_reference_emit(&pass) != 0) {
      fprintf(stderr, "tuple-pass emission failed\n");
      return 1;
   }
   if (r300_r2vb_float2_tuple_pass_validate_reloc_sites(&pass) != 0) {
      fprintf(stderr, "tuple-pass relocation sites invalid\n");
      r300_r2vb_float2_tuple_pass_release(&pass);
      return 1;
   }

   uint8_t *ib_bytes = malloc(pass.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_r2vb_float2_tuple_pass_release(&pass);
      return 1;
   }
   r300_triangle_ib_serialize(pass.ib, pass.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       pass.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   /* The vertex stream both LOAD_VBPNTR arrays fetch: slot positions
    * then model records, the bytes an attended run uploads before
    * submission.
    */
   uint8_t vertex_bytes[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *
                        (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +
                         R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES)];
   if (r300_r2vb_float2_tuple_vertex_stream(
          r300_r2vb_float2_tuple_reference_records,
          R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_bytes,
          (uint32_t)sizeof(vertex_bytes)) != 0) {
      fprintf(stderr, "vertex stream serialization failed\n");
      r300_r2vb_float2_tuple_pass_release(&pass);
      return 1;
   }
   rc |= write_file(dir, "vertex.bin", vertex_bytes, sizeof(vertex_bytes));

   struct r300_r2vb_producer_layout layout;
   if (r300_r2vb_producer_layout_single_row(
          R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, &layout) != 0) {
      r300_r2vb_float2_tuple_pass_release(&pass);
      return 1;
   }

   const uint32_t carrier_size_bytes =
      layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;

   char bo_table[768];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": %u, \"role\": \"carrier\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": %u, \"size\": %u},\n"
      "    {\"slot\": %u, \"role\": \"vertex\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": 0, \"size\": %u}\n"
      "  ]\n"
      "}\n",
      (unsigned)R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER, RADEON_GEM_DOMAIN_GTT,
      RADEON_GEM_DOMAIN_GTT, carrier_size_bytes,
      (unsigned)R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX, RADEON_GEM_DOMAIN_GTT,
      (unsigned)sizeof(vertex_bytes));
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(pass.ib, pass.ib_size_dwords,
                               ib_blake3_hex);

   char sites[192];
   int sites_len = 0;
   for (uint32_t i = 0; i < pass.reloc_site_count; i++) {
      sites_len += snprintf(&sites[sites_len],
                            sizeof(sites) - (size_t)sites_len,
                            "%s{\"slot\": %u, \"ib_index\": %u}",
                            i == 0 ? "" : ", ", pass.reloc_sites[i].slot,
                            pass.reloc_sites[i].ib_index);
   }

   uint32_t expected[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 4];
   if (r300_r2vb_float2_tuple_reference_expected(
          expected, (uint32_t)ARRAY_SIZE(expected)) != 0) {
      fprintf(stderr, "tuple identity delivery failed\n");
      r300_r2vb_float2_tuple_pass_release(&pass);
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
      "  \"schema\": \"r300-r2vb-float2-tuple-pass/1\",\n"
      "  \"emitter\": \"r300_r2vb_float2_tuple_pass\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"vertex_count\": %u,\n"
      "  \"vap_vtx_size_dwords\": %u,\n"
      "  \"vertex_size_bytes\": %u,\n"
      "  \"carrier_pitch_pixels\": %u,\n"
      "  \"carrier_height\": %u,\n"
      "  \"carrier_cpp_bytes\": %u,\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"carrier_poison_dword\": \"0x%08x\",\n"
      "  \"expected_carrier_dwords\": [%s]\n"
      "}\n",
      pass.ib_size_dwords, ib_blake3_hex, sites, layout.count,
      R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS,
      (unsigned)sizeof(vertex_bytes), layout.pitch_pixels, layout.height,
      R300_R2VB_PRODUCER_CPP_BYTES, carrier_size_bytes,
      R300_R2VB_PRODUCER_POISON_DWORD, carrier);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_r2vb_float2_tuple_pass_release(&pass);

   if (rc == 0) {
      printf("r300_r2vb_float2_tuple_manifest: wrote ib.bin, vertex.bin, "
             "bo_table.json, manifest.json to %s\n",
             dir);
   }
   return rc;
}
