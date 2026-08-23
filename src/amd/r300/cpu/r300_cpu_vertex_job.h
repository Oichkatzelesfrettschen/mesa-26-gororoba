/*
 * SPDX-License-Identifier: MIT
 *
 * CPU vertex-job execution: runs a compiler-lowered r300_vertex_job
 * over bound vertex records and serializes the positions into the
 * native TCL-bypass carrier.  The scalar interpreter is the authority;
 * the SIMD candidates are correctness-qualified specializations whose
 * dispatch requires end-to-end native-path timing.
 */

#ifndef R300_CPU_VERTEX_JOB_H
#define R300_CPU_VERTEX_JOB_H

#include "r300_cpu_vertex.h"

#include "amd/r300/common/r300_vertex_job.h"

/* Structural validation, independent of any vertex data: opcode set,
 * register and constant bounds, attribute-0-only inputs, every source
 * register written before its first read, at most one STORE_VARYING
 * ahead of the final instruction, and exactly one STORE_POSITION as
 * the final instruction.  Returns 0 or -EINVAL.
 */
int r300_cpu_vertex_job_validate(const struct r300_vertex_job *job);

/* Runs the validated job once per vertex in [first_vertex,
 * first_vertex + vertex_count) and writes each vertex's record --
 * the stored position as a packed vec4, then the stored varying when
 * the job carries one -- at carrier[record_dwords * relative_vertex]
 * with record_dwords = r300_vertex_job_record_dwords(job), the carrier
 * layout the VAP fetch of the same stream produces.  All refusals
 * precede the first carrier write: -EINVAL for a job that fails
 * validation, a zero vertex count, or a record range the stream bound
 * cannot prove readable (64-bit last-byte arithmetic, matching the
 * gather contract); -ENOSPC when record_dwords * vertex_count exceeds
 * carrier_dwords; -EINVAL when the carrier range overlaps the stream
 * bytes, so an in-place rewrite cannot corrupt its own input.
 * -ENOTSUP reports that the C floating-point environment cannot provide
 * the round-to-nearest, denormal-preserving execution policy.  Carrier
 * dwords past record_dwords * vertex_count stay untouched.
 */
int r300_cpu_vertex_job_execute(const struct r300_vertex_job *job,
                                const struct r300_vertex_stream *stream,
                                uint32_t first_vertex, uint32_t vertex_count,
                                uint32_t *carrier, uint32_t carrier_dwords);

/* The SIMD execution candidates carry the same contract, refusals, and
 * carrier contract as r300_cpu_vertex_job_execute; the differential test
 * enforces bit identity after arithmetic NaNs canonicalize to 0x7fc00000.
 * Byte-copy operations preserve every payload bit.  Packed
 * single-precision arithmetic keeps the scalar policy exactly: FMAD
 * commits its product to binary32 before the add, FFMA rounds the
 * combined operation once, and DP4 accumulates in component order from
 * the packed products so signed zeros survive.  Execution saves the
 * caller's floating-point environment, admits FE_DFL_ENV only after
 * calibrating round-to-nearest and denormal preservation, and restores
 * the caller's state.  Each returns -ENOTSUP when that environment is
 * unavailable and -ENOSYS on a build without its instruction set, so a
 * bench lane never times a different implementation than its label names.
 *
 * SSE2 supplies every operator the job IR needs: movdqu for the
 * unaligned loads and stores that move NaN payloads, denormals, and
 * signed zeros verbatim, addps and mulps for the elementwise operators,
 * mulps then addps for FMAD, scalar fmaf per lane for FFMA, and a
 * component-order scalar sum over the packed products
 * for DP4.
 *
 * SSE3 adds one form this kernel can take and three it cannot.  Per AMD
 * APM Vol. 4, HADDPS adds adjacent lane pairs, which associates the DP4
 * sum as (a+b)+(c+d) where the scalar policy pins ((a+b)+c)+d; binary32
 * addition is not associative, so the horizontal form changes the
 * result and the bit-identity contract excludes it.  HSUBPS and ADDSUBPS
 * compute no sum this IR expresses.  LDDQU is a load, so it moves the
 * same bytes movdqu moves and the SSE3 candidate takes it for the input
 * and constant loads; whether it costs less on this part is the timing
 * bench's question, not this contract's.
 *
 * The K8 target's CPUID ceiling is SSE3 (CPUID.00000001:ECX[0]; the
 * part reports no SSE4A, POPCNT, or AVX), so SSE3 is the last vector
 * extension a candidate here can name.
 */
int r300_cpu_vertex_job_execute_sse2(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords);

int r300_cpu_vertex_job_execute_sse3(
   const struct r300_vertex_job *job,
   const struct r300_vertex_stream *stream, uint32_t first_vertex,
   uint32_t vertex_count, uint32_t *carrier, uint32_t carrier_dwords);

/* Names the implementation the CPU vertex route executes: "scalar" until
 * end-to-end native-path timing selects a correctness-qualified candidate.
 */
const char *r300_cpu_vertex_job_implementation(void);

#endif /* R300_CPU_VERTEX_JOB_H */
