/*
 * SPDX-License-Identifier: MIT
 *
 * Portable CPU vertex executor: gathers API vertex records into the
 * native TCL-bypass carrier.
 */

#ifndef R300_CPU_VERTEX_H
#define R300_CPU_VERTEX_H

#include <stdbool.h>
#include <stdint.h>

#include "amd/r300/common/r300_vertex_stream.h"

/* The executor realizes the PSC lane semantics on the host: each output
 * lane takes the physical component or synthesized constant that
 * r300_vertex_format_semantics.select names, so the CPU-built carrier
 * and a VAP fetch of the same records describe one logical vec4 stream.
 *
 * Both sides of the gather are byte-defined, not value-defined.  Input
 * records carry IEEE-754 binary32 components in little-endian byte
 * order -- the encoding the R300 VAP fetches from memory -- and the
 * carrier is little-endian dwords, the same canonical encoding the IB
 * artifact rule uses.  A physical lane is a verbatim 4-byte copy and a
 * synthesized lane writes the fixed byte sequences 00 00 00 00 and
 * 00 00 80 3f, so the baseline is endian-neutral: the same bytes land
 * on x86, x86-64, and PowerPC hosts with no swap logic, and NaN
 * payloads, denormals, and negative zero survive unchanged.
 *
 * Performance selection is separate from the contract.  General code speed
 * rides each build profile's compiler flags; the explicit SIMD path is an
 * unmeasured correctness-qualified specialization until a target timing
 * run establishes a dispatch policy.  The SIMD candidates are SSE2 and
 * SSE3 -- the K8 primary target's CPUID ceiling is k8-sse3, so SSE3 is
 * the last vector extension the target implements -- and both are
 * correctness-qualified by the oracle.  r300_cpu_vertex_gather selects
 * the SSE2 path on builds that carry it; the r300_cpu_vertex_bench
 * measurement on the target host is the authority for changing that
 * selection.
 */

/* Gathers vertex_count records starting at first_vertex into the
 * carrier as packed logical vec4 dwords (VAP_VTX_SIZE = 4) in the
 * little-endian carrier encoding.  The carrier is the caller's mapped
 * GTT storage, capacity in dwords; a gather that would overrun refuses
 * with -ENOSPC before writing, and an unknown format refuses with
 * -EINVAL.  Returns 0 on success.
 */
int r300_cpu_vertex_gather(int format_id,
                           const struct r300_vertex_stream *stream,
                           uint32_t first_vertex, uint32_t vertex_count,
                           uint32_t *carrier, uint32_t carrier_dwords);

/* True when every record of the range lies inside the stream bound; the
 * gather reads such a range verbatim, and under the stream's robust rule
 * substitutes zeros for the records outside it.
 */
bool r300_cpu_vertex_range_in_bounds(int format_id,
                                     const struct r300_vertex_stream *stream,
                                     uint32_t first_vertex,
                                     uint32_t vertex_count);

/* The portable byte-copy baseline the SIMD paths qualify against; the
 * same contract as r300_cpu_vertex_gather.
 */
int r300_cpu_vertex_gather_baseline(
   int format_id, const struct r300_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

/* The named SIMD candidates, for the oracle and the timing bench.
 * Each returns -ENOSYS on a build without its instruction set and
 * -EINVAL when the vocabulary row deviates from the identity selector
 * pattern the SIMD kernels encode, so a bench lane never silently
 * times a different implementation than its label names.
 */
int r300_cpu_vertex_gather_sse2(
   int format_id, const struct r300_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

int r300_cpu_vertex_gather_sse3(
   int format_id, const struct r300_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

/* Names the implementation r300_cpu_vertex_gather selected on this
 * build: "sse2" or "baseline".
 */
const char *r300_cpu_vertex_implementation(void);

#endif /* R300_CPU_VERTEX_H */
