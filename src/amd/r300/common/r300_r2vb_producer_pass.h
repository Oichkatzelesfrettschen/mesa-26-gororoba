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
struct r300_fragment_binary;

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
 * r300_r2vb_fp24_identity_admits with -EDOM before writing any dword,
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

/* One C4_32_FP texel holds four binary32 components, so a slot occupies
 * sixteen carrier bytes.
 */
#define R300_R2VB_PRODUCER_CPP_BYTES 16u

/* The value a carrier is filled with before the pass runs.  A read-back
 * dword still holding it names a slot the pass left unwritten, so the
 * pattern stays disjoint from every dword the delivery identity produces
 * and a carrier check has a decidable negative side.
 */
#define R300_R2VB_PRODUCER_POISON_DWORD 0xdeadbeefu

/* The complete immediate pass includes the fixed producer state and
 * publication tail around the embedded draw.  The shared one-input producer
 * admission keeps that whole IB at the established 1024-vertex ceiling; the
 * packet field alone can encode 2047 and is not the submit-safe bound.
 */
#define R300_R2VB_PRODUCER_MAX_COUNT 1024u

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
   /* The US program the rasterized slot pixels shade through: a
    * varying-passthrough binary that moves interpolator 0 to the color
    * output, so the carrier receives the embedded record.  A pass without
    * its own program executes whatever US code the previous client left
    * resident, so the emission refuses a missing or unvalidated binary
    * with -EINVAL.
    */
   const struct r300_fragment_binary *fragment_binary;
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
 * vertex-cache content).  The varying routing and the US program land
 * between the VAP tuple and the draw: RS_COUNT declares the four
 * interpolated components, RS_IP_0 routes the TEX0 varying's four
 * channels, RS_INST_0 delivers them to US input register zero, and the
 * params' fragment binary supplies the US code that moves that register
 * to the color output.
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

/* Initializes the reference producer fragment binary from the compiled
 * varying-passthrough block.  The caller finishes it with
 * r300_fragment_binary_finish.  Returns 0 or a negative errno.
 */
int r300_r2vb_producer_reference_fs(struct r300_fragment_binary *fs);

/* Emits the reference producer pass: the fixed triangle's three vertex
 * records (r300_tcl_bypass_triangle_vertices) through the single-row
 * layout with the first-draw contract prefix and the reference fragment
 * binary, carrier offset zero.  Every pre-hardware consumer takes the
 * pass from here so their IBs stay byte-identical.  Returns 0 or a
 * negative errno; the caller owns the returned IB allocation.
 */
int r300_r2vb_producer_reference_emit(struct r300_r2vb_producer_ib *out);

/* The reference pass carries the fixed triangle's vertex count; the
 * manifest derives its published layout from the same value, so the two
 * cannot desynchronize.
 */
#define R300_R2VB_PRODUCER_REFERENCE_COUNT 3u

/* Writes the carrier content the reference pass is expected to deliver:
 * the delivery identity over the same three FLOAT_4 records the
 * reference emission embeds, four dwords per slot.  The manifest
 * publishes these dwords from this one source, so a read-back check and
 * the published oracle cannot diverge.  Returns 0 or a negative errno;
 * -ENOSPC names storage shorter than the reference extent.
 */
int r300_r2vb_producer_reference_expected(uint32_t *expected,
                                          uint32_t expected_dwords);

/* FP24 boundary sweep records: twelve components covering the edges of
 * the delivery-admission domain -- +0, the minimum normal lattice
 * magnitude and its first step, the 1.0 neighborhood's smallest and
 * largest mantissa steps, the exponent-carry neighbor, a mid-range
 * multi-bit mantissa, and the maximum-exponent magnitudes up to the
 * largest finite lattice value.  Every component is a fixed point of
 * r300_fp24_quantize_bits with the sign bit clear, so the delivery
 * identity predicts the carrier byte-exact; a silicon mismatch on any
 * lane falsifies the admission window at that edge rather than the
 * transport.  The count equals the reference pass count, so the sweep
 * rides the producer cell's frozen carrier geometry and only the IB
 * digest separates the two streams.
 */
#define R300_R2VB_PRODUCER_FP24_SWEEP_COUNT 3u

extern const float r300_r2vb_producer_fp24_sweep_records
   [R300_R2VB_PRODUCER_FP24_SWEEP_COUNT][4];

/* Emits the sweep pass: the sweep records through the same single-row
 * layout, first-draw contract prefix, and reference fragment binary as
 * the reference emission, carrier offset zero.  Returns 0 or a negative
 * errno; the caller owns the returned IB allocation.
 */
int r300_r2vb_producer_fp24_sweep_emit(struct r300_r2vb_producer_ib *out);

/* Writes the carrier content the sweep pass is expected to deliver: the
 * delivery identity over the sweep records.  Returns 0 or a negative
 * errno; -ENOSPC names storage shorter than the sweep extent.
 */
int r300_r2vb_producer_fp24_sweep_expected(uint32_t *expected,
                                           uint32_t expected_dwords);

/* The read-back verdict over one carrier allocation.  expected_pass
 * covers the slots the pass writes; tail_poison_pass covers every dword
 * past that extent, where the padding slot of an odd count and any
 * allocation headroom must still hold the poison the prefill left.
 */
struct r300_r2vb_producer_carrier_verdict {
   bool expected_pass;
   bool tail_poison_pass;
   uint32_t mismatched_dwords;
   /* Mismatched dwords still holding the poison value: poison_dwords
    * equal to the expected extent names a carrier the pass never wrote,
    * and a smaller count names delivered-but-wrong values.
    */
   uint32_t poison_dwords;
   uint32_t disturbed_tail_dwords;
};

/* Compares a mapped carrier against the expected dwords and the poison
 * contract.  An expected dword equal to the poison value makes the
 * negative side undecidable -- a slot the pass never wrote would read as
 * delivered -- so the check refuses that pairing with -EINVAL before
 * reading the carrier.  -EINVAL also names a carrier shorter than the
 * expected extent or a byte size outside whole dwords.  Returns 0 with
 * the verdict filled, or a negative errno.
 */
int r300_r2vb_producer_carrier_check(
   const uint32_t *expected, uint32_t expected_dwords, uint32_t poison,
   const void *carrier, uint32_t carrier_bytes,
   struct r300_r2vb_producer_carrier_verdict *out);

#endif /* R300_R2VB_PRODUCER_PASS_H */
