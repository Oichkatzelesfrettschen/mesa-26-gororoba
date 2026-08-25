/*
 * SPDX-License-Identifier: MIT
 *
 * CPU compute-job executor.
 */

#include "r300_cpu_compute_job.h"

#include <errno.h>
#include <string.h>

int r300_cpu_compute_job_validate(const struct r300_compute_job *job)
{
   if (job == NULL)
      return -EINVAL;
   if (job->op != R300_COMPUTE_JOB_OP_IDENTITY &&
       job->op != R300_COMPUTE_JOB_OP_BITWISE_NOT)
      return -EINVAL;
   if (job->local_size[0] == 0 || job->local_size[1] == 0 ||
       job->local_size[2] == 0)
      return -EINVAL;
   const uint64_t volume = (uint64_t)job->local_size[0] *
                           job->local_size[1] * job->local_size[2];
   if (volume > R300_COMPUTE_JOB_MAX_INVOCATIONS)
      return -ERANGE;
   return 0;
}

int r300_cpu_compute_job_invocations(const struct r300_compute_job *job,
                                     const uint32_t group_counts[3],
                                     uint32_t *out)
{
   int rc = r300_cpu_compute_job_validate(job);
   if (rc != 0)
      return rc;
   if (group_counts == NULL || out == NULL)
      return -EINVAL;
   if (group_counts[0] == 0 || group_counts[1] == 0 ||
       group_counts[2] == 0)
      return -EINVAL;
   /* Each factor is bounded before it multiplies, so the running
    * product stays inside 64 bits and one final comparison decides.
    */
   uint64_t total = 1;
   for (unsigned axis = 0; axis < 3; axis++) {
      if (group_counts[axis] > R300_COMPUTE_JOB_MAX_INVOCATIONS)
         return -ERANGE;
      total *= group_counts[axis];
      if (total > R300_COMPUTE_JOB_MAX_INVOCATIONS)
         return -ERANGE;
      total *= job->local_size[axis];
      if (total > R300_COMPUTE_JOB_MAX_INVOCATIONS)
         return -ERANGE;
   }
   *out = (uint32_t)total;
   return 0;
}

int r300_cpu_compute_job_execute(const struct r300_compute_job *job,
                                 const uint32_t group_counts[3],
                                 const void *input, uint64_t input_bytes,
                                 void *output, uint64_t output_bytes)
{
   uint32_t invocations = 0;
   int rc = r300_cpu_compute_job_invocations(job, group_counts,
                                             &invocations);
   if (rc != 0)
      return rc;
   if (input == NULL || output == NULL)
      return -EINVAL;
   const uint64_t bytes =
      (uint64_t)invocations * R300_COMPUTE_JOB_ELEMENT_BYTES;
   if (input_bytes < bytes || output_bytes < bytes)
      return -ERANGE;
   /* Both maps read every element before the write that would cover
    * it, so overlapping ranges would make the result depend on element
    * order; the refusal keeps every admitted execution
    * order-independent.
    */
   const uintptr_t in_addr = (uintptr_t)input;
   const uintptr_t out_addr = (uintptr_t)output;
   if (in_addr < out_addr + bytes && out_addr < in_addr + bytes)
      return -EINVAL;
   if (job->op == R300_COMPUTE_JOB_OP_IDENTITY) {
      memcpy(output, input, bytes);
      return 0;
   }
   /* The complement moves through memcpy at element granularity, so a
    * mapped range at any alignment carries the same bit pattern the
    * shader's 32-bit word type names.
    */
   const unsigned char *in_bytes = input;
   unsigned char *out_bytes = output;
   for (uint32_t i = 0; i < invocations; i++) {
      uint32_t word;
      memcpy(&word, in_bytes + (size_t)i * R300_COMPUTE_JOB_ELEMENT_BYTES,
             sizeof(word));
      word = ~word;
      memcpy(out_bytes + (size_t)i * R300_COMPUTE_JOB_ELEMENT_BYTES, &word,
             sizeof(word));
   }
   return 0;
}
