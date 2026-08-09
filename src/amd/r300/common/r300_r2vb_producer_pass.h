/*
 * SPDX-License-Identifier: MIT
 *
 * R2VB producer-pass PM4 emitter: the raster pass that writes an F32_4
 * vertex carrier through the color backend.
 */

#ifndef R300_R2VB_PRODUCER_PASS_H
#define R300_R2VB_PRODUCER_PASS_H

#include <stdbool.h>
#include <stdint.h>

struct r300_first_draw_contract;

/* The producer pass renders one point per vertex into an ARGB32323232
 * (C4_32_FP) color target, so each rasterized pixel stores one FP32x4
 * record and the target re-binds verbatim as a vertex array.  The slot
 * grid is a single row: slot v lands at pixel (v, 0), the pitch is the
 * count rounded up to the register's two-pixel granularity, and the
 * padding slot of an odd count stays unwritten.  The embedded vertex
 * records travel pre-swizzled as (z, y, x, w); the target prologue's
 * BGRA US output select reverses that order, so the carrier stores the
 * source (x, y, z, w) bytes.
 *
 * The US datapath narrows every routed value to s1e7m16, so the pass
 * carries a record byte-exact only when each component is a fixed point
 * of the FP24 round trip; the emission refuses a record outside
 * r300_r2vb_f32_4_identity_admits with -EDOM before writing any dword,
 * keeping the emitted stream inside the domain where delivery is the
 * identity.
 */

/* The one BO the pass references: the carrier, bound as the color target
 * here and as the vertex source of the consuming draw.  The kernel reads
 * it through the color backend (write) and the vertex fetch (read), so
 * the submission binds it read-write in the GTT domain.
 */
enum r300_r2vb_producer_slot {
   R300_R2VB_PRODUCER_SLOT_CARRIER = 0,
   R300_R2VB_PRODUCER_SLOT_COUNT = 1,
};

/* Single-row slot grid.  width and pitch_pixels stay equal: the row is
 * the allocation, and RB3D_COLORPITCH0's pitch field carries two-pixel
 * granularity, so both round the count up to even.
 */
struct r300_r2vb_producer_layout {
   uint32_t count;
   uint32_t width;
   uint32_t height;
   uint32_t pitch_pixels;
};

/* The draw packet's 14-bit payload field holds 0x4000 dwords: the
 * VAP_VF_CNTL dword plus eight dwords per vertex, so 2047 vertices is
 * the largest encodable embedded row.  The pitch register's 13-bit pixel
 * field admits more, so the packet header is the binding ceiling.
 */
#define R300_R2VB_PRODUCER_MAX_COUNT 2047u

/* Resolves the single-row layout for count vertices.  Returns -EINVAL
 * outside 1..R300_R2VB_PRODUCER_MAX_COUNT.
 */
int r300_r2vb_producer_layout_single_row(
   uint32_t count, struct r300_r2vb_producer_layout *out);

struct r300_r2vb_producer_params {
   /* Byte offset of slot (0, 0) inside the carrier BO. */
   uint32_t carrier_offset;
   struct r300_r2vb_producer_layout layout;
   /* layout.count source records, each (x, y, z, w) binary32. */
   const float (*records)[4];
   /* When set, the emission opens with the neutral first-draw state
    * contract, so the pass establishes every register it depends on and
    * parses standalone: the kernel tracker reads texture, blend, and
    * z-state from this stream rather than from the previous client.
    */
   const struct r300_first_draw_contract *first_draw_contract;
};

struct r300_r2vb_producer_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_r2vb_producer_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_r2vb_producer_reloc_site
      reloc_sites[R300_R2VB_PRODUCER_SLOT_COUNT];
   uint32_t reloc_site_count;
   /* Set when the emission allocated ib, so the release frees what it
    * owns and leaves caller storage alone.
    */
   bool owns_ib;
};

/* Emits the complete producer pass: the first-draw contract prefix when
 * the params carry one, then the target prologue (destination-cache
 * barrier, carrier retarget to C4_32_FP with the one relocation, blend
 * and alpha-test disabled with a full channel mask, one-pixel point
 * raster state, clip disabled, VTE passthrough), the embedded POINTS
 * draw (VAP_VTX_SIZE = 8: four slot-position dwords plus one
 * pre-swizzled record per vertex), and the publication tail (color-cache
 * flush, 3D idle-clean wait, VAP_PVS_STATE_FLUSH_REG = 0, the engine
 * sync that keeps a later vertex fetch of the same BO from reading stale
 * vertex-cache content).  The producer fragment program and its
 * rasterizer-to-US routing stay outside this emission.
 *
 * TODO: missing work --
 *          the producer US program block and the RS_COUNT / RS_IP /
 *          RS_INST varying routing that feed it, the registers a live
 *          producer draw shades through.
 *      reason --
 *          the native lane owns no compiled producer fragment binary;
 *          the passthrough US program exists only inside the Gallium
 *          producer's shader cache.
 *      tracking-artifact --
 *          r300_r2vb_producer_pass_emit and the Gallium reference
 *          r300_r2vb_get_transform_fs.
 *
 * Returns 0 or a negative errno; -EDOM names a record outside the FP24
 * fixed-point domain.  The caller owns the returned IB allocation.
 */
int r300_r2vb_producer_pass_emit(
   const struct r300_r2vb_producer_params *params,
   struct r300_r2vb_producer_ib *out);

/* Emits into caller storage of exactly capacity dwords.  A destination
 * too small refuses with -ENOSPC and reports zero delivered dwords, so
 * no caller reads the partially placed words as a stream; the -EDOM
 * domain refusal alone leaves the destination untouched.
 */
int r300_r2vb_producer_pass_emit_into(
   const struct r300_r2vb_producer_params *params, uint32_t *words,
   uint32_t capacity, struct r300_r2vb_producer_ib *out);

void r300_r2vb_producer_pass_release(struct r300_r2vb_producer_ib *ib);

/* Checks the emitted relocation site against the stream it indexes: one
 * carrier site, inside the stream, preceded by the relocation NOP header,
 * carrying the carrier slot's chunk payload.  Returns 0 or a negative
 * errno.
 */
int r300_r2vb_producer_pass_validate_reloc_sites(
   const struct r300_r2vb_producer_ib *ib);

/* Emits the reference producer pass: the fixed triangle's three vertex
 * records (r300_tcl_bypass_triangle_vertices) through the single-row
 * layout with the first-draw contract prefix, carrier offset zero.  Every
 * pre-hardware consumer takes the pass from here so their IBs stay
 * byte-identical.  Returns 0 or a negative errno; the caller owns the
 * returned IB allocation.
 */
int r300_r2vb_producer_reference_emit(struct r300_r2vb_producer_ib *out);

/* The reference pass carries the fixed triangle's vertex count; the
 * manifest derives its published layout from the same value, so the two
 * cannot desynchronize.
 */
#define R300_R2VB_PRODUCER_REFERENCE_COUNT 3u

#endif /* R300_R2VB_PRODUCER_PASS_H */
