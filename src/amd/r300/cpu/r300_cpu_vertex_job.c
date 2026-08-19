/*
 * SPDX-License-Identifier: MIT
 *
 * Scalar CPU vertex-job interpreter.
 */

#include "r300_cpu_vertex_job.h"

#include "amd/r300/common/r300_vertex_format.h"

#include <errno.h>
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

static uint32_t float_to_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
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
                         const struct r300_cpu_vertex_stream *stream,
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

   /* Whole-range last-byte bound in 64-bit arithmetic, so the
    * per-vertex gathers below cannot fail after the carrier has been
    * written.  A zero stride repeats the first record. */
   const struct r300_vertex_format_semantics *format =
      r300_vertex_format_semantics(job->input_format_id);
   const uint64_t last_vertex = (uint64_t)first_vertex + vertex_count - 1;
   const uint64_t last_byte =
      last_vertex * stream->stride + format->semantic_record_bytes;
   if (last_byte > stream->size_bytes)
      return -EINVAL;

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
                                const struct r300_cpu_vertex_stream *stream,
                                uint32_t first_vertex, uint32_t vertex_count,
                                uint32_t *carrier, uint32_t carrier_dwords)
{
   int error = execute_guard(job, stream, first_vertex, vertex_count,
                             carrier, carrier_dwords);
   if (error)
      return error;

   for (uint32_t v = 0; v < vertex_count; v++) {
      uint32_t temps[R300_VERTEX_JOB_MAX_TEMPS][4];
      uint32_t input[4];
      error = r300_cpu_vertex_gather(job->input_format_id, stream,
                                     first_vertex + v, 1, input, 4);
      if (error)
         return error;

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
               temps[inst->dst][lane] = float_to_bits(
                  bits_to_float(temps[inst->src0][lane]) +
                  bits_to_float(temps[inst->src1][lane]));
            break;
         case R300_VERTEX_JOB_OP_FMUL:
            for (uint32_t lane = 0; lane < 4; lane++)
               temps[inst->dst][lane] = float_to_bits(
                  bits_to_float(temps[inst->src0][lane]) *
                  bits_to_float(temps[inst->src1][lane]));
            break;
         case R300_VERTEX_JOB_OP_FMAD:
            /* Two roundings: the product commits to binary32 before
             * the add, matching the fused-operator-free SSE2/SSE3
             * substrate the K8 target implements. */
            for (uint32_t lane = 0; lane < 4; lane++) {
               const float product =
                  bits_to_float(temps[inst->src0][lane]) *
                  bits_to_float(temps[inst->src1][lane]);
               temps[inst->dst][lane] = float_to_bits(
                  product + bits_to_float(temps[inst->src2][lane]));
            }
            break;
         case R300_VERTEX_JOB_OP_DP4: {
            /* Seed with the first product: an additive zero seed would
             * rewrite an all-negative-zero dot product to +0. */
            float sum = bits_to_float(temps[inst->src0][0]) *
                        bits_to_float(temps[inst->src1][0]);
            for (uint32_t lane = 1; lane < 4; lane++)
               sum += bits_to_float(temps[inst->src0][lane]) *
                      bits_to_float(temps[inst->src1][lane]);
            const uint32_t bits = float_to_bits(sum);
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
   return 0;
}

#ifdef __SSE2__

#include <emmintrin.h>

int r300_cpu_vertex_job_execute_sse2(
   const struct r300_vertex_job *job,
   const struct r300_cpu_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords)
{
   int error = execute_guard(job, stream, first_vertex, vertex_count,
                             carrier, carrier_dwords);
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
         return error;

      for (uint32_t i = 0; i < job->instruction_count; i++) {
         const struct r300_vertex_job_instruction *inst =
            &job->instructions[i];
         switch (inst->opcode) {
         case R300_VERTEX_JOB_OP_LOAD_INPUT:
            temps[inst->dst] = _mm_loadu_si128((const __m128i *)input);
            break;
         case R300_VERTEX_JOB_OP_LOAD_CONSTANT:
            temps[inst->dst] = _mm_loadu_si128(
               (const __m128i *)job->constants[inst->src0]);
            break;
         case R300_VERTEX_JOB_OP_MOV:
            temps[inst->dst] = temps[inst->src0];
            break;
         case R300_VERTEX_JOB_OP_FADD:
            temps[inst->dst] = _mm_castps_si128(
               _mm_add_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1])));
            break;
         case R300_VERTEX_JOB_OP_FMUL:
            temps[inst->dst] = _mm_castps_si128(
               _mm_mul_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1])));
            break;
         case R300_VERTEX_JOB_OP_FMAD:
            /* mulps then addps: the product commits to binary32 before
             * the add, the same two roundings the scalar policy pins. */
            temps[inst->dst] = _mm_castps_si128(_mm_add_ps(
               _mm_mul_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1])),
               _mm_castsi128_ps(temps[inst->src2])));
            break;
         case R300_VERTEX_JOB_OP_DP4: {
            /* Packed products, then a component-order scalar sum seeded
             * by lane 0's product, matching the scalar interpreter's
             * signed-zero-preserving accumulation exactly. */
            const __m128 products =
               _mm_mul_ps(_mm_castsi128_ps(temps[inst->src0]),
                          _mm_castsi128_ps(temps[inst->src1]));
            float lanes[4];
            _mm_storeu_ps(lanes, products);
            float sum = lanes[0];
            for (uint32_t lane = 1; lane < 4; lane++)
               sum += lanes[lane];
            temps[inst->dst] = _mm_castps_si128(_mm_set1_ps(sum));
            break;
         }
         case R300_VERTEX_JOB_OP_STORE_POSITION:
            _mm_storeu_si128((__m128i *)&carrier[(uint64_t)v * 4],
                             temps[inst->src0]);
            break;
         }
      }
   }
   return 0;
}

#else

int r300_cpu_vertex_job_execute_sse2(
   const struct r300_vertex_job *job,
   const struct r300_cpu_vertex_stream *stream, uint32_t first_vertex,
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
