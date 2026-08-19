/*
 * SPDX-License-Identifier: MIT
 *
 * Scalar CPU vertex-job interpreter: executes a compiler-lowered
 * r300_vertex_job over bound vertex records and serializes the
 * positions into the native TCL-bypass carrier.
 */

#ifndef R300_CPU_VERTEX_JOB_H
#define R300_CPU_VERTEX_JOB_H

#include "r300_cpu_vertex.h"

#include "amd/r300/compiler/r300_vertex_job.h"

/* Structural validation, independent of any vertex data: opcode set,
 * register and constant bounds, attribute-0-only inputs, every source
 * register written before its first read, and exactly one
 * STORE_POSITION as the final instruction.  Returns 0 or -EINVAL.
 */
int r300_cpu_vertex_job_validate(const struct r300_vertex_job *job);

/* Runs the validated job once per vertex in [first_vertex,
 * first_vertex + vertex_count) and writes each stored position as a
 * packed little-endian vec4 at carrier[4 * relative_vertex], the
 * carrier layout the VAP fetch of the same stream produces.  All
 * refusals precede the first carrier write: -EINVAL for a job that
 * fails validation, a zero vertex count, or a record range the stream
 * bound cannot prove readable (64-bit last-byte arithmetic, matching
 * the gather contract); -ENOSPC when 4 * vertex_count exceeds
 * carrier_dwords; -EINVAL when the carrier range overlaps the stream
 * bytes, so an in-place rewrite cannot corrupt its own input.
 * Carrier dwords past 4 * vertex_count stay untouched.
 */
int r300_cpu_vertex_job_execute(const struct r300_vertex_job *job,
                                const struct r300_cpu_vertex_stream *stream,
                                uint32_t first_vertex, uint32_t vertex_count,
                                uint32_t *carrier, uint32_t carrier_dwords);

#endif /* R300_CPU_VERTEX_JOB_H */
