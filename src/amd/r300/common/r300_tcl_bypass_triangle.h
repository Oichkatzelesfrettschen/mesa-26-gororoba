/*
 * SPDX-License-Identifier: MIT
 *
 * Fixed TCL-bypass triangle cell: the first native hardware witness.
 */

#ifndef R300_TCL_BYPASS_TRIANGLE_H
#define R300_TCL_BYPASS_TRIANGLE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct r300_fragment_binary;
struct r300_first_draw_contract;

/* BO slots the cell references; the transport binds slot order to the
 * relocation-list order at submission.
 */
enum r300_tcl_bypass_triangle_slot {
   R300_TRIANGLE_SLOT_VERTEX = 0,
   R300_TRIANGLE_SLOT_COLOR = 1,
   R300_TRIANGLE_SLOT_COUNT = 2,
};

struct r300_tcl_bypass_triangle_params {
   /* Byte offset of the first vertex inside the vertex BO. */
   uint32_t vertex_offset;
   /* RB3D_COLORPITCH0 value: pitch in pixels plus format and endian
    * fields, chosen by the caller from the color BO's layout.
    */
   uint32_t color_pitch_format;
   const struct r300_fragment_binary *fragment_binary;
   /* When set, the emission opens with the neutral first-draw state
    * contract, so the stream establishes every register the draw depends
    * on and renders independent of predecessor context.  A NULL contract
    * emits the bare cell, whose output rides inherited state; the
    * poison-model calibration in r300_first_draw_state_test consumes that
    * form as its known-bad input.
    */
   const struct r300_first_draw_contract *first_draw_contract;
};

/* One IB position whose payload names a relocation slot. */
struct r300_tcl_bypass_triangle_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

/* The emitter references each slot exactly once, so the site array holds one
 * entry per slot.  The site validator proves slot uniqueness in one uint32_t
 * of slot bits, so the slot space stays inside 32.
 */
#define R300_TRIANGLE_MAX_RELOC_SITES R300_TRIANGLE_SLOT_COUNT
static_assert(R300_TRIANGLE_SLOT_COUNT <= 32,
              "slot uniqueness is proven in a 32-bit mask");

struct r300_tcl_bypass_triangle_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_tcl_bypass_triangle_reloc_site
      reloc_sites[R300_TRIANGLE_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   /* Set when the emission allocated ib, so the release frees what it owns
    * and leaves caller storage alone.
    */
   bool owns_ib;
};

/* Emits the complete fixed cell: the first-draw contract prefix when the
 * params carry one, then TCL bypass, one FLOAT_4 position stream
 * with identity PSC selectors, VAP_VTX_SIZE = 4, position-only VAP output,
 * the owned fragment binary verbatim, one color target, depth disabled,
 * destination-cache publication, one vertex-list triangle draw.  Returns 0
 * or a negative errno; the caller owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_emit(
   const struct r300_tcl_bypass_triangle_params *params,
   struct r300_tcl_bypass_triangle_ib *out);

/* Emits into caller storage of exactly capacity dwords.  The write refuses
 * with -ENOSPC the moment an operation would pass that bound, leaving the
 * words already placed intact and reporting no dword count, so a destination
 * too small for the cell yields a refusal rather than a short stream.
 */
int r300_tcl_bypass_triangle_emit_into(
   const struct r300_tcl_bypass_triangle_params *params, uint32_t *words,
   uint32_t capacity, struct r300_tcl_bypass_triangle_ib *out);

void r300_tcl_bypass_triangle_release(struct r300_tcl_bypass_triangle_ib *ib);

/* Checks the emitted relocation sites against the stream they index: one site
 * per slot in stream order, each inside the stream, and each naming the slot
 * whose payload sits at that index.  Returns 0 or a negative errno.
 */
int r300_tcl_bypass_triangle_validate_reloc_sites(
   const struct r300_tcl_bypass_triangle_ib *ib);

/* Builds the cell's fragment binary from the compiled constant-color US
 * block (r300_tcl_bypass_triangle_fs_block.h, baked by
 * r300_tcl_bypass_fs_tool from the classic compiler ladder).  Every
 * pre-hardware consumer -- manifest tool, native recorder, harness --
 * takes the block from here so their IBs stay byte-identical.  Returns 0
 * or a negative errno; the caller owns the binary.
 */
int r300_tcl_bypass_triangle_reference_fs(struct r300_fragment_binary *fs);

/* Resolves the first-draw contract for the cell's 64x64 target and three
 * vertices with the texture block disabled.  Every pre-hardware consumer
 * -- manifest tool, native recorder, harness reference -- takes the
 * contract from here so their contract-prefixed cells stay byte-identical.
 * Returns
 * 0 or a negative errno.
 */
int r300_tcl_bypass_triangle_reference_contract(
   struct r300_first_draw_contract *out);

/* Emits the complete reference cell -- reference fragment binary,
 * reference first-draw contract, vertex offset zero, linear 64-pixel
 * ARGB8888 pitch -- so every fixed-cell authority (native recorder,
 * arming runner, manifest tool, harness reference) produces one
 * byte-identical IB from one construction.  Returns 0 or a negative
 * errno; the caller owns the returned IB allocation.
 */
int r300_tcl_bypass_triangle_reference_emit(
   struct r300_tcl_bypass_triangle_ib *out);

/* Packs RB3D_COLORPITCH0 for a linear little-endian ARGB8888 target the
 * way r300_texture.c derives surf->pitch: the pitch in pixels ORed with
 * the ARGB8888 color-format field, tiling and endian fields zero.  Returns
 * 0 when the pitch is odd or exceeds the register's pitch field.
 */
uint32_t r300_rb3d_colorpitch0_pack_argb8888(uint32_t pitch_pixels);

/* The compiled fragment program writes opaque green; this is that color as
 * one ARGB8888 dword, the value the output oracle demands from interior
 * pixels.  The attended run is the falsifier for the byte order.
 */
#define R300_TRIANGLE_DRAW_COLOR_ARGB8888 0xff00ff00u

/* Deterministic pre-draw fill for the color target; it differs from the
 * draw color in every byte lane, so any device write is detectable.
 */
#define R300_TRIANGLE_COLOR_SENTINEL 0xa5a5a5a5u

/* Output-oracle verdict over a sentinel-initialized 64-pixel-pitch
 * ARGB8888 target.  executed reports any deviation from the sentinel;
 * interior demands the draw color at sample points inside the triangle;
 * exterior demands the sentinel at in-target points outside it; canary
 * demands the sentinel in every row past the 64-row render extent.
 */
struct r300_triangle_oracle_verdict {
   bool executed;
   bool interior_pass;
   bool exterior_pass;
   bool canary_pass;
};

void r300_tcl_bypass_triangle_oracle(
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_triangle_oracle_verdict *verdict);

/* The pretransformed screen-space triangle for a 64x64 color target: three
 * FLOAT_4 positions, sixteen bytes each, the payload of the cell's vertex
 * BO.
 */
#define R300_TRIANGLE_VERTEX_DWORDS 12
extern const float
   r300_tcl_bypass_triangle_vertices[R300_TRIANGLE_VERTEX_DWORDS];

#endif /* R300_TCL_BYPASS_TRIANGLE_H */
