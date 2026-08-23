/*
 * SPDX-License-Identifier: MIT
 *
 * Manifest writer for the fetched GPU-producer route: retained no-submit
 * evidence in the exact form the offline kernel-parser replays consume,
 * plus the IB digest an attended run's authorization declares.
 */

#include "r300_r2vb_fetched_producer.h"
#include "r300_r2vb_producer_pass.h"
#include "r300_tcl_bypass_triangle.h"
#include "r300_vertex_format.h"

#include "drm-uapi/radeon_drm.h"

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

static const char *
role_name(enum r300_r2vb_bo_role role)
{
   switch (role) {
   case R300_R2VB_BO_SLOT:
      return "slot";
   case R300_R2VB_BO_MODEL:
      return "source";
   case R300_R2VB_BO_CARRIER:
      return "carrier";
   case R300_R2VB_BO_COLOR:
      return "color";
   }
   return "unknown";
}

int
main(int argc, char **argv)
{
   if (argc != 2 && argc != 3) {
      fprintf(stderr, "usage: %s <output-directory> [f32_4|f32_3|f32_2]\n",
              argv[0]);
      return 2;
   }
   const char *dir = argv[1];
   const char *width = argc == 3 ? argv[2] : "f32_4";
   int format_id;
   if (strcmp(width, "f32_4") == 0)
      format_id = R300_VERTEX_FORMAT_F32_4;
   else if (strcmp(width, "f32_3") == 0)
      format_id = R300_VERTEX_FORMAT_F32_3;
   else if (strcmp(width, "f32_2") == 0)
      format_id = R300_VERTEX_FORMAT_F32_2;
   else {
      fprintf(stderr, "unknown width %s\n", width);
      return 2;
   }
   const struct r300_vertex_format_semantics *fmt =
      r300_vertex_format_semantics((enum r300_vertex_format_id)format_id);

   struct r300_r2vb_fetched_route_ib route;
   if (r300_r2vb_fetched_route_reference_compose(format_id, &route) != 0) {
      fprintf(stderr, "fetched route composition failed\n");
      return 1;
   }

   uint8_t *ib_bytes = malloc(route.ib_size_dwords * sizeof(uint32_t));
   if (ib_bytes == NULL) {
      fprintf(stderr, "ib.bin serialization allocation failed\n");
      r300_r2vb_fetched_route_release(&route);
      return 1;
   }
   r300_triangle_ib_serialize(route.ib, route.ib_size_dwords, ib_bytes);
   int rc = write_file(dir, "ib.bin", ib_bytes,
                       route.ib_size_dwords * sizeof(uint32_t));
   free(ib_bytes);

   /* The four allocations the transport makes, in relocation-chunk
    * order: the carrier the producer writes and the consumer fetches,
    * the triangle's 64-pixel-pitch B8G8R8A8 target with its canary row,
    * the one-page slot-position BO, and the one-page source BO.
    */
   struct r300_r2vb_producer_layout layout;
   if (r300_r2vb_producer_layout_single_row(
          R300_R2VB_PRODUCER_REFERENCE_COUNT, &layout) != 0) {
      r300_r2vb_fetched_route_release(&route);
      return 1;
   }
   const uint32_t carrier_size_bytes =
      layout.pitch_pixels * layout.height * R300_R2VB_PRODUCER_CPP_BYTES;

   char bo_table[1024];
   int bo_table_len = snprintf(
      bo_table, sizeof(bo_table),
      "{\n"
      "  \"slots\": [\n"
      "    {\"slot\": 0, \"role\": \"carrier\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": %u, \"size\": %u},\n"
      "    {\"slot\": 1, \"role\": \"color\", \"domain\": \"GTT\","
      " \"read_domains\": 0, \"write_domain\": %u, \"size\": %u},\n"
      "    {\"slot\": 2, \"role\": \"slot\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": 0, \"size\": %u},\n"
      "    {\"slot\": 3, \"role\": \"source\", \"domain\": \"GTT\","
      " \"read_domains\": %u, \"write_domain\": 0, \"size\": %u}\n"
      "  ]\n"
      "}\n",
      RADEON_GEM_DOMAIN_GTT, RADEON_GEM_DOMAIN_GTT, carrier_size_bytes,
      RADEON_GEM_DOMAIN_GTT, (unsigned)R300_TRIANGLE_COLOR_BYTES,
      RADEON_GEM_DOMAIN_GTT, R300_R2VB_FETCHED_REFERENCE_BO_BYTES,
      RADEON_GEM_DOMAIN_GTT, R300_R2VB_FETCHED_REFERENCE_BO_BYTES);
   rc |= write_file(dir, "bo_table.json", bo_table, (size_t)bo_table_len);

   char ib_blake3_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords,
                               ib_blake3_hex);

   char sites[512];
   int sites_len = 0;
   for (uint32_t i = 0; i < route.composition.reloc_count; i++) {
      const struct r300_pm4_composed_reloc *r = &route.composition.relocs[i];
      sites_len += snprintf(&sites[sites_len],
                            sizeof(sites) - (size_t)sites_len,
                            "%s{\"role\": \"%s\", \"ib_index\": %u, "
                            "\"read_domains\": %u, \"write_domain\": %u}",
                            i == 0 ? "" : ", ", role_name(r->role),
                            r->ib_index, r->read_domains, r->write_domain);
   }

   char manifest[2048];
   int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-r2vb-fetched-route/1\",\n"
      "  \"emitter\": \"r300_r2vb_fetched_route\",\n"
      "  \"source_format\": \"%s\",\n"
      "  \"source_record_bytes\": %u,\n"
      "  \"source_stride_bytes\": %u,\n"
      "  \"source_fetch_dwords\": %u,\n"
      "  \"slot_record_bytes\": %u,\n"
      "  \"vertex_count\": %u,\n"
      "  \"ib_dwords\": %u,\n"
      "  \"consumer_start_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [%s],\n"
      "  \"carrier_size_bytes\": %u,\n"
      "  \"carrier_poison_dword\": \"0x%08x\",\n"
      "  \"color_size_bytes\": %u,\n"
      "  \"color_sentinel_dword\": \"0x%08x\"\n"
      "}\n",
      width, fmt->semantic_record_bytes, fmt->semantic_record_bytes,
      fmt->hardware_fetch_dwords,
      (unsigned)R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES,
      (unsigned)R300_R2VB_PRODUCER_REFERENCE_COUNT, route.ib_size_dwords,
      route.consumer_start_dwords, ib_blake3_hex, sites, carrier_size_bytes,
      R300_R2VB_PRODUCER_POISON_DWORD, (unsigned)R300_TRIANGLE_COLOR_BYTES,
      R300_TRIANGLE_COLOR_SENTINEL);
   rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);

   r300_r2vb_fetched_route_release(&route);

   if (rc == 0) {
      printf("r300_r2vb_fetched_route_manifest: wrote ib.bin, "
             "bo_table.json, manifest.json (%s) to %s\n",
             width, dir);
   }
   return rc;
}
