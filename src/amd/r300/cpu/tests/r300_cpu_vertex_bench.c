/*
 * SPDX-License-Identifier: MIT
 *
 * Timing bench for the CPU vertex gather: baseline versus the SSE2 and
 * SSE3 candidates, with a memcpy lane as the copy-bandwidth ceiling.
 *
 * The bench decides the auto-dispatch selection on the target host: a
 * tuned candidate stays only where it beats the portable baseline by a
 * margin the copy ceiling shows is real.  Each lane verifies its output
 * against the baseline before any timing, so a lane's row is evidence
 * about the implementation its label names.  Results are host-specific
 * and belong in an evidence bundle, not in a meson test verdict; the
 * one meson-visible property is that the bench runs to completion.
 *
 * Usage: r300_cpu_vertex_bench [reps]
 *   reps: timing repetitions per row (default 9); the row reports the
 *   best rep, which is the least-preempted one.
 *
 * Output: TSV rows
 *   implementation  format  stride  vertex_count  reps  best_ns_per_vertex
 */

#include "r300_cpu_vertex.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VERTICES (1u << 18)

typedef int (*gather_fn)(int, const struct r300_cpu_vertex_stream *,
                         uint32_t, uint32_t, uint32_t *, uint32_t);

/* The memcpy ceiling: one bulk copy of the bytes the F32_4 packed
 * gather moves.  It obeys no format semantics, so it runs only on that
 * shape and bounds what any gather implementation can reach.
 */
static int
gather_memcpy_ceiling(int format_id,
                      const struct r300_cpu_vertex_stream *stream,
                      uint32_t first_vertex, uint32_t vertex_count,
                      uint32_t *carrier, uint32_t carrier_dwords)
{
   if (format_id != R300_VERTEX_FORMAT_F32_4 || stream->stride != 16)
      return -EINVAL;
   if (vertex_count > carrier_dwords / 4)
      return -ENOSPC;
   memcpy(carrier, stream->data + (uint64_t)first_vertex * 16,
          (uint64_t)vertex_count * 16);
   return 0;
}

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
fill_records(uint8_t *data, uint64_t bytes, uint32_t seed)
{
   uint32_t state = seed;
   for (uint64_t i = 0; i < bytes; i++) {
      state = state * 1664525u + 1013904223u;
      data[i] = (uint8_t)(state >> 24);
   }
}

static uint8_t *records;
static uint32_t *carrier;
static uint32_t *reference;

static void
bench_row(const char *label, gather_fn fn, int format_id,
          uint32_t stride, uint32_t vertex_count, unsigned reps)
{
   const struct r300_cpu_vertex_stream stream = {
      .data = records,
      .stride = stride,
      .size_bytes = (uint64_t)MAX_VERTICES * stride,
   };

   /* Availability probe and pre-timing calibration: the lane's bytes
    * equal the baseline's before its clock starts.
    */
   int rc = fn(format_id, &stream, 0, vertex_count, carrier,
               MAX_VERTICES * 4);
   if (rc == -ENOSYS || rc == -EINVAL)
      return;
   if (rc != 0 ||
       r300_cpu_vertex_gather_baseline(format_id, &stream, 0, vertex_count,
                                       reference, MAX_VERTICES * 4) != 0 ||
       memcmp(carrier, reference, (uint64_t)vertex_count * 16) != 0) {
      fprintf(stderr,
              "calibration failure: %s format %d stride %u count %u\n",
              label, format_id, stride, vertex_count);
      exit(1);
   }

   /* Inner iterations amortize clock granularity for small counts. */
   unsigned inner = vertex_count < 4096 ? 4096 / (vertex_count ? vertex_count : 1)
                                        : 1;
   uint64_t best = UINT64_MAX;
   for (unsigned r = 0; r < reps; r++) {
      uint64_t t0 = now_ns();
      for (unsigned i = 0; i < inner; i++)
         fn(format_id, &stream, 0, vertex_count, carrier,
            MAX_VERTICES * 4);
      uint64_t dt = now_ns() - t0;
      if (dt < best)
         best = dt;
   }
   double per_vertex =
      (double)best / ((double)inner * (double)vertex_count);
   printf("%s\t%d\t%" PRIu32 "\t%" PRIu32 "\t%u\t%.3f\n", label, format_id,
          stride, vertex_count, reps, per_vertex);
}

int
main(int argc, char **argv)
{
   unsigned reps = 9;
   if (argc > 1) {
      long parsed = strtol(argv[1], NULL, 10);
      if (parsed < 1 || parsed > 1000) {
         fprintf(stderr, "reps must be 1..1000\n");
         return 1;
      }
      reps = (unsigned)parsed;
   }

   records = malloc((uint64_t)MAX_VERTICES * 32);
   carrier = malloc((uint64_t)MAX_VERTICES * 16);
   reference = malloc((uint64_t)MAX_VERTICES * 16);
   if (records == NULL || carrier == NULL || reference == NULL) {
      fprintf(stderr, "allocation failure\n");
      return 1;
   }
   fill_records(records, (uint64_t)MAX_VERTICES * 32, 0xbe4c0000u);

   printf("implementation\tformat\tstride\tvertex_count\treps\t"
          "best_ns_per_vertex\n");

   static const struct { const char *label; gather_fn fn; } lanes[] = {
      { "baseline", r300_cpu_vertex_gather_baseline },
      { "sse2", r300_cpu_vertex_gather_sse2 },
      { "sse3", r300_cpu_vertex_gather_sse3 },
      { "memcpy-ceiling", gather_memcpy_ceiling },
   };
   static const int formats[] = {
      R300_VERTEX_FORMAT_F32_4,
      R300_VERTEX_FORMAT_F32_3,
      R300_VERTEX_FORMAT_F32_2,
   };
   static const uint32_t counts[] = { 3, 4096, MAX_VERTICES };

   for (unsigned l = 0; l < 4; l++) {
      for (unsigned f = 0; f < 3; f++) {
         const struct r300_vertex_format_semantics *format =
            r300_vertex_format_semantics(
               (enum r300_vertex_format_id)formats[f]);
         uint32_t strides[2] = { format->semantic_record_bytes,
                                 format->semantic_record_bytes + 8 };
         for (unsigned s = 0; s < 2; s++) {
            for (unsigned c = 0; c < 3; c++) {
               /* The padded stride at the max count would read past the
                * record area; the bound refuses it, so skip the shape.
                */
               if ((uint64_t)counts[c] * strides[s] >
                   (uint64_t)MAX_VERTICES * 32)
                  continue;
               bench_row(lanes[l].label, lanes[l].fn, formats[f],
                         strides[s], counts[c], reps);
            }
         }
      }
   }

   free(records);
   free(carrier);
   free(reference);
   return 0;
}
