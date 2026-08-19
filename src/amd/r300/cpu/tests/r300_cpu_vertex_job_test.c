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

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static struct r300_cpu_vertex_stream stream_of(const void *data,
                                               uint32_t stride,
                                               uint64_t size_bytes)
{
   struct r300_cpu_vertex_stream stream = {
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
      .input_format_id = format_id,
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
   const struct r300_cpu_vertex_stream stream =
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
   const struct r300_cpu_vertex_stream stream =
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
   const struct r300_cpu_vertex_stream stream = stream_of(input, 16, 16);
   int rc = r300_cpu_vertex_job_execute(job, &stream, 0, 1, carrier, 4);
   assert(rc == 0);
   memcpy(out, carrier, sizeof(carrier));
}

static void test_arithmetic_exact(void)
{
   /* t0 = input; t1 = const0; t2 = t0 + t1; t3 = t2 * t1; store t3. */
   struct r300_vertex_job job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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

   /* inf + (-inf) is a NaN under the host binary32 policy. */
   struct r300_vertex_job nan_job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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
   assert((out[0] & 0x7f800000u) == 0x7f800000u && (out[0] & 0x007fffffu));
}

static void test_fmad_two_roundings(void)
{
   /* a = b = 1 + 2^-12; a * b = 1 + 2^-11 + 2^-24 rounds (ties to
    * even) to 1 + 2^-11 in binary32, so the two-rounding FMAD of
    * a, b, -(1 + 2^-11) is exactly +0; a fused operator would return
    * 2^-24.  This pins the documented double rounding. */
   const float a = 1.0f + 0x1.0p-12f;
   const float c = -(1.0f + 0x1.0p-11f);
   struct r300_vertex_job job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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
}

static void test_dp4_order_and_broadcast(void)
{
   /* dot((1,2,3,4), (2,3,4,5)) = 2 + 6 + 12 + 20 = 40 exactly, and
    * the scalar broadcasts to all four lanes. */
   struct r300_vertex_job job = {
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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
   bad.input_format_id = R300_VERTEX_FORMAT_INVALID;
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
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
      .instruction_count = 3,
      .instructions = {
         { R300_VERTEX_JOB_OP_LOAD_INPUT, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
         { R300_VERTEX_JOB_OP_STORE_POSITION, 0, 0, 0, 0 },
      },
   };
   expect_refusal(&two_stores, -EINVAL);
}

static void test_execute_refusals_no_partial_write(void)
{
   const uint32_t records[3][4] = { { 1, 2, 3, 4 } };
   const struct r300_vertex_job job = identity_job(R300_VERTEX_FORMAT_F32_4);
   struct r300_cpu_vertex_stream stream =
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
   for (uint32_t i = 0; i < CARRIER_DWORDS; i++)
      assert(carrier[i] == CANARY);

   /* Carrier overlapping the stream bytes refuses before writing. */
   static uint32_t shared[16] = { 1, 2, 3, 4 };
   struct r300_cpu_vertex_stream aliased =
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
      .input_format_id = R300_VERTEX_FORMAT_F32_4,
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
   const struct r300_cpu_vertex_stream stream =
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

int main(void)
{
   test_identity_preserves_bits();
   test_f32_2_synthesis();
   test_arithmetic_exact();
   test_fmad_two_roundings();
   test_dp4_order_and_broadcast();
   test_mov_preserves_nan_payload();
   test_validation_refusals();
   test_execute_refusals_no_partial_write();
   test_determinism();
   printf("r300_cpu_vertex_job_test: all cases pass\n");
   return 0;
}
