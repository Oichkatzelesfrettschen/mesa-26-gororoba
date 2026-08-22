/*
 * SPDX-License-Identifier: MIT
 *
 * Differential equivalence of the SIMD vertex-job candidates against
 * the scalar interpreter: randomized valid jobs over bit-pattern-hostile
 * inputs produce byte-identical carriers, numeric-policy witnesses
 * reproduce on each entry directly, and every refusal returns the scalar
 * entry's errno.  Test verdicts ride live asserts, so NDEBUG is undefined
 * ahead of assert.h.
 */

#undef NDEBUG

#include "r300_cpu_vertex_job.h"

#include "amd/r300/common/r300_vertex_format.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __SSE__
#include <xmmintrin.h>
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif
#endif

#define VERTEX_COUNT 8u
#define STREAM_STRIDE 16u
#define RANDOM_JOBS 256u

/* xorshift32: a fixed-seed deterministic generator, so every run
 * exercises the same job population.
 */
/* Both SIMD candidates carry one contract, so one differential body
 * qualifies each of them: they share the scalar authority, the same
 * refusals, and the same carrier bytes, and they differ only in the
 * unaligned load form.  A candidate its build carries no instruction
 * set for reports -ENOSYS and the lane skips rather than passing
 * silently.
 */
struct simd_lane {
   const char *name;
   int (*execute)(const struct r300_vertex_job *job,
                  const struct r300_vertex_stream *stream,
                  uint32_t first_vertex, uint32_t vertex_count,
                  uint32_t *carrier, uint32_t carrier_dwords);
};

static const struct simd_lane simd_lanes[] = {
   { "sse2", r300_cpu_vertex_job_execute_sse2 },
   { "sse3", r300_cpu_vertex_job_execute_sse3 },
};

static uint32_t prng_state = 0x1234abcdu;

static uint32_t prng(void)
{
   uint32_t x = prng_state;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   prng_state = x;
   return x;
}

/* Bit patterns weighted toward the values that distinguish a bit-copy
 * register file from a value model: NaN payloads, denormals, signed
 * zeros, and infinities, beside ordinary magnitudes and raw noise.
 */
static uint32_t hostile_bits(void)
{
   static const uint32_t pool[] = {
      0x00000000u, /* +0 */
      0x80000000u, /* -0 */
      0x7fa00001u, /* signaling NaN payload */
      0x7fc00123u, /* quiet NaN payload */
      0x00000001u, /* smallest denormal */
      0x807fffffu, /* negative denormal */
      0x7f800000u, /* +inf */
      0xff800000u, /* -inf */
      0x3f800000u, /* 1.0 */
      0xbf800000u, /* -1.0 */
      0x38801000u, /* 1 + 2^-12 neighborhood */
   };
   const uint32_t roll = prng();
   if ((roll & 3u) == 0)
      return pool[roll % (sizeof(pool) / sizeof(pool[0]))];
   return prng();
}

/* Builds one random structurally valid job: every source register is
 * already written, the final instruction is the one STORE_POSITION,
 * and the first instruction loads the input so register 0 is live.
 */
static void random_job(struct r300_vertex_job *job)
{
   memset(job, 0, sizeof(*job));
   job->input_format_id = R300_VERTEX_FORMAT_F32_4;
   job->constant_count = 1 + prng() % R300_VERTEX_JOB_MAX_CONSTANTS;
   for (uint32_t c = 0; c < job->constant_count; c++)
      for (uint32_t lane = 0; lane < 4; lane++)
         job->constants[c][lane] = hostile_bits();

   const uint32_t body = 1 + prng() % (R300_VERTEX_JOB_MAX_INSTRUCTIONS - 2);
   uint32_t written = 0;
   job->instructions[0] = (struct r300_vertex_job_instruction){
      .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT,
      .dst = 0,
   };
   written |= 1u;
   for (uint32_t i = 1; i <= body; i++) {
      struct r300_vertex_job_instruction *inst = &job->instructions[i];
      const uint32_t live_count = (uint32_t)__builtin_popcount(written);
      uint8_t live[R300_VERTEX_JOB_MAX_TEMPS];
      uint32_t n = 0;
      for (uint8_t t = 0; t < R300_VERTEX_JOB_MAX_TEMPS; t++)
         if (written & (1u << t))
            live[n++] = t;
      assert(n == live_count && n > 0);

      switch (prng() % 7) {
      case 0:
         inst->opcode = R300_VERTEX_JOB_OP_LOAD_INPUT;
         inst->src0 = 0;
         break;
      case 1:
         inst->opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT;
         inst->src0 = (uint8_t)(prng() % job->constant_count);
         break;
      case 2:
         inst->opcode = R300_VERTEX_JOB_OP_MOV;
         inst->src0 = live[prng() % n];
         break;
      case 3:
         inst->opcode = R300_VERTEX_JOB_OP_FADD;
         inst->src0 = live[prng() % n];
         inst->src1 = live[prng() % n];
         break;
      case 4:
         inst->opcode = (prng() & 1) ? R300_VERTEX_JOB_OP_FMUL
                                     : R300_VERTEX_JOB_OP_DP4;
         inst->src0 = live[prng() % n];
         inst->src1 = live[prng() % n];
         break;
      case 5:
         inst->opcode = R300_VERTEX_JOB_OP_FMAD;
         inst->src0 = live[prng() % n];
         inst->src1 = live[prng() % n];
         inst->src2 = live[prng() % n];
         break;
      default:
         inst->opcode = R300_VERTEX_JOB_OP_FFMA;
         inst->src0 = live[prng() % n];
         inst->src1 = live[prng() % n];
         inst->src2 = live[prng() % n];
         break;
      }
      inst->dst = (uint8_t)(prng() % R300_VERTEX_JOB_MAX_TEMPS);
      written |= 1u << inst->dst;
   }
   job->instructions[body + 1] = (struct r300_vertex_job_instruction){
      .opcode = R300_VERTEX_JOB_OP_STORE_POSITION,
      .src0 = job->instructions[body].dst,
   };
   /* The store reads the final generated result, so the last randomized
    * operator always reaches the differential oracle. */
   job->instruction_count = body + 2;
}

static void differential_random_jobs(const struct simd_lane *lane)
{
   uint32_t stream_bytes[VERTEX_COUNT * 4];
   for (uint32_t i = 0; i < VERTEX_COUNT * 4; i++)
      stream_bytes[i] = hostile_bits();
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)stream_bytes,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(stream_bytes),
   };

   for (uint32_t trial = 0; trial < RANDOM_JOBS; trial++) {
      struct r300_vertex_job job;
      random_job(&job);
      assert(r300_cpu_vertex_job_validate(&job) == 0);

      uint32_t scalar[VERTEX_COUNT * 4];
      uint32_t sse2[VERTEX_COUNT * 4];
      memset(scalar, 0xa5, sizeof(scalar));
      memset(sse2, 0x5a, sizeof(sse2));
      assert(r300_cpu_vertex_job_execute(&job, &stream, 0, VERTEX_COUNT,
                                         scalar, VERTEX_COUNT * 4) == 0);
      assert(lane->execute(&job, &stream, 0,
                                              VERTEX_COUNT, sse2,
                                              VERTEX_COUNT * 4) == 0);
      if (memcmp(scalar, sse2, sizeof(scalar)) != 0) {
         fprintf(stderr, "divergence at trial %u\n", trial);
         for (uint32_t d = 0; d < VERTEX_COUNT * 4; d++)
            if (scalar[d] != sse2[d])
               fprintf(stderr, "  dword %u: scalar %08x %s %08x\n", d,
                       scalar[d], lane->name, sse2[d]);
         assert(!"scalar and SIMD carriers diverged");
      }
   }
}

/* The FMAD two-rounding witness on the SSE2 entry: a = 1 + 2^-12, so
 * a*a rounds ties-to-even to 1 + 2^-11 and FMAD(a, a, -(1 + 2^-11))
 * yields +0; a fused operator would yield 2^-24.
 */
static void witness_fmad_two_roundings(const struct simd_lane *lane)
{
   struct r300_vertex_job job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .constant_count = 2,
      .instruction_count = 4,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT, .dst = 1,
           .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT, .dst = 2,
           .src0 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_FMAD, .dst = 3, .src0 = 1,
           .src1 = 1, .src2 = 2 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 3 },
      },
   };
   for (uint32_t lane = 0; lane < 4; lane++) {
      job.constants[0][lane] = 0x3f800800u; /* 1 + 2^-12 */
      job.constants[1][lane] = 0xbf801000u; /* -(1 + 2^-11) */
   }
   const uint32_t stream_bytes[4] = { 0, 0, 0, 0 };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)stream_bytes,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(stream_bytes),
   };
   uint32_t out[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
   assert(lane->execute(&job, &stream, 0, 1, out, 4) ==
          0);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x00000000u);

   /* One rounding keeps the exact product's 2^-24 residual.  Reusing the
    * two-rounding FMAD path produces +0 and fails this leg.
    */
   job.instructions[2].opcode = R300_VERTEX_JOB_OP_FFMA;
   assert(lane->execute(&job, &stream, 0, 1, out, 4) == 0);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x33800000u); /* 2^-24 */
}

static void witness_float_environment(const struct simd_lane *lane)
{
   struct r300_vertex_job job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .constant_count = 1,
      .instruction_count = 4,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0 },
         { .opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT, .dst = 1,
           .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_FADD, .dst = 2, .src0 = 0,
           .src1 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 2 },
      },
   };
   fenv_t original_environment;
   assert(fegetenv(&original_environment) == 0);
   assert(fesetround(FE_UPWARD) == 0);
#ifdef __SSE__
   const unsigned hostile_mxcsr =
      _mm_getcsr() | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
   _mm_setcsr(hostile_mxcsr);
#endif

   const uint32_t input[4] = {
      0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)input,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(input),
   };
   for (uint32_t component = 0; component < 4; component++)
      job.constants[0][component] = 0x33800000u; /* 2^-24 */
   uint32_t out[4];
   assert(lane->execute(&job, &stream, 0, 1, out, 4) == 0);
   for (uint32_t component = 0; component < 4; component++)
      assert(out[component] == 0x3f800000u);
   assert(fegetround() == FE_UPWARD);
#ifdef __SSE__
   assert(_mm_getcsr() == hostile_mxcsr);
#endif

   job.instructions[2].opcode = R300_VERTEX_JOB_OP_FMUL;
   for (uint32_t component = 0; component < 4; component++)
      job.constants[0][component] = 0x3f000000u; /* 0.5 */
   const uint32_t smallest_normal[4] = {
      0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u,
   };
   const struct r300_vertex_stream denorm_stream = {
      .data = (const uint8_t *)smallest_normal,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(smallest_normal),
   };
   assert(lane->execute(&job, &denorm_stream, 0, 1, out, 4) == 0);
   for (uint32_t component = 0; component < 4; component++)
      assert(out[component] == 0x00400000u);
   assert(fegetround() == FE_UPWARD);
#ifdef __SSE__
   assert(_mm_getcsr() == hostile_mxcsr);
#endif
   assert(fesetenv(&original_environment) == 0);
}

/* An all-negative-zero DP4 keeps -0 on the SSE2 entry: the sum seeds
 * from the first product instead of an additive +0. */
static void witness_dp4_signed_zero(const struct simd_lane *lane)
{
   struct r300_vertex_job job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .constant_count = 2,
      .instruction_count = 4,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT, .dst = 0,
           .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT, .dst = 1,
           .src0 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_DP4, .dst = 2, .src0 = 0,
           .src1 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 2 },
      },
   };
   for (uint32_t lane = 0; lane < 4; lane++) {
      job.constants[0][lane] = 0x80000000u; /* -0 */
      job.constants[1][lane] = 0x3f800000u; /* 1.0 */
   }
   const uint32_t stream_bytes[4] = { 0, 0, 0, 0 };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)stream_bytes,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(stream_bytes),
   };
   uint32_t out[4] = { 0 };
   assert(lane->execute(&job, &stream, 0, 1, out, 4) ==
          0);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x80000000u);
}

/* Arithmetic canonicalizes every NaN to one quiet payload, while the MOV
 * path retains the input payload.  The witness catches compiler-dependent
 * scalar-versus-packed source selection without weakening byte-copy state.
 */
static void witness_nan_policy(const struct simd_lane *lane)
{
   struct r300_vertex_job arithmetic = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .constant_count = 1,
      .instruction_count = 4,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0 },
         { .opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT, .dst = 1,
           .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_FADD, .dst = 2, .src0 = 0,
           .src1 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 2 },
      },
      .constants = {
         { 0xff800000u, 0x7fc00123u, 0x7fa00001u, 0x00000000u },
      },
   };
   const uint32_t input[4] = {
      0x7f800000u, 0x3f800000u, 0x3f800000u, 0x80000000u,
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)input,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(input),
   };
   uint32_t scalar[4] = { 0 };
   uint32_t candidate[4] = { 0 };
   assert(r300_cpu_vertex_job_execute(&arithmetic, &stream, 0, 1,
                                      scalar, 4) == 0);
   assert(lane->execute(&arithmetic, &stream, 0, 1, candidate, 4) == 0);
   assert(memcmp(scalar, candidate, sizeof(scalar)) == 0);
   assert(scalar[0] == 0x7fc00000u);
   assert(scalar[1] == 0x7fc00000u);
   assert(scalar[2] == 0x7fc00000u);

   struct r300_vertex_job copy = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .instruction_count = 3,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0 },
         { .opcode = R300_VERTEX_JOB_OP_MOV, .dst = 1, .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 1 },
      },
   };
   assert(lane->execute(&copy, &stream, 0, 1, candidate, 4) == 0);
   assert(memcmp(input, candidate, sizeof(input)) == 0);
}

/* Refusal parity: the SSE2 entry shares the scalar guard, so every
 * refusal returns the same errno with no carrier write. */
static void refusal_parity(const struct simd_lane *lane)
{
   struct r300_vertex_job job;
   random_job(&job);
   uint32_t stream_bytes[VERTEX_COUNT * 4] = { 0 };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)stream_bytes,
      .stride = STREAM_STRIDE,
      .size_bytes = sizeof(stream_bytes),
   };
   uint32_t out[VERTEX_COUNT * 4];

   assert(lane->execute(&job, &stream, 0, VERTEX_COUNT,
                                           out, VERTEX_COUNT * 4 - 1) ==
          -ENOSPC);
   assert(lane->execute(&job, &stream, 1, VERTEX_COUNT,
                                           out, VERTEX_COUNT * 4) ==
          -EINVAL);
   assert(lane->execute(&job, &stream, 0, VERTEX_COUNT,
                                           stream_bytes,
                                           VERTEX_COUNT * 4) == -EINVAL);
   job.instruction_count = 0;
   assert(lane->execute(&job, &stream, 0, VERTEX_COUNT,
                                           out, VERTEX_COUNT * 4) ==
          -EINVAL);
}

int main(void)
{
   unsigned qualified = 0;

   for (unsigned l = 0; l < ARRAY_SIZE(simd_lanes); l++) {
      const struct simd_lane *lane = &simd_lanes[l];

      /* A candidate reports -ENOSYS where the build carries no such
       * instruction set, and the differential claim is x86-only, so that
       * lane skips while the others still run. */
      struct r300_vertex_job probe = {
         .input_format_id = R300_VERTEX_FORMAT_F32_4,
         .instruction_count = 2,
         .instructions = {
            { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0 },
            { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 0 },
         },
      };
      const uint32_t stream_bytes[4] = { 1, 2, 3, 4 };
      const struct r300_vertex_stream stream = {
         .data = (const uint8_t *)stream_bytes,
         .stride = STREAM_STRIDE,
         .size_bytes = sizeof(stream_bytes),
      };
      uint32_t out[4];
      const int probe_result = lane->execute(&probe, &stream, 0, 1, out, 4);
      if (probe_result == -ENOSYS) {
         printf("r300_cpu_vertex_job_simd_test: %s unavailable, skipped\n",
                lane->name);
         continue;
      }
      assert(probe_result == 0);
      assert(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);

      /* Each lane draws the same job sequence, so a divergence names the
       * candidate rather than the draw it happened to get. */
      prng_state = 0x1234abcdu;
      differential_random_jobs(lane);
      witness_fmad_two_roundings(lane);
      witness_float_environment(lane);
      witness_dp4_signed_zero(lane);
      witness_nan_policy(lane);
      refusal_parity(lane);
      printf("r300_cpu_vertex_job_simd_test: %s bit-identical over %u "
             "random jobs; all checks passed\n",
             lane->name, RANDOM_JOBS);
      qualified++;
   }

   /* Every candidate absent means the build carries no SIMD lane at all,
    * which is a skip rather than a pass: nothing was compared. */
   if (qualified == 0) {
      printf("r300_cpu_vertex_job_simd_test: no SIMD candidate in this "
             "build, skipped\n");
      return 77;
   }
#ifdef R300_CPU_VERTEX_JOB_SIMD_REQUIRE_SSE3
   if (qualified != ARRAY_SIZE(simd_lanes)) {
      fprintf(stderr,
              "r300_cpu_vertex_job_simd_test: target build omitted a SIMD "
              "candidate\n");
      return 1;
   }
#endif
   return 0;
}
