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
 * and belong in an evidence bundle, not in a meson test verdict, so the
 * build registers no test over this executable.
 *
 * The implementations alternate inside each input shape -- shape-major,
 * lane-inner -- so frequency, thermal, and cache drift across the run
 * lands inside every lane's rows rather than between the lanes under
 * comparison.  The base-offset dimension starts the record stream 0 and
 * 4 bytes past the 16-byte-aligned allocation, because the unaligned
 * 16-byte load is the one form the SSE2/SSE3 candidates differ in and
 * an aligned-only corpus never exercises it.
 *
 * Usage: r300_cpu_vertex_bench [reps]
 *   reps: timing repetitions per row (default 9); the row reports the
 *   best rep, which is the least-preempted one.
 *
 * Output: TSV rows
 *   implementation  format  stride  base_offset  vertex_count  reps
 *   best_ns_per_vertex
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

/* Known-bad calibration: a corrupted carrier byte trips the same
 * comparison bench_row uses as its verdict, so the comparison is proven
 * live before any timing row it certifies.
 */
static void
calibrate_known_bad(void)
{
   const struct r300_cpu_vertex_stream stream = {
      .data = records,
      .stride = 16,
      .size_bytes = (uint64_t)MAX_VERTICES * 16,
   };
   if (r300_cpu_vertex_gather_baseline(R300_VERTEX_FORMAT_F32_4, &stream,
                                       0, 64, carrier, MAX_VERTICES * 4) != 0 ||
       r300_cpu_vertex_gather_baseline(R300_VERTEX_FORMAT_F32_4, &stream,
                                       0, 64, reference,
                                       MAX_VERTICES * 4) != 0) {
      fprintf(stderr, "known-bad calibration: baseline gather failed\n");
      exit(1);
   }
   ((uint8_t *)carrier)[17] ^= 0x40;
   if (memcmp(carrier, reference, 64 * 16) == 0) {
      fprintf(stderr,
              "known-bad calibration: corrupted carrier compared equal\n");
      exit(1);
   }
}

static void
bench_row(const char *label, gather_fn fn, int format_id,
          uint32_t stride, uint32_t base_offset, uint32_t vertex_count,
          unsigned reps, int allow_unsupported)
{
   const struct r300_cpu_vertex_stream stream = {
      .data = records + base_offset,
      .stride = stride,
      .size_bytes = (uint64_t)MAX_VERTICES * stride,
   };

   /* Availability probe and pre-timing calibration: the lane's bytes
    * equal the baseline's before its clock starts.  -ENOSYS means the
    * build carries no such instruction set and the lane reports absent.
    * -EINVAL from a tuned lane means the vocabulary row no longer
    * matches the kernel's encoded pattern, which is a calibration
    * failure; only the memcpy ceiling declares shapes outside its
    * contract, and it alone may skip on -EINVAL.
    */
   int rc = fn(format_id, &stream, 0, vertex_count, carrier,
               MAX_VERTICES * 4);
   if (rc == -ENOSYS)
      return;
   if (rc == -EINVAL) {
      if (allow_unsupported)
         return;
      fprintf(stderr,
              "calibration failure: %s refused format %d stride %u\n",
              label, format_id, stride);
      exit(1);
   }
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
   printf("%s\t%d\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%u\t%.3f\n",
          label, format_id, stride, base_offset, vertex_count, reps,
          per_vertex);
}

int
main(int argc, char **argv)
{
   unsigned reps = 9;
   if (argc > 1) {
      char *end = NULL;
      long parsed = strtol(argv[1], &end, 10);
      if (end == argv[1] || *end != '\0' || parsed < 1 || parsed > 1000) {
         fprintf(stderr, "reps must be a whole number 1..1000\n");
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

   calibrate_known_bad();

   /* Decision-grade rows come from optimized codegen with assertions
    * compiled out; any other build is identified so its rows read as
    * smoke output.
    */
#if !defined(NDEBUG) || !defined(__OPTIMIZE__)
   fprintf(stderr,
           "warning: assertions or unoptimized codegen in this build; "
           "rows are smoke output, not dispatch evidence\n");
#endif

   printf("implementation\tformat\tstride\tbase_offset\tvertex_count\t"
          "reps\tbest_ns_per_vertex\n");

   static const struct {
      const char *label;
      gather_fn fn;
      int allow_unsupported;
   } lanes[] = {
      { "baseline", r300_cpu_vertex_gather_baseline, 0 },
      { "sse2", r300_cpu_vertex_gather_sse2, 0 },
      { "sse3", r300_cpu_vertex_gather_sse3, 0 },
      { "memcpy-ceiling", gather_memcpy_ceiling, 1 },
   };
   static const int formats[] = {
      R300_VERTEX_FORMAT_F32_4,
      R300_VERTEX_FORMAT_F32_3,
      R300_VERTEX_FORMAT_F32_2,
      R300_VERTEX_FORMAT_F32_1,
   };
   static const uint32_t counts[] = { 3, 4096, MAX_VERTICES };
   static const uint32_t offsets[] = { 0, 4 };

   for (unsigned f = 0; f < 4; f++) {
      const struct r300_vertex_format_semantics *format =
         r300_vertex_format_semantics(
            (enum r300_vertex_format_id)formats[f]);
      uint32_t strides[2] = { format->semantic_record_bytes,
                              format->semantic_record_bytes + 8 };
      for (unsigned s = 0; s < 2; s++) {
         for (unsigned o = 0; o < 2; o++) {
            for (unsigned c = 0; c < 3; c++) {
               /* The padded stride at the max count would read past the
                * record area; the bound refuses it, so skip the shape.
                */
               if ((uint64_t)counts[c] * strides[s] + offsets[o] >
                   (uint64_t)MAX_VERTICES * 32)
                  continue;
               for (unsigned l = 0; l < 4; l++)
                  bench_row(lanes[l].label, lanes[l].fn, formats[f],
                            strides[s], offsets[o], counts[c], reps,
                            lanes[l].allow_unsupported);
            }
         }
      }
   }

   free(records);
   free(carrier);
   free(reference);
   return 0;
}
