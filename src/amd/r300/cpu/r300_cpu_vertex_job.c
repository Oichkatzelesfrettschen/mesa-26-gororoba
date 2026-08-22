/*
 * SPDX-License-Identifier: MIT
 *
 * Scalar CPU vertex-job interpreter.
 */

#include "r300_cpu_vertex_job.h"

#include "amd/r300/common/r300_vertex_format.h"
#include "util/macros.h"

#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Lanes travel as 32-bit patterns; arithmetic converts through memcpy
 * so every operator is one host binary32 operation with one rounding.
 */
static float bits_to_float(uint32_t bits)
{
   float value;
   memcpy(&value, &bits, sizeof(value));
   return value;
}

/* Host scalar code and packed SSE choose different source NaN payloads on
 * some GCC versions.  Arithmetic results therefore use one quiet NaN, while
 * LOAD_INPUT, LOAD_CONSTANT, and MOV keep their byte-copy payload contract.
 */
static uint32_t float_result_to_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   if ((bits & 0x7f800000u) == 0x7f800000u &&
       (bits & 0x007fffffu) != 0)
      return 0x7fc00000u;
   return bits;
}

struct r300_cpu_float_environment {
   fenv_t caller;
};

/* The CPU route implements one shader arithmetic environment independent of
 * application-controlled rounding and denormal modes.  The executor admits
 * FE_DFL_ENV only when it realizes round-to-nearest and preserves denormals.
 */
static int
float_environment_enter(struct r300_cpu_float_environment *environment)
{
   if (fegetenv(&environment->caller) != 0)
      return -ENOTSUP;
   if (fesetenv(FE_DFL_ENV) != 0) {
      (void)fesetenv(&environment->caller);
      return -ENOTSUP;
   }
   const volatile float smallest_normal = bits_to_float(0x00800000u);
   const volatile float one_half = 0.5f;
   if (fegetround() != FE_TONEAREST ||
       float_result_to_bits(smallest_normal * one_half) != 0x00400000u) {
      (void)fesetenv(&environment->caller);
      return -ENOTSUP;
   }
   return 0;
}

static void
float_environment_leave(const struct r300_cpu_float_environment *environment)
{
   (void)fesetenv(&environment->caller);
}

int r300_cpu_vertex_job_validate(const struct r300_vertex_job *job)
{
   if (!job || job->instruction_count == 0 ||
       job->instruction_count > R300_VERTEX_JOB_MAX_INSTRUCTIONS ||
       job->constant_count > R300_VERTEX_JOB_MAX_CONSTANTS)
      return -EINVAL;
   if (!r300_vertex_format_semantics(job->input_format_id))
      return -EINVAL;

   uint32_t written = 0;
   for (uint32_t i = 0; i < job->instruction_count; i++) {
      const struct r300_vertex_job_instruction *inst = &job->instructions[i];
      const bool last = i + 1 == job->instruction_count;
      uint32_t reads = 0;
      switch (inst->opcode) {
      case R300_VERTEX_JOB_OP_LOAD_INPUT:
         /* The single-binding subset carries attribute 0 only. */
         if (inst->src0 != 0)
            return -EINVAL;
         break;
      case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
         if (inst->src0 >= job->constant_count)
            return -EINVAL;
         break;
      case R300_VERTEX_JOB_OP_MOV:
         reads = 1;
         break;
      case R300_VERTEX_JOB_OP_FADD:
      case R300_VERTEX_JOB_OP_FMUL:
      case R300_VERTEX_JOB_OP_DP4:
         reads = 2;
         break;
      case R300_VERTEX_JOB_OP_FMAD:
      case R300_VERTEX_JOB_OP_FFMA:
         reads = 3;
         break;
      case R300_VERTEX_JOB_OP_STORE_POSITION:
         if (!last || inst->src0 >= R300_VERTEX_JOB_MAX_TEMPS ||
             !(written & (1u << inst->src0)))
            return -EINVAL;
         continue;
      default:
         return -EINVAL;
      }
      /* The one store is the final instruction, so everything else is
       * a register write and its sources must already hold values. */
      if (last || inst->dst >= R300_VERTEX_JOB_MAX_TEMPS)
         return -EINVAL;
      const uint8_t srcs[3] = { inst->src0, inst->src1, inst->src2 };
      for (uint32_t s = 0; s < reads; s++) {
         if (srcs[s] >= R300_VERTEX_JOB_MAX_TEMPS ||
             !(written & (1u << srcs[s])))
            return -EINVAL;
      }
      written |= 1u << inst->dst;
   }
   return 0;
}

/* The shared execution guard: every refusal both entry points share,
 * proven before either kernel writes a carrier byte.
 */
static int execute_guard(const struct r300_vertex_job *job,
                         const struct r300_vertex_stream *stream,
                         uint32_t first_vertex, uint32_t vertex_count,
                         const uint32_t *carrier, uint32_t carrier_dwords)
{
   int error = r300_cpu_vertex_job_validate(job);
   if (error)
      return error;
   if (!stream || !stream->data || !carrier || vertex_count == 0)
      return -EINVAL;
   if ((uint64_t)vertex_count * 4 > carrier_dwords)
      return -ENOSPC;

   /* Whole-range last-byte bound, so the per-vertex gathers below cannot
    * fail after the carrier has been written.  The comparison divides,
    * matching r300_cpu_vertex_gather's own bound: last_vertex reaches
    * 2^32.3 and the stride reaches 2^32, so the product form wraps and a
    * wrapped residue admits a range the per-vertex gather then refuses
    * mid-carrier.  A zero stride repeats the first record, which the
    * record-bytes bound alone covers.
    */
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics(job->input_format_id);
   if (stream->size_bytes < format->semantic_record_bytes)
      return -EINVAL;
   if (stream->stride != 0) {
      const uint64_t last_vertex = (uint64_t)first_vertex + vertex_count - 1;
      if (last_vertex > (stream->size_bytes -
                         format->semantic_record_bytes) / stream->stride)
         return -EINVAL;
   }

   /* The carrier may share an allocation with the stream (an in-place
    * restaging), but an overlapping byte range would let stores
    * corrupt records later vertices read. */
   const uintptr_t stream_lo = (uintptr_t)stream->data;
   const uintptr_t stream_hi = stream_lo + (uintptr_t)stream->size_bytes;
   const uintptr_t carrier_lo = (uintptr_t)carrier;
   const uintptr_t carrier_hi =
      carrier_lo + (uintptr_t)carrier_dwords * sizeof(uint32_t);
   if (carrier_lo < stream_hi && stream_lo < carrier_hi)
      return -EINVAL;
   return 0;
}

int r300_cpu_vertex_job_execute(const struct r300_vertex_job *job,
                                const struct r300_vertex_stream *stream,
                                uint32_t first_vertex, uint32_t vertex_count,
                                uint32_t *carrier, uint32_t carrier_dwords)
{
   int error = execute_guard(job, stream, first_vertex, vertex_count,
                             carrier, carrier_dwords);
   if (error)
      return error;

   struct r300_cpu_float_environment environment;
   error = float_environment_enter(&environment);
   if (error)
      return error;

   for (uint32_t v = 0; v < vertex_count; v++) {
      uint32_t temps[R300_VERTEX_JOB_MAX_TEMPS][4];
      uint32_t input[4];
      error = r300_cpu_vertex_gather(job->input_format_id, stream,
                                     first_vertex + v, 1, input, 4);
      if (error)
         goto out;

      for (uint32_t i = 0; i < job->instruction_count; i++) {
         const struct r300_vertex_job_instruction *inst =
            &job->instructions[i];
         switch (inst->opcode) {
         case R300_VERTEX_JOB_OP_LOAD_INPUT:
            memcpy(temps[inst->dst], input, sizeof(input));
            break;
         case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
            memcpy(temps[inst->dst], job->constants[inst->src0],
                   sizeof(temps[inst->dst]));
            break;
         case R300_VERTEX_JOB_OP_MOV:
            memcpy(temps[inst->dst], temps[inst->src0],
                   sizeof(temps[inst->dst]));
            break;
         case R300_VERTEX_JOB_OP_FADD:
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] = float_result_to_bits(
                  bits_to_float(temps[inst->src0][lane]) +
                  bits_to_float(temps[inst->src1][lane]));
            break;
         case R300_VERTEX_JOB_OP_FMUL:
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] = float_result_to_bits(
                  bits_to_float(temps[inst->src0][lane]) *
                  bits_to_float(temps[inst->src1][lane]));
            break;
         case R300_VERTEX_JOB_OP_FMAD:
            /* Two roundings: the product commits to binary32 before
             * the add, matching the fused-operator-free SSE2/SSE3
             * substrate the K8 target implements. */
            for (uint32_t lane = 0; lane < 4; lane++) {
               const volatile float product =
                  bits_to_float(temps[inst->src0][lane]) *
                  bits_to_float(temps[inst->src1][lane]);
               temps[inst->dst][lane] = float_result_to_bits(
                  product + bits_to_float(temps[inst->src2][lane]));
            }
            break;
         case R300_VERTEX_JOB_OP_FFMA:
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] = float_result_to_bits(fmaf(
                  bits_to_float(temps[inst->src0][lane]),
                  bits_to_float(temps[inst->src1][lane]),
                  bits_to_float(temps[inst->src2][lane])));
            break;
         case R300_VERTEX_JOB_OP_DP4: {
            /* Seed with the first product: an additive zero seed would
             * rewrite an all-negative-zero dot product to +0.  Each
             * product commits to binary32 through a volatile object
             * before it joins the sum, the same defense FMAD above
             * carries: an accumulate written as sum += a * b is a
             * contraction candidate, and a target with a fused operator
             * would round the pair once and diverge from the packed
             * multiply-then-add the SSE2 kernel executes.
             */
            const volatile float seed =
               bits_to_float(temps[inst->src0][0]) *
               bits_to_float(temps[inst->src1][0]);
            float sum = seed;
            for (uint32_t lane = 1; lane < 4; lane++) {
               const volatile float product =
                  bits_to_float(temps[inst->src0][lane]) *
                  bits_to_float(temps[inst->src1][lane]);
               sum += product;
            }
            const uint32_t bits = float_result_to_bits(sum);
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] = bits;
            break;
         }
         case R300_VERTEX_JOB_OP_STORE_POSITION:
            memcpy(&carrier[(uint64_t)v * 4], temps[inst->src0],
                   4 * sizeof(uint32_t));
            break;
         }
      }
   }
out:
   float_environment_leave(&environment);
   return error;
}

#ifdef __SSE2__

#include <emmintrin.h>
#ifdef __SSE3__
#include <pmmintrin.h>
#endif

/* The two SIMD candidates differ in one operation, the unaligned
 * 128-bit load, so the body is written once and each candidate
 * instantiates it with its own load.  Forced inlining keeps the
 * constant selector out of the loop: each candidate compiles to a
 * straight kernel carrying its own load form, which is what the bench
 * times and what the objdump beside the rows shows.
 */
enum simd_load_form {
   SIMD_LOAD_MOVDQU,
   SIMD_LOAD_LDDQU,
};

static ALWAYS_INLINE __m128i
simd_load(const void *from, enum simd_load_form form)
{
#ifdef __SSE3__
   if (form == SIMD_LOAD_LDDQU)
      return _mm_lddqu_si128((const __m128i *)from);
#else
   (void)form;
#endif
   return _mm_loadu_si128((const __m128i *)from);
}

/* Canonicalize arithmetic NaNs with integer operations after each packed
 * result.  This is the SIMD form of float_result_to_bits and leaves every
 * finite value, infinity, denormal, and signed zero bit-exact.
 */
static ALWAYS_INLINE __m128i
simd_float_result(__m128i bits)
{
   const __m128i exponent_mask = _mm_set1_epi32(0x7f800000u);
   const __m128i mantissa_mask = _mm_set1_epi32(0x007fffffu);
   const __m128i zero = _mm_setzero_si128();
   const __m128i exponent_all_ones =
      _mm_cmpeq_epi32(_mm_and_si128(bits, exponent_mask), exponent_mask);
   const __m128i mantissa_zero =
      _mm_cmpeq_epi32(_mm_and_si128(bits, mantissa_mask), zero);
   const __m128i nan_mask = _mm_andnot_si128(mantissa_zero,
                                             exponent_all_ones);
   const __m128i canonical_nan = _mm_set1_epi32(0x7fc00000u);
   return _mm_or_si128(_mm_andnot_si128(nan_mask, bits),
                       _mm_and_si128(nan_mask, canonical_nan));
}

static ALWAYS_INLINE int
execute_simd(const struct r300_vertex_job *job,
             const struct r300_vertex_stream *stream,
             uint32_t first_vertex, uint32_t vertex_count,
             uint32_t *carrier, uint32_t carrier_dwords,
             enum simd_load_form form)
{
   int error = execute_guard(job, stream, first_vertex, vertex_count,
                             carrier, carrier_dwords);
   if (error)
      return error;

   struct r300_cpu_float_environment environment;
   error = float_environment_enter(&environment);
   if (error)
      return error;

   for (uint32_t v = 0; v < vertex_count; v++) {
      /* Registers stay 32-bit patterns; the unaligned integer loads and
       * stores move NaN payloads, denormals, and signed zeros verbatim,
       * and each packed operator rounds per lane exactly as the scalar
       * interpreter's per-lane host binary32 operation does. */
      __m128i temps[R300_VERTEX_JOB_MAX_TEMPS];
      uint32_t input[4];
      error = r300_cpu_vertex_gather(job->input_format_id, stream,
                                     first_vertex + v, 1, input, 4);
      if (error)
         goto out;

      for (uint32_t i = 0; i < job->instruction_count; i++) {
         const struct r300_vertex_job_instruction *inst =
            &job->instructions[i];
         switch (inst->opcode) {
         case R300_VERTEX_JOB_OP_LOAD_INPUT:
            temps[inst->dst] = simd_load(input, form);
            break;
         case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
            temps[inst->dst] = simd_load(job->constants[inst->src0], form);
            break;
         case R300_VERTEX_JOB_OP_MOV:
            temps[inst->dst] = temps[inst->src0];
            break;
         case R300_VERTEX_JOB_OP_FADD:
            temps[inst->dst] = simd_float_result(_mm_castps_si128(
               _mm_add_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1]))));
            break;
         case R300_VERTEX_JOB_OP_FMUL:
            temps[inst->dst] = simd_float_result(_mm_castps_si128(
               _mm_mul_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1]))));
            break;
         case R300_VERTEX_JOB_OP_FMAD:
            /* mulps then addps: the product commits to binary32 before
             * the add, the same two roundings the scalar policy pins. */
            temps[inst->dst] = simd_float_result(_mm_castps_si128(_mm_add_ps(
               _mm_mul_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1])),
               _mm_castsi128_ps(temps[inst->src2]))));
            break;
         case R300_VERTEX_JOB_OP_FFMA: {
            float src0[4];
            float src1[4];
            float src2[4];
            float result[4];
            _mm_storeu_ps(src0, _mm_castsi128_ps(temps[inst->src0]));
            _mm_storeu_ps(src1, _mm_castsi128_ps(temps[inst->src1]));
            _mm_storeu_ps(src2, _mm_castsi128_ps(temps[inst->src2]));
            for (uint32_t lane = 0; lane < 4; lane++)
               result[lane] = fmaf(src0[lane], src1[lane], src2[lane]);
            temps[inst->dst] = simd_float_result(
               _mm_castps_si128(_mm_loadu_ps(result)));
            break;
         }
         case R300_VERTEX_JOB_OP_DP4: {
            /* Packed products, then a component-order scalar sum seeded
             * by lane 0's product, matching the scalar interpreter's
             * signed-zero-preserving accumulation exactly.  HADDPS adds
             * adjacent lane pairs, so it associates the sum differently
             * and stays out of both candidates. */
            const __m128 products =
               _mm_mul_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1]));
            float lanes[4];
            _mm_storeu_ps(lanes, products);
            float sum = lanes[0];
            for (uint32_t lane = 1; lane < 4; lane++)
               sum += lanes[lane];
            temps[inst->dst] = simd_float_result(
               _mm_castps_si128(_mm_set1_ps(sum)));
            break;
         }
         case R300_VERTEX_JOB_OP_STORE_POSITION:
            _mm_storeu_si128((__m128i *)&carrier[(uint64_t)v * 4],
                             temps[inst->src0]);
            break;
         }
      }
   }
out:
   float_environment_leave(&environment);
   return error;
}

int r300_cpu_vertex_job_execute_sse2(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   return execute_simd(job, stream, first_vertex, vertex_count, carrier,
                       carrier_dwords, SIMD_LOAD_MOVDQU);
}

#ifdef __SSE3__

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   return execute_simd(job, stream, first_vertex, vertex_count, carrier,
                       carrier_dwords, SIMD_LOAD_LDDQU);
}

#else

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   (void)job;
   (void)stream;
   (void)first_vertex;
   (void)vertex_count;
   (void)carrier;
   (void)carrier_dwords;
   return -ENOSYS;
}

#endif /* __SSE3__ */

#else

int r300_cpu_vertex_job_execute_sse2(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   (void)job;
   (void)stream;
   (void)first_vertex;
   (void)vertex_count;
   (void)carrier;
   (void)carrier_dwords;
   return -ENOSYS;
}

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   (void)job;
   (void)stream;
   (void)first_vertex;
   (void)vertex_count;
   (void)carrier;
   (void)carrier_dwords;
   return -ENOSYS;
}

#endif /* __SSE2__ */

/* The native route executes the scalar interpreter.  The executor-only heap
 * benchmark qualifies candidates and measures their arithmetic cost.  SIMD
 * dispatch requires end-to-end timing over the command-buffer-owned mapped
 * GTT carrier, including repeated submission writes and cache publication.
 */
const char *r300_cpu_vertex_job_implementation(void)
{
   return "scalar";
}
