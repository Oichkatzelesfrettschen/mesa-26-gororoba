/*
 * SPDX-License-Identifier: MIT
 *
 * Neutral first-draw state contract for R300-class TCL-bypass rendering.
 */

#ifndef R300_FIRST_DRAW_STATE_H
#define R300_FIRST_DRAW_STATE_H

#include "amd_family.h"

#include <stdbool.h>
#include <stdint.h>

/* A first draw on a fresh context inherits whatever register values the
 * previous client left behind. On RS482, US_OUT_FMT_0 = UNUSED,
 * RB3D_COLOR_CHANNEL_MASK = 0, and SC_SCREENDOOR = 0 each alone suppress
 * every color write and produce byte-identical unwritten targets, so a
 * command stream that does not establish this state itself renders only by
 * inheritance. The contract enumerates the registers a verified-rendering
 * r300g triangle establishes before its draw, assigns each a semantic
 * disposition, and emits the subset a self-contained draw must own in
 * pipeline order.
 */

/* Why each register is, or is not, part of the emitted contract. */
enum r300_first_draw_disposition {
   /* Silicon-proven color-write gate: absence alone blanks the target. */
   R300_FDS_PROVEN_GATE,
   /* State a first draw must establish for its result to be
    * context-independent.
    */
   R300_FDS_REQUIRED_INVARIANT,
   /* A feature the draw does not use, written to its disabled value. */
   R300_FDS_EXPLICIT_DISABLE,
   /* Geometry or raster parameter derived from the draw's own inputs. */
   R300_FDS_GEOMETRY_PARAMETER,
   /* Derived from the bound resource's layout (pitch, tiling, format). */
   R300_FDS_RESOURCE_LAYOUT,
   /* Value depends on the chip's pipe/PVS configuration. */
   R300_FDS_CHIP_DERIVED,
   /* Ordering or cache-domain barrier, position-sensitive. */
   R300_FDS_ORDERING_BARRIER,
   /* Written by the r300g reference for a resource or feature the neutral
    * cell does not bind; copying it would import a nonexistent binding, so
    * it stays out of the emission.
    */
   R300_FDS_REFERENCE_ARTIFACT,
};

struct r300_first_draw_contract_entry {
   uint16_t reg;
   uint32_t value;
   enum r300_first_draw_disposition disposition;
   const char *name;
};

struct r300_first_draw_params {
   /* Mesa chip family. The contract accepts CHIP_RS480, which identifies the
    * RS482/RS485 TCL-bypass path that owns these register values.
    */
   enum radeon_family chip_family;
   /* Render-target extent in pixels; scissor and clip derive from it. */
   uint32_t width;
   uint32_t height;
   /* Highest vertex index the draw fetches. */
   uint32_t max_vtx_index;
   /* The draw binds no texture, so the TX block is explicitly disabled;
    * a texturing caller owns its own TX state on top of the contract.
    */
   bool texture_enabled;
};

/* The resolved contract: every entry the parameters select, in the order
 * the emitter writes them.
 */
struct r300_first_draw_contract {
   struct r300_first_draw_contract_entry entries[96];
   uint32_t count;
};

/* Resolves the contract for the given parameters. Entries carry final
 * register values; parameter-derived entries (scissor, clip, max index)
 * are computed here so the emitter and the checker share one authority.
 * Returns 0, -ENOTSUP for a chip family outside CHIP_RS480, or a negative
 * errno for out-of-range parameters.
 */
int r300_first_draw_contract_resolve(const struct r300_first_draw_params *params,
                                     struct r300_first_draw_contract *out);

/* Appends the contract's register writes to ib, in pipeline order:
 * ordering barriers and cache flushes first, unpipelined raster and
 * backend state, scissor and clip, VAP and RS linkage, pipelined US
 * output format, texture disable last before the caller's fragment
 * program and draw. Returns the number of dwords written, or a negative
 * errno when max_dwords cannot hold the emission.
 */
int r300_first_draw_state_emit(const struct r300_first_draw_contract *contract,
                               uint32_t *ib, uint32_t max_dwords);

/* The emission is one single-register PACKET0 per entry -- a header and a
 * payload -- so the stream is exactly twice the entry count.  A caller
 * reserving room for the emission takes the size from here, so the emitter
 * stays the one authority over its own extent.
 */
static inline uint32_t
r300_first_draw_state_dwords(const struct r300_first_draw_contract *contract)
{
   return contract->count * 2;
}

/* Poison-model checker: applies the command stream over an arbitrary
 * predecessor register state and reports every contract clause the final
 * state leaves unsatisfied. Ordering-barrier clauses require their writes
 * before a recognized draw packet. The report is the complete set because the
 * open gates are indistinguishable on silicon -- US_OUT_FMT_0 UNUSED, a
 * zero color channel mask, and a zero screendoor each alone produce the
 * same byte-identical unwritten target -- so only the full set tells the
 * caller which writes remain missing.
 */
struct r300_first_draw_check_report {
   /* Indices into the contract's entries, one per unsatisfied clause. */
   uint32_t unsatisfied[96];
   uint32_t unsatisfied_count;
};

/* Seeds every contract register with `poison`, replays the PACKET0 writes
 * of ib over the seed, and records each contract entry whose final value
 * differs from the contract. Returns the unsatisfied count.
 */
uint32_t
r300_first_draw_state_check(const struct r300_first_draw_contract *contract,
                            const uint32_t *ib, uint32_t ib_dwords,
                            uint32_t poison,
                            struct r300_first_draw_check_report *report);

#endif /* R300_FIRST_DRAW_STATE_H */
