/*
 * SPDX-License-Identifier: MIT
 *
 * R2VB FLOAT_4 + FLOAT_2 tuple producer-pass PM4 emitter: the fetched
 * raster pass that carries a two-dword model stream through the PSC XY01
 * expansion into an F32_4 vertex carrier.
 */

#ifndef R300_R2VB_FLOAT2_TUPLE_PASS_H
#define R300_R2VB_FLOAT2_TUPLE_PASS_H

#include <stdbool.h>
#include <stdint.h>

struct r300_first_draw_contract;
struct r300_fragment_binary;

/* The tuple pass renders one point per vertex into an ARGB32323232
 * (C4_32_FP) color target, the same single-row carrier grid as the
 * immediate producer, but the vertex data arrives through 3D_LOAD_VBPNTR
 * instead of the embedded draw body: a FLOAT_4 slot-position array and a
 * FLOAT_2 model array, six fetched dwords per vertex.  The PSC declares
 * the model element under the XY01 selector, so the hardware expands
 * each two-dword record to the logical input (x, y, 0, 1), the RS routes
 * that vector to the US, and the straight RGBA output select stores it
 * unchanged: the expected carrier slot is (x, y, 0.0, 1.0) per record,
 * the XY01 expansion witnessed through the color backend.
 *
 * The US datapath narrows every routed value to s1e7m16, so the pass
 * refuses a model component outside r300_r2vb_fp24_identity_admits with
 * -EDOM before writing any dword.  The synthesized 0.0 and 1.0 lanes
 * are FP24 fixed points by construction.
 */

/* The two BOs the pass references.  The carrier binds as the color
 * target (write); the vertex BO carries both fetch arrays, the slot
 * positions at offset zero and the model records behind them, and both
 * LOAD_VBPNTR pointers resolve through the same relocation slot.
 */
enum r300_r2vb_float2_tuple_slot {
   R300_R2VB_FLOAT2_TUPLE_SLOT_CARRIER = 0,
   R300_R2VB_FLOAT2_TUPLE_SLOT_VERTEX = 1,
   R300_R2VB_FLOAT2_TUPLE_SLOT_COUNT = 2,
};

/* The COLOROFFSET site plus one site per LOAD_VBPNTR pointer. */
#define R300_R2VB_FLOAT2_TUPLE_RELOC_SITES 3u

/* Per-vertex fetch geometry: the FLOAT_4 slot array and the FLOAT_2
 * model array, and the six-dword VAP_VTX_SIZE their sum programs.
 */
#define R300_R2VB_FLOAT2_TUPLE_SLOT_STRIDE_BYTES 16u
#define R300_R2VB_FLOAT2_TUPLE_MODEL_STRIDE_BYTES 8u
#define R300_R2VB_FLOAT2_TUPLE_VTX_SIZE_DWORDS 6u

struct r300_r2vb_float2_tuple_params {
   /* Byte offset of slot (0, 0) inside the carrier BO. */
   uint32_t carrier_offset;
   /* Byte offset of the slot-position array inside the vertex BO; the
    * model array follows at count * 16 bytes.
    */
   uint32_t vertex_offset;
   uint32_t count;
   /* count model records, each (x, y) binary32. */
   const float (*records)[2];
   /* The varying-passthrough US program; see the producer params. */
   const struct r300_fragment_binary *fragment_binary;
   /* When set, the emission opens with the neutral first-draw state
    * contract, so the pass parses standalone.
    */
   const struct r300_first_draw_contract *first_draw_contract;
};

struct r300_r2vb_float2_tuple_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

struct r300_r2vb_float2_tuple_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_r2vb_float2_tuple_reloc_site
      reloc_sites[R300_R2VB_FLOAT2_TUPLE_RELOC_SITES];
   uint32_t reloc_site_count;
   bool owns_ib;
};

/* Emits the complete tuple pass: the first-draw contract prefix when the
 * params carry one, the producer target prologue and carrier retarget,
 * the straight RGBA C4_32_FP output select, the two-element PSC tuple
 * (FLOAT_4 identity at vector 0, FLOAT_2 XY01 at vector 6, LAST on the
 * model element, zero tail), VAP_VTX_SIZE = 6, the two-array
 * 3D_LOAD_VBPNTR with one relocation per pointer, the vertex-list POINTS
 * draw, and the producer publication tail.  Returns 0 or a negative
 * errno; -EDOM names a model component outside the FP24 fixed-point
 * domain.  The caller owns the returned IB allocation.
 */
int r300_r2vb_float2_tuple_pass_emit(
   const struct r300_r2vb_float2_tuple_params *params,
   struct r300_r2vb_float2_tuple_ib *out);

void r300_r2vb_float2_tuple_pass_release(
   struct r300_r2vb_float2_tuple_ib *ib);

/* Proves each recorded relocation site is the payload dword of a
 * relocation NOP carrying its slot's payload: site 0 the carrier at the
 * color offset, sites 1 and 2 the vertex BO behind the LOAD_VBPNTR
 * pointers.  Returns 0, -EINVAL, or -ERANGE.
 */
int r300_r2vb_float2_tuple_pass_validate_reloc_sites(
   const struct r300_r2vb_float2_tuple_ib *ib);

/* Serializes the vertex BO content the emitted pass fetches: count slot
 * positions (v + 0.5, 0.5, 0, 1) followed by the count FLOAT_2 model
 * records, little-endian, the layout both LOAD_VBPNTR arrays address.
 * vertex_bytes must hold count * 24 bytes.
 */
int r300_r2vb_float2_tuple_vertex_stream(const float (*records)[2],
                                         uint32_t count,
                                         uint8_t *vertex_bytes,
                                         uint32_t vertex_capacity_bytes);

/* Writes the carrier content the tuple pass is expected to deliver: for
 * each model record, the XY01 expansion (x, y, 0.0, 1.0) as binary32
 * bit patterns.  Returns 0, -EINVAL, -EDOM, or -ENOSPC.
 */
int r300_r2vb_float2_tuple_expected(const float (*records)[2],
                                    uint32_t count, uint32_t *expected,
                                    uint32_t expected_dwords);

/* The retained reference records: three admitted lattice points whose
 * XY01 expansions are pairwise distinct from the poison dword and from
 * each other, so the carrier check decides every slot independently.
 */
#define R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT 3u

extern const float r300_r2vb_float2_tuple_reference_records
   [R300_R2VB_FLOAT2_TUPLE_REFERENCE_COUNT][2];

/* Emits the reference tuple pass: the reference records through the
 * first-draw contract and the producer fragment binary, carrier and
 * vertex offsets zero.
 */
int r300_r2vb_float2_tuple_reference_emit(
   struct r300_r2vb_float2_tuple_ib *out);

/* The delivery identity over the reference records. */
int r300_r2vb_float2_tuple_reference_expected(uint32_t *expected,
                                              uint32_t expected_dwords);

/* The burst pass raises the GPU duty cycle of one submission along the
 * draws-per-IB axis: one IB carries the reference tuple pass whole as
 * member 0 -- first-draw contract prefix included -- followed by
 * draws - 1 further members, each the identical prefix-free emission
 * retargeted to its own carrier row.  Every member's bytes come from
 * r300_r2vb_float2_tuple_pass_emit unchanged, so member content stays
 * byte-identical to the silicon-qualified single pass; only
 * RB3D_COLOROFFSET0 differs, by one carrier row per member.  Each
 * member's publication tail flushes the color caches before the next
 * retarget, so the members own disjoint carrier rows and the delivery
 * check decides each row independently.
 */
#define R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS 64u

struct r300_r2vb_float2_tuple_burst_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   uint32_t draws;
   /* Member relocation sites in member order, three per member with the
    * single pass's [carrier, vertex, vertex] pattern, ib_index rebased
    * into the composed stream.
    */
   struct r300_r2vb_float2_tuple_reloc_site
      reloc_sites[R300_R2VB_FLOAT2_TUPLE_RELOC_SITES *
                  R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS];
   uint32_t reloc_site_count;
   /* The composed dword index where each member's emission begins;
    * retained evidence splits the stream here when a verdict needs the
    * per-member bytes.
    */
   uint32_t member_start[R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS];
   bool owns_ib;
};

/* One carrier row per member: the reference layout's pitch times its
 * height times the 16-byte texel.  RB3D_COLOROFFSET0 takes each
 * member's row offset directly, so the stride is also the member's
 * color-offset step and the burst carrier allocation is draws rows.
 */
int r300_r2vb_float2_tuple_burst_member_stride_bytes(uint32_t *out);

/* Emits the burst over the reference records.  draws outside
 * 1..R300_R2VB_FLOAT2_TUPLE_BURST_MAX_DRAWS refuses with -EINVAL.  The
 * caller owns the returned IB allocation.
 */
int r300_r2vb_float2_tuple_burst_reference_emit(
   uint32_t draws, struct r300_r2vb_float2_tuple_burst_ib *out);

void r300_r2vb_float2_tuple_burst_release(
   struct r300_r2vb_float2_tuple_burst_ib *ib);

/* Proves the composed sites hold the single pass's per-member pattern:
 * three sites per member in [carrier, vertex, vertex] order, each the
 * payload dword of a relocation NOP carrying its slot's payload, with
 * ib_index strictly increasing across the stream and every member-0
 * site at or past that member's recorded start.  Returns 0, -EINVAL,
 * or -ERANGE.
 */
int r300_r2vb_float2_tuple_burst_validate_reloc_sites(
   const struct r300_r2vb_float2_tuple_burst_ib *ib);

#endif
