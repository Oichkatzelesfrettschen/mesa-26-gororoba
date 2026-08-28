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
 * The lanes rotate inside every timing repetition of each input shape,
 * so frequency, thermal, and cache drift across the run lands inside
 * every lane's repetition set rather than between the lanes under
 * comparison.  The base-offset dimension starts the record stream 0,
 * 4, 8, and 12 bytes past the 16-byte-aligned allocation -- every
 * four-byte-aligned residue a packed stream can carry -- because the
 * unaligned 16-byte load is the one form the SSE2/SSE3 candidates
 * differ in and an aligned-only corpus never exercises it.
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

#include "git_sha1.h"

#ifdef R300_CPU_VERTEX_BENCH_REQUIRE_K8
#include <cpuid.h>
#endif

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VERTICES (1u << 18)

typedef int (*gather_fn)(int, const struct r300_vertex_stream *,
                         uint32_t, uint32_t, uint32_t *, uint32_t);

/* The memcpy ceiling: one bulk copy of the bytes the F32_4 packed
 * gather moves.  It obeys no format semantics, so it runs only on that
 * shape and bounds what any gather implementation can reach.
 */
static int
gather_memcpy_ceiling(int format_id,
                      const struct r300_vertex_stream *stream,
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
   const struct r300_vertex_stream stream = {
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

/* Availability probe and pre-timing calibration: the lane's bytes equal
 * the baseline's before its clock starts.  -ENOSYS means the build
 * carries no such instruction set and the lane reports absent.  -EINVAL
 * from a tuned lane means the vocabulary row no longer matches the
 * kernel's encoded pattern, which is a calibration failure; only the
 * memcpy ceiling declares shapes outside its contract, and it alone may
 * skip on -EINVAL.  Returns 1 for a timeable lane and 0 for a skipped
 * one.
 */
static int
calibrate_lane(const char *label, gather_fn fn, int format_id,
               const struct r300_vertex_stream *stream,
               uint32_t vertex_count, int allow_unsupported)
{
   int rc = fn(format_id, stream, 0, vertex_count, carrier,
               MAX_VERTICES * 4);
   if (rc == -ENOSYS)
      return 0;
   if (rc == -EINVAL) {
      if (allow_unsupported)
         return 0;
      fprintf(stderr,
              "calibration failure: %s refused format %d stride %u\n",
              label, format_id, stream->stride);
      exit(1);
   }
   if (rc != 0 ||
       r300_cpu_vertex_gather_baseline(format_id, stream, 0, vertex_count,
                                       reference, MAX_VERTICES * 4) != 0 ||
       memcmp(carrier, reference, (uint64_t)vertex_count * 16) != 0) {
      fprintf(stderr,
              "calibration failure: %s format %d stride %u count %u\n",
              label, format_id, stream->stride, vertex_count);
      exit(1);
   }
   return 1;
}

static uint64_t
time_one_rep(gather_fn fn, int format_id,
             const struct r300_vertex_stream *stream,
             uint32_t vertex_count, unsigned inner)
{
   uint64_t t0 = now_ns();
   for (unsigned i = 0; i < inner; i++)
      fn(format_id, stream, 0, vertex_count, carrier, MAX_VERTICES * 4);
   return now_ns() - t0;
}

#define LANE_COUNT 4

struct bench_lane {
   const char *label;
   gather_fn fn;
   int allow_unsupported;
};

/* One input-shape cell across every lane: calibrate each lane, then
 * rotate the lanes inside every repetition, so frequency, thermal, and
 * scheduler drift lands within each lane's repetition set rather than
 * between the lanes under comparison; each lane's row reports its best
 * repetition.
 */
static void
bench_shape(const struct bench_lane *lanes, int format_id, uint32_t stride,
            uint32_t base_offset, uint32_t vertex_count, unsigned reps)
{
   const struct r300_vertex_stream stream = {
      .data = records + base_offset,
      .stride = stride,
      .size_bytes = (uint64_t)MAX_VERTICES * stride,
   };

   /* The rotation runs over the compact set of lanes this shape can
    * time, so every present lane leads equally often whatever slots
    * calibrated absent.
    */
   unsigned present[LANE_COUNT];
   unsigned present_count = 0;
   uint64_t best[LANE_COUNT];
   for (unsigned l = 0; l < LANE_COUNT; l++) {
      if (calibrate_lane(lanes[l].label, lanes[l].fn, format_id, &stream,
                         vertex_count, lanes[l].allow_unsupported))
         present[present_count++] = l;
      best[l] = UINT64_MAX;
   }
   if (present_count == 0)
      return;

   /* Inner iterations amortize clock granularity for small counts. */
   unsigned inner = vertex_count < 4096 ? 4096 / (vertex_count ? vertex_count : 1)
                                        : 1;
   /* The starting lane rotates with the repetition index, and the
    * repetition count rounds up to whole rotation cycles, so every
    * present lane leads exactly rounded / present_count times and no
    * lane occupies a fixed phase of the repetition; a partial cycle
    * would hand the earlier lanes an extra lead.  The row reports the
    * executed count.
    */
   unsigned rounded =
      ((reps + present_count - 1) / present_count) * present_count;
   for (unsigned r = 0; r < rounded; r++) {
      for (unsigned i = 0; i < present_count; i++) {
         unsigned l = present[(r + i) % present_count];
         uint64_t dt = time_one_rep(lanes[l].fn, format_id, &stream,
                                    vertex_count, inner);
         if (dt < best[l])
            best[l] = dt;
      }
   }

   for (unsigned i = 0; i < present_count; i++) {
      unsigned l = present[i];
      double per_vertex =
         (double)best[l] / ((double)inner * (double)vertex_count);
      printf("%s\t%d\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu32 "\t%u\t%.3f\n",
             lanes[l].label, format_id, stride, base_offset, vertex_count,
             rounded, per_vertex);
   }
}

int
main(int argc, char **argv)
{
   /* The K8 target binary's rows decide the TL-66 dispatch, and K8
    * codegen on another microarchitecture times that host's pipeline,
    * so the executing CPU must identify as AMD family 0Fh (K8) or the
    * rows mark as smoke output.  The SSE3 feature bit is a hard
    * refusal: the whole binary compiles at -march=k8-sse3, so a host
    * without SSE3 -- an early-revision family 0Fh part -- would fault
    * mid-run instead of producing marked rows.
    */
#ifdef R300_CPU_VERTEX_BENCH_REQUIRE_K8
   {
      unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
      int is_k8 = 0;
      int has_sse3 = 0;
      if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) &&
          ebx == 0x68747541u /* "Auth" */ &&
          edx == 0x69746e65u /* "enti" */ &&
          ecx == 0x444d4163u /* "cAMD" */ &&
          __get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
         unsigned base_family = (eax >> 8) & 0xf;
         unsigned ext_family = (eax >> 20) & 0xff;
         is_k8 = base_family == 0xf && ext_family == 0;
         has_sse3 = (ecx & bit_SSE3) != 0;
      }
      if (!has_sse3) {
         fprintf(stderr,
                 "error: executing CPU lacks SSE3; this binary's "
                 "k8-sse3 codegen would fault\n");
         return 1;
      }
      if (!is_k8) {
         fprintf(stderr,
                 "warning: executing CPU is not AMD family 0Fh (K8); "
                 "rows are smoke output, not dispatch evidence\n");
         printf("# non-K8 host: rows are smoke output, not dispatch "
                "evidence\n");
      }
   }
#endif

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

   /* Qualification rows come from a Meson buildtype=release build:
    * the build system defines R300_CPU_VERTEX_BENCH_MESON_RELEASE
    * there, because compiler macros alone cannot separate release from
    * debugoptimized with b_ndebug.  Any other build marks the row
    * stream itself, so a recorder consuming stdout carries the
    * non-qualification status with the rows.
    */
#if !defined(R300_CPU_VERTEX_BENCH_MESON_RELEASE) || \
   !defined(NDEBUG) || !defined(__OPTIMIZE__)
   fprintf(stderr,
           "warning: this is not a release build with optimized "
           "codegen; rows are smoke output, not dispatch evidence\n");
   printf("# non-release build: rows are smoke output, not dispatch "
          "evidence\n");
#endif

   /* The row stream carries the source identity the binary was built
    * from, so a retained bundle binds the timings to a commit; the
    * clean-tree, declared-SHA, isolated-worktree requirement for a
    * qualification run is procedural and its verdict rides the
    * evidence bundle.
    */
   printf("# source%s\n",
          MESA_GIT_SHA1[0] != '\0' ? MESA_GIT_SHA1 : " unknown");


   /* Absent ISA lanes mark the stream: a tuned entry point compiled out
    * of this build reports -ENOSYS, and the marker line tells a
    * collector this stream is not the three-way result -- a silent
    * omission would let a two-way stream read as complete.
    */
   {
      const struct r300_vertex_stream probe_stream = {
         .data = records,
         .stride = 16,
         .size_bytes = 64,
      };
      static const struct { const char *label; gather_fn fn; } tuned[] = {
         { "sse2", r300_cpu_vertex_gather_sse2 },
         { "sse3", r300_cpu_vertex_gather_sse3 },
      };
      for (unsigned t = 0; t < 2; t++) {
         if (tuned[t].fn(R300_VERTEX_FORMAT_F32_4, &probe_stream, 0, 3,
                         carrier, MAX_VERTICES * 4) == -ENOSYS) {
            printf("# lane absent: %s (build carries no such instruction "
                   "set)\n", tuned[t].label);
         }
      }
   }

   printf("implementation\tformat\tstride\tbase_offset\tvertex_count\t"
          "reps\tbest_ns_per_vertex\n");

   static const struct bench_lane lanes[LANE_COUNT] = {
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
   /* Every four-byte-aligned start a packed stream can carry: the
    * stride preserves the initial alignment across vertices, so these
    * offsets cover each residue of the 16-byte load the SSE2/SSE3
    * candidates differ in.
    */
   static const uint32_t offsets[] = { 0, 4, 8, 12 };

   for (unsigned f = 0; f < 4; f++) {
      const struct r300_vertex_format_semantics *format =
         r300_vertex_format_semantics(
            (enum r300_vertex_format_id)formats[f]);
      uint32_t strides[2] = { format->semantic_record_bytes,
                              format->semantic_record_bytes + 8 };
      for (unsigned s = 0; s < 2; s++) {
         for (unsigned o = 0; o < 4; o++) {
            for (unsigned c = 0; c < 3; c++) {
               /* The padded stride at the max count would read past the
                * record area; the bound refuses it, so skip the shape.
                */
               if ((uint64_t)counts[c] * strides[s] + offsets[o] >
                   (uint64_t)MAX_VERTICES * 32)
                  continue;
               bench_shape(lanes, formats[f], strides[s], offsets[o],
                           counts[c], reps);
            }
         }
      }
   }

   free(records);
   free(carrier);
   free(reference);
   return 0;
}
