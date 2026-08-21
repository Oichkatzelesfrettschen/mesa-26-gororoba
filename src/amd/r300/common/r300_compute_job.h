/*
 * SPDX-License-Identifier: MIT
 *
 * CPU compute-job IR: the compiler's immutable lowering output for the
 * admitted compute-dispatch subset.  The job describes one elementwise
 * kernel over two storage-buffer ranges; the executor runs it once per
 * flattened invocation.
 */

#ifndef R300_COMPUTE_JOB_H
#define R300_COMPUTE_JOB_H

#include <stdint.h>

/* Invocation ceiling for one dispatch: the product of the group counts
 * and the workgroup size stays below it, so every byte-offset
 * computation fits 32 bits with the 4-byte element factor applied.
 * The bound is validation policy, not silicon.
 */
#define R300_COMPUTE_JOB_MAX_INVOCATIONS (1u << 26)

/* Element width of the admitted subset: one 32-bit word per
 * invocation, moved as a bit pattern, so NaN payloads and every other
 * encoding survive the identity verbatim.
 */
#define R300_COMPUTE_JOB_ELEMENT_BYTES 4u

enum r300_compute_job_op {
   /* out[i] = in[i] for each flattened invocation i: the identity map
    * over 32-bit elements.
    */
   R300_COMPUTE_JOB_OP_IDENTITY = 0,
};

/* One admitted compute kernel: the op, the two storage-buffer bindings
 * it names on descriptor set 0, and the workgroup size its module
 * declared.  The flattened invocation index addresses both ranges at
 * R300_COMPUTE_JOB_ELEMENT_BYTES stride.
 */
struct r300_compute_job {
   uint8_t op;
   uint32_t input_binding;
   uint32_t output_binding;
   uint32_t local_size[3];
};

#endif /* R300_COMPUTE_JOB_H */
