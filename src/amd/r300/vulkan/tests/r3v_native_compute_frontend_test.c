/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration for the direct SPIR-V compute admission: the reference
 * identity-map and bitwise-complement modules admit by meaning and
 * execute to exact bytes through the CPU executor, the reference
 * scatter module refuses, and each malformed or out-of-grammar word
 * stream refuses without a job.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "amd/r300/common/r300_compute_spirv.h"
#include "amd/r300/cpu/r300_cpu_compute_job.h"

#include "r3v_native_reference_spirv.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The reference identity-map module lowers to the identity job over
 * bindings 0 and 1 with the declared 64x1x1 workgroup, and the job
 * executes to exact bytes through the CPU executor.
 */
static void test_reference_identity_module(void)
{
   struct r300_compute_job job;
   const char *reason = NULL;
   bool admitted = r300_compute_job_from_spirv(
      r3v_reference_identity_map_spirv,
      sizeof(r3v_reference_identity_map_spirv) / 4, "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "identity refusal: %s\n", reason);
   assert(admitted);
   assert(job.op == R300_COMPUTE_JOB_OP_IDENTITY);
   assert(job.input_binding == 0 && job.output_binding == 1);
   assert(job.local_size[0] == 64 && job.local_size[1] == 1 &&
          job.local_size[2] == 1);

   assert(r300_cpu_compute_job_validate(&job) == 0);

   /* One workgroup: 64 words move verbatim, bit patterns preserved,
    * and bytes past the invocation range stay untouched.
    */
   uint32_t input[65];
   uint32_t output[65];
   for (uint32_t i = 0; i < 65; i++) {
      input[i] = 0x80000000u + i * 0x01010101u;
      output[i] = 0xdeadbeefu;
   }
   input[3] = 0x7fc00123u; /* NaN payload survives the bit copy. */
   const uint32_t groups[3] = { 1, 1, 1 };
   int rc = r300_cpu_compute_job_execute(&job, groups, input,
                                         sizeof(input), output,
                                         sizeof(output));
   assert(rc == 0);
   assert(memcmp(output, input, 64 * 4) == 0);
   assert(output[64] == 0xdeadbeefu);
}

/* The reference bitwise-complement module lowers to the complement job
 * over bindings 0 and 1, and the job flips every bit of every element
 * through the CPU executor while bytes past the invocation range stay
 * untouched.
 */
static void test_reference_bitwise_not_module(void)
{
   struct r300_compute_job job;
   const char *reason = NULL;
   bool admitted = r300_compute_job_from_spirv(
      r3v_reference_bitwise_not_spirv,
      sizeof(r3v_reference_bitwise_not_spirv) / 4, "main", &job, &reason);
   if (!admitted)
      fprintf(stderr, "bitwise-complement refusal: %s\n", reason);
   assert(admitted);
   assert(job.op == R300_COMPUTE_JOB_OP_BITWISE_NOT);
   assert(job.input_binding == 0 && job.output_binding == 1);
   assert(job.local_size[0] == 64 && job.local_size[1] == 1 &&
          job.local_size[2] == 1);
   assert(r300_cpu_compute_job_validate(&job) == 0);

   uint32_t input[65];
   uint32_t output[65];
   for (uint32_t i = 0; i < 65; i++) {
      input[i] = 0x80000000u + i * 0x01010101u;
      output[i] = 0xdeadbeefu;
   }
   input[3] = 0x7fc00123u;
   input[4] = 0x00000000u;
   input[5] = 0xffffffffu;
   const uint32_t groups[3] = { 1, 1, 1 };
   int rc = r300_cpu_compute_job_execute(&job, groups, input,
                                         sizeof(input), output,
                                         sizeof(output));
   assert(rc == 0);
   for (uint32_t i = 0; i < 64; i++)
      assert(output[i] == ~input[i]);
   assert(output[64] == 0xdeadbeefu);
}

/* The known-bad mutation the complement grammar must refuse: the
 * module computes the complement and then stores the loaded word, so
 * the admitted meaning would drop the arithmetic step the module
 * carries.  Swapping the store's value operand back to the load's
 * result id builds exactly that module.
 */
static void test_unread_complement_refuses(void)
{
   const size_t words = sizeof(r3v_reference_bitwise_not_spirv) / 4;
   uint32_t mutated[2048];
   assert(words <= 2048);
   memcpy(mutated, r3v_reference_bitwise_not_spirv, words * 4);

   /* OpNot (opcode 200) names its result in word 2 and its operand in
    * word 3; the store that follows carries the result as its value.
    */
   size_t at = 5;
   uint32_t complement = 0;
   uint32_t loaded = 0;
   bool rewrote = false;
   while (at < words) {
      const uint32_t opcode = mutated[at] & 0xffffu;
      const uint32_t len = mutated[at] >> 16;
      assert(len != 0 && at + len <= words);
      if (opcode == 200 && len == 4) {
         complement = mutated[at + 2];
         loaded = mutated[at + 3];
      } else if (opcode == 62 && complement != 0 &&
                 mutated[at + 2] == complement) {
         mutated[at + 2] = loaded;
         rewrote = true;
      }
      at += len;
   }
   assert(rewrote);

   struct r300_compute_job job;
   const char *reason = NULL;
   assert(!r300_compute_job_from_spirv(mutated, words, "main", &job,
                                       &reason));
   assert(strcmp(reason,
                 "stored value leaves the bitwise complement unread") == 0);
}

/* The reference scatter module (out[i + 1] = in[i]) refuses: its
 * address arithmetic falls outside the identity grammar.
 */
static void test_reference_scatter_module(void)
{
   struct r300_compute_job job;
   const char *reason = NULL;
   bool admitted = r300_compute_job_from_spirv(
      r3v_reference_scatter_reject_spirv,
      sizeof(r3v_reference_scatter_reject_spirv) / 4, "main", &job, &reason);
   assert(!admitted);
   assert(reason != NULL);
   assert(strstr(reason, "arithmetic") != NULL);
}

/* A vertex-stage module refuses: its entry point is outside the
 * GLCompute model.
 */
static void test_wrong_stage_refuses(void)
{
   struct r300_compute_job job;
   const char *reason = NULL;
   bool admitted = r300_compute_job_from_spirv(
      r3v_reference_vertex_spirv,
      sizeof(r3v_reference_vertex_spirv) / 4, "main", &job, &reason);
   assert(!admitted);
   assert(reason != NULL);
}

/* Word-stream hardening: the reader refuses malformed streams by
 * bounds rather than reading past them.
 */
static void test_malformed_streams_refuse(void)
{
   struct r300_compute_job job;
   const char *reason = NULL;
   const size_t words =
      sizeof(r3v_reference_identity_map_spirv) / 4;

   /* Empty and header-only streams. */
   assert(!r300_compute_job_from_spirv(NULL, 0, "main", &job, &reason));
   assert(!r300_compute_job_from_spirv(r3v_reference_identity_map_spirv,
                                       4, "main", &job, &reason));

   /* A wrong magic number. */
   uint32_t mutated[2048];
   assert(words <= 2048);
   memcpy(mutated, r3v_reference_identity_map_spirv, words * 4);
   mutated[0] ^= 1;
   assert(!r300_compute_job_from_spirv(mutated, words, "main", &job, &reason));

   /* The OpEntryPoint literal binds to the requested name byte for
    * byte; a NULL request refuses before the module is read.
    */
   assert(!r300_compute_job_from_spirv(r3v_reference_identity_map_spirv,
                                       words, "other", &job, &reason));
   assert(strcmp(reason, "entry point name outside the request") == 0);
   assert(!r300_compute_job_from_spirv(r3v_reference_identity_map_spirv,
                                       words, NULL, &job, &reason));

   /* A one-word final instruction refuses on its length before any
    * operand is read; after the kernel's store only the return and the
    * function end are admitted, so it reports as work after the store.
    */
   memcpy(mutated, r3v_reference_identity_map_spirv, words * 4);
   mutated[words] = (1u << 16) | 19u;
   assert(!r300_compute_job_from_spirv(mutated, words + 1, "main", &job,
                                       &reason));
   assert(strcmp(reason, "instruction after the final output store") == 0);

   /* Truncation at every boundary keeps the reader inside the stream:
    * either an instruction overruns or the function never completes.
    */
   for (size_t cut = 5; cut < words; cut++) {
      assert(!r300_compute_job_from_spirv(r3v_reference_identity_map_spirv,
                                          cut, "main", &job, &reason));
   }

   /* Every single-word mutation either still refuses or still admits
    * exactly the identity job shape: no mutation reaches a different
    * job, so the admitted meaning is not steerable by a stray word.
    */
   for (size_t i = 5; i < words; i++) {
      memcpy(mutated, r3v_reference_identity_map_spirv, words * 4);
      mutated[i] ^= 0x10001u;
      struct r300_compute_job mutated_job;
      if (r300_compute_job_from_spirv(mutated, words, "main", &mutated_job,
                                      &reason)) {
         assert(mutated_job.op == R300_COMPUTE_JOB_OP_IDENTITY);
         assert(mutated_job.input_binding !=
                mutated_job.output_binding);
      }
   }

   /* The complement module carries the same property: a stray word
    * either refuses or leaves the complement job's shape intact.
    */
   const size_t not_words = sizeof(r3v_reference_bitwise_not_spirv) / 4;
   assert(not_words <= 2048);
   for (size_t i = 5; i < not_words; i++) {
      memcpy(mutated, r3v_reference_bitwise_not_spirv, not_words * 4);
      mutated[i] ^= 0x10001u;
      struct r300_compute_job mutated_job;
      if (r300_compute_job_from_spirv(mutated, not_words, "main",
                                      &mutated_job, &reason)) {
         assert(mutated_job.op == R300_COMPUTE_JOB_OP_BITWISE_NOT);
         assert(mutated_job.input_binding !=
                mutated_job.output_binding);
      }
   }
}

int main(void)
{
   test_reference_identity_module();
   test_reference_bitwise_not_module();
   test_unread_complement_refuses();
   test_reference_scatter_module();
   test_wrong_stage_refuses();
   test_malformed_streams_refuse();
   printf("r3v-native-compute-frontend: all cases passed\n");
   return 0;
}
