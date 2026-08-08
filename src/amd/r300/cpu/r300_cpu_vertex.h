/*
 * SPDX-License-Identifier: MIT
 *
 * Portable CPU vertex executor: gathers API vertex records into the
 * native TCL-bypass carrier.
 */

#ifndef R300_CPU_VERTEX_H
#define R300_CPU_VERTEX_H

#include <stdint.h>

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
 * Tuning is a separate property from the contract.  General code speed
 * rides each build profile's compiler flags; an explicit SIMD path is a
 * second implementation to qualify and exists only where a measurement
 * on the target host justifies it.  The one tuned path here is SSE2
 * (the K8 primary target implements SSE2/SSE3), correctness-qualified
 * by the oracle and awaiting its K8 timing measurement.
 */

/* One bound attribute stream: data points at the first record of the
 * first vertex (binding base plus attribute offset already applied),
 * and stride is the byte distance between records.
 */
struct r300_cpu_vertex_stream {
   const uint8_t *data;
   uint32_t stride;
};

/* Gathers vertex_count records starting at first_vertex into the
 * carrier as packed logical vec4 dwords (VAP_VTX_SIZE = 4) in the
 * little-endian carrier encoding.  The carrier is the caller's mapped
 * GTT storage, capacity in dwords; a gather that would overrun refuses
 * with -ENOSPC before writing, and an unknown format refuses with
 * -EINVAL.  Returns 0 on success.
 */
int r300_cpu_vertex_gather(int format_id,
                           const struct r300_cpu_vertex_stream *stream,
                           uint32_t first_vertex, uint32_t vertex_count,
                           uint32_t *carrier, uint32_t carrier_dwords);

/* The portable byte-copy baseline the tuned paths qualify against; the
 * same contract as r300_cpu_vertex_gather.
 */
int r300_cpu_vertex_gather_baseline(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

/* Names the implementation r300_cpu_vertex_gather selected on this
 * build: "sse2" or "baseline".
 */
const char *r300_cpu_vertex_implementation(void);

#endif /* R300_CPU_VERTEX_H */
