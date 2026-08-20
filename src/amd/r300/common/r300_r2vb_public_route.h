/*
 * SPDX-License-Identifier: MIT
 *
 * Public GPU-producer route stream: the R2VB producer pass composed
 * ahead of the TCL-bypass consumer in one indirect buffer.
 */

#ifndef R300_R2VB_PUBLIC_ROUTE_H
#define R300_R2VB_PUBLIC_ROUTE_H

#include <stdbool.h>
#include <stdint.h>

#include "r300_tcl_bypass_triangle.h"

/* The route delivers the application's vertex records through silicon:
 * the producer pass rasterizes one point per record into the carrier,
 * and the consumer cell fetches that same buffer object as its vertex
 * stream and rasterizes the triangle into the color target.  One
 * submission carries both, so the carrier is written and read inside a
 * single kernel command-stream validation and the publication tail
 * between them is what orders the write before the fetch.
 *
 * The producer references the carrier alone, the consumer references
 * the carrier as its vertex slot and the color target as its color
 * slot, so the composed stream carries three relocation sites over two
 * slots: the carrier at slot 0 rides read-write GTT, the color target
 * at slot 1 write-only GTT.
 */
enum r300_r2vb_public_route_slot {
   R300_R2VB_PUBLIC_ROUTE_SLOT_CARRIER = R300_TRIANGLE_SLOT_VERTEX,
   R300_R2VB_PUBLIC_ROUTE_SLOT_COLOR = R300_TRIANGLE_SLOT_COLOR,
   R300_R2VB_PUBLIC_ROUTE_SLOT_COUNT = R300_TRIANGLE_SLOT_COUNT,
};

#define R300_R2VB_PUBLIC_ROUTE_MAX_RELOC_SITES 3u

struct r300_r2vb_public_route_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_r2vb_public_route_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   /* Index of the consumer's first dword: the producer occupies
    * [0, consumer_start_dwords) and the consumer the remainder, so a
    * reader splits the composed stream without re-emitting either half.
    */
   uint32_t consumer_start_dwords;
   struct r300_r2vb_public_route_reloc_site
      reloc_sites[R300_R2VB_PUBLIC_ROUTE_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   bool owns_ib;
};

/* Composes the route's stream over caller records at a consumer extent
 * inside the published maximum.  The producer half is the
 * reference-shaped emission over the records
 * (r300_r2vb_producer_records_emit) and the consumer half is
 * r300_tcl_bypass_triangle_extent_emit, so the composed bytes are the
 * bytes the driver's submit-time admission installs when the
 * application's records reach the same emitter.  Returns 0 or a
 * negative errno; -EDOM names a record outside the FP24 fixed-point
 * domain and -EINVAL an extent outside 1..64 on either axis.  The
 * caller owns the returned allocation.
 */
int r300_r2vb_public_route_compose(
   const float (*records)[4], uint32_t width, uint32_t height,
   struct r300_r2vb_public_route_ib *out);

/* Composes the route over the fixed triangle's own vertex records at
 * the consumer's maximum extent.  The producer half is then
 * byte-identical to r300_r2vb_producer_reference_emit and the consumer
 * half to the qualified cell, so this stream is the one an attended run
 * authorizes and the one the offline replay parses.
 */
int r300_r2vb_public_route_reference_compose(
   struct r300_r2vb_public_route_ib *out);

void r300_r2vb_public_route_release(struct r300_r2vb_public_route_ib *ib);

/* Checks the composed relocation sites against the stream they index:
 * three sites in stream order, each inside the stream, each preceded by
 * the relocation NOP header, and each carrying its slot's chunk
 * payload.  Returns 0 or a negative errno.
 */
int r300_r2vb_public_route_validate_reloc_sites(
   const struct r300_r2vb_public_route_ib *ib);

#endif /* R300_R2VB_PUBLIC_ROUTE_H */
