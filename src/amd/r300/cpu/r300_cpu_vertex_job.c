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

   uint32_t written = 0;
   bool varying_stored = false;
   for (uint32_t i = 0; i < job->instruction_count; i++) {
      const struct r300_vertex_job_instruction *inst = &job->instructions[i];
      const bool last = i + 1 == job->instruction_count;
      uint32_t reads = 0;
      switch (inst->opcode) {
      case R300_VERTEX_JOB_OP_LOAD_INPUT:
         /* A read slot carries a bound format; the executor gathers
          * it from that slot's stream. */
         if (inst->src0 >= R300_VERTEX_JOB_MAX_INPUTS ||
             !r300_vertex_format_semantics(
                job->input_format_ids[inst->src0]))
            return -EINVAL;
         break;
      case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
         if (inst->src0 >= job->constant_count)
            return -EINVAL;
         break;
      case R300_VERTEX_JOB_OP_LOAD_SYSTEM_VALUE:
         if (inst->src0 >= R300_VERTEX_JOB_SV_COUNT)
            return -EINVAL;
         break;
      case R300_VERTEX_JOB_OP_MOV:
      case R300_VERTEX_JOB_OP_CONVERT_S_TO_F:
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
      case R300_VERTEX_JOB_OP_STORE_VARYING:
         /* One varying at most, stored from a written register ahead
          * of the final position store; the record layout derives from
          * its presence alone. */
         if (last || varying_stored ||
             inst->src0 >= R300_VERTEX_JOB_MAX_TEMPS ||
             !(written & (1u << inst->src0)))
            return -EINVAL;
         varying_stored = true;
         continue;
      default:
         return -EINVAL;
      }
      /* The stores write the carrier alone, so everything else is a
       * register write and its sources must already hold values. */
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

/* The vertex number the relative vertex v executes: the v-th entry of
 * the draw's vertex-id list, or first_vertex + v over a linear range.
 */
static inline uint32_t vertex_number(const struct r300_cpu_vertex_draw *draw,
                                     uint32_t v)
{
   return draw->vertex_ids ? draw->vertex_ids[v] : draw->first_vertex + v;
}

/* The record an instance-rate stream serves to relative instance i:
 * the Vulkan vertex input address calculation, first_instance plus the
 * relative instance divided by the divisor, or first_instance alone
 * under divisor zero.
 */
static inline uint32_t instance_record(const struct r300_vertex_stream *stream,
                                       const struct r300_cpu_vertex_draw *draw,
                                       uint32_t i)
{
   return stream->instance_divisor
             ? draw->first_instance + i / stream->instance_divisor
             : draw->first_instance;
}

/* The records an instance-rate stream serves across the draw: one
 * under divisor zero, otherwise the quotient of the last relative
 * instance plus one.  Returns false when the last record's number
 * leaves the 32-bit record space the gather and the VAP address.
 */
static bool instance_record_range(const struct r300_vertex_stream *stream,
                                  const struct r300_cpu_vertex_draw *draw,
                                  uint32_t *count)
{
   const uint64_t last = stream->instance_divisor
                            ? (uint64_t)(draw->instance_count - 1) /
                                 stream->instance_divisor
                            : 0;
   if ((uint64_t)draw->first_instance + last > UINT32_MAX)
      return false;
   *count = (uint32_t)last + 1;
   return true;
}

/* The shared execution guard: every refusal every entry point shares,
 * proven before any kernel writes a carrier byte.  streams[slot]
 * serves each slot the job reads; slots it leaves unread need no
 * stream.  A vertex-id list bounds each listed vertex on its own; a
 * linear range bounds its last record.
 */
static int execute_guard(const struct r300_vertex_job *job,
                         const struct r300_vertex_stream *streams,
                         const struct r300_cpu_vertex_draw *draw,
                         const uint32_t *carrier, uint32_t carrier_dwords)
{
   int error = r300_cpu_vertex_job_validate(job);
   if (error)
      return error;
   if (!streams || !carrier || !draw || draw->vertex_count == 0 ||
       draw->instance_count == 0)
      return -EINVAL;
   if ((uint64_t)draw->vertex_count * draw->instance_count *
          r300_vertex_job_record_dwords(job) >
       carrier_dwords)
      return -ENOSPC;

   const uintptr_t carrier_lo = (uintptr_t)carrier;
   const uintptr_t carrier_hi =
      carrier_lo + (uintptr_t)carrier_dwords * sizeof(uint32_t);
   const uint32_t input_mask = r300_vertex_job_input_mask(job);
   for (uint32_t slot = 0; slot < R300_VERTEX_JOB_MAX_INPUTS; slot++) {
      if (!(input_mask & (1u << slot)))
         continue;
      const struct r300_vertex_stream *stream = &streams[slot];
      if (!stream->data)
         return -EINVAL;
      /* Whole-range bound up front, so the per-vertex gathers below
       * cannot fail after the carrier has been written; under the
       * stream's robust rule the gathers substitute instead of
       * refusing, and the range needs no bound. */
      uint32_t instance_records = 0;
      if (stream->instance_rate &&
          !instance_record_range(stream, draw, &instance_records))
         return -EINVAL;
      if (!stream->oob_reads_zero) {
         if (stream->instance_rate) {
            if (!r300_cpu_vertex_range_in_bounds(
                   job->input_format_ids[slot], stream, draw->first_instance,
                   instance_records))
               return -EINVAL;
         } else if (draw->vertex_ids) {
            for (uint32_t v = 0; v < draw->vertex_count; v++) {
               if (!r300_cpu_vertex_range_in_bounds(
                      job->input_format_ids[slot], stream,
                      draw->vertex_ids[v], 1))
                  return -EINVAL;
            }
         } else if (!r300_cpu_vertex_range_in_bounds(
                       job->input_format_ids[slot], stream,
                       draw->first_vertex, draw->vertex_count)) {
            return -EINVAL;
         }
      }

      /* The carrier may share an allocation with a stream (an in-place
       * restaging), but an overlapping byte range would let stores
       * corrupt records later vertices read. */
      const uintptr_t stream_lo = (uintptr_t)stream->data;
      const uintptr_t stream_hi = stream_lo + (uintptr_t)stream->size_bytes;
      if (carrier_lo < stream_hi && stream_lo < carrier_hi)
         return -EINVAL;
   }
   return 0;
}

/* Gathers the current record's logical vec4 from every slot the job
 * reads -- the vertex number's record from a per-vertex stream, the
 * relative instance's record from an instance-rate stream; an unread
 * slot's lanes stay unwritten and no LOAD_INPUT names it, so
 * validation keeps the read set and the gather set equal. */
static int gather_inputs(const struct r300_vertex_job *job,
                         const struct r300_vertex_stream *streams,
                         const struct r300_cpu_vertex_draw *draw,
                         uint32_t input_mask, uint32_t vertex,
                         uint32_t instance,
                         uint32_t inputs[R300_VERTEX_JOB_MAX_INPUTS][4])
{
   for (uint32_t slot = 0; slot < R300_VERTEX_JOB_MAX_INPUTS; slot++) {
      if (!(input_mask & (1u << slot)))
         continue;
      const struct r300_vertex_stream *stream = &streams[slot];
      const uint32_t record = stream->instance_rate
                                 ? instance_record(stream, draw, instance)
                                 : vertex;
      int error = r300_cpu_vertex_gather(job->input_format_ids[slot], stream,
                                         record, 1, inputs[slot], 4);
      if (error)
         return error;
   }
   return 0;
}

/* The system value a record observes: the vertex number, or the
 * instance index first_instance + relative instance. */
static inline uint32_t system_value(enum r300_vertex_job_system_value sv,
                                    const struct r300_cpu_vertex_draw *draw,
                                    uint32_t vertex, uint32_t instance)
{
   return sv == R300_VERTEX_JOB_SV_VERTEX_INDEX
             ? vertex
             : draw->first_instance + instance;
}

static inline uint32_t int_to_float_bits(uint32_t bits)
{
   int32_t value;
   memcpy(&value, &bits, sizeof(value));
   return float_result_to_bits((float)value);
}

/* The scalar interpreter over the draw: instance-major records, each
 * instance's vertices in order over a linear range or a vertex-id
 * list. */
static int execute_scalar(const struct r300_vertex_job *job,
                          const struct r300_vertex_stream *streams,
                          const struct r300_cpu_vertex_draw *draw,
                          uint32_t *carrier, uint32_t carrier_dwords)
{
   int error = execute_guard(job, streams, draw, carrier, carrier_dwords);
   if (error)
      return error;

   struct r300_cpu_float_environment environment;
   error = float_environment_enter(&environment);
   if (error)
      return error;

   const uint32_t record_dwords = r300_vertex_job_record_dwords(job);
   const uint32_t input_mask = r300_vertex_job_input_mask(job);
   const uint64_t records = (uint64_t)draw->vertex_count * draw->instance_count;
   for (uint64_t r = 0; r < records; r++) {
      const uint32_t instance = (uint32_t)(r / draw->vertex_count);
      const uint32_t v = (uint32_t)(r % draw->vertex_count);
      const uint32_t vertex = vertex_number(draw, v);
      uint32_t *record = &carrier[r * record_dwords];
      uint32_t temps[R300_VERTEX_JOB_MAX_TEMPS][4];
      uint32_t inputs[R300_VERTEX_JOB_MAX_INPUTS][4];
      error = gather_inputs(job, streams, draw, input_mask, vertex, instance,
                            inputs);
      if (error)
         goto out;

      for (uint32_t i = 0; i < job->instruction_count; i++) {
         const struct r300_vertex_job_instruction *inst =
            &job->instructions[i];
         switch (inst->opcode) {
         case R300_VERTEX_JOB_OP_LOAD_INPUT:
            memcpy(temps[inst->dst], inputs[inst->src0],
                   sizeof(inputs[inst->src0]));
            break;
         case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
            memcpy(temps[inst->dst], job->constants[inst->src0],
                   sizeof(temps[inst->dst]));
            break;
         case R300_VERTEX_JOB_OP_MOV:
            memcpy(temps[inst->dst], temps[inst->src0],
                   sizeof(temps[inst->dst]));
            break;
         case R300_VERTEX_JOB_OP_LOAD_SYSTEM_VALUE: {
            const uint32_t value = system_value(
               (enum r300_vertex_job_system_value)inst->src0, draw, vertex,
               instance);
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] = value;
            break;
         }
         case R300_VERTEX_JOB_OP_CONVERT_S_TO_F:
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] =
                  int_to_float_bits(temps[inst->src0][lane]);
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
            memcpy(record, temps[inst->src0], 4 * sizeof(uint32_t));
            break;
         case R300_VERTEX_JOB_OP_STORE_VARYING:
            memcpy(&record[R300_VERTEX_JOB_POSITION_DWORDS],
                   temps[inst->src0], 4 * sizeof(uint32_t));
            break;
         }
      }
   }
out:
   float_environment_leave(&environment);
   return error;
}

int r300_cpu_vertex_job_execute_draw(const struct r300_vertex_job *job,
                                     const struct r300_vertex_stream *streams,
                                     const struct r300_cpu_vertex_draw *draw,
                                     uint32_t *carrier,
                                     uint32_t carrier_dwords)
{
   return execute_scalar(job, streams, draw, carrier, carrier_dwords);
}

int r300_cpu_vertex_job_execute(const struct r300_vertex_job *job,
                                const struct r300_vertex_stream *streams,
                                uint32_t first_vertex, uint32_t vertex_count,
                                uint32_t *carrier, uint32_t carrier_dwords)
{
   const struct r300_cpu_vertex_draw draw = {
      .first_vertex = first_vertex,
      .vertex_count = vertex_count,
      .instance_count = 1,
   };
   return execute_scalar(job, streams, &draw, carrier, carrier_dwords);
}

int r300_cpu_vertex_job_execute_indexed(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *streams, const uint32_t *vertex_ids,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   if (!vertex_ids)
      return -EINVAL;
   const struct r300_cpu_vertex_draw draw = {
      .vertex_ids = vertex_ids,
      .vertex_count = vertex_count,
      .instance_count = 1,
   };
   return execute_scalar(job, streams, &draw, carrier, carrier_dwords);
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
             const struct r300_vertex_stream *streams,
             uint32_t first_vertex, uint32_t vertex_count,
             uint32_t *carrier, uint32_t carrier_dwords,
             enum simd_load_form form)
{
   /* The linear contract: one instance from instance zero, so an
    * instance-rate stream serves its first_instance record (zero) and
    * the instance index reads zero. */
   const struct r300_cpu_vertex_draw draw = {
      .first_vertex = first_vertex,
      .vertex_count = vertex_count,
      .instance_count = 1,
   };
   int error = execute_guard(job, streams, &draw, carrier, carrier_dwords);
   if (error)
      return error;

   struct r300_cpu_float_environment environment;
   error = float_environment_enter(&environment);
   if (error)
      return error;

   const uint32_t record_dwords = r300_vertex_job_record_dwords(job);
   const uint32_t input_mask = r300_vertex_job_input_mask(job);
   for (uint32_t v = 0; v < vertex_count; v++) {
      uint32_t *record = &carrier[(uint64_t)v * record_dwords];
      /* Registers stay 32-bit patterns; the unaligned integer loads and
       * stores move NaN payloads, denormals, and signed zeros verbatim,
       * and each packed operator rounds per lane exactly as the scalar
       * interpreter's per-lane host binary32 operation does. */
      __m128i temps[R300_VERTEX_JOB_MAX_TEMPS];
      uint32_t inputs[R300_VERTEX_JOB_MAX_INPUTS][4];
      error = gather_inputs(job, streams, &draw, input_mask, first_vertex + v,
                            0, inputs);
      if (error)
         goto out;

      for (uint32_t i = 0; i < job->instruction_count; i++) {
         const struct r300_vertex_job_instruction *inst =
            &job->instructions[i];
         switch (inst->opcode) {
         case R300_VERTEX_JOB_OP_LOAD_INPUT:
            temps[inst->dst] = simd_load(inputs[inst->src0], form);
            break;
         case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
            temps[inst->dst] = simd_load(job->constants[inst->src0], form);
            break;
         case R300_VERTEX_JOB_OP_MOV:
            temps[inst->dst] = temps[inst->src0];
            break;
         case R300_VERTEX_JOB_OP_LOAD_SYSTEM_VALUE:
            temps[inst->dst] = _mm_set1_epi32((int)system_value(
               (enum r300_vertex_job_system_value)inst->src0, &draw,
               first_vertex + v, 0));
            break;
         case R300_VERTEX_JOB_OP_CONVERT_S_TO_F:
            /* cvtdq2ps rounds each lane under MXCSR's round-to-nearest,
             * the scalar interpreter's (float)(int32_t) conversion. */
            temps[inst->dst] = simd_float_result(
               _mm_castps_si128(_mm_cvtepi32_ps(temps[inst->src0])));
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
            _mm_storeu_si128((__m128i *)record, temps[inst->src0]);
            break;
         case R300_VERTEX_JOB_OP_STORE_VARYING:
            _mm_storeu_si128(
               (__m128i *)&record[R300_VERTEX_JOB_POSITION_DWORDS],
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
   const struct r300_vertex_stream *streams, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   return execute_simd(job, streams, first_vertex, vertex_count, carrier,
                       carrier_dwords, SIMD_LOAD_MOVDQU);
}

#ifdef __SSE3__

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *streams, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   return execute_simd(job, streams, first_vertex, vertex_count, carrier,
                       carrier_dwords, SIMD_LOAD_LDDQU);
}

#else

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *streams, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   (void)job;
   (void)streams;
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
   const struct r300_vertex_stream *streams, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   (void)job;
   (void)streams;
   (void)first_vertex;
   (void)vertex_count;
   (void)carrier;
   (void)carrier_dwords;
   return -ENOSYS;
}

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *streams, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   (void)job;
   (void)streams;
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
