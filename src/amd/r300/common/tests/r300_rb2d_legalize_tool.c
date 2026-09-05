/*
 * SPDX-License-Identifier: MIT
 *
 * Emits one legalized RB2D fill stream for the kernel-replay differential.
 *
 * Usage: r300_rb2d_legalize_tool <offset> <size> <bo_size> <contract:v1|v2>
 *            <evidence:planned|host|replay|silicon> <pitch|0> <out-dir>
 *
 * Writes out-dir/ib.bin (little-endian dwords), out-dir/bundle.txt (the
 * replay tool's bundle: family and one destination object of bo_size
 * bytes), and out-dir/windows.txt (one line per rectangle: base pitch cpp
 * x y width height), and prints the legalization result on stdout.  Exit
 * 0 on a legalized stream, 1 on a legalize refusal (the refusal is
 * printed), 2 on usage or I/O failure.
 */

#include "r300_rb2d_legalize.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct r300_rb2d_window windows[R300_RB2D_LEGALIZE_MAX_WINDOWS];
static uint32_t words[R300_RB2D_LEGALIZE_MAX_WINDOWS * 64u];

static int
parse_u64(const char *s, uint64_t *out)
{
   char *end = NULL;
   errno = 0;
   const unsigned long long v = strtoull(s, &end, 0);
   if (errno != 0 || end == s || *end != '\0')
      return -1;
   *out = v;
   return 0;
}

static int
write_file(const char *dir, const char *name, const void *data, size_t len)
{
   char path[4096];
   if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
      return -1;
   FILE *f = fopen(path, "wb");
   if (f == NULL)
      return -1;
   const int ok = fwrite(data, 1, len, f) == len;
   return fclose(f) == 0 && ok ? 0 : -1;
}

int
main(int argc, char **argv)
{
   if (argc != 8) {
      fprintf(stderr, "usage: %s <offset> <size> <bo_size> <v1|v2> "
                      "<planned|host|replay|silicon> <pitch|0> <out-dir>\n",
              argv[0]);
      return 2;
   }
   uint64_t offset, size, bo_size, pitch;
   if (parse_u64(argv[1], &offset) || parse_u64(argv[2], &size) ||
       parse_u64(argv[3], &bo_size) || parse_u64(argv[6], &pitch) ||
       pitch > UINT32_MAX) {
      fprintf(stderr, "malformed number\n");
      return 2;
   }
   enum r300_rb2d_contract contract;
   if (strcmp(argv[4], "v1") == 0)
      contract = R300_RB2D_CONTRACT_CONST_FILL_V1;
   else if (strcmp(argv[4], "v2") == 0)
      contract = R300_RB2D_CONTRACT_CONST_FILL_V2;
   else
      return 2;
   enum r300_rb2d_pitch_evidence_class evidence;
   if (strcmp(argv[5], "planned") == 0)
      evidence = R300_RB2D_PITCH_EVIDENCE_PLANNED;
   else if (strcmp(argv[5], "host") == 0)
      evidence = R300_RB2D_PITCH_EVIDENCE_HOST_MODEL;
   else if (strcmp(argv[5], "replay") == 0)
      evidence = R300_RB2D_PITCH_EVIDENCE_KERNEL_REPLAY;
   else if (strcmp(argv[5], "silicon") == 0)
      evidence = R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT;
   else
      return 2;

   const struct r300_rb2d_legalize_request req = {
      .byte_offset = offset,
      .byte_size = size,
      .pattern = 0x11223344u,
      .bo_size = bo_size,
      .usage = R300_RB2D_USAGE_FILL_BUFFER,
      .contract = contract,
      .minimum_evidence = evidence,
      .pinned_pitch_bytes = (uint32_t)pitch,
   };
   struct r300_rb2d_legalize_result result;
   const uint32_t n = r300_rb2d_legalize_linear_span(
      &req, windows, R300_RB2D_LEGALIZE_MAX_WINDOWS, &result);
   if (n == 0u) {
      printf("refusal=%s span_refusal=%s window_refusal=%s\n",
             r300_rb2d_legalize_refusal_name(result.refusal),
             r300_rb2d_span_refusal_name(result.span_refusal),
             r300_rb2d_window_refusal_name(result.window_refusal));
      return 1;
   }

   struct r300_rb2d_legalized_ib ib;
   const int r = r300_rb2d_legalize_emit(windows, n, words,
                                         (uint32_t)(sizeof(words) / 4u), &ib);
   if (r != 0) {
      fprintf(stderr, "emit failed: %d\n", r);
      return 2;
   }

   uint8_t *le = malloc((size_t)ib.ib_size_dwords * 4u);
   if (le == NULL)
      return 2;
   for (uint32_t i = 0; i < ib.ib_size_dwords; i++) {
      le[4 * i + 0] = (uint8_t)(ib.ib[i] & 0xffu);
      le[4 * i + 1] = (uint8_t)((ib.ib[i] >> 8) & 0xffu);
      le[4 * i + 2] = (uint8_t)((ib.ib[i] >> 16) & 0xffu);
      le[4 * i + 3] = (uint8_t)((ib.ib[i] >> 24) & 0xffu);
   }
   char bundle[256];
   const int blen = snprintf(bundle, sizeof(bundle),
                             "family rs480\nbo 0 role=destination size=%llu "
                             "read_domains=0x0 write_domain=0x2\n",
                             (unsigned long long)bo_size);
   char *wtxt = malloc((size_t)result.rect_count * 96u + 1u);
   if (wtxt == NULL)
      return 2;
   size_t at = 0;
   for (uint32_t s = 0; s < n; s++) {
      for (uint32_t k = 0; k < windows[s].rect_count; k++) {
         const struct r300_rb2d_fill_rect *rc = &windows[s].rects[k];
         at += (size_t)sprintf(wtxt + at, "%llu %u %u %u %u %u %u\n",
                               (unsigned long long)windows[s].bo_base,
                               windows[s].pitch_bytes, windows[s].cpp, rc->x,
                               rc->y, rc->width, rc->height);
      }
   }
   if (write_file(argv[7], "ib.bin", le, (size_t)ib.ib_size_dwords * 4u) ||
       write_file(argv[7], "bundle.txt", bundle, (size_t)blen) ||
       write_file(argv[7], "windows.txt", wtxt, at)) {
      fprintf(stderr, "cannot write into %s\n", argv[7]);
      return 2;
   }
   printf("windows=%u rects=%u sites=%u dwords=%u pitch=%u format=%u\n",
          result.window_count, result.rect_count, result.relocation_sites,
          result.ib_dwords, result.pitch_bytes, (unsigned)result.format);
   free(le);
   free(wtxt);
   return 0;
}
