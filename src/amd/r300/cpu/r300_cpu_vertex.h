/*
 * SPDX-License-Identifier: MIT
 *
 * K8-safe CPU vertex executor: gathers API vertex records into the
 * native TCL-bypass carrier.
 */

#ifndef R300_CPU_VERTEX_H
#define R300_CPU_VERTEX_H

#include <stdint.h>

/* The executor realizes the PSC lane semantics on the host: each output
 * lane takes the physical component or synthesized constant that
 * r300_vertex_format_semantics.select names, so the CPU-built carrier
 * and a VAP fetch of the same records describe one logical vec4 stream.
 * Every lane move is a 32-bit bit copy and the synthesized constants
 * are the exact encodings 0x00000000 and 0x3f800000, so the output is
 * bit-identical across implementations and hosts; NaN payloads,
 * denormals, and negative zero survive the gather unchanged.
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
 * carrier as packed logical vec4 dwords (VAP_VTX_SIZE = 4).  The
 * carrier is the caller's mapped GTT storage, capacity in dwords;
 * a gather that would overrun refuses with -ENOSPC before writing,
 * and an unknown format refuses with -EINVAL.  Returns 0 on success.
 */
int r300_cpu_vertex_gather(int format_id,
                           const struct r300_cpu_vertex_stream *stream,
                           uint32_t first_vertex, uint32_t vertex_count,
                           uint32_t *carrier, uint32_t carrier_dwords);

/* The scalar reference the specializations qualify against; the same
 * contract as r300_cpu_vertex_gather.
 */
int r300_cpu_vertex_gather_scalar(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

/* Names the implementation r300_cpu_vertex_gather selected on this
 * build: "sse2" or "scalar".
 */
const char *r300_cpu_vertex_implementation(void);

#endif /* R300_CPU_VERTEX_H */
