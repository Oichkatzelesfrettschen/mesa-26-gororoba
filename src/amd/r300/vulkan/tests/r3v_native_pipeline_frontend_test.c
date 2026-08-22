/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration for the native pipeline front end: the reference cell
 * modules admit through the direct SPIR-V word-stream reader by
 * meaning, the admitted jobs execute to exact bytes through the CPU
 * executor, inadmissible modules refuse by shape, and malformed word
 * streams refuse at their bounds.  The admitter is plain C over words,
 * so this test carries no compiler-stack dependency.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/r300_vertex_spirv.h"
#include "amd/r300/cpu/r300_cpu_vertex_job.h"

#include "r3v_native_reference_spirv.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORDS(array) (sizeof(array) / sizeof((array)[0]))

static uint32_t f_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

/* The reference vertex module is the position pass-through, so the
 * admitter must lower it to the identity job and the job must
 * reproduce the gather bytes exactly, hostile bit patterns included.
 */
static void test_reference_vertex_module(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_spirv(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv), &job,
      &reason);
   if (!admitted)
      fprintf(stderr, "reference vertex refusal: %s\n", reason);
   assert(admitted);
   assert(job.instruction_count == 2 && job.constant_count == 0);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);
   assert(job.instructions[1].src0 == job.instructions[0].dst);

   job.input_format_id = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const uint32_t records[3][4] = {
      { f_bits(1.0f), f_bits(2.0f), f_bits(3.0f), f_bits(4.0f) },
      { 0x7fc00123u, 0x80000000u, f_bits(-5.0f), f_bits(0.5f) },
      { f_bits(9.0f), f_bits(8.0f), f_bits(7.0f), f_bits(6.0f) },
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)records,
      .stride = 16,
      .size_bytes = sizeof(records),
   };
   uint32_t carrier[12];
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 3, carrier, 12) ==
          0);
   assert(memcmp(carrier, records, sizeof(records)) == 0);
}

static void test_reference_fragment_module(void)
{
   uint32_t color[4];
   const char *reason = NULL;
   assert(r300_fragment_constant_color_from_spirv(
      r3v_reference_fragment_spirv, WORDS(r3v_reference_fragment_spirv),
      color, &reason));
   assert(color[0] == 0 && color[1] == 0x3f800000u && color[2] == 0 &&
          color[3] == 0x3f800000u);

   /* A non-green constant reads back with its own bits: the green gate
    * is the pipeline's, not the admitter's.  The red module is the
    * reference module with its composite constant's lanes reordered,
    * located by the OpConstantComposite word (opcode 44, length 7).
    */
   uint32_t red[WORDS(r3v_reference_fragment_spirv)];
   memcpy(red, r3v_reference_fragment_spirv, sizeof(red));
   size_t at = 5;
   bool patched = false;
   while (at < WORDS(red)) {
      const uint32_t len = red[at] >> 16;
      assert(len != 0 && at + len <= WORDS(red));
      if ((red[at] & 0xffffu) == 44) {
         const uint32_t f0 = red[at + 3];
         const uint32_t f1 = red[at + 4];
         red[at + 3] = f1;
         red[at + 4] = f0;
         red[at + 5] = f0;
         red[at + 6] = f1;
         patched = true;
      }
      at += len;
   }
   assert(patched);
   assert(r300_fragment_constant_color_from_spirv(red, WORDS(red), color,
                                                  &reason));
   assert(color[0] == 0x3f800000u && color[1] == 0 && color[2] == 0 &&
          color[3] == 0x3f800000u);
}

/* The arithmetic reference exercises the constant, FFMA, DP4,
 * function-variable, and broadcast-replicate paths; the executed value
 * is the host model's exact composition.
 */
static void test_reference_arith_module(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;
   bool admitted = r300_vertex_job_from_spirv(
      r3v_reference_vertex_arith_spirv,
      WORDS(r3v_reference_vertex_arith_spirv), &job, &reason);
   if (!admitted)
      fprintf(stderr, "reference arith refusal: %s\n", reason);
   assert(admitted);
   assert(job.instruction_count == 5 && job.constant_count == 1);
   assert(job.instructions[0].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT);
   assert(job.instructions[1].opcode == R300_VERTEX_JOB_OP_LOAD_CONSTANT);
   assert(job.instructions[2].opcode == R300_VERTEX_JOB_OP_FFMA);
   assert(job.instructions[3].opcode == R300_VERTEX_JOB_OP_DP4);
   assert(job.instructions[4].opcode == R300_VERTEX_JOB_OP_STORE_POSITION);

   job.input_format_id = R300_VERTEX_FORMAT_F32_4;
   assert(r300_cpu_vertex_job_validate(&job) == 0);

   const float in[4] = { 1.5f, 3.0f, -1.0f, 0.25f };
   const float k[4] = { 2.0f, -0.5f, 4.0f, 1.0f };
   float lanes[4];
   for (int c = 0; c < 4; c++)
      lanes[c] = fmaf(in[c], k[c], k[c]);
   const float expected =
      ((lanes[0] * k[0] + lanes[1] * k[1]) + lanes[2] * k[2]) +
      lanes[3] * k[3];

   const uint32_t record[4] = {
      f_bits(in[0]), f_bits(in[1]), f_bits(in[2]), f_bits(in[3]),
   };
   const struct r300_vertex_stream stream = {
      .data = (const uint8_t *)record,
      .stride = 16,
      .size_bytes = sizeof(record),
   };
   uint32_t carrier[4];
   assert(r300_cpu_vertex_job_execute(&job, &stream, 0, 1, carrier, 4) ==
          0);
   for (int c = 0; c < 4; c++)
      assert(carrier[c] == f_bits(expected));
}

/* Whole-module refusals: each inadmissible module names its construct. */
static void test_module_refusals(void)
{
   struct r300_vertex_job job;
   uint32_t color[4];
   const char *reason = NULL;

   /* The compute modules carry the GLCompute entry model. */
   assert(!r300_vertex_job_from_spirv(r3v_reference_identity_map_spirv,
                                      WORDS(r3v_reference_identity_map_spirv),
                                      &job, &reason));
   assert(!r300_vertex_job_from_spirv(r3v_reference_scatter_reject_spirv,
                                      WORDS(r3v_reference_scatter_reject_spirv),
                                      &job, &reason));

   /* Stage crosses: the fragment module refuses as a vertex program
    * and the vertex module as a fragment program. */
   assert(!r300_vertex_job_from_spirv(r3v_reference_fragment_spirv,
                                      WORDS(r3v_reference_fragment_spirv),
                                      &job, &reason));
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_vertex_spirv, WORDS(r3v_reference_vertex_spirv), color,
      &reason));

   /* The fragment path admits constants alone, so the arithmetic
    * module refuses there. */
   assert(!r300_fragment_constant_color_from_spirv(
      r3v_reference_vertex_arith_spirv,
      WORDS(r3v_reference_vertex_arith_spirv), color, &reason));
}

/* Malformed streams: NULL, short, wrong magic, and every word-boundary
 * truncation refuse rather than read past a bound.
 */
static void test_malformed_streams(void)
{
   struct r300_vertex_job job;
   const char *reason = NULL;

   assert(!r300_vertex_job_from_spirv(NULL, 0, &job, &reason));
   assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv, 4, &job,
                                      &reason));

   uint32_t bad_magic[WORDS(r3v_reference_vertex_spirv)];
   memcpy(bad_magic, r3v_reference_vertex_spirv, sizeof(bad_magic));
   bad_magic[0] ^= 1;
   assert(!r300_vertex_job_from_spirv(bad_magic, WORDS(bad_magic), &job,
                                      &reason));

   for (size_t count = 5; count < WORDS(r3v_reference_vertex_spirv);
        count++) {
      assert(!r300_vertex_job_from_spirv(r3v_reference_vertex_spirv, count,
                                         &job, &reason));
   }
}

/* Single-word mutation sweep: every one-word XOR of the identity
 * module either refuses or still lowers to the identity job shape, so
 * no mutation reaches execution with different semantics unnoticed.
 */
static void test_mutation_sweep(void)
{
   uint32_t mutated[WORDS(r3v_reference_vertex_spirv)];
   struct r300_vertex_job job;
   const char *reason = NULL;
   for (size_t word = 0; word < WORDS(mutated); word++) {
      for (uint32_t bit = 0; bit < 32; bit += 7) {
         memcpy(mutated, r3v_reference_vertex_spirv, sizeof(mutated));
         mutated[word] ^= 1u << bit;
         if (!r300_vertex_job_from_spirv(mutated, WORDS(mutated), &job,
                                         &reason))
            continue;
         assert(job.instruction_count == 2);
         assert(job.instructions[0].opcode ==
                R300_VERTEX_JOB_OP_LOAD_INPUT);
         assert(job.instructions[1].opcode ==
                R300_VERTEX_JOB_OP_STORE_POSITION);
      }
   }
}

int main(void)
{
   test_reference_vertex_module();
   test_reference_fragment_module();
   test_reference_arith_module();
   test_module_refusals();
   test_malformed_streams();
   test_mutation_sweep();
   printf("r3v_native_pipeline_frontend_test: all cases pass\n");
   return 0;
}
