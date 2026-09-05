/*
 * SPDX-License-Identifier: MIT
 *
 * RB2D legalizer: the compiler in front of the 2D packet emitter.  A
 * linear buffer has no rows, so DST_PITCH_OFFSET, DST_Y_X, and
 * DST_WIDTH_HEIGHT are an address-generator instruction set the driver
 * targets, and the hardware grids -- 1 KiB base, 64-byte pitch, 16-bit
 * coordinate fields, the 13-bit scissor, the datatype table, the
 * relocation-backed object -- are that target's constraints.  The
 * legalizer lowers an arbitrary dword-aligned byte interval into windows
 * that satisfy every constraint, the same way a register allocator lowers
 * an unbounded virtual register file onto a fixed one.
 *
 *    byte interval [offset, offset + size)
 *      -> carrier choice          pitch and format from the evidence registry
 *      -> window sequence         each rebased on the 1 KiB grid, y small
 *      -> rectangle tiling        first-row remainder, whole rows, tail
 *      -> window check            every invariant below, per window
 *      -> coverage check          exact byte-set equality across windows
 *      -> emission                DAG-ordered state, typed relocations
 *
 * Three acceptances stay distinct.  Semantic acceptance asks whether the
 * Vulkan operation can execute at all; legalized-plan acceptance asks
 * whether it decomposes into hardware-valid windows; raw-stream acceptance
 * asks whether one exact packet stream is safe, and that one is the
 * kernel's, deliberately narrow.  The legalizer moves an operation from
 * the first to the second; it never widens the third.
 */

#ifndef R300_RB2D_LEGALIZE_H
#define R300_RB2D_LEGALIZE_H

#include "r300_rb2d_fill.h"
#include "r300_rb2d_linear_span.h"
#include "r300_rb2d_pitch_evidence.h"

#include <stdbool.h>
#include <stdint.h>

/* The two route contracts.  V1 is the qualified public fill: the 256-byte
 * ARGB8888 carrier, one window, at most three rectangles, and the exact
 * 38-dword stream the silicon receipt retains for the attended cell.  V2
 * admits any carrier the evidence registry admits, several rebased
 * windows in one stream, and one relocation site per window; it is
 * qualified separately and the route selects it only under its own
 * receipt. */
enum r300_rb2d_contract {
   R300_RB2D_CONTRACT_CONST_FILL_V1 = 0,
   R300_RB2D_CONTRACT_CONST_FILL_V2,
   R300_RB2D_CONTRACT_COUNT,
};

#define R300_RB2D_CONTRACT_V1_PITCH_BYTES R300_RB2D_SPAN_PITCH_DIRECT_WRITE
#define R300_RB2D_CONTRACT_V1_MAX_WINDOWS 1u
#define R300_RB2D_CONTRACT_V1_MAX_RECTS 3u

struct r300_rb2d_legalize_request {
   uint64_t byte_offset;
   uint64_t byte_size;
   /* The 32-bit pattern in destination byte order. */
   uint32_t pattern;
   /* Size of the object the destination relocation will consume; the
    * kernel bounds the footprint against radeon_bo_size of that object. */
   uint64_t bo_size;
   enum r300_rb2d_usage usage;
   enum r300_rb2d_contract contract;
   /* The lowest evidence class a carrier needs to be chosen.  Execution
    * passes SILICON_RECEIPT; the cost-model tests pass PLANNED to rank
    * candidates nothing has run. */
   enum r300_rb2d_pitch_evidence_class minimum_evidence;
   /* A pitch to use instead of the chooser's, or zero for the chooser.  A
    * pinned pitch still has to be admitted at minimum_evidence. */
   uint32_t pinned_pitch_bytes;
};

/* One surface window: a rebased DST_PITCH_OFFSET surface and the
 * rectangles cut on it.  height_rows is the surface height the plan
 * declares, the row past the last rectangle. */
struct r300_rb2d_window {
   uint64_t bo_base;
   uint32_t pitch_bytes;
   uint32_t cpp;
   enum r300_rb2d_format format;
   uint32_t height_rows;
   struct r300_rb2d_fill_rect rects[R300_RB2D_SPAN_MAX_RECTS_PER_SEGMENT];
   uint32_t rect_count;
   /* Index of this window's relocation site in the emitted stream's site
    * list; each window binds the destination once. */
   uint32_t relocation_site;
};

/* Every invariant a window is held to, in check order, so a refusal
 * names one fact.  The list is the directive's: base on the 1 KiB grid,
 * pitch on the 64-byte grid and inside its field, x inside the row, x +
 * width inside the row, x + width and y + height inside the safe scissor
 * end, the last written byte inside the object, the address
 * representable, no zero rectangle, a supported datatype. */
enum r300_rb2d_window_refusal {
   R300_RB2D_WINDOW_OK = 0,
   R300_RB2D_WINDOW_REFUSE_NULL,
   R300_RB2D_WINDOW_REFUSE_FORMAT,
   R300_RB2D_WINDOW_REFUSE_CPP_MISMATCH,
   R300_RB2D_WINDOW_REFUSE_BASE_GRID,
   R300_RB2D_WINDOW_REFUSE_BASE_FIELD,
   R300_RB2D_WINDOW_REFUSE_PITCH_ZERO,
   R300_RB2D_WINDOW_REFUSE_PITCH_GRID,
   R300_RB2D_WINDOW_REFUSE_PITCH_FIELD,
   R300_RB2D_WINDOW_REFUSE_NO_RECTS,
   R300_RB2D_WINDOW_REFUSE_RECT_EMPTY,
   R300_RB2D_WINDOW_REFUSE_X_PAST_ROW,
   R300_RB2D_WINDOW_REFUSE_WIDTH_PAST_ROW,
   R300_RB2D_WINDOW_REFUSE_BEYOND_SCISSOR,
   R300_RB2D_WINDOW_REFUSE_HEIGHT_MISMATCH,
   R300_RB2D_WINDOW_REFUSE_ADDRESS_WIDTH,
   R300_RB2D_WINDOW_REFUSE_PAST_OBJECT,
   R300_RB2D_WINDOW_REFUSE_PLAN_REJECTED,
   R300_RB2D_WINDOW_REFUSAL_COUNT,
};

const char *r300_rb2d_window_refusal_name(enum r300_rb2d_window_refusal r);

/* Holds one window to every invariant above against the object size the
 * relocation will bind.  Independent of the legalizer: a window built by
 * hand is judged the same way. */
enum r300_rb2d_window_refusal
r300_rb2d_window_check(const struct r300_rb2d_window *w, uint64_t bo_size);

/* The window as the fill plan the emitter consumes.  The plan's rects
 * point into the window, so the window outlives the plan. */
void r300_rb2d_window_to_fill_plan(const struct r300_rb2d_window *w,
                                   struct r300_rb2d_fill_plan *plan);

/* Why a request could not be legalized.  Carrier refusals come first,
 * then the span decomposition's own refusal, then a contract bound, then
 * a window or coverage failure that would be a legalizer defect. */
enum r300_rb2d_legalize_refusal {
   R300_RB2D_LEGALIZE_OK = 0,
   R300_RB2D_LEGALIZE_REFUSE_NULL,
   R300_RB2D_LEGALIZE_REFUSE_USAGE,
   R300_RB2D_LEGALIZE_REFUSE_CONTRACT,
   R300_RB2D_LEGALIZE_REFUSE_SIZE_ZERO,
   R300_RB2D_LEGALIZE_REFUSE_NO_ADMITTED_CARRIER,
   R300_RB2D_LEGALIZE_REFUSE_PINNED_PITCH_UNADMITTED,
   R300_RB2D_LEGALIZE_REFUSE_SPAN,
   R300_RB2D_LEGALIZE_REFUSE_STORAGE,
   R300_RB2D_LEGALIZE_REFUSE_CONTRACT_WINDOWS,
   R300_RB2D_LEGALIZE_REFUSE_CONTRACT_RECTS,
   R300_RB2D_LEGALIZE_REFUSE_WINDOW,
   R300_RB2D_LEGALIZE_REFUSE_COVERAGE,
   R300_RB2D_LEGALIZE_REFUSAL_COUNT,
};

const char *
r300_rb2d_legalize_refusal_name(enum r300_rb2d_legalize_refusal r);

/* The outcome of a legalization: the carrier chosen, the windows, and the
 * span refusal when the decomposition itself refused. */
struct r300_rb2d_legalize_result {
   enum r300_rb2d_legalize_refusal refusal;
   enum r300_rb2d_span_refusal span_refusal;
   enum r300_rb2d_window_refusal window_refusal;
   uint32_t pitch_bytes;
   enum r300_rb2d_format format;
   uint32_t window_count;
   uint32_t rect_count;
   uint32_t relocation_sites;
   uint32_t ib_dwords;
};

/* Lowers a request into windows[] of capacity max_windows.  Every window
 * is checked, the union of every rectangle's bytes is checked equal to the
 * request interval, and the result carries the counts the cost model
 * reads.  A refusal writes no window.  Returns the window count, zero on
 * refusal. */
uint32_t
r300_rb2d_legalize_linear_span(const struct r300_rb2d_legalize_request *req,
                               struct r300_rb2d_window *windows,
                               uint32_t max_windows,
                               struct r300_rb2d_legalize_result *result);

/* Cost weights over the counts a legalization produces.  Legality and
 * evidence admission are decided before any weight is read. */
struct r300_rb2d_cost_weights {
   uint32_t per_window;
   uint32_t per_relocation_site;
   uint32_t per_rect;
   uint32_t per_ib_dword;
};

/* The default weights: a window rebind is a relocation the kernel walks
 * and a surface the engine re-latches, so it costs most; a rectangle is a
 * launch; a dword is bus traffic. */
extern const struct r300_rb2d_cost_weights r300_rb2d_default_cost_weights;

uint64_t r300_rb2d_legalize_cost(const struct r300_rb2d_legalize_result *r,
                                 const struct r300_rb2d_cost_weights *w);

/* Chooses the cheapest admitted carrier for a request by legalizing it on
 * every registry row that reaches minimum_evidence under the request's
 * usage and contract, ranking by cost with the smaller pitch winning a
 * tie.  Returns the pitch, or zero with *format untouched when no row is
 * admitted or none legalizes. */
uint32_t r300_rb2d_choose_pitch(const struct r300_rb2d_legalize_request *req,
                                const struct r300_rb2d_cost_weights *weights,
                                enum r300_rb2d_format *format_out);

/* Relocation sites an emitted multi-window stream carries: one per
 * window. */
#define R300_RB2D_LEGALIZE_MAX_WINDOWS 64u

struct r300_rb2d_legalized_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_rb2d_fill_reloc_site sites[R300_RB2D_LEGALIZE_MAX_WINDOWS];
   uint32_t site_count;
};

/* Emits windows in order into one stream: per window the destination
 * bound by its relocation, the common state on that surface, the
 * rectangles, and the epilogue.  A V1 legalization emits the exact bytes
 * r300_rb2d_fill_emit_into emits for its plan.  Returns 0, -EINVAL, or
 * -ENOSPC. */
int r300_rb2d_legalize_emit(const struct r300_rb2d_window *windows,
                            uint32_t window_count, uint32_t *words,
                            uint32_t capacity,
                            struct r300_rb2d_legalized_ib *out);

/* Dwords r300_rb2d_legalize_emit writes for a window list. */
bool r300_rb2d_legalize_dwords(const struct r300_rb2d_window *windows,
                               uint32_t window_count, uint32_t *dwords_out);

/* The byte set a window list touches, replayed onto a per-byte count of
 * a buffer of bo_size bytes.  Writes 1 for each covered byte and returns
 * false on any byte counted twice or any rectangle past the buffer.  This
 * is the coverage oracle and reads no legalizer state. */
bool r300_rb2d_windows_touched_bytes(const struct r300_rb2d_window *windows,
                                     uint32_t window_count,
                                     uint8_t *counts, uint64_t bo_size);

#endif /* R300_RB2D_LEGALIZE_H */
