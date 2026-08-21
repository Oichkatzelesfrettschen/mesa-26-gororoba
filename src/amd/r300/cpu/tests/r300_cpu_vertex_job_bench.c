/*
 * SPDX-License-Identifier: MIT
 *
 * Timing bench for the CPU vertex-job executor: the scalar interpreter
 * against the SIMD execution candidates.
 *
 * The bench screens SIMD candidates inside the CPU vertex-job executor.
 * The scalar interpreter is the authority for the bytes, so every lane
 * verifies its carrier against the interpreter's before its clock starts
 * and a mismatch ends the run.  This microbenchmark reuses one heap
 * allocation.  The native draw path retains one GTT carrier BO with the
 * command buffer; each submission maps, rewrites, publishes, and unmaps that
 * BO, so an end-to-end dispatch decision requires native-path timing.
 * Results are host-specific and belong in an evidence bundle, not in a
 * meson test verdict, so the build registers no test over this executable.
 *
 * A job executor's cost splits between per-vertex dispatch overhead and
 * per-instruction arithmetic, and the two answer differently.  The
 * shape dimension separates them: `identity` is the two-instruction job
 * the experimental R2VB routes accept for their format subsets; `constant`
 * is the minimal CPU-routed job;
 * `affine` and `dp4_chain` carry the arithmetic a lowered vertex shader
 * produces; `producer_max_chain` runs the deepest dependent chain the
 * producer can emit, where per-instruction cost dominates, and its name
 * states that the depth is that ceiling rather than a pick.  A kernel
 * that wins one end and loses the other selects by shape rather than
 * outright, which the rows have to be able to show.
 *
 * The lanes rotate inside every timing repetition of each shape, so
 * frequency, thermal, and cache drift across the run lands inside every
 * lane's repetition set rather than between the lanes under comparison.
 *
 * Usage: r300_cpu_vertex_job_bench [reps]
 *   reps: timing repetitions per row (default 9); the row reports the
 *   best rep, which is the least-preempted one.
 *
 * The stream layout is the other dimension the executors are sensitive
 * to, because both lanes gather through r300_cpu_vertex_gather inside
 * their measured path.  The pipeline admits a zero binding stride and
 * every stride at least the semantic record size, and the draw adds the
 * attribute offset to the stream base, so a tightly packed
 * malloc-aligned stream is one admitted layout among several: a padded
 * stride spreads records across cache lines, a zero stride collapses
 * every vertex onto one record, and a four-byte offset moves the
 * mapped pointer off its 16-byte alignment.
 *
 * Output: TSV rows
 *   implementation  job_shape  route_scope  carrier_state  layout  format
 *   stride  base_offset  vertex_count  instruction_count  reps
 *   best_ns_per_vertex
 */

#include "r300_cpu_vertex_job.h"

#include "git_sha1.h"

/* Host qualification reads CPUID, which exists on x86 alone.  The
 * build targets other architectures too, and there the part cannot be
 * identified at all, so the stream marks itself rather than claiming a
 * qualification it did not perform.
 */
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#define R300_CPU_VERTEX_BENCH_HAVE_CPUID 1
#endif

#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/r300_vertex_job.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VERTICES (1u << 16)
/* The record area holds the widest layout the bench walks: every vertex
 * at the padded stride, plus the largest base offset.
 */
#define MAX_STRIDE 24u
#define MAX_BASE_OFFSET 4u
#define RECORD_SPAN ((uint64_t)MAX_VERTICES * MAX_STRIDE + MAX_BASE_OFFSET)

typedef int (*execute_fn)(const struct r300_vertex_job *,
                          const struct r300_cpu_vertex_stream *,
                          uint32_t, uint32_t, uint32_t *, uint32_t);

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Records carry arbitrary bit patterns rather than tidy floats: arithmetic
 * canonicalizes NaNs, while denormals and ordinary magnitudes still expose
 * a microarchitecture's slow paths.
 */
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

static uint32_t
f_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

/* LOAD_INPUT t0; STORE_POSITION t0 -- the job the public GPU route admits.
 */
static struct r300_vertex_job
identity_job(int format_id)
{
   struct r300_vertex_job job = {
      .input_format_id = format_id,
      .instruction_count = 2,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   return job;
}

/* LOAD_CONSTANT t0; STORE_POSITION t0 -- the smallest job that bypasses the
 * identity-only GPU route and reaches the CPU executor.
 */
static struct r300_vertex_job
constant_job(int format_id)
{
   struct r300_vertex_job job = {
      .input_format_id = format_id,
      .instruction_count = 2,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
      .constant_count = 1,
      .constants = {
         { f_bits(0.25f), f_bits(-0.5f), f_bits(0.75f), f_bits(1.0f) },
      },
   };
   return job;
}

/* position * scale + bias through one FMAD: the smallest job carrying
 * real arithmetic, and the shape a scale-bias vertex shader lowers to.
 */
static struct r300_vertex_job
affine_job(int format_id)
{
   struct r300_vertex_job job = {
      .input_format_id = format_id,
      .instruction_count = 5,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 2, 1, 0, 0 },
         { R300_VERTEX_JOB_OP_FMAD, 3, 0, 1, 2 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 3, 0, 0 },
      },
      .constant_count = 2,
      .constants = {
         { f_bits(2.0f), f_bits(-0.5f), f_bits(4.0f), f_bits(1.0f) },
         { f_bits(0.25f), f_bits(1.5f), f_bits(-2.0f), f_bits(0.0f) },
      },
   };
   return job;
}

/* Four DP4s against four constant rows, summed: the horizontal-reduce
 * shape, where a packed kernel's advantage is smallest because each
 * dot collapses a vector to one broadcast value.
 */
static struct r300_vertex_job
dp4_chain_job(int format_id)
{
   struct r300_vertex_job job = {
      .input_format_id = format_id,
      .instruction_count = 13,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 2, 1, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 3, 2, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 4, 3, 0, 0 },
         { R300_VERTEX_JOB_OP_DP4, 5, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_DP4, 6, 0, 2, 0 },
         { R300_VERTEX_JOB_OP_DP4, 7, 0, 3, 0 },
         { R300_VERTEX_JOB_OP_DP4, 8, 0, 4, 0 },
         { R300_VERTEX_JOB_OP_FADD, 9, 5, 6, 0 },
         { R300_VERTEX_JOB_OP_FADD, 10, 7, 8, 0 },
         { R300_VERTEX_JOB_OP_FADD, 11, 9, 10, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 11, 0, 0 },
      },
      .constant_count = 4,
      .constants = {
         { f_bits(1.0f), f_bits(0.0f), f_bits(0.0f), f_bits(0.5f) },
         { f_bits(0.0f), f_bits(1.0f), f_bits(0.0f), f_bits(-0.5f) },
         { f_bits(0.0f), f_bits(0.0f), f_bits(1.0f), f_bits(0.25f) },
         { f_bits(0.25f), f_bits(0.25f), f_bits(0.25f), f_bits(1.0f) },
      },
   };
   return job;
}

/* A dependent FMAD chain as deep as the producer can emit: each
 * operation reads the previous result, so the row measures
 * per-instruction cost with the interpreter's dispatch amortized across
 * the chain.  r300_vertex_job_from_nir binds every result through
 * bind_new_temp, which allocates monotonically and refuses past the
 * 16-temp file (rg --fixed-strings bind_new_temp src/amd/r300/), so one
 * input plus 15 arithmetic results fills the producer's file.  The input
 * supplies the remaining operands and consumes no constant temp.
 */
static struct r300_vertex_job
producer_max_chain_job(int format_id, uint32_t chain_length)
{
   struct r300_vertex_job job = {
      .input_format_id = format_id,
      .instruction_count = 1,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
      },
   };
   uint8_t previous = 0;
   if (chain_length > R300_VERTEX_JOB_MAX_TEMPS - 1)
      chain_length = R300_VERTEX_JOB_MAX_TEMPS - 1;
   for (uint32_t i = 0; i < chain_length; i++) {
      uint8_t dst = (uint8_t)(1 + i);
      job.instructions[job.instruction_count++] =
         (struct r300_vertex_job_instruction){
            R300_VERTEX_JOB_OP_FMAD, dst, previous, 0, 0,
         };
      previous = dst;
   }
   job.instructions[job.instruction_count++] =
      (struct r300_vertex_job_instruction){
         R300_VERTEX_JOB_OP_STORE_POSITION, 0, previous, 0, 0,
      };
   return job;
}

/* A dependent chain of one opcode, as deep as the producer can emit.
 * r300_vertex_job_nir admits nir_op_fadd and nir_op_fmul independently
 * of nir_op_ffma (rg --fixed-strings nir_op_fadd src/amd/r300/), so a
 * shader lowering to an add-only or multiply-only chain is an admitted
 * job whose executor mix the FMAD chain never times.  Scalar-to-packed
 * cost differs per opcode, so each admitted arithmetic opcode carries
 * its own shape rather than standing in for the others.
 */
static struct r300_vertex_job
opcode_chain_job(int format_id, uint8_t opcode, uint32_t chain_length)
{
   struct r300_vertex_job job = {
      .input_format_id = format_id,
      .instruction_count = 1,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
      },
   };
   uint8_t previous = 0;
   if (chain_length > R300_VERTEX_JOB_MAX_TEMPS - 1)
      chain_length = R300_VERTEX_JOB_MAX_TEMPS - 1;
   for (uint32_t i = 0; i < chain_length; i++) {
      uint8_t dst = (uint8_t)(1 + i);
      job.instructions[job.instruction_count++] =
         (struct r300_vertex_job_instruction){ opcode, dst, previous, 0, 0 };
      previous = dst;
   }
   job.instructions[job.instruction_count++] =
      (struct r300_vertex_job_instruction){
         R300_VERTEX_JOB_OP_STORE_POSITION, 0, previous, 0, 0,
      };
   return job;
}

/* Known-bad calibration: a corrupted carrier byte trips the same
 * comparison the lane calibration uses as its verdict, so the
 * comparison is proven live before any timing row it certifies.
 */
static void
calibrate_known_bad(void)
{
   const struct r300_vertex_job job = identity_job(R300_VERTEX_FORMAT_F32_4);
   const struct r300_cpu_vertex_stream stream = {
      .data = records,
      .stride = 16,
      .size_bytes = RECORD_SPAN,
   };
   if (r300_cpu_vertex_job_execute(&job, &stream, 0, 64, carrier,
                                   MAX_VERTICES * 4) != 0 ||
       r300_cpu_vertex_job_execute(&job, &stream, 0, 64, reference,
                                   MAX_VERTICES * 4) != 0) {
      fprintf(stderr, "known-bad calibration: scalar execution failed\n");
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
 * the scalar interpreter's before its clock starts.  -ENOSYS means the
 * build carries no such instruction set and the lane reports absent.
 * Any other refusal, or a byte difference, is a calibration failure:
 * the scalar interpreter is the authority, and a kernel disagreeing
 * with it has nothing to time.  Returns 1 for a timeable lane.
 */
static int
calibrate_lane(const char *label, const char *shape, execute_fn fn,
               const struct r300_vertex_job *job,
               const struct r300_cpu_vertex_stream *stream,
               uint32_t vertex_count)
{
   int rc = fn(job, stream, 0, vertex_count, carrier, MAX_VERTICES * 4);
   if (rc == -ENOSYS)
      return 0;
   if (rc != 0 ||
       r300_cpu_vertex_job_execute(job, stream, 0, vertex_count, reference,
                                   MAX_VERTICES * 4) != 0 ||
       memcmp(carrier, reference, (uint64_t)vertex_count * 16) != 0) {
      fprintf(stderr,
              "calibration failure: %s shape %s format %d stride %" PRIu32
              " count %" PRIu32 "\n",
              label, shape, job->input_format_id, stream->stride,
              vertex_count);
      exit(1);
   }
   return 1;
}

static uint64_t
time_one_rep(execute_fn fn, const struct r300_vertex_job *job,
             const struct r300_cpu_vertex_stream *stream,
             uint32_t vertex_count, unsigned inner)
{
   uint64_t t0 = now_ns();
   for (unsigned i = 0; i < inner; i++)
      fn(job, stream, 0, vertex_count, carrier, MAX_VERTICES * 4);
   return now_ns() - t0;
}

#define LANE_COUNT 3

struct bench_lane {
   const char *label;
   execute_fn fn;
};

/* A three-vertex non-identity job always reaches the CPU executor.  Identity
 * can instead reach the two experimental R2VB routes when their exact gates
 * open: F32_4 is a device-producer or host-model candidate, F32_3 and F32_2
 * are host-model candidates, and F32_1 remains on the CPU route.  Larger
 * counts exist only
 * to expose executor scaling because the native draw admits three vertices.
 */
static const char *
native_route_scope(int format_id, bool identity, uint32_t vertex_count)
{
   if (vertex_count != 3)
      return "executor_diagnostic";
   if (!identity || format_id == R300_VERTEX_FORMAT_F32_1)
      return "cpu_route";
   if (format_id == R300_VERTEX_FORMAT_F32_4)
      return "gpu_or_host_model_route_candidate";
   return "host_model_route_candidate";
}

/* One shape-by-count cell across every lane: calibrate each lane, then
 * rotate the lanes inside every repetition so scheduler and thermal
 * drift lands within each lane's repetition set; each lane's row
 * reports its best repetition.
 */
static void
bench_cell(const struct bench_lane *lanes, const char *shape,
           const char *route_scope, const char *layout,
           const struct r300_vertex_job *job,
           uint32_t stride, uint32_t base_offset, uint32_t vertex_count,
           unsigned reps)
{
   /* A zero stride reads one record for every vertex, so the stream
    * spans that record alone; any other stride spans the records the
    * count walks.  The bound is what the executor validates against.
    */
   const struct r300_cpu_vertex_stream stream = {
      .data = records + base_offset,
      .stride = stride,
      .size_bytes = stride == 0
         ? (uint64_t)RECORD_SPAN
         : (uint64_t)vertex_count * stride,
   };

   unsigned present[LANE_COUNT];
   unsigned present_count = 0;
   uint64_t best[LANE_COUNT];
   for (unsigned l = 0; l < LANE_COUNT; l++) {
      if (calibrate_lane(lanes[l].label, shape, lanes[l].fn, job, &stream,
                         vertex_count))
         present[present_count++] = l;
      best[l] = UINT64_MAX;
   }
   if (present_count == 0)
      return;

   /* Inner iterations amortize clock granularity for small counts. */
   unsigned inner = vertex_count < 4096
      ? 4096 / (vertex_count ? vertex_count : 1) : 1;
   /* The repetition count rounds up to whole rotation cycles, so every
    * present lane leads exactly rounded / present_count times; a
    * partial cycle would hand the earlier lanes an extra lead.  The row
    * reports the executed count.
    */
   unsigned rounded =
      ((reps + present_count - 1) / present_count) * present_count;
   for (unsigned r = 0; r < rounded; r++) {
      for (unsigned i = 0; i < present_count; i++) {
         unsigned l = present[(r + i) % present_count];
         uint64_t dt = time_one_rep(lanes[l].fn, job, &stream, vertex_count,
                                    inner);
         if (dt < best[l])
            best[l] = dt;
      }
   }

   for (unsigned i = 0; i < present_count; i++) {
      unsigned l = present[i];
      double per_vertex =
         (double)best[l] / ((double)inner * (double)vertex_count);
      printf("%s\t%s\t%s\treused_heap\t%s\t%d\t%" PRIu32 "\t%" PRIu32
             "\t%" PRIu32
             "\t%" PRIu32 "\t%u\t%.3f\n",
             lanes[l].label, shape, route_scope, layout,
             job->input_format_id, stride, base_offset, vertex_count,
             job->instruction_count, rounded, per_vertex);
   }
}

int
main(int argc, char **argv)
{
   /* Rows decide one part's dispatch, and the same code elsewhere times
    * that host's pipeline instead, so the executing CPU must identify as
    * the qualified part or the rows mark as smoke output.  Both binaries
    * run this check: the generic one is built unconditionally and its
    * unmarked rows would otherwise read as decision-grade to a recorder.  Family 0Fh alone is too wide: the K8 desktop,
    * mobile, and server models differ in cache and timing, so the check
    * reads the model too.  Model 0x68 is the Turion 64 X2 TL-66 the
    * measurement frame names; the observed stepping rides in the stream
    * rather than gating it, so a different revision of the same part
    * stays visible in the rows it produced.  The SSE3 feature bit is a
    * hard refusal: the whole binary compiles at -march=k8-sse3, so a
    * host without SSE3 would fault mid-run instead of producing marked
    * rows.
    */
#ifdef R300_CPU_VERTEX_BENCH_HAVE_CPUID
   {
      unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
      int is_qualified = 0;
      int has_sse3 = 0;
      unsigned family = 0, model = 0, stepping = 0;
      if (__get_cpuid(0, &eax, &ebx, &ecx, &edx) &&
          ebx == 0x68747541u /* "Auth" */ &&
          edx == 0x69746e65u /* "enti" */ &&
          ecx == 0x444d4163u /* "cAMD" */ &&
          __get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
         family = ((eax >> 8) & 0xf) + ((eax >> 20) & 0xff);
         model = ((eax >> 4) & 0xf) | (((eax >> 16) & 0xf) << 4);
         stepping = eax & 0xf;
         is_qualified = family == 0xf && model == 0x68;
         has_sse3 = (ecx & bit_SSE3) != 0;
      }
#ifdef R300_CPU_VERTEX_BENCH_REQUIRE_K8
      /* This binary compiles wholly at -march=k8-sse3, so a host
       * without SSE3 would fault mid-run instead of producing marked
       * rows.  The generic binary carries no such instruction floor and
       * runs anywhere, marked. */
      if (!has_sse3) {
         fprintf(stderr,
                 "error: executing CPU lacks SSE3; this binary's "
                 "k8-sse3 codegen would fault\n");
         return 1;
      }
#endif
      printf("# cpuid family 0x%x model 0x%x stepping 0x%x sse3 %d\n",
             family, model, stepping, has_sse3);
      if (!is_qualified) {
         fprintf(stderr,
                 "warning: executing CPU is not the qualified Turion 64 X2 "
                 "TL-66 (family 0x0f model 0x68); rows are smoke output, "
                 "not dispatch evidence\n");
         printf("# unqualified host: rows are smoke output, not dispatch "
                "evidence\n");
      }
   }
#else
   fprintf(stderr,
           "warning: this architecture carries no CPUID, so the executing "
           "part is unidentified; rows are smoke output, not dispatch "
           "evidence\n");
   printf("# unqualified host: rows are smoke output, not dispatch "
          "evidence\n");
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

   records = malloc(RECORD_SPAN);
   carrier = malloc((uint64_t)MAX_VERTICES * 16);
   reference = malloc((uint64_t)MAX_VERTICES * 16);
   if (records == NULL || carrier == NULL || reference == NULL) {
      fprintf(stderr, "allocation failure\n");
      return 1;
   }
   fill_records(records, RECORD_SPAN, 0x7ab10000u);

   calibrate_known_bad();

   /* Decision-grade rows come from a Meson buildtype=release build:
    * the build system defines R300_CPU_VERTEX_BENCH_MESON_RELEASE
    * there, because compiler macros alone cannot separate release from
    * debugoptimized with b_ndebug.  Any other build marks the row
    * stream itself, so a recorder consuming stdout carries the
    * non-decision-grade status with the rows.
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
    * from, so a retained bundle binds the timings to a commit.
    */
   /* A row binds to the commit its binary was built from.  An exported
    * tree resolves no .git, so MESA_GIT_SHA1 is empty and the binding
    * the frame requires does not exist; the stream says so rather than
    * printing an unknown source among otherwise unmarked rows.
    */
   if (MESA_GIT_SHA1[0] != '\0') {
      printf("# source%s\n", MESA_GIT_SHA1);
      /* git_sha1_gen.py resolves HEAD alone, so a build carrying
       * uncommitted changes prints this same identity while its sources
       * differ from it.  The stream cannot observe the tree it was
       * compiled from, so it names that limit and the check that closes
       * it: a decision-grade run records an empty status alongside these
       * rows, in an isolated worktree at the declared commit.
       */
      printf("# source binding: commit identity alone; the clean tree is "
             "established by the run record, not by this stream\n");
   } else {
      fprintf(stderr,
              "warning: the source identity is unavailable; rows are "
              "smoke output, not dispatch evidence\n");
      printf("# unknown source: rows are smoke output, not dispatch "
             "evidence\n");
   }

   /* This bench and the native ICD link the same libr300_cpu object set.
    * The K8 bench recompiles the same sources under
    * an -march override, so its rows describe codegen the profile does
    * not produce.  The filename separates the two streams, and a
    * locator travels apart from its bytes, so the fact rides in the
    * stream as well.
    */
#ifdef R300_CPU_VERTEX_BENCH_REQUIRE_K8
   printf("# lane codegen: sources recompiled under an -march override; "
          "a driver links libr300_cpu at the profile's flags instead\n");
#else
   printf("# lane codegen: libr300_cpu at the enclosing profile's "
          "flags, shared with the native ICD\n");
#endif
   printf("# evidence scope: executor microbenchmark; native dispatch "
          "requires command-buffer-owned mapped GTT carrier timing\n");
   printf("# carrier state: reused heap allocation\n");
   /* -march decides which instruction sets the compiler may emit for
    * the interpreter, so the ISA the profile grants it is the fact that
    * separates the two streams' arithmetic.
    */
#ifdef __SSE3__
   printf("# lane isa: sse2, sse3\n");
#else
   printf("# lane isa: sse2\n");
#endif

   /* An absent ISA lane marks the stream: a kernel compiled out of this
    * build reports -ENOSYS, and the marker tells a collector the stream
    * is not the two-way result, which a silent omission would hide.
    */
   {
      const struct r300_vertex_job probe =
         identity_job(R300_VERTEX_FORMAT_F32_4);
      const struct r300_cpu_vertex_stream probe_stream = {
         .data = records,
         .stride = 16,
         .size_bytes = 64,
      };
      if (r300_cpu_vertex_job_execute_sse2(&probe, &probe_stream, 0, 3,
                                           carrier,
                                           MAX_VERTICES * 4) == -ENOSYS) {
         printf("# lane absent: sse2 (build carries no such instruction "
                "set)\n");
      }
      if (r300_cpu_vertex_job_execute_sse3(&probe, &probe_stream, 0, 3,
                                           carrier,
                                           MAX_VERTICES * 4) == -ENOSYS) {
         printf("# lane absent: sse3 (build carries no such instruction "
                "set)\n");
      }
   }

   printf("implementation\tjob_shape\troute_scope\tcarrier_state\tlayout\t"
          "format\tstride\tbase_offset\tvertex_count\tinstruction_count\t"
          "reps\tbest_ns_per_vertex\n");

   static const struct bench_lane lanes[LANE_COUNT] = {
      { "scalar", r300_cpu_vertex_job_execute },
      { "sse2", r300_cpu_vertex_job_execute_sse2 },
      { "sse3", r300_cpu_vertex_job_execute_sse3 },
   };
   /* Every format the pipeline admits as the bound attribute
    * (rg --fixed-strings attribute_format_id src/amd/r300/): each
    * carries its own gather and expansion, so a dispatch decision
    * covering only some of them leaves the rest unmeasured.
    */
   static const int formats[] = {
      R300_VERTEX_FORMAT_F32_4,
      R300_VERTEX_FORMAT_F32_3,
      R300_VERTEX_FORMAT_F32_2,
      R300_VERTEX_FORMAT_F32_1,
   };
   /* The native path admits exactly three vertices.  Larger counts expose
    * executor scaling and carry an explicit diagnostic scope.
    */
   static const uint32_t counts[] = { 3, 64, 4096, MAX_VERTICES };

   for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
      const struct r300_vertex_format_semantics *format =
         r300_vertex_format_semantics(
            (enum r300_vertex_format_id)formats[f]);
      uint32_t stride = format->semantic_record_bytes;
      struct r300_vertex_job jobs[7] = {
         identity_job(formats[f]),
         constant_job(formats[f]),
         affine_job(formats[f]),
         dp4_chain_job(formats[f]),
         producer_max_chain_job(formats[f], R300_VERTEX_JOB_MAX_TEMPS - 1),
         opcode_chain_job(formats[f], R300_VERTEX_JOB_OP_FADD,
                          R300_VERTEX_JOB_MAX_TEMPS - 1),
         opcode_chain_job(formats[f], R300_VERTEX_JOB_OP_FMUL,
                          R300_VERTEX_JOB_MAX_TEMPS - 1),
      };
      static const char *const shapes[7] = {
         "identity", "constant", "affine", "dp4_chain",
         "producer_max_chain", "fadd_chain", "fmul_chain",
      };
      /* The admitted layouts: the packed stream the route builds, a
       * padded stride spreading records across cache lines, a zero
       * stride collapsing every vertex onto one record, and a
       * four-byte offset moving the mapped pointer off 16-byte
       * alignment.  Each is what the pipeline accepts, so each is a
       * workload the dispatch decision covers.
       */
      const struct { const char *label; uint32_t stride, base_offset; }
      layouts[4] = {
         { "packed", stride, 0 },
         { "padded", stride + 8, 0 },
         { "collapsed", 0, 0 },
         { "offset4", stride, 4 },
      };
      for (unsigned j = 0; j < 7; j++) {
         for (unsigned y = 0; y < 4; y++) {
            for (unsigned c = 0; c < 4; c++) {
               if ((uint64_t)counts[c] * layouts[y].stride
                   + layouts[y].base_offset > RECORD_SPAN)
                  continue;
               const char *route_scope =
                  native_route_scope(formats[f], j == 0, counts[c]);
               bench_cell(lanes, shapes[j], route_scope, layouts[y].label,
                          &jobs[j],
                          layouts[y].stride, layouts[y].base_offset,
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
