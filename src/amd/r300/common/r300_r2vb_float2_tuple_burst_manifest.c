/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the R2VB FLOAT_2 tuple burst pass: retained
 * no-submit evidence in the exact form the offline kernel-parser
 * replays consume, one carrier row per member.
 */

#include "r300_r2vb_float2_tuple_pass.h"
#include "r300_r2vb_producer_pass.h"
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
   if (argc != 3) {
      fprintf(stderr, "usage: %s <output-directory> <draws-1-to-64>\n",
              argv[0]);
      return 2;
   }
   const char *dir = argv[1];
   char *end = NULL;
   unsigned long draws_arg = strtoul(argv[2], &end, 10);
   if (end == argv[2] || *end != '\0' || draws_arg < 1 ||
       draws_arg > R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS) {
      fprintf(stderr, "draws outside 1..%u\n",
              R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS);
      return 2;
   }
   const uint32_t draws = (uint32_t)draws_arg;

   struct r300_r2vb_float2_tuple_burst_ib burst;
   if (r300_r2vb_float2_tuple_burst_reference_emit(draws, &burst) != 0) {
      fprintf(stderr, "burst emission failed\n");
      return 1;
   }
   if (r300_r2vb_float2_tuple_burst_validate_reloc_sites(&burst) != 0) {
      fprintf(stderr, "burst relocation sites invalid\n");
      r300_r2vb_float2_tuple_burst_release(&burst);
      return 1;
   }

   uint8_t *ib_bytes = malloc(burst.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_r2vb_float2_tuple_burst_release(&burst);
      return 1;
   }
   r300_triangle_ib_serialize(burst.ib, burst.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       burst.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   uint8_t vertex_bytes[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT *
                        (R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES +
                         R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES)];
   if (r300_r2vb_float2_tuple_vertex_stream(
          r300_r2vb_float2_tuple_reference_records,
          R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT, vertex_bytes,
          (uint32_t)sizeof(vertex_bytes)) != 0) {
      fprintf(stderr, "vertex stream serialization failed\n");
      r300_r2vb_float2_tuple_burst_release(&burst);
      return 1;
   }
   rc |= write_file(dir, "vertex.bin", vertex_bytes, sizeof(vertex_bytes));

   uint32_t member_stride = 0;
   if (r300_r2vb_float2_tuple_burst_member_stride_bytes(&member_stride) !=
       0) {
      r300_r2vb_float2_tuple_burst_release(&burst);
      return 1;
   }
   const uint32_t carrier_size_bytes = draws * member_stride;

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
   r300_triangle_ib_digest_hex(burst.ib, burst.ib_size_dwords,
                               ib_blake3_hex);

   char starts[1024];
   int starts_len = 0;
   for (uint32_t m = 0; m < draws; m++) {
      starts_len += snprintf(&starts[starts_len],
                             sizeof(starts) - (size_t)starts_len, "%s%u",
                             m == 0 ? "" : ", ", burst.member_start[m]);
      if ((size_t)starts_len >= sizeof(starts) - 16) {
         fprintf(stderr, "member-start serialization overflow\n");
         r300_r2vb_float2_tuple_burst_release(&burst);
         return 1;
      }
   }

   uint32_t expected[R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT * 4];
   if (r300_r2vb_float2_tuple_reference_expected(
          expected, (uint32_t)ARRAY_SIZE(expected)) != 0) {
      fprintf(stderr, "tuple identity delivery failed\n");
      r300_r2vb_float2_tuple_burst_release(&burst);
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

   char manifest[3072];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-r2vb-float2-tuple-burst-pass/1\",\n"
      "  \"emitter\": \"r300_r2vb_float2_tuple_burst\",\n"
      "  \"draws\": %u,\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"member_start_dwords\": [%s],\n"
      "  \"member_stride_bytes\": %u,\n"
      "  \"vertex_count\": %u,\n"
      "  \"vap_vtx_size_dwords\": %u,\n"
      "  \"vertex_size_bytes\": %u,\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"carrier_poison_dword\": \"0x%08x\",\n"
      "  \"expected_member_dwords\": [%s]\n"
      "}\n",
      draws, burst.ib_size_dwords, ib_blake3_hex, starts, member_stride,
      R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT,
      R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS,
      (unsigned)sizeof(vertex_bytes), carrier_size_bytes,
      R300_R2VB_PRODUCER_POISON_DWORD, carrier);
   if (manifest_len <= 0 || (size_t)manifest_len >= sizeof(manifest)) {
      fprintf(stderr, "manifest serialization overflow\n");
      r300_r2vb_float2_tuple_burst_release(&burst);
      return 1;
   }
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_r2vb_float2_tuple_burst_release(&burst);

   if (rc == 0) {
      printf("r300_r2vb_float2_tuple_burst_manifest: wrote ib.bin, "
             "vertex.bin, bo_table.json, manifest.json (draws=%u) to %s\n",
             draws, dir);
   }
   return rc;
}
