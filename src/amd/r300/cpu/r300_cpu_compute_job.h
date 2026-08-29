/*
 * SPDX-License-Identifier: MIT
 *
 * CPU compute-job executor: decodes and runs the compiler's
 * r300_compute_job IR over two host memory ranges, once per flattened
 * invocation.
 */

#ifndef R300_CPU_COMPUTE_JOB_H
#define R300_CPU_COMPUTE_JOB_H

#include "amd/r300/common/r300_compute_job.h"

#include <stdint.h>

/* Structural validation ahead of execution: a known op, nonzero X
 * workgroup dimensions, singleton Y/Z dimensions for the admitted
 * gl_GlobalInvocationID.x address domain, and a workgroup volume that
 * stays inside R300_COMPUTE_JOB_MAX_INVOCATIONS.  Returns 0 or a negative
 * errno.
 */
int r300_cpu_compute_job_validate(const struct r300_compute_job *job);

/* The dispatch's flattened invocation count: the X group count times the
 * job's X workgroup size, overflow-checked against the invocation ceiling.
 * Y/Z group counts stay one for the admitted x-only address domain.  A zero
 * group count returns 0 with *out set to zero and performs no work.
 */
int r300_cpu_compute_job_invocations(const struct r300_compute_job *job,
                                     const uint32_t group_counts[3],
                                     uint32_t *out);

/* Executes the job over the two ranges: element i of the output range
 * receives the op applied to element i of the input range, as a bit
 * copy for the identity op.  Both ranges must cover the invocation
 * count at R300_COMPUTE_JOB_ELEMENT_BYTES stride.  Exact aliases are
 * admitted for the elementwise maps; partial overlaps are refused before
 * the first output write.  A zero invocation count returns success without
 * reading either range.  Returns 0 or a negative errno.
 */
int r300_cpu_compute_job_execute(const struct r300_compute_job *job,
                                 const uint32_t group_counts[3],
                                 const void *input, uint64_t input_bytes,
                                 void *output, uint64_t output_bytes);

#endif /* R300_CPU_COMPUTE_JOB_H */
