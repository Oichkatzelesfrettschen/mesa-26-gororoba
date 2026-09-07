/*
 * SPDX-License-Identifier: MIT
 *
 * Depth control cell: a draw whose color output and depth memory both
 * follow the depth test.
 */

#ifndef R300_ZB_DEPTH_CONTROL_CELL_H
#define R300_ZB_DEPTH_CONTROL_CELL_H

#include "r300_zb_depth_layout.h"
#include "r300_zb_depth_surface.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct r300_fragment_binary;
struct r300_first_draw_contract;

/* The cell draws one triangle list of two triangles over disjoint halves
 * of the target, at two window-space depths, against a depth surface the
 * host fills with a sentinel between the two.  R300_ZS_LESS admits the
 * near triangle and rejects the far one, so the near half carries the
 * draw color over a depth value below the sentinel while the far half
 * keeps the color sentinel over the depth sentinel untouched.  Both
 * halves are read back: the color proves the test gates the write, the
 * depth proves a passing fragment reaches memory.
 *
 * Disjoint halves establish comparison against depth memory and the
 * depth write.  Occlusion between two primitives covering one pixel is a
 * separate property this cell leaves unmeasured.
 *
 * The first-draw contract's ZB_ZCACHE_CTLSTAT ordering barrier is what
 * makes the host's pre-draw depth fill the comparison's other operand:
 * the barrier retires the Z cache before the draw, so the test reads the
 * surface rather than a stale line.
 */

/* BO slots the cell references; the transport binds slot order to the
 * relocation-list order at submission.
 */
enum r300_zb_depth_control_slot {
   R300_ZB_DEPTH_CONTROL_SLOT_VERTEX = 0,
   R300_ZB_DEPTH_CONTROL_SLOT_COLOR = 1,
   R300_ZB_DEPTH_CONTROL_SLOT_DEPTH = 2,
   R300_ZB_DEPTH_CONTROL_SLOT_COUNT = 3,
};

struct r300_zb_depth_control_params {
   /* Byte offset of the first vertex inside the vertex BO. */
   uint32_t vertex_offset;
   /* RB3D_COLORPITCH0 value: pitch in pixels plus format and endian
    * fields, chosen by the caller from the color BO's layout.
    */
   uint32_t color_pitch_format;
   /* Byte offset of the depth surface inside the depth BO; ZB_DEPTHOFFSET
    * encodes bits 31 to 5, so the low five bits stay clear.
    */
   uint32_t depth_offset_bytes;
   /* The depth surface the cell binds.  NULL takes
    * r300_zb_depth_surface_z16_linear, the surface the cell has always
    * emitted, so a caller that names no surface produces the retained
    * stream.  A surface the cell cannot fill from the host is refused:
    * the pre-draw depth fill is the comparison's other operand, and a
    * tiled surface's bytes follow a transform this tree does not carry.
    */
   const struct r300_zb_depth_surface *surface;
   const struct r300_fragment_binary *fragment_binary;
   /* The contract establishes every register the draw depends on.  A
    * control whose verdict is the difference between two halves of one
    * target reads a predecessor's depth state as its own, so the cell
    * refuses a null contract.
    */
   const struct r300_first_draw_contract *first_draw_contract;
};

/* One IB position whose payload names a relocation slot. */
struct r300_zb_depth_control_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

#define R300_ZB_DEPTH_CONTROL_MAX_RELOC_SITES R300_ZB_DEPTH_CONTROL_SLOT_COUNT
static_assert(R300_ZB_DEPTH_CONTROL_SLOT_COUNT <= 32,
              "slot uniqueness is proven in a 32-bit mask");

struct r300_zb_depth_control_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_zb_depth_control_reloc_site
      reloc_sites[R300_ZB_DEPTH_CONTROL_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   /* Set when the emission allocated ib, so the release frees what it
    * owns and leaves caller storage alone.
    */
   bool owns_ib;
};

/* The cell's render geometry.  The manifest publishes these and the
 * contract resolution derives scissor, clip, and the vertex bound from
 * them.  Both allocations carry one row past the render extent, which the
 * oracles read as their canary.
 */
#define R300_ZB_DEPTH_CONTROL_TARGET_WIDTH 64u
#define R300_ZB_DEPTH_CONTROL_TARGET_HEIGHT 64u
#define R300_ZB_DEPTH_CONTROL_PITCH_PIXELS 64u
#define R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS 65u
#define R300_ZB_DEPTH_CONTROL_COLOR_BYTES \
   (R300_ZB_DEPTH_CONTROL_PITCH_PIXELS * \
    R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS * 4u)
/* R300_DEPTHFORMAT_16BIT_INT_Z stores two bytes per pixel, the width the
 * kernel's r300_packet0_check reads out of ZB_FORMAT as track->zb.cpp.
 * Symbol discovery uses (rg --fixed-strings r300_packet0_check
 * drivers/gpu/drm/radeon/; rg --fixed-strings track->zb.cpp
 * drivers/gpu/drm/radeon/) in the Linux kernel source tree.
 */
#define R300_ZB_DEPTH_CONTROL_DEPTH_CPP 2u
#define R300_ZB_DEPTH_CONTROL_DEPTH_BYTES \
   (R300_ZB_DEPTH_CONTROL_PITCH_PIXELS * \
    R300_ZB_DEPTH_CONTROL_ALLOCATION_ROWS * R300_ZB_DEPTH_CONTROL_DEPTH_CPP)

/* Deterministic pre-draw fill for the depth surface.  It sits between the
 * two triangles' window-space depths, so R300_ZS_LESS separates them
 * against one fill: the near triangle's fragments compare below it and
 * the far triangle's above it.
 */
#define R300_ZB_DEPTH_CONTROL_DEPTH_SENTINEL 0x8000u

/* Window-space depths of the two triangles, in the [0, 1] range VTE's
 * pretransformed Z format carries.
 */
#define R300_ZB_DEPTH_CONTROL_NEAR_Z 0.25f
#define R300_ZB_DEPTH_CONTROL_FAR_Z 0.75f

/* Six FLOAT_4 positions, sixteen bytes each: triangle 0 near in the left
 * half, triangle 1 far in the right half, the payload of the cell's
 * vertex BO.
 */
#define R300_ZB_DEPTH_CONTROL_VERTEX_DWORDS 24
extern const float
   r300_zb_depth_control_vertices[R300_ZB_DEPTH_CONTROL_VERTEX_DWORDS];

/* Dwords r300_zb_depth_control_emit_into writes past the fragment
 * binary's own size, so a caller sizes its storage before building.
 */
#define R300_ZB_DEPTH_CONTROL_MAX_DWORDS 512

/* Emits the complete cell: the first-draw contract prefix, TCL bypass,
 * one FLOAT_4 position stream with identity PSC selectors, the owned
 * fragment binary verbatim, one color target, the depth binding and test
 * state, one six-vertex triangle-list draw, and the color and depth cache
 * publications.  Returns 0 or a negative errno; the caller owns the
 * returned IB allocation.
 */
int r300_zb_depth_control_emit(
   const struct r300_zb_depth_control_params *params,
   struct r300_zb_depth_control_ib *out);

/* Emits into caller storage of exactly capacity dwords.  The write
 * refuses with -ENOSPC the moment an operation would pass that bound,
 * leaving the words already placed intact and reporting no dword count,
 * so a destination too small for the cell yields a refusal rather than a
 * short stream.
 */
int r300_zb_depth_control_emit_into(
   const struct r300_zb_depth_control_params *params, uint32_t *words,
   uint32_t capacity, struct r300_zb_depth_control_ib *out);

void r300_zb_depth_control_release(struct r300_zb_depth_control_ib *ib);

/* Checks the emitted relocation sites against the stream they index: one
 * site per slot in stream order, each inside the stream, and each naming
 * the slot whose payload sits at that index.  Returns 0 or a negative
 * errno.
 */
int r300_zb_depth_control_validate_reloc_sites(
   const struct r300_zb_depth_control_ib *ib);

/* Resolves the contract for the cell's 64x64 target and six vertices with
 * the texture block disabled, then places the B8G8R8A8 output format.
 * Every pre-hardware consumer -- manifest tool, harness reference, test
 * -- takes the contract from here so their cells stay byte-identical.
 * Returns 0 or a negative errno.
 */
int r300_zb_depth_control_reference_contract(
   struct r300_first_draw_contract *out);

/* Emits the complete reference cell -- the triangle cell's compiled
 * constant-color fragment binary, the reference contract, vertex offset
 * zero, depth offset zero, linear 64-pixel B8G8R8A8 pitch -- so every
 * authority produces one byte-identical IB from one construction.
 * Returns 0 or a negative errno; the caller owns the returned IB.
 */
int r300_zb_depth_control_reference_emit(
   struct r300_zb_depth_control_ib *out);

/* The reference cell bound to a named depth surface: everything above
 * with the surface substituted, so the stream differs from the retained
 * one in ZB_FORMAT and the DEPTHPITCH tile bits alone.  NULL takes the
 * Z16 linear surface and reproduces that stream exactly.  Returns 0 or a
 * negative errno; the caller owns the returned IB.
 */
int r300_zb_depth_control_reference_emit_surface(
   const struct r300_zb_depth_surface *surface,
   struct r300_zb_depth_control_ib *out);

/* The ZB_FORMAT value the emitted stream carries, read back out of the
 * one-dword PACKET0 run that writes it.  The kernel's r300_packet0_check
 * reads the same dword into track->zb.cpp and sizes its depth bound from
 * it, so this is the format the hardware and the parser will act on
 * rather than the one a caller believes it selected.
 *
 * Returns 0 with the value written, or -EINVAL when ib is null, dwords
 * is short, or the stream carries no such run.  A consumer holding a
 * separate record of the surface compares the two and refuses a
 * disagreement, which turns the agreement between a recorded selection
 * and a recorded stream into a check rather than a convention.
 */
int r300_zb_depth_control_ib_depth_format(const uint32_t *ib, uint32_t dwords,
                                          uint32_t *format_out);

/* The dword index of the draw packet's header, which a replay names when
 * it reports a verdict at a packet index.
 */
uint32_t r300_zb_depth_control_draw_dword(
   const struct r300_zb_depth_control_ib *ib);

/* Color-output verdict over a sentinel-initialized target at the cell's
 * pitch.  executed reports any deviation from the color sentinel;
 * near_pass demands the draw color at margin-checked sample points inside
 * the near triangle; far_pass demands the sentinel inside the far
 * triangle, which is the depth test rejecting those fragments;
 * exterior_pass demands the sentinel at in-extent points outside both
 * triangles; canary_pass demands it in the sub-pitch padding band of
 * every rendered row and in the row past the render extent.
 *
 * near_colored and far_colored count the samples of each region that
 * carried the draw color.  R300_ZS_LESS compares the incoming fragment's
 * depth against the stored value, so an implementation comparing the
 * other way renders the exact complement of this image; the two counts
 * name which half the device colored rather than reporting a mismatch
 * whose sense the reader has to reconstruct.
 *
 * A pass requires its sample count positive, so a buffer too short to
 * carry the geometry fails every pass with zero samples.
 */
struct r300_zb_depth_control_color_verdict {
   bool executed;
   bool near_pass;
   bool far_pass;
   bool exterior_pass;
   bool canary_pass;
   uint32_t near_samples;
   uint32_t far_samples;
   uint32_t exterior_samples;
   uint32_t near_colored;
   uint32_t far_colored;
};

void r300_zb_depth_control_color_oracle(
   const uint32_t *pixels, uint32_t size_bytes,
   struct r300_zb_depth_control_color_verdict *verdict);

/* Depth verdict over a sentinel-initialized depth surface at the cell's
 * pitch.  written reports any deviation from the packed depth sentinel;
 * near_pass demands a stored depth code strictly below the sentinel code
 * and above zero inside the near triangle, which is what R300_ZS_LESS
 * with Z_WRITE_ENABLE stores for a near plane inside the range;
 * far_pass, exterior_pass, and canary_pass demand the sentinel word
 * exactly.
 *
 * Both near bounds are one-sided, so the comparison's own ordering is the
 * predicate and no window-space-Z to depth-code rounding rule enters a
 * pass condition.  The lower bound separates the near depth from a
 * surface nothing wrote: an allocation of zeros clears the upper bound
 * under any rounding rule, at either pixel width.  near_min and near_max
 * carry the observed range as data, which a manifest retains and a later
 * run compares against.  They are depth codes, so a Z24 surface reports
 * the 24-bit value rather than the packed word, and a verdict with no
 * near sample reports zero for both rather than the scan's seed.
 *
 * The near distribution carries what the range cannot.  A minimum and a
 * maximum admit two populations between them -- half the region at one
 * depth code and half at another -- which matters the moment a stored
 * code becomes a marker a later cell reads back.  near_distinct holds
 * the distinct codes the near region stored and near_distinct_counts the
 * samples at each, in first-seen order, so the counts sum to
 * near_samples exactly when near_distinct_overflow is clear.  The
 * capacity is small because the near triangle draws at one window-space
 * depth: a second code is already the finding, and a region beyond the
 * capacity sets the overflow flag and stops recording rather than
 * dropping codes silently.
 *
 * The stencil fields observe rather than assert.  ZB_CNTL leaves the
 * stencil test and stencil writes disabled, so whether a depth write
 * preserves the low byte of a packed Z24/S8 word is a property of the
 * part that nothing in this tree establishes.  stencil_observed says the
 * format stores a stencil component at all, which the surface decides
 * and no sample can change; stencil_min and stencil_max carry the range
 * seen across the near triangle, where the depth write lands, and report
 * zero when no sample filled them.  A verdict never fails on them -- a stencil surprise is a
 * finding to record, not a depth result to reject -- and a 16-bit
 * surface reports stencil_observed false with both bounds zero.
 */
/* Distinct near-region depth codes one verdict records before it stops
 * distinguishing them.  One plane at one depth stores one code, so this
 * carries a wide margin over the expected population. */
#define R300_ZB_DEPTH_CONTROL_MAX_NEAR_CODES 8u

struct r300_zb_depth_control_depth_verdict {
   bool written;
   bool near_pass;
   bool far_pass;
   bool exterior_pass;
   bool canary_pass;
   uint32_t near_samples;
   uint32_t far_samples;
   uint32_t exterior_samples;
   uint32_t near_min;
   uint32_t near_max;
   uint32_t near_distinct[R300_ZB_DEPTH_CONTROL_MAX_NEAR_CODES];
   uint32_t near_distinct_counts[R300_ZB_DEPTH_CONTROL_MAX_NEAR_CODES];
   uint32_t near_distinct_count;
   bool near_distinct_overflow;
   bool stencil_observed;
   uint32_t stencil_min;
   uint32_t stencil_max;
};

/* The depth verdict over any surface the resolver addresses.  The
 * surface supplies the format, the geometry, and the sentinel code; the
 * resolver supplies the byte each logical pixel occupies; bytes and
 * size_bytes are the raw allocation as the host mapped it, read from
 * base_offset_bytes.
 *
 * Every read is bound-checked against size_bytes through the resolved
 * offset rather than against a precomputed pixel count, so a descriptor
 * naming a pitch the allocation does not carry produces a failed verdict
 * instead of a read past the mapping.  A resolver that refuses a
 * coordinate, an allocation too short to hold the surface, or a null
 * argument yields the zeroed verdict, whose passes are all false with
 * zero samples.
 */
void r300_zb_depth_control_depth_oracle_surface(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zb_depth_address_resolver *resolver,
   uint64_t base_offset_bytes, const void *bytes, uint64_t size_bytes,
   struct r300_zb_depth_control_depth_verdict *verdict);

/* The Z16 linear reading of the same verdict: the retained cell's
 * surface, the row-major resolver, and an allocation whose storage
 * begins at byte zero.  The reference cell's expected verdicts are
 * stated against this entry point and do not move. */
void r300_zb_depth_control_depth_oracle(
   const uint16_t *depth, uint32_t size_bytes,
   struct r300_zb_depth_control_depth_verdict *verdict);

#endif /* R300_ZB_DEPTH_CONTROL_CELL_H */
