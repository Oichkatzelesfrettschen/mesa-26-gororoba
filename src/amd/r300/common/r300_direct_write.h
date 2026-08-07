/*
 * SPDX-License-Identifier: MIT
 *
 * Fixed 2D solid-fill cell: the direct native write/readback control.
 */

#ifndef R300_DIRECT_WRITE_H
#define R300_DIRECT_WRITE_H

#include <stdbool.h>
#include <stdint.h>

/* The control writes the successor's target geometry through the 2D
 * engine alone: two 1x1 solid fills into a 64x64 linear ARGB8888
 * surface, no VAP, RS, US, RB3D, or ZB state and no source fetch, so a
 * landed byte proves the transport carries device writes outside the
 * 3D color-write gates.  The register contract and its kernel-source
 * derivation live in docs/hardware/r300-direct-write-2d-fill-authority.md;
 * the run procedure and failure classification live in
 * docs/hardware/r300-direct-write-control.md.
 */

enum r300_direct_write_slot {
   R300_DIRECT_WRITE_SLOT_COLOR = 0,
   R300_DIRECT_WRITE_SLOT_COUNT = 1,
};

/* The two probe pixels and their values.  Each value is asymmetric
 * across all four byte lanes and the two differ in every lane, so the
 * readback separates byte-order, address, and extent failures a single
 * value fuses.
 */
#define R300_DIRECT_WRITE_A_X 16u
#define R300_DIRECT_WRITE_A_Y 16u
#define R300_DIRECT_WRITE_A_VALUE 0x11223344u
#define R300_DIRECT_WRITE_B_X 47u
#define R300_DIRECT_WRITE_B_Y 47u
#define R300_DIRECT_WRITE_B_VALUE 0xa1b2c3d4u

/* One IB position whose payload names a relocation slot; the cell
 * carries exactly one, the DST_PITCH_OFFSET payload.
 */
struct r300_direct_write_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_direct_write_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_direct_write_reloc_site
      reloc_sites[R300_DIRECT_WRITE_SLOT_COUNT];
   uint32_t reloc_site_count;
   bool owns_ib;
};

/* Emits the complete fixed control cell: destination pitch/offset bound
 * by relocation, explicit 2D scissor, solid-brush raster state, the two
 * 1x1 fills, then 2D destination-cache flush and engine-idle wait.
 * Returns 0 or a negative errno; the caller owns the IB allocation.
 */
int r300_direct_write_emit(struct r300_direct_write_ib *out);

/* Emits into caller storage of exactly capacity dwords; refuses with
 * -ENOSPC before any half-written stream is reported.
 */
int r300_direct_write_emit_into(uint32_t *words, uint32_t capacity,
                                struct r300_direct_write_ib *out);

void r300_direct_write_release(struct r300_direct_write_ib *ib);

/* Checks the emitted relocation sites against the stream: one site,
 * inside the stream, naming the color slot at the recorded index.
 */
int r300_direct_write_validate_reloc_sites(
   const struct r300_direct_write_ib *ib);

/* Output oracle over a sentinel-initialized 64-pixel-pitch ARGB8888
 * target.  executed reports any deviation from the sentinel; value_a
 * and value_b demand the exact probe values at their pixels; sentinel
 * demands the sentinel at every other target pixel; canary demands the
 * sentinel in every allocation row past the 64-row render extent.  The
 * classification of each verdict combination is the direct-write
 * control document's observable table.
 */
struct r300_direct_write_verdict {
   bool executed;
   bool value_a_pass;
   bool value_b_pass;
   bool sentinel_pass;
   bool canary_pass;
};

void r300_direct_write_oracle(const uint32_t *pixels, uint32_t size_bytes,
                              struct r300_direct_write_verdict *verdict);

#endif /* R300_DIRECT_WRITE_H */
