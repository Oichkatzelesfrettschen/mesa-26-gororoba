/*
 * SPDX-License-Identifier: MIT
 *
 * R2VB fetched producer: the raster pass that writes an F32 vertex carrier
 * while the VAP fetches the application's records from their own buffer
 * object instead of reading them as embedded draw dwords.
 */

#ifndef R300_R2VB_FETCHED_PRODUCER_H
#define R300_R2VB_FETCHED_PRODUCER_H

#include "r300_pm4_compose.h"
#include "r300_r2vb_fetch_pass.h"
#include "r300_r2vb_producer_pass.h"

#include <stdbool.h>
#include <stdint.h>

struct r300_first_draw_contract;
struct r300_fragment_binary;

/* The fetched producer keeps the immediate producer's color backend, US
 * program, varying routing, slot grid, and publication tail, and replaces
 * the embedded 3D_DRAW_IMMD_2 body with the two-array fetched draw body
 * r300_r2vb_fetch_pass_emit emits: array 0 is a driver-owned slot BO
 * holding one (v + 0.5, 0.5, 0, 1) position per vertex, array 1 is the
 * application's vertex BO at its bound offset and stride.  The immediate
 * pass carries its record pre-swizzled as (z, y, x, w) so the target
 * prologue's BGRA output select stores (x, y, z, w); the fetched pass
 * moves that reordering into the source element's PROG_STREAM_CNTL_EXT
 * swizzle, so every US, RS, and RB register value the silicon-qualified
 * immediate pass established stays byte-identical and the fetch
 * mechanism is the one difference between the two streams.  F32_3 and
 * F32_2 records fill their missing lanes through the same swizzle's
 * FP_ZERO and FP_ONE selects -- the lanes r300_r2vb_identity_deliver
 * synthesizes host-side -- so one emitter covers the three admitted
 * widths and the delivery oracle stays the identity over each.
 *
 * The emitter reads no record bytes, so the FP24 fixed-point admission
 * the immediate emitter applies with -EDOM moves to the caller: the
 * caller derives the expected carrier through r300_r2vb_identity_deliver
 * over the source stream before composing, and that call refuses an
 * out-of-domain component.
 */

/* The three BOs the pass references.  The slot order is the fragment's
 * relocation payload order; the composer rewrites each payload to the
 * caller's chunk index by role, so the slot numbers are fragment-local.
 */
enum r300_r2vb_fetched_producer_slot {
   R300_R2VB_FETCHED_PRODUCER_SLOT_CARRIER = 0,
   R300_R2VB_FETCHED_PRODUCER_SLOT_SLOT = 1,
   R300_R2VB_FETCHED_PRODUCER_SLOT_SOURCE = 2,
   R300_R2VB_FETCHED_PRODUCER_SLOT_COUNT = 3,
};

/* Bytes per slot-position record: one FP32x4 per vertex. */
#define R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES 16u

/* The application vertex stream the pass fetches.  offset_bytes is the
 * first fetched record's byte offset inside the BO (bind offset, attribute
 * offset, and first_vertex * stride already summed); the VBPNTR pointer
 * dword and stride field are dword-granular, so both stay multiples of
 * four; a stride below the record size makes consecutive fetches overlap
 * and refuses.
 */
struct r300_r2vb_fetched_source {
   int format_id;
   uint32_t offset_bytes;
   uint32_t stride_bytes;
   uint64_t bo_size_bytes;
};

struct r300_r2vb_fetched_producer_params {
   /* Byte offset of slot (0, 0) inside the carrier BO. */
   uint32_t carrier_offset;
   struct r300_r2vb_producer_layout layout;
   const struct r300_fragment_binary *fragment_binary;
   const struct r300_first_draw_contract *first_draw_contract;
   struct r300_r2vb_fetched_source source;
   /* The slot BO: layout.count records of
    * R300_R2VB_FETCHED_PRODUCER_SLOT_RECORD_BYTES from slot_offset_bytes.
    */
   uint32_t slot_offset_bytes;
   uint64_t slot_bo_size_bytes;
};

struct r300_r2vb_fetched_producer_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   /* Three sites in stream order: carrier (color target), slot array,
    * source array.
    */
   struct r300_r2vb_producer_reloc_site
      reloc_sites[R300_R2VB_FETCHED_PRODUCER_SLOT_COUNT];
   uint32_t reloc_site_count;
   /* The fetched draw body's first dword: the R300_R2VB_FETCH_PASS_DWORDS
    * run r300_r2vb_fetch_pass_emit wrote, so a reader compares the state
    * outside it against the immediate pass and the body against the
    * fetch-pass contract.
    */
   uint32_t fetch_body_start;
   bool owns_ib;
};

/* Writes the slot-position records the pass fetches: slot v is the point
 * (v + 0.5, 0.5, 0, 1), the center of pixel (v, 0) in the single-row
 * grid.  Returns 0, or -ENOSPC when words holds fewer than count * 4
 * dwords, or -EINVAL on a null destination or zero count.
 */
int r300_r2vb_fetched_producer_slot_positions(uint32_t count,
                                              uint32_t *words,
                                              uint32_t word_count);

/* Derives the fetch state for a source width: FLOAT_4 slot element in VAP
 * vector 0, the source element's data type in vector 6 and terminating
 * the fetch, identity swizzle on the slot and the reversed
 * (z, y, x, w) swizzle on the source with FP_ZERO/FP_ONE filling the
 * lanes a narrower width lacks, VAP_VTX_SIZE 4 + the source's fetch
 * dwords, and the immediate producer's VTX_STATE_CNTL, VSM, OUTPUT_VTX_FMT,
 * GB_ENABLE, RS_COUNT, RS_IP_0, and RS_INST_0 values.  Returns 0 or
 * -EINVAL for a width outside F32_4, F32_3, F32_2.
 */
int r300_r2vb_fetched_producer_fetch_state(
   int format_id, struct r300_r2vb_fetch_state *out);

/* Emits the complete pass: the first-draw contract prefix when the
 * params carry one, the immediate producer's target prologue (with the
 * carrier relocation), color backend, raster and VAP mode state, the US
 * program, then the fetched draw body with its two relocations, then the
 * publication tail.  Returns 0 or a negative errno: -EINVAL for an
 * invalid layout, source width, alignment, or missing fragment binary;
 * -ERANGE when the source or slot array's last fetched byte lies past
 * its BO.  The caller owns the returned IB allocation.
 */
int r300_r2vb_fetched_producer_emit(
   const struct r300_r2vb_fetched_producer_params *params,
   struct r300_r2vb_fetched_producer_ib *out);

int r300_r2vb_fetched_producer_emit_into(
   const struct r300_r2vb_fetched_producer_params *params, uint32_t *words,
   uint32_t capacity, struct r300_r2vb_fetched_producer_ib *out);

void r300_r2vb_fetched_producer_release(
   struct r300_r2vb_fetched_producer_ib *ib);

/* Checks the three sites against the stream: stream order carrier, slot,
 * source; each inside the stream behind the relocation NOP header; each
 * carrying its slot's chunk payload; the slot and source sites inside the
 * fetched body.  Returns 0 or a negative errno.
 */
int r300_r2vb_fetched_producer_validate_reloc_sites(
   const struct r300_r2vb_fetched_producer_ib *ib);

/* Emits the reference-shaped pass for a width: the reference count and
 * single-row layout, the first-draw contract prefix, the reference
 * fragment binary, carrier and slot offsets zero, the source at offset
 * zero with the width's record size as stride, and one page for each
 * fetched BO.  Every pre-hardware consumer takes the pass from here so
 * their streams stay byte-identical.
 */
int r300_r2vb_fetched_producer_reference_emit(
   int format_id, struct r300_r2vb_fetched_producer_ib *out);

/* The composed fetched route: the fetched producer ahead of the TCL-bypass
 * consumer in one IB, bound through r300_pm4_compose over the four BO
 * roles -- CARRIER written by the producer's color backend and read by the
 * consumer's vertex fetch, SLOT and SOURCE read by the producer's vertex
 * fetch, COLOR written by the consumer.  The composer refuses a second
 * writer of any role, so a fragment that misdeclares a read site as a
 * write fails the composition instead of producing a stream with two
 * owners of one destination.
 */
struct r300_r2vb_fetched_route_params {
   struct r300_r2vb_fetched_source source;
   uint32_t slot_offset_bytes;
   uint64_t slot_bo_size_bytes;
   /* The consumer half: its emitted words and the sites inside them.  The
    * producer half is always r300_r2vb_fetched_producer_reference_emit's
    * geometry over the params' source, so the consumer is the one caller
    * input -- a recorded consumer IB composes exactly as the reference
    * extent emission does.
    */
   const uint32_t *consumer_words;
   uint32_t consumer_dwords;
   uint32_t consumer_carrier_site;
   uint32_t consumer_color_site;
   /* Relocation-chunk index for each role, indexed by
    * enum r300_r2vb_bo_role.
    */
   struct r300_pm4_role_map roles;
};

struct r300_r2vb_fetched_route_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   uint32_t consumer_start_dwords;
   struct r300_pm4_composition composition;
};

/* Composes the route.  Returns 0 or the first refusal: the producer
 * emitter's errno, the composer's errno (-EEXIST for a doubly written
 * role, -ENOENT for an unbound role), or -ENOMEM.  The caller frees
 * out->ib.
 */
int r300_r2vb_fetched_route_compose(
   const struct r300_r2vb_fetched_route_params *params,
   struct r300_r2vb_fetched_route_ib *out);

void r300_r2vb_fetched_route_release(struct r300_r2vb_fetched_route_ib *ib);

/* Composes the reference route for a width: the reference producer over
 * a one-page source at offset zero, the reference consumer at its maximum
 * extent, and the role map CARRIER 0, COLOR 1, SLOT 2, SOURCE 3 -- the
 * order the driver's reference list binds.  This is the stream whose
 * digest an offline composition and the driver's submit-time composition
 * both produce for the same geometry.
 */
int r300_r2vb_fetched_route_reference_compose(
   int format_id, struct r300_r2vb_fetched_route_ib *out);

/* Finds every relocation NOP site in a PM4 stream by walking packet
 * headers: each site is the payload dword behind a CP_PACKET3 NOP of one
 * payload dword.  Returns the site count, or -EINVAL on a malformed
 * header or a packet running past the stream, or -ENOSPC when more sites
 * exist than the destination holds.
 */
int r300_pm4_scan_reloc_sites(const uint32_t *words, uint32_t dword_count,
                              uint32_t *site_indices, uint32_t *payloads,
                              uint32_t max_sites);

#endif /* R300_R2VB_FETCHED_PRODUCER_H */
