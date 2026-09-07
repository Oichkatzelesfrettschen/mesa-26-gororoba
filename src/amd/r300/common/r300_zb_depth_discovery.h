/*
 * SPDX-License-Identifier: MIT
 *
 * Depth address discovery: the declared experiment that writes one
 * logical pixel, and the oracle that finds which physical byte moved.
 *
 * The depth control cell reads a surface it can address, so its oracle
 * asks a resolver where a coordinate lives and inspects that byte.  A
 * tiled surface has no resolver in this tree, which is the fact this
 * apparatus exists to change: it initializes the whole allocation to one
 * repeated word, draws a single logical pixel, reads every byte back,
 * and reports which ones moved.  The scan carries no resolver, because a
 * resolver asked where to look would then be confirmed by the answer it
 * supplied.
 *
 * A constant initial image is what makes a tiled surface observable
 * without a transform.  Tiling permutes complete pixels, and a constant
 * image is invariant under a permutation, so a host that can map the
 * allocation and fill it uniformly holds both halves of the experiment
 * whether or not it can compute an address.  The scenario therefore
 * demands raw_allocation_mapping and uniform_packed_initialization and
 * nothing more; logical_pixel_addressing and logical_image_readback stay
 * out of the admission, since both tiled surfaces answer false to them
 * and requiring either would refuse the campaign this apparatus serves.
 *
 * Three byte classes partition the allocation, and the third is not
 * decoration.  The campaign holds one allocation size across every
 * tiling mode so the transport, the queue predicate, and the retained
 * artifact do not move between rungs, while the storage envelope does
 * move: 16640 bytes linear, 16896 microtiled, 20480 microtiled and
 * macrotiled.  With a 2048-byte guard on each side the layout spans
 * 20736, 20992, and 24576 bytes, so a constant 24576-byte allocation
 * leaves 3840 bytes past the linear suffix guard and 3584 past the
 * microtiled one.  Those bytes are neither guard nor storage.  Counting
 * them as storage would put slack into the denominator and attribute a
 * change in them to a discovered depth address; counting them as guard
 * would claim a guard verdict the layout never stated.  They are
 * inspected, counted, and reported as unclaimed, and a single-pixel
 * observation requires zero changed bytes in them.
 *
 * The experiment's identity is not its command stream.  A stencil seed
 * reaches the device through the host's initial fill rather than through
 * any IB dword, so the seed-A and seed-B runs emit byte-identical
 * streams and an IB digest cannot separate them.  The scenario digest
 * covers the declared coordinate, marker, seed, surface, and envelope,
 * so a retained artifact names the experiment that ran.
 */

#ifndef R300_ZB_DEPTH_DISCOVERY_H
#define R300_ZB_DEPTH_DISCOVERY_H

#include "r300_zb_depth_layout.h"
#include "r300_zb_depth_surface.h"

#include <stdbool.h>
#include <stdint.h>

/* Bytes the campaign allocates for every depth surface it discovers,
 * chosen so the largest admitted envelope -- microtiled and macrotiled,
 * 20480 bytes -- fits between two 2048-byte guards. */
#define R300_ZB_DISCOVERY_ALLOCATION_BYTES 24576u
/* Guard bytes on each side.  One macrotile, so a guard is never a
 * partial tile and the storage envelope begins on a macrotile
 * boundary. */
#define R300_ZB_DISCOVERY_GUARD_BYTES 2048u

/* The byte a host writes across the guard ranges.  It is not a packed
 * depth word: a guard carries no pixel, and a value distinct from every
 * initial storage word keeps a guard byte recognizable in a raw dump. */
#define R300_ZB_DISCOVERY_GUARD_FILL 0xa3u

/* One declared discovery experiment.  Every field is stated before the
 * run and retained with its result, so the observation is read against
 * the experiment that was declared rather than against the stream that
 * happened to be emitted. */
struct r300_zb_depth_discovery_scenario {
   const char *name;
   const struct r300_zb_depth_surface *surface;
   /* The one logical pixel the scissor admits.  The oracle reports it
    * back for retention and never uses it to choose where to look. */
   uint32_t pixel_x;
   uint32_t pixel_y;
   /* Depth code every storage slot carries before the draw. */
   uint32_t initial_depth_code;
   /* Stencil byte every storage slot carries before the draw.  A
    * 16-bit surface stores none and admits zero alone. */
   uint32_t initial_stencil;
   /* The depth code the draw writes, distinct from initial_depth_code so
    * a changed slot is a written slot.  The fragment's window-space
    * depth produces it, and the run records what actually landed rather
    * than asserting this value. */
   uint32_t marker_depth_code;
   /* Bytes the host allocates and maps. */
   uint64_t allocation_bytes;
   /* Guard bytes on each side of the storage envelope. */
   uint32_t guard_bytes;
};

/* The linear rung: the Z24 surface whose byte for a coordinate is known
 * in closed form, so a discovery run over it is checked against an
 * answer the apparatus did not produce.  Stencil seed zero. */
extern const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_linear;

/* The two stencil-seed rungs on that same surface.  A zero-initialized
 * stencil cannot separate "the depth write preserved the stencil byte"
 * from "the depth write replaced it with zero", so each seed is bound
 * into its own experiment and the pair separates the two. */
extern const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_linear_seed_5a;
extern const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_linear_seed_a5;

/* The tiled rungs, where a logical coordinate and a physical byte first
 * separate. */
extern const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_microtiled;
extern const struct r300_zb_depth_discovery_scenario
   r300_zb_depth_discovery_z24_macrotiled;

/* Holds a scenario to what the surface, the allocation, and the packing
 * admit: a surface that passes its own check and offers raw mapping and
 * uniform packed initialization, a coordinate inside the render extent,
 * an initial pair and a marker that both pack under the format, a
 * marker distinct from the initial code, a guard that is a multiple of
 * the layout's base alignment, and an allocation large enough for both
 * guards and the storage envelope.  Returns 0 or -EINVAL. */
int r300_zb_depth_discovery_scenario_check(
   const struct r300_zb_depth_discovery_scenario *scenario);

/* The scenario's layout: r300_zb_depth_layout_compute at its surface and
 * guard.  Returns 0, or the refusal the scenario check or the layout
 * computation produced. */
int r300_zb_depth_discovery_layout(
   const struct r300_zb_depth_discovery_scenario *scenario,
   struct r300_zb_depth_layout *out);

/* Holds a layout to the mapping it claims to describe, independently of
 * the helper that produced it: the prefix ends where the envelope
 * begins, the envelope ends where the suffix begins, no interval
 * overlaps another or wraps, the envelope is a whole number of packed
 * slots, its base sits on the layout's stated alignment, and every
 * interval lies inside size_bytes.  Returns 0 or -EINVAL.
 *
 * A discovery run calls this before classifying a single byte.  The
 * guard predicate answers questions about the layout it is handed, so a
 * layout that does not describe the allocation would produce confident
 * classifications of the wrong bytes. */
int r300_zb_depth_discovery_layout_validate(
   const struct r300_zb_depth_layout *layout, uint64_t size_bytes);

/* The packed word every storage slot carries before the draw:
 * initial_depth_code against initial_stencil under the surface's
 * format.  Returns 0 or -EINVAL. */
int r300_zb_depth_discovery_initial_word(
   const struct r300_zb_depth_discovery_scenario *scenario,
   uint32_t *word_out);

/* Which class one allocation byte falls in. */
enum r300_zb_discovery_region {
   /* Inside one of the layout's two guard ranges. */
   R300_ZB_DISCOVERY_REGION_GUARD = 0,
   /* Inside the storage envelope. */
   R300_ZB_DISCOVERY_REGION_STORAGE = 1,
   /* Inside the allocation and outside both: the slack a constant
    * allocation leaves past a smaller envelope. */
   R300_ZB_DISCOVERY_REGION_UNCLAIMED = 2,
};

enum r300_zb_discovery_region r300_zb_depth_discovery_region_of(
   const struct r300_zb_depth_layout *layout, uint64_t size_bytes,
   uint64_t byte_offset);

/* One storage slot whose packed word moved.  Both components are
 * reported for both states, so a stencil-only change is never read as a
 * discovered depth address and a depth change is never discarded
 * because the stencil moved with it. */
struct r300_zb_discovery_slot_change {
   /* Byte offset inside the allocation, not inside the envelope: a
    * retained observation is compared against an address model that
    * addresses the buffer object. */
   uint64_t byte_offset;
   uint32_t before_word;
   uint32_t after_word;
   uint32_t before_depth;
   uint32_t after_depth;
   uint32_t before_stencil;
   uint32_t after_stencil;
   bool depth_changed;
   bool stencil_changed;
};

/* Changed slots one observation retains.  A single-pixel experiment
 * expects one; the capacity carries a wide margin so an unexpected
 * population is reported as data rather than truncated to the expected
 * answer, and an experiment beyond it sets the overflow flag while the
 * counts stay exact. */
#define R300_ZB_DISCOVERY_MAX_CHANGES 64u

struct r300_zb_discovery_observation {
   /* The oracle read both images and classified every byte.  A cleared
    * flag means no byte was judged, so every count below is zero
    * because nothing was inspected rather than because nothing
    * changed. */
   bool judged;
   /* The declared experiment, carried through for retention. */
   uint32_t pixel_x;
   uint32_t pixel_y;
   /* Storage slots inspected, and the four disjoint classes they fall
    * in.  The four sum to slots_inspected exactly. */
   uint32_t slots_inspected;
   uint32_t slots_unchanged;
   uint32_t slots_depth_only;
   uint32_t slots_stencil_only;
   uint32_t slots_both;
   /* Slots whose depth component moved: depth_only + both.  This is the
    * count of depth locations the draw wrote, and a single-pixel
    * experiment requires exactly one. */
   uint32_t depth_locations;
   /* Guard and unclaimed bytes, inspected and changed.  Both are judged
    * as bytes rather than as slots, because neither carries a pixel and
    * neither has a packing to read. */
   uint64_t guard_bytes_inspected;
   uint64_t guard_bytes_changed;
   uint64_t unclaimed_bytes_inspected;
   uint64_t unclaimed_bytes_changed;
   struct r300_zb_discovery_slot_change changes[R300_ZB_DISCOVERY_MAX_CHANGES];
   uint32_t change_count;
   bool change_overflow;
};

/* Classifies every byte of the allocation between the two images.
 *
 * before and after are the raw mapping as the host read it, each
 * size_bytes long; layout describes the same allocation and is validated
 * through r300_zb_depth_discovery_layout_validate before a byte is
 * classified.  A null argument, a size disagreement, or a layout that
 * does not describe the mapping yields the zeroed observation with
 * judged clear.
 *
 * The scan visits every storage slot in offset order and reports each
 * changed one with its own byte offset, whatever that offset is.  A
 * write outside a row-major position is a valid observation here, and
 * comparing an observed offset against an address model is the separate
 * job of a validator that owns the model. */
void r300_zb_depth_discovery_observe(
   const struct r300_zb_depth_discovery_scenario *scenario,
   const struct r300_zb_depth_layout *layout, const void *before,
   const void *after, uint64_t size_bytes,
   struct r300_zb_discovery_observation *out);

/* Color verdict over the linear color attachment, the logical-coordinate
 * oracle the depth scan cannot supply.
 *
 * The expected mask comes from the declared rectangle alone.  Decoding
 * the emitted scissor and accepting whatever region it names would
 * confirm the emitter against itself; a run whose scissor reached a
 * different pixel than the one declared must fail here, which is what
 * makes this an independent check on the depth observation's coordinate.
 */
struct r300_zb_discovery_color_verdict {
   bool judged;
   /* Pixels inside the declared rectangle, and how many carried the
    * draw color. */
   uint32_t inside_samples;
   uint32_t inside_colored;
   /* Pixels inside the render extent and outside the rectangle, and how
    * many carried the draw color.  A correct run reports zero. */
   uint32_t outside_samples;
   uint32_t outside_colored;
   /* Pixels of the allocation outside the render extent -- the padding
    * band inside each row's pitch and every row past the extent -- and
    * how many left the sentinel.  A correct run reports zero: the
    * scissor confines the write to one pixel, so a changed byte out here
    * is a write past the target rather than a wrong pixel inside it. */
   uint32_t beyond_samples;
   uint32_t beyond_changed;
   /* Every declared pixel colored, no other in-extent pixel touched, and
    * nothing beyond the extent changed. */
   bool exact;
};

/* pixels is the color allocation as a row-major B8G8R8A8 image of
 * pitch_pixels by rows; sentinel is the word the host filled it with and
 * draw_color the word the fragment program writes.  width and height
 * bound the render extent, and the rectangle is stated in the same
 * coordinates.  A rectangle outside the extent, a buffer too short, or a
 * null argument yields the zeroed verdict. */
void r300_zb_depth_discovery_color_observe(
   const uint32_t *pixels, uint64_t size_bytes, uint32_t pitch_pixels,
   uint32_t width, uint32_t height, uint32_t rect_x, uint32_t rect_y,
   uint32_t rect_width, uint32_t rect_height, uint32_t sentinel,
   uint32_t draw_color, struct r300_zb_discovery_color_verdict *out);

#endif /* R300_ZB_DEPTH_DISCOVERY_H */
