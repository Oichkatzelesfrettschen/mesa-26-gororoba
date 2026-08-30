/*
 * SPDX-License-Identifier: MIT
 *
 * Qualification for the scalar CPU vertex-job interpreter: exact
 * carrier bytes for hand-built jobs, arithmetic policy (two-rounding
 * FMAD, ordered DP4, bit-copy data movement), refusal legs with
 * no-partial-write proof, and determinism.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_cpu_vertex_job.h"

#include "amd/r300/common/r300_vertex_format.h"
#include "util/detect.h"

#include <assert.h>
#include <errno.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if DETECT_ARCH_SSE
#include <xmmintrin.h>
#ifndef _MM_DENORMALS_ZERO_MASK
#define _MM_DENORMALS_ZERO_MASK 0x0040
#endif
#endif

#define CARRIER_DWORDS 64u
#define CANARY 0xdeadbeefu

static uint32_t f_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

static void fill_canary(uint32_t *carrier)
{
   for (uint32_t i = 0; i < CARRIER_DWORDS; i++)
      carrier[i] = CANARY;
}

static struct r300_vertex_stream stream_of(const void *data,
                                               uint32_t stride,
                                               uint64_t size_bytes)
{
   struct r300_vertex_stream stream = {
      .data = (const uint8_t *)data,
      .stride = stride,
      .size_bytes = size_bytes,
   };
   return stream;
}

/* LOAD_INPUT t0; STORE_POSITION t0. */
static struct r300_vertex_job identity_job(int format_id)
{
   struct r300_vertex_job job = {
      .input_format_ids = { format_id },
      .instruction_count = 2,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   return job;
}

static void test_identity_preserves_bits(void)
{
   /* Special encodings survive the bit-copy path: NaN payloads, a
    * denormal, negative zero, and infinity. */
   const uint32_t records[3][4] = {
      { 0x7fa00001u, 0x7fc00123u, 0x00000001u, 0x80000000u },
      { 0x7f800000u, f_bits(1.5f), f_bits(-2.25f), f_bits(0.0f) },
      { f_bits(3.0f), f_bits(4.0f), f_bits(5.0f), f_bits(6.0f) },
   };
   const struct r300_vertex_job job = identity_job(R300_VERTEX_FORMAT_F32_4);
   const struct r300_vertex_stream stream =
      stream_of(records, 16, sizeof(records));
   uint32_t carrier[CARRIER_DWORDS];
   fill_canary(carrier);
   int rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier,
                                        CARRIER_DWORDS);
   assert(rc == 0);
   assert(memcmp(carrier, records, sizeof(records)) == 0);
   /* Tail dwords past 4 * vertex_count stay untouched. */
   for (uint32_t i = 12; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);
}

static void test_f32_2_synthesis(void)
{
   /* F32_2 gathers x and y and synthesizes z = 0, w = 1 through the
    * PSC selectors the gather realizes. */
   const uint32_t records[2][2] = {
      { f_bits(7.0f), f_bits(-8.0f) },
      { 0x7fc00042u, f_bits(0.5f) },
   };
   const struct r300_vertex_job job = identity_job(R300_VERTEX_FORMAT_F32_2);
   const struct r300_vertex_stream stream =
      stream_of(records, 8, sizeof(records));
   uint32_t carrier[CARRIER_DWORDS];
   fill_canary(carrier);
   int rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 2, carrier,
                                        CARRIER_DWORDS);
   assert(rc == 0);
   const uint32_t expected[8] = {
      f_bits(7.0f), f_bits(-8.0f), 0, 0x3f800000u,
      0x7fc00042u, f_bits(0.5f), 0, 0x3f800000u,
   };
   assert(memcmp(carrier, expected, sizeof(expected)) == 0);
}

/* One vertex through an arithmetic job; returns the stored vec4. */
static void run_one(const struct r300_vertex_job *job,
                    const uint32_t input[4], uint32_t out[4])
{
   uint32_t carrier[4];
   const struct r300_vertex_stream stream = stream_of(input, 16, 16);
   int rc = r300_cpu_vertex_job_execute(job, &stream, 0, 1, carrier, 4);
   assert(rc == 0);
   memcpy(out, carrier, sizeof(carrier));
}

static void test_arithmetic_exact(void)
{
   /* t0 = input; t1 = const0; t2 = t0 + t1; t3 = t2 * t1; store t3. */
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 5,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FADD, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_FMUL, 3, 2, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 3, 0, 0 },
      },
      .constant_count = 1,
      .constants = {
         { f_bits(2.0f), f_bits(-0.5f), f_bits(4.0f), f_bits(1.0f) },
      },
   };
   const uint32_t input[4] = {
      f_bits(1.5f), f_bits(3.0f), f_bits(-1.0f), f_bits(0.25f),
   };
   uint32_t out[4];
   run_one(&job, input, out);
   assert(out[0] == f_bits((1.5f + 2.0f) * 2.0f));
   assert(out[1] == f_bits((3.0f + -0.5f) * -0.5f));
   assert(out[2] == f_bits((-1.0f + 4.0f) * 4.0f));
   assert(out[3] == f_bits((0.25f + 1.0f) * 1.0f));

   /* Arithmetic NaNs canonicalize independently of the host compiler's
    * source-payload choice.
    */
   struct r300_vertex_job nan_job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FADD, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 2, 0, 0 },
      },
      .constant_count = 1,
      .constants = { { 0xff800000u, 0, 0, 0 } },
   };
   const uint32_t inf_input[4] = { 0x7f800000u, 0, 0, 0 };
   run_one(&nan_job, inf_input, out);
   assert(out[0] == 0x7fc00000u);
}

static void test_multiply_add_rounding(void)
{
   /* a = b = 1 + 2^-12; a * b = 1 + 2^-11 + 2^-24 rounds (ties to
    * even) to 1 + 2^-11 in binary32, so the two-rounding FMAD of
    * a, b, -(1 + 2^-11) is exactly +0; a fused operator would return
    * 2^-24.  This pins the documented double rounding. */
   const float a = 1.0f + 0x1.0p-12f;
   const float c = -(1.0f + 0x1.0p-11f);
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FMAD, 2, 0, 0, 1 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 2, 0, 0 },
      },
      .constant_count = 1,
      .constants = { { f_bits(c), f_bits(c), f_bits(c), f_bits(c) } },
   };
   const uint32_t input[4] = { f_bits(a), f_bits(a), f_bits(a), f_bits(a) };
   uint32_t out[4];
   run_one(&job, input, out);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0);

   /* One rounding keeps the exact product's 2^-24 residual.  Reusing the
    * two-rounding FMAD path produces +0 and fails this leg.
    */
   job.instructions[2].opcode = R300_VERTEX_JOB_OP_FFMA;
   run_one(&job, input, out);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x33800000u); /* 2^-24 */
}

static void test_dot_product_rounding(void)
{
   /* DP4 sums four products in component order, each rounded to
    * binary32 before it joins the sum.  Component 0 seeds the sum with
    * -1, component 1 contributes (1 + 2^-12)^2, which rounds (ties to
    * even) to 1 + 2^-11, and the ordered sum is 2^-11 exactly.  A fused
    * multiply-add over the second component would round the pair once
    * and keep the 2^-24 residual, giving 2^-11 + 2^-24, so this leg
    * pins the accumulator against contraction on an FMA-capable target.
    */
   const float a = 1.0f + 0x1.0p-12f;
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_DP4, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 2, 0, 0 },
      },
      .constant_count = 1,
      .constants = { { f_bits(-1.0f), f_bits(a), 0u, 0u } },
   };
   const uint32_t input[4] = { f_bits(1.0f), f_bits(a), 0u, 0u };
   uint32_t out[4];
   run_one(&job, input, out);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x3a000000u); /* 2^-11 */
}

static void test_float_environment_isolation(void)
{
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FADD, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 2, 0, 0 },
      },
      .constant_count = 1,
   };
   fenv_t original_environment;
   assert(fegetenv(&original_environment) == 0);
   assert(fesetround(FE_UPWARD) == 0);

#if DETECT_ARCH_SSE
   const unsigned hostile_mxcsr =
      _mm_getcsr() | _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK;
   _mm_setcsr(hostile_mxcsr);
#endif

   const uint32_t ones[4] = {
      0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
   };
   uint32_t out[4];
   for (uint32_t lane = 0; lane < 4; lane++)
      job.constants[0][lane] = 0x33800000u; /* 2^-24 */
   run_one(&job, ones, out);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x3f800000u);
   assert(fegetround() == FE_UPWARD);
#if DETECT_ARCH_SSE
   assert(_mm_getcsr() == hostile_mxcsr);
#endif

   job.instructions[2].opcode = R300_VERTEX_JOB_OP_FMUL;
   for (uint32_t lane = 0; lane < 4; lane++)
      job.constants[0][lane] = 0x3f000000u; /* 0.5 */
   const uint32_t smallest_normal[4] = {
      0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u,
   };
   run_one(&job, smallest_normal, out);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == 0x00400000u);
   assert(fegetround() == FE_UPWARD);
#if DETECT_ARCH_SSE
   assert(_mm_getcsr() == hostile_mxcsr);
#endif

   assert(fesetenv(&original_environment) == 0);
}

static void test_dp4_order_and_broadcast(void)
{
   /* dot((1,2,3,4), (2,3,4,5)) = 2 + 6 + 12 + 20 = 40 exactly, and
    * the scalar broadcasts to all four lanes. */
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_DP4, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 2, 0, 0 },
      },
      .constant_count = 1,
      .constants = {
         { f_bits(2.0f), f_bits(3.0f), f_bits(4.0f), f_bits(5.0f) },
      },
   };
   const uint32_t input[4] = {
      f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f),
   };
   uint32_t out[4];
   run_one(&job, input, out);
   for (uint32_t lane = 0; lane < 4; lane++)
      assert(out[lane] == f_bits(40.0f));

   /* All-negative-zero products keep the -0 sign through the seeded
    * component-order sum. */
   job.constants[0][0] = 0x80000000u;
   job.constants[0][1] = 0x80000000u;
   job.constants[0][2] = 0x80000000u;
   job.constants[0][3] = 0x80000000u;
   const uint32_t ones[4] = {
      f_bits(1.0f), f_bits(1.0f), f_bits(1.0f), f_bits(1.0f),
   };
   run_one(&job, ones, out);
   assert(out[0] == 0x80000000u);
}

static void test_mov_preserves_nan_payload(void)
{
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_MOV, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 1, 0, 0 },
      },
   };
   const uint32_t input[4] = {
      0x7fa00001u, 0x7fc00123u, 0x80000000u, 0x00000001u,
   };
   uint32_t out[4];
   run_one(&job, input, out);
   assert(memcmp(out, input, sizeof(input)) == 0);
}

static void expect_refusal(const struct r300_vertex_job *job, int expected)
{
   int rc = r300_cpu_vertex_job_validate(job);
   assert(rc == expected);
}

static void test_validation_refusals(void)
{
   struct r300_vertex_job job = identity_job(R300_VERTEX_FORMAT_F32_4);

   expect_refusal(NULL, -EINVAL);

   struct r300_vertex_job bad = job;
   bad.instruction_count = 0;
   expect_refusal(&bad, -EINVAL);
   bad = job;
   bad.instruction_count = R300_VERTEX_JOB_MAX_INSTRUCTIONS + 1;
   expect_refusal(&bad, -EINVAL);
   bad = job;
   bad.input_format_ids[0] = R300_VERTEX_FORMAT_INVALID;
   expect_refusal(&bad, -EINVAL);
   bad = job;
   bad.instructions[0].opcode = 0x7f;
   expect_refusal(&bad, -EINVAL);
   bad = job;
   bad.instructions[0].dst = R300_VERTEX_JOB_MAX_TEMPS;
   expect_refusal(&bad, -EINVAL);
   /* The subset admits attribute 0 only. */
   bad = job;
   bad.instructions[0].src0 = 1;
   expect_refusal(&bad, -EINVAL);
   /* Constant index outside the declared count. */
   bad = job;
   bad.instructions[0].opcode = R300_VERTEX_JOB_OP_LOAD_CONSTANT;
   expect_refusal(&bad, -EINVAL);
   /* Read before write. */
   bad = job;
   bad.instructions[1].src0 = 1;
   expect_refusal(&bad, -EINVAL);
   struct r300_vertex_job unwritten = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FADD, 1, 0, 2, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 1, 0, 0 },
      },
   };
   expect_refusal(&unwritten, -EINVAL);
   /* Store missing, doubled, or before the end. */
   bad = job;
   bad.instructions[1].opcode = R300_VERTEX_JOB_OP_MOV;
   expect_refusal(&bad, -EINVAL);
   struct r300_vertex_job two_stores = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   expect_refusal(&two_stores, -EINVAL);
   /* The varying store: at most one, from a written register, ahead of
    * the final position store. */
   struct r300_vertex_job varying_last = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 0, 0, 0 },
      },
   };
   expect_refusal(&varying_last, -EINVAL);
   /* One store per location; the stored set runs contiguously from
    * location 0 and stays below R300_VERTEX_JOB_MAX_VARYINGS. */
   struct r300_vertex_job two_varyings = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   expect_refusal(&two_varyings, -EINVAL);
   two_varyings.instructions[2].dst = 1;
   assert(r300_cpu_vertex_job_validate(&two_varyings) == 0);
   assert(r300_vertex_job_varying_mask(&two_varyings) == 0x3u);
   assert(r300_vertex_job_record_dwords(&two_varyings) == 12);
   two_varyings.instructions[2].dst = R300_VERTEX_JOB_MAX_VARYINGS;
   expect_refusal(&two_varyings, -EINVAL);
   struct r300_vertex_job location_1_alone = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   expect_refusal(&location_1_alone, -EINVAL);
   struct r300_vertex_job varying_unwritten = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 1, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   expect_refusal(&varying_unwritten, -EINVAL);
}

/* A job storing a varying writes eight-dword records: position then
 * varying per vertex at the eight-dword stride, the placement the
 * consumer's two-FLOAT_4 fetch reads; the record count sets -ENOSPC,
 * and dwords past the last record stay untouched.
 */
static void test_varying_store_records(void)
{
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .constant_count = 1,
      .instruction_count = 5,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FADD, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 2, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   for (uint32_t lane = 0; lane < 4; lane++)
      job.constants[0][lane] = f_bits(0.5f);
   assert(r300_vertex_job_has_varying(&job));
   assert(r300_vertex_job_record_dwords(&job) == 8);
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const uint32_t records[3][4] = {
      { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
      { f_bits(-1.0f), f_bits(0.0f), f_bits(0.25f), f_bits(1.0f) },
      { 0x7fc00123u, 0x80000000u, f_bits(8.0f), f_bits(1.0f) },
   };
   struct r300_vertex_stream stream = stream_of(records, 16, sizeof(records));
   uint32_t carrier[CARRIER_DWORDS];
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 24) == 0);
   for (uint32_t v = 0; v < 3; v++) {
      assert(memcmp(&carrier[v * 8], records[v], 16) == 0);
      for (uint32_t lane = 0; lane < 4; lane++) {
         float in;
         memcpy(&in, &records[v][lane], sizeof(in));
         const float sum = in + 0.5f;
         uint32_t expect = f_bits(sum);
         if ((expect & 0x7f800000u) == 0x7f800000u &&
             (expect & 0x007fffffu) != 0)
            expect = 0x7fc00000u;
         assert(carrier[v * 8 + 4 + lane] == expect);
      }
   }
   for (uint32_t i = 24; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);

   /* One dword short of three eight-dword records refuses whole. */
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 23) ==
          -ENOSPC);
   for (uint32_t i = 0; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);
}

/* A job storing locations 0 and 1 writes twelve-dword records: position,
 * location 0, location 1 per vertex, the placement the mixed carrier
 * consumer's three-FLOAT_4 fetch reads; the store order in the job does
 * not move the vectors.
 */
static void test_two_varying_store_records(void)
{
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .constant_count = 1,
      .instruction_count = 6,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FADD, 2, 0, 1, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 1, 2, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 1, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   for (uint32_t lane = 0; lane < 4; lane++)
      job.constants[0][lane] = f_bits(0.5f);
   assert(r300_vertex_job_varying_count(&job) == 2);
   assert(r300_vertex_job_record_dwords(&job) == 12);
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const uint32_t records[3][4] = {
      { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
      { f_bits(-1.0f), f_bits(0.0f), f_bits(0.25f), f_bits(1.0f) },
      { f_bits(0.5f), f_bits(-2.0f), f_bits(8.0f), f_bits(1.0f) },
   };
   struct r300_vertex_stream stream = stream_of(records, 16, sizeof(records));
   uint32_t carrier[CARRIER_DWORDS];
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 36) == 0);
   for (uint32_t v = 0; v < 3; v++) {
      assert(memcmp(&carrier[v * 12], records[v], 16) == 0);
      for (uint32_t lane = 0; lane < 4; lane++) {
         assert(carrier[v * 12 + 4 + lane] == f_bits(0.5f));
         float in;
         memcpy(&in, &records[v][lane], sizeof(in));
         assert(carrier[v * 12 + 8 + lane] == f_bits(in + 0.5f));
      }
   }
   for (uint32_t i = 36; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 35) ==
          -ENOSPC);
}

/* Two attribute slots: slot 0 (F32_4) feeds the position, slot 1
 * (F32_3, alpha synthesized as 1) feeds the varying, each from its own
 * stream at its own stride.  Every per-slot refusal precedes the first
 * carrier write: a read slot with no bound format fails validation, a
 * read slot whose stream has no data or whose range the bound cannot
 * prove refuses, a carrier overlapping the second stream refuses, and
 * under the robust rule the second stream substitutes zeros on its
 * own.
 */
static void test_multi_attribute_slots(void)
{
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4,
                            R300_VERTEX_FORMAT_F32_3 },
      .instruction_count = 4,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 1, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_VARYING, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 1, 0, 0 },
      },
   };
   assert(r300_vertex_job_input_mask(&job) == 0x3u);
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const uint32_t positions[3][4] = {
      { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
      { f_bits(-1.0f), f_bits(0.0f), f_bits(0.25f), f_bits(1.0f) },
      { 0x7fc00123u, 0x80000000u, f_bits(8.0f), f_bits(1.0f) },
   };
   /* Three-component colors at a 20-byte stride with 8 pad bytes. */
   uint8_t colors[3 * 20];
   memset(colors, 0xcd, sizeof(colors));
   const uint32_t color_values[3][3] = {
      { f_bits(0.125f), f_bits(0.25f), f_bits(0.5f) },
      { 0x80000000u, f_bits(1.0f), 0x00000001u },
      { f_bits(0.875f), 0x7fc00321u, f_bits(0.0f) },
   };
   for (uint32_t v = 0; v < 3; v++)
      memcpy(&colors[v * 20], color_values[v], 12);
   struct r300_vertex_stream streams[2] = {
      stream_of(positions, 16, sizeof(positions)),
      stream_of(colors, 20, sizeof(colors)),
   };
   uint32_t carrier[CARRIER_DWORDS];
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, streams, 0, 3, carrier, 24) ==
          0);
   for (uint32_t v = 0; v < 3; v++) {
      assert(memcmp(&carrier[v * 8], positions[v], 16) == 0);
      assert(memcmp(&carrier[v * 8 + 4], color_values[v], 12) == 0);
      assert(carrier[v * 8 + 7] == f_bits(1.0f));
   }
   for (uint32_t i = 24; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);

   /* A slot read without a bound format fails validation. */
   struct r300_vertex_job unbound = job;
   unbound.input_format_ids[1] = R300_VERTEX_FORMAT_INVALID;
   assert(r300_cpu_vertex_job_validate(&unbound) == -EINVAL);
   /* A slot index beyond the slot count fails validation. */
   struct r300_vertex_job beyond = job;
   beyond.instructions[0].src0 = R300_VERTEX_JOB_MAX_INPUTS;
   assert(r300_cpu_vertex_job_validate(&beyond) == -EINVAL);
   /* A job reading slot 1 alone validates with slot 0 left unbound. */
   struct r300_vertex_job second_only = {
      .input_format_ids = { [1] = R300_VERTEX_FORMAT_F32_3 },
      .instruction_count = 2,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 1, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   assert(r300_vertex_job_input_mask(&second_only) == 0x2u);
   assert(r300_cpu_vertex_job_validate(&second_only) == 0);
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&second_only, streams, 0, 3, carrier,
                                      12) == 0);
   assert(memcmp(&carrier[4], color_values[1], 12) == 0 &&
          carrier[7] == f_bits(1.0f));

   /* The second stream without data refuses before any write. */
   struct r300_vertex_stream no_data[2] = { streams[0], streams[1] };
   no_data[1].data = NULL;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, no_data, 0, 3, carrier, 24) ==
          -EINVAL);
   for (uint32_t i = 0; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);
   /* The second stream one byte short of its last record refuses. */
   struct r300_vertex_stream short_second[2] = { streams[0], streams[1] };
   short_second[1].size_bytes = 2 * 20 + 11;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, short_second, 0, 3, carrier,
                                      24) == -EINVAL);
   for (uint32_t i = 0; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);
   /* Under the robust rule the same short stream reads its third record
    * as zeros with the synthesized alpha, the first stream untouched. */
   short_second[1].oob_reads_zero = true;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute(&job, short_second, 0, 3, carrier,
                                      24) == 0);
   assert(memcmp(&carrier[16], positions[2], 16) == 0);
   assert(carrier[20] == 0 && carrier[21] == 0 && carrier[22] == 0 &&
          carrier[23] == f_bits(1.0f));
   assert(memcmp(&carrier[12], color_values[1], 12) == 0);

   /* A carrier overlapping the second stream refuses whole. */
   uint32_t shared[64];
   memset(shared, 0, sizeof(shared));
   memcpy(shared, colors, sizeof(colors));
   struct r300_vertex_stream aliased[2] = { streams[0],
                                            stream_of(shared, 20, 60) };
   assert(r300_cpu_vertex_job_execute(&job, aliased, 0, 3, shared + 8,
                                      24) == -EINVAL);
   /* The same carrier past the second stream's bytes executes. */
   assert(r300_cpu_vertex_job_execute(&job, aliased, 0, 3, shared + 15,
                                      24) == 0);
   assert(memcmp(&shared[15 + 4], color_values[0], 12) == 0);
}

static void test_execute_refusals_no_partial_write(void)
{
   const uint32_t records[3][4] = { { 1, 2, 3, 4 } };
   const struct r300_vertex_job job = identity_job(R300_VERTEX_FORMAT_F32_4);
   struct r300_vertex_stream stream =
      stream_of(records, 16, sizeof(records));
   uint32_t carrier[CARRIER_DWORDS];

   fill_canary(carrier);
   int rc = r300_cpu_vertex_job_execute(&job, NULL, 0, 1, carrier,
                                        CARRIER_DWORDS);
   assert(rc == -EINVAL);
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 0, carrier,
                                    CARRIER_DWORDS);
   assert(rc == -EINVAL);
   /* One dword short of 3 vertices: -ENOSPC, nothing written. */
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 11);
   assert(rc == -ENOSPC);
   /* Fourth vertex lies past the stream bound. */
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 4, carrier,
                                    CARRIER_DWORDS);
   assert(rc == -EINVAL);
   /* first_vertex + count overflow territory stays refused. */
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0xffffffffu, 2, carrier,
                                    CARRIER_DWORDS);
   assert(rc == -EINVAL);
   /* A robust stream still cannot represent a logical vertex above the
    * 32-bit gather address space; the guard rejects the range before the
    * robust zero substitution can hide the wrapped index. */
   stream.oob_reads_zero = true;
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0xffffffffu, 2, carrier,
                                    CARRIER_DWORDS);
   assert(rc == -EINVAL);
   for (uint32_t i = 0; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);

   /* Carrier overlapping the stream bytes refuses before writing. */
   static uint32_t shared[16] = { 1, 2, 3, 4 };
   struct r300_vertex_stream aliased =
      stream_of(shared, 16, sizeof(shared));
   rc = r300_cpu_vertex_job_execute(&job, &aliased, 0, 1, shared + 8, 8);
   assert(rc == -EINVAL);
   assert(shared[8] == 0);
}

static void test_determinism(void)
{
   const uint32_t records[3][4] = {
      { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
      { 0x7fc00001u, 0x80000000u, f_bits(-5.5f), f_bits(0.125f) },
      { f_bits(9.0f), f_bits(-9.0f), f_bits(0.0f), f_bits(1.0f) },
   };
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 5,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_LOAD_CONSTANT, 1, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_FMAD, 2, 0, 1, 1 },
         { R300_VERTEX_JOB_OP_DP4, 3, 2, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 3, 0, 0 },
      },
      .constant_count = 1,
      .constants = {
         { f_bits(1.25f), f_bits(-2.0f), f_bits(3.5f), f_bits(0.75f) },
      },
   };
   const struct r300_vertex_stream stream =
      stream_of(records, 16, sizeof(records));
   uint32_t first[CARRIER_DWORDS], second[CARRIER_DWORDS];
   fill_canary(first);
   fill_canary(second);
   int rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 3, first,
                                        CARRIER_DWORDS);
   assert(rc == 0);
   rc = r300_cpu_vertex_job_execute(&job, &stream, 0, 3, second,
                                    CARRIER_DWORDS);
   assert(rc == 0);
   assert(memcmp(first, second, sizeof(first)) == 0);
}

/* Robust reads: with oob_reads_zero clear an out-of-bounds range refuses
 * before any write; set, the records inside the bound gather verbatim and
 * each record outside reads raw zeros with the absent components
 * synthesized (0, 0, 1), on the dispatcher, the baseline, and the job
 * executor alike.
 */
static void test_robust_out_of_bounds_reads_zero(void)
{
   const float records[8] = { 1.0f, 2.0f, 3.0f, 4.0f,
                              5.0f, 6.0f, 7.0f, 8.0f };
   uint32_t carrier[CARRIER_DWORDS];
   uint32_t baseline[CARRIER_DWORDS];

   /* Two 16-byte records, three vertices requested: the third is outside. */
   struct r300_vertex_stream stream = stream_of(records, 16, 32);
   fill_canary(carrier);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &stream, 0, 3,
                                 carrier, CARRIER_DWORDS) == -EINVAL);
   assert(carrier[0] == CANARY && carrier[11] == CANARY);
   assert(!r300_cpu_vertex_range_in_bounds(R300_VERTEX_FORMAT_F32_4,
                                           &stream, 0, 3));
   assert(r300_cpu_vertex_range_in_bounds(R300_VERTEX_FORMAT_F32_4,
                                          &stream, 0, 2));

   stream.oob_reads_zero = true;
   static const struct {
      enum r300_vertex_format_id format;
      uint32_t tail[4];
   } rows[] = {
      { R300_VERTEX_FORMAT_F32_4, { 0, 0, 0, 0 } },
      { R300_VERTEX_FORMAT_F32_3, { 0, 0, 0, 0x3f800000u } },
      { R300_VERTEX_FORMAT_F32_2, { 0, 0, 0, 0x3f800000u } },
   };
   for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
      fill_canary(carrier);
      fill_canary(baseline);
      assert(r300_cpu_vertex_gather(rows[i].format, &stream, 0, 3, carrier,
                                    CARRIER_DWORDS) == 0);
      assert(r300_cpu_vertex_gather_baseline(rows[i].format, &stream, 0, 3,
                                             baseline, CARRIER_DWORDS) == 0);
      assert(memcmp(carrier, baseline, 12 * sizeof(uint32_t)) == 0);
      assert(carrier[0] == f_bits(1.0f) && carrier[4] == f_bits(5.0f));
      assert(memcmp(&carrier[8], rows[i].tail, sizeof(rows[i].tail)) == 0);
      assert(carrier[12] == CANARY);

      /* The identity job executes the same rule. */
      struct r300_vertex_job job = {
         .input_format_ids[0] = rows[i].format,
         .instruction_count = 2,
         .instructions = {
            { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0 },
            { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 0 },
         },
      };
      fill_canary(baseline);
      assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, baseline,
                                         CARRIER_DWORDS) == 0);
      assert(memcmp(carrier, baseline, 12 * sizeof(uint32_t)) == 0);
      stream.oob_reads_zero = false;
      fill_canary(baseline);
      assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, baseline,
                                         CARRIER_DWORDS) == -EINVAL);
      assert(baseline[0] == CANARY);
      stream.oob_reads_zero = true;
   }

   /* A stream shorter than one record reads every vertex as zeros. */
   struct r300_vertex_stream short_stream = stream_of(records, 16, 8);
   short_stream.oob_reads_zero = true;
   fill_canary(carrier);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_3, &short_stream,
                                 0, 2, carrier, CARRIER_DWORDS) == 0);
   assert(carrier[0] == 0 && carrier[3] == 0x3f800000u &&
          carrier[4] == 0 && carrier[7] == 0x3f800000u);

   /* Zero stride reads the base record for every vertex, in or out. */
   struct r300_vertex_stream constant = stream_of(records, 0, 16);
   constant.oob_reads_zero = true;
   fill_canary(carrier);
   assert(r300_cpu_vertex_gather(R300_VERTEX_FORMAT_F32_4, &constant, 0, 3,
                                 carrier, CARRIER_DWORDS) == 0);
   assert(carrier[8] == f_bits(1.0f) && carrier[11] == f_bits(4.0f));
}

/* The indexed form: a vertex-id list permutes and repeats the records
 * of the linear form, each listed vertex bounds on its own (robust
 * substitutes, clear refuses before any write), and a NULL list
 * refuses. */
static void test_indexed_execution(void)
{
   const float records[16] = { 1.0f, 2.0f, 3.0f, 4.0f,
                               5.0f, 6.0f, 7.0f, 8.0f,
                               9.0f, 10.0f, 11.0f, 12.0f,
                               13.0f, 14.0f, 15.0f, 16.0f };
   struct r300_vertex_stream stream = stream_of(records, 16, 64);
   struct r300_vertex_job job = {
      .input_format_ids[0] = R300_VERTEX_FORMAT_F32_4,
      .instruction_count = 2,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 0 },
      },
   };
   uint32_t carrier[CARRIER_DWORDS];
   uint32_t linear[CARRIER_DWORDS];

   /* A permutation with a repeat: records 3, 0, 3. */
   const uint32_t ids[3] = { 3, 0, 3 };
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_indexed(&job, &stream, ids, 3, carrier,
                                              CARRIER_DWORDS) == 0);
   assert(carrier[0] == f_bits(13.0f) && carrier[3] == f_bits(16.0f));
   assert(carrier[4] == f_bits(1.0f) && carrier[7] == f_bits(4.0f));
   assert(carrier[8] == f_bits(13.0f) && carrier[11] == f_bits(16.0f));
   assert(carrier[12] == CANARY);

   /* The identity list equals the linear form byte for byte. */
   const uint32_t identity[3] = { 1, 2, 3 };
   fill_canary(carrier);
   fill_canary(linear);
   assert(r300_cpu_vertex_job_execute_indexed(&job, &stream, identity, 3,
                                              carrier, CARRIER_DWORDS) == 0);
   assert(r300_cpu_vertex_job_execute(&job, &stream, 1, 3, linear,
                                      CARRIER_DWORDS) == 0);
   assert(memcmp(carrier, linear, sizeof(carrier)) == 0);

   /* A listed vertex past the bound: clear refuses before any write,
    * robust reads it as zeros while the in-bounds entries deliver, and
    * the same holds for the wrapped 32-bit sum a negative base vertex
    * forms. */
   const uint32_t out_of_bounds[3] = { 0, 4, 1 };
   const uint32_t wrapped[3] = { 0, 0xffffffffu, 1 };
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_indexed(&job, &stream, out_of_bounds,
                                              3, carrier,
                                              CARRIER_DWORDS) == -EINVAL);
   assert(carrier[0] == CANARY && carrier[11] == CANARY);
   assert(r300_cpu_vertex_job_execute_indexed(&job, &stream, wrapped, 3,
                                              carrier,
                                              CARRIER_DWORDS) == -EINVAL);
   assert(carrier[0] == CANARY);
   stream.oob_reads_zero = true;
   for (int list = 0; list < 2; list++) {
      fill_canary(carrier);
      assert(r300_cpu_vertex_job_execute_indexed(
                &job, &stream, list ? wrapped : out_of_bounds, 3, carrier,
                CARRIER_DWORDS) == 0);
      assert(carrier[0] == f_bits(1.0f) && carrier[3] == f_bits(4.0f));
      assert(carrier[4] == 0 && carrier[5] == 0 && carrier[6] == 0 &&
             carrier[7] == 0);
      assert(carrier[8] == f_bits(5.0f) && carrier[11] == f_bits(8.0f));
   }
   stream.oob_reads_zero = false;

   /* A NULL list and a zero count refuse. */
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_indexed(&job, &stream, NULL, 3, carrier,
                                              CARRIER_DWORDS) == -EINVAL);
   assert(r300_cpu_vertex_job_execute_indexed(&job, &stream, ids, 0, carrier,
                                              CARRIER_DWORDS) == -EINVAL);
   assert(carrier[0] == CANARY);
}

/* Instanced execution: instance-major records, instance-rate streams
 * under the Vulkan address calculation at divisors 1, 2, and 0, the
 * per-instance record bound (clear refuses, robust reads zeros), the
 * 32-bit record-space refusal, and a zero instance count. */
static void test_instanced_execution(void)
{
   /* Three positions at stride 16 and four offset records at stride
    * 16; the job adds the instance-rate offset to the position. */
   const float positions[12] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f,  6.0f,
                                 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f };
   const float offsets[16] = { 100.0f, 0.0f, 0.0f, 0.0f, 200.0f, 0.0f,
                               0.0f,   0.0f, 300.0f, 0.0f, 0.0f, 0.0f,
                               400.0f, 0.0f, 0.0f,   0.0f };
   struct r300_vertex_stream streams[2] = {
      stream_of(positions, 16, sizeof(positions)),
      stream_of(offsets, 16, sizeof(offsets)),
   };
   streams[1].instance_rate = true;
   streams[1].instance_divisor = 1;
   struct r300_vertex_job job = {
      .input_format_ids = { R300_VERTEX_FORMAT_F32_4,
                            R300_VERTEX_FORMAT_F32_4 },
      .instruction_count = 4,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 0, .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_LOAD_INPUT, .dst = 1, .src0 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_FADD, .dst = 2, .src0 = 0,
           .src1 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 2 },
      },
   };
   uint32_t carrier[CARRIER_DWORDS];

   /* Two instances from first_instance 1: instance i reads offset
    * record 1 + i, and the records land instance-major. */
   struct r300_cpu_vertex_draw draw = {
      .vertex_count = 3,
      .first_instance = 1,
      .instance_count = 2,
   };
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   for (unsigned i = 0; i < 2; i++) {
      for (unsigned v = 0; v < 3; v++) {
         const uint32_t *record = &carrier[(i * 3 + v) * 4];
         assert(record[0] == f_bits(positions[v * 4] + offsets[(1 + i) * 4]));
         assert(record[1] == f_bits(positions[v * 4 + 1]));
         assert(record[3] == f_bits(positions[v * 4 + 3]));
      }
   }
   assert(carrier[24] == CANARY);

   /* Divisor 2 over four instances from first_instance 0: instances 0
    * and 1 read record 0, instances 2 and 3 read record 1. */
   streams[1].instance_divisor = 2;
   draw.first_instance = 0;
   draw.instance_count = 4;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   for (unsigned i = 0; i < 4; i++)
      assert(carrier[i * 12] == f_bits(positions[0] + offsets[(i / 2) * 4]));
   assert(carrier[48] == CANARY);

   /* Divisor 0: every instance reads first_instance's record. */
   streams[1].instance_divisor = 0;
   draw.first_instance = 3;
   draw.instance_count = 4;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   for (unsigned i = 0; i < 4; i++)
      assert(carrier[i * 12] == f_bits(positions[0] + offsets[12]));

   /* The instance-record bound: first_instance 3 over two instances at
    * divisor 1 reads record 4 of a four-record stream -- clear refuses
    * before any write, robust reads the record as zeros. */
   streams[1].instance_divisor = 1;
   draw.first_instance = 3;
   draw.instance_count = 2;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == -EINVAL);
   assert(carrier[0] == CANARY && carrier[23] == CANARY);
   streams[1].oob_reads_zero = true;
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   assert(carrier[0] == f_bits(positions[0] + offsets[12]));
   assert(carrier[12] == f_bits(positions[0]));
   assert(carrier[15] == f_bits(positions[3]));

   /* A last instance record past the 32-bit record space refuses on a
    * robust stream too: the record number names nothing the gather or
    * the VAP can address. */
   draw.first_instance = 0xffffffffu;
   draw.instance_count = 2;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == -EINVAL);
   assert(carrier[0] == CANARY);
   /* Divisor 0 addresses first_instance alone, so the same draw
    * executes. */
   streams[1].instance_divisor = 0;
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   assert(carrier[0] == f_bits(positions[0]));
   streams[1].oob_reads_zero = false;
   streams[1].instance_divisor = 1;

   /* A zero instance count refuses; the carrier capacity counts every
    * instance's records. */
   draw.first_instance = 0;
   draw.instance_count = 0;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == -EINVAL);
   draw.instance_count = 4;
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           47) == -ENOSPC);
   assert(carrier[0] == CANARY);

   /* The one-instance linear entry equals the draw form byte for byte
    * over an instance-rate stream: instance 0 reads record 0. */
   uint32_t linear[CARRIER_DWORDS];
   draw.instance_count = 1;
   fill_canary(carrier);
   fill_canary(linear);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   assert(r300_cpu_vertex_job_execute(&job, streams, 0, 3, linear,
                                      CARRIER_DWORDS) == 0);
   assert(memcmp(carrier, linear, sizeof(carrier)) == 0);
}

/* The draw system values: VertexIndex is the vertex number (the linear
 * first_vertex sum or the listed id), InstanceIndex is first_instance
 * plus the relative instance -- the Vulkan values, which carry the
 * draw's base -- broadcast as int patterns and converted to float under
 * round-to-nearest-even. */
static void test_system_values(void)
{
   struct r300_vertex_job job = {
      .instruction_count = 5,
      .instructions = {
         { .opcode = R300_VERTEX_JOB_OP_LOAD_SYSTEM_VALUE, .dst = 0,
           .src0 = R300_VERTEX_JOB_SV_VERTEX_INDEX },
         { .opcode = R300_VERTEX_JOB_OP_LOAD_SYSTEM_VALUE, .dst = 1,
           .src0 = R300_VERTEX_JOB_SV_INSTANCE_INDEX },
         { .opcode = R300_VERTEX_JOB_OP_CONVERT_S_TO_F, .dst = 2,
           .src0 = 0 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_VARYING, .src0 = 1 },
         { .opcode = R300_VERTEX_JOB_OP_STORE_POSITION, .src0 = 2 },
      },
   };
   assert(r300_cpu_vertex_job_validate(&job) == 0);
   /* The job reads no stream; a zeroed stream table serves it. */
   const struct r300_vertex_stream streams[R300_VERTEX_JOB_MAX_INPUTS] = {
      0
   };
   uint32_t carrier[CARRIER_DWORDS];

   /* Linear from vertex 5, two instances from instance 7. */
   struct r300_cpu_vertex_draw draw = {
      .first_vertex = 5,
      .vertex_count = 3,
      .first_instance = 7,
      .instance_count = 2,
   };
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   for (unsigned i = 0; i < 2; i++) {
      for (unsigned v = 0; v < 3; v++) {
         const uint32_t *record = &carrier[(i * 3 + v) * 8];
         for (unsigned lane = 0; lane < 4; lane++) {
            assert(record[lane] == f_bits((float)(5 + v)));
            assert(record[4 + lane] == 7 + i);
         }
      }
   }
   assert(carrier[48] == CANARY);

   /* The listed vertex numbers after a base-vertex sum, including a
    * wrapped negative one, and the conversion's rounding: 2^24 + 1 is
    * a tie that rounds to even, and 0xffffffff converts as -1. */
   const uint32_t ids[3] = { 9, 16777217u, 0xffffffffu };
   draw.vertex_ids = ids;
   draw.instance_count = 1;
   fill_canary(carrier);
   assert(r300_cpu_vertex_job_execute_draw(&job, streams, &draw, carrier,
                                           CARRIER_DWORDS) == 0);
   assert(carrier[0] == f_bits(9.0f));
   assert(carrier[8] == f_bits(16777216.0f));
   assert(carrier[16] == f_bits(-1.0f));
   assert(carrier[4] == 7 && carrier[12] == 7 && carrier[20] == 7);

   /* Validation: a system value outside the two, and a conversion of
    * an unwritten register, refuse. */
   struct r300_vertex_job bad = job;
   bad.instructions[0].src0 = R300_VERTEX_JOB_SV_COUNT;
   assert(r300_cpu_vertex_job_validate(&bad) == -EINVAL);
   bad = job;
   bad.instructions[2].src0 = 5;
   assert(r300_cpu_vertex_job_validate(&bad) == -EINVAL);
}

int main(void)
{
   test_identity_preserves_bits();
   test_robust_out_of_bounds_reads_zero();
   test_f32_2_synthesis();
   test_arithmetic_exact();
   test_multiply_add_rounding();
   test_dot_product_rounding();
   test_float_environment_isolation();
   test_dp4_order_and_broadcast();
   test_mov_preserves_nan_payload();
   test_validation_refusals();
   test_varying_store_records();
   test_two_varying_store_records();
   test_multi_attribute_slots();
   test_indexed_execution();
   test_instanced_execution();
   test_system_values();
   test_execute_refusals_no_partial_write();
   test_determinism();
   printf("r300_cpu_vertex_job_test: all cases pass\n");
   return 0;
}
