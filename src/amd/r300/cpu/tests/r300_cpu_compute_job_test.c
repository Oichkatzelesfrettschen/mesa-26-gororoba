/*
 * SPDX-License-Identifier: MIT
 *
 * CPU compute-job executor calibration: exact bytes for the identity
 * op, a complete refusal matrix with no partial output write, and the
 * overflow-checked invocation arithmetic at its bounds.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r300_cpu_compute_job.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct r300_compute_job identity_job(void)
{
   return (struct r300_compute_job){
      .op = R300_COMPUTE_JOB_OP_IDENTITY,
      .input_binding = 0,
      .output_binding = 1,
      .local_size = { 64, 1, 1 },
   };
}

static void test_exact_bytes(void)
{
   struct r300_compute_job job = identity_job();
   assert(r300_cpu_compute_job_validate(&job) == 0);

   uint32_t input[130];
   uint32_t output[130];
   for (uint32_t i = 0; i < 130; i++) {
      input[i] = 0xa5000000u ^ (i * 0x9e3779b9u);
      output[i] = 0xdeadbeefu;
   }
   /* Hostile bit patterns move verbatim: NaN payload, negative zero,
    * denormal, infinity.
    */
   input[0] = 0x7fc01234u;
   input[1] = 0x80000000u;
   input[2] = 0x00000001u;
   input[3] = 0xff800000u;

   const uint32_t groups[3] = { 2, 1, 1 };
   uint32_t invocations = 0;
   assert(r300_cpu_compute_job_invocations(&job, groups, &invocations) == 0);
   assert(invocations == 128);
   assert(r300_cpu_compute_job_execute(&job, groups, input, sizeof(input),
                                       output, sizeof(output)) == 0);
   assert(memcmp(output, input, 128 * 4) == 0);
   assert(output[128] == 0xdeadbeefu && output[129] == 0xdeadbeefu);
}

static void test_refusals_leave_output_untouched(void)
{
   struct r300_compute_job job = identity_job();
   uint32_t input[64] = { 0 };
   uint32_t output[64];
   memset(output, 0xcd, sizeof(output));
   uint32_t poison[64];
   memcpy(poison, output, sizeof(poison));

   const uint32_t one_group[3] = { 1, 1, 1 };

   struct r300_compute_job bad_op = job;
   bad_op.op = 0x7f;
   assert(r300_cpu_compute_job_execute(&bad_op, one_group, input,
                                       sizeof(input), output,
                                       sizeof(output)) == -EINVAL);

   struct r300_compute_job zero_local = job;
   zero_local.local_size[1] = 0;
   assert(r300_cpu_compute_job_execute(&zero_local, one_group, input,
                                       sizeof(input), output,
                                       sizeof(output)) == -EINVAL);

   /* Vulkan permits a dispatch with an empty workgroup grid.  Every zero
    * axis returns a successful no-op, including when no range is mapped.
    */
   for (unsigned axis = 0; axis < 3; axis++) {
      uint32_t zero_group[3] = { 1, 1, 1 };
      zero_group[axis] = 0;
      uint32_t invocations = UINT32_MAX;
      assert(r300_cpu_compute_job_invocations(&job, zero_group,
                                              &invocations) == 0);
      assert(invocations == 0);
      assert(r300_cpu_compute_job_execute(&job, zero_group, NULL, 0, NULL,
                                          0) == 0);
   }

   /* One byte short on either range refuses before any write. */
   assert(r300_cpu_compute_job_execute(&job, one_group, input,
                                       64 * 4 - 1, output,
                                       sizeof(output)) == -ERANGE);
   assert(r300_cpu_compute_job_execute(&job, one_group, input,
                                       sizeof(input), output,
                                       64 * 4 - 1) == -ERANGE);

   assert(r300_cpu_compute_job_execute(&job, one_group, NULL,
                                       sizeof(input), output,
                                       sizeof(output)) == -EINVAL);
   assert(r300_cpu_compute_job_execute(&job, one_group, input,
                                       sizeof(input), NULL,
                                       sizeof(output)) == -EINVAL);

   /* Exact aliases are elementwise-safe and leave the input unchanged. */
   uint32_t alias[64];
   for (uint32_t i = 0; i < 64; i++)
      alias[i] = 0x12340000u + i;
   uint32_t alias_copy[64];
   memcpy(alias_copy, alias, sizeof(alias));
   assert(r300_cpu_compute_job_execute(&job, one_group, alias,
                                       sizeof(alias), alias,
                                       sizeof(alias)) == 0);
   assert(memcmp(alias, alias_copy, sizeof(alias)) == 0);

   /* Partial overlap still refuses: the copy would order-depend. */
   uint32_t overlap[65] = { 0 };
   assert(r300_cpu_compute_job_execute(&job, one_group, overlap,
                                       sizeof(overlap), overlap + 1,
                                       sizeof(overlap) - sizeof(*overlap)) ==
          -EINVAL);

   assert(memcmp(output, poison, sizeof(output)) == 0);
}

static void test_x_only_dispatch_domain(void)
{
   struct r300_compute_job job = identity_job();
   const uint32_t one_group[3] = { 1, 1, 1 };
   uint32_t invocations = 0;

   struct r300_compute_job y_local = job;
   y_local.local_size[1] = 2;
   assert(r300_cpu_compute_job_validate(&y_local) == -EINVAL);
   assert(r300_cpu_compute_job_invocations(&y_local, one_group,
                                           &invocations) == -EINVAL);

   struct r300_compute_job z_local = job;
   z_local.local_size[2] = 2;
   assert(r300_cpu_compute_job_validate(&z_local) == -EINVAL);

   const uint32_t y_groups[3] = { 1, 2, 1 };
   assert(r300_cpu_compute_job_invocations(&job, y_groups,
                                           &invocations) == -EINVAL);
   const uint32_t z_groups[3] = { 1, 1, 2 };
   assert(r300_cpu_compute_job_invocations(&job, z_groups,
                                           &invocations) == -EINVAL);
}

static void test_invocation_bounds(void)
{
   struct r300_compute_job job = identity_job();
   uint32_t invocations = 0;

   /* The ceiling itself admits: 2^26 invocations as 2^20 groups of 64. */
   const uint32_t at_bound[3] = { 1u << 20, 1, 1 };
   assert(r300_cpu_compute_job_invocations(&job, at_bound,
                                           &invocations) == 0);
   assert(invocations == R300_COMPUTE_JOB_MAX_INVOCATIONS);

   /* One group past it refuses. */
   const uint32_t past_bound[3] = { (1u << 20) + 1, 1, 1 };
   assert(r300_cpu_compute_job_invocations(&job, past_bound,
                                           &invocations) == -ERANGE);

   /* The X product is checked before its 32-bit result narrows. */
   const uint32_t x_overflow[3] = { 1u << 25, 1, 1 };
   assert(r300_cpu_compute_job_invocations(&job, x_overflow,
                                           &invocations) == -ERANGE);

   /* A workgroup volume past the ceiling refuses at validation. */
   struct r300_compute_job huge_local = job;
   huge_local.local_size[0] = R300_COMPUTE_JOB_MAX_INVOCATIONS + 1;
   assert(r300_cpu_compute_job_validate(&huge_local) == -ERANGE);
}

int main(void)
{
   test_exact_bytes();
   test_refusals_leave_output_untouched();
   test_x_only_dispatch_domain();
   test_invocation_bounds();
   printf("r300-cpu-compute-job: all cases passed\n");
   return 0;
}
