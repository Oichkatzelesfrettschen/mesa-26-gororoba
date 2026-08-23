/*
 * SPDX-License-Identifier: MIT
 *
 * Writes the reference compute identity carrier pass as an offline
 * replay bundle: ib.bin in the canonical little-endian encoding and a
 * manifest naming the dword count, digest, and the three relocation
 * roles (carrier written, slot read, source read).
 */

#include "r300_compute_identity_carrier.h"
#include "r300_tcl_bypass_triangle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
write_file(const char *dir, const char *name, const void *data, size_t size)
{
   char path[4096];
   const int length = snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (length <= 0 || (size_t)length >= sizeof(path))
      return 1;
   FILE *f = fopen(path, "wb");
   if (f == NULL)
      return 1;
   const size_t written = fwrite(data, 1, size, f);
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

   struct r300_r2vb_fetched_producer_ib pass;
   if (r300_compute_identity_carrier_reference_emit(&pass) != 0) {
      fprintf(stderr, "compute identity carrier emission failed\n");
      return 1;
   }
   uint8_t *bytes = malloc(pass.ib_size_dwords * sizeof(uint32_t));
   if (bytes == NULL) {
      r300_r2vb_fetched_producer_release(&pass);
      return 1;
   }
   r300_triangle_ib_serialize(pass.ib, pass.ib_size_dwords, bytes);
   int rc = write_file(dir, "ib.bin", bytes,
                       pass.ib_size_dwords * sizeof(uint32_t));
   free(bytes);

   char hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(pass.ib, pass.ib_size_dwords, hex);
   char manifest[1024];
   const int manifest_len = snprintf(
      manifest, sizeof(manifest),
      "{\n"
      "  \"schema\": \"r300-compute-identity-carrier/1\",\n"
      "  \"cell_kind\": \"compute-identity-carrier\",\n"
      "  \"emitter\": \"r300_compute_identity_carrier\",\n"
      "  \"record_count\": %u,\n"
      "  \"ib_dwords\": %u,\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"reloc_sites\": [\n"
      "    {\"index\": %u, \"role\": \"carrier\", \"domain\": \"GTT\","
      " \"write\": true},\n"
      "    {\"index\": %u, \"role\": \"slot\", \"domain\": \"GTT\","
      " \"write\": false},\n"
      "    {\"index\": %u, \"role\": \"source\", \"domain\": \"GTT\","
      " \"write\": false}\n"
      "  ]\n"
      "}\n",
      R300_COMPUTE_IDENTITY_CARRIER_REFERENCE_RECORDS, pass.ib_size_dwords,
      hex, pass.reloc_sites[0].ib_index, pass.reloc_sites[1].ib_index,
      pass.reloc_sites[2].ib_index);
   if (manifest_len <= 0 || (size_t)manifest_len >= sizeof(manifest))
      rc = 1;
   else
      rc |= write_file(dir, "manifest.json", manifest, (size_t)manifest_len);
   r300_r2vb_fetched_producer_release(&pass);
   if (rc == 0)
      printf("r300_compute_identity_carrier_manifest: wrote ib.bin and "
             "manifest.json to %s\n", dir);
   return rc;
}
