/*
 * SPDX-License-Identifier: MIT
 *
 * Address-discovery cell: one covering primitive confined by the
 * scissor to a single logical pixel, over a depth surface whose
 * physical byte for that pixel is what the run establishes.
 *
 * The geometry covers the whole target and the scissor confines the
 * write, rather than the other way round.  A one-pixel triangle would
 * make coverage of the sample a question about the fill rule at three
 * edges; a covering primitive puts the sample strictly inside one
 * triangle, so the confinement is a rectangle the contract states and
 * the color attachment reads back.
 *
 * The comparison is ALWAYS with depth writes enabled, so the depth
 * result is a write rather than a test outcome: this cell measures where
 * a write lands, and the ordinary depth control measures whether the
 * test gates it.  Two controls come off the same emitter -- depth writes
 * disabled under ALWAYS, which must move color and no depth code, and
 * comparison NEVER, which must move nothing -- so a run that finds an
 * address and a run that finds none are the same apparatus at two
 * parameter values.
 *
 * Compression stays off in a way the emitted stream carries rather than
 * the contract table alone.  r300_zb_depth_state_emit writes ZB_BW_CNTL
 * after the contract does, so the executing value is
 * R300_HIZ_DISABLE | R300_FAST_FILL_DISABLE, which composes to zero:
 * RD_COMP, WR_COMP, HiZ, fast fill, and ZB_CB_CLEAR all clear.
 * ZB_CB_CLEAR matters here beyond compression -- the R3xx reference
 * describes that mode as cache-line-granular write-only operation, which
 * leaves the untouched portion of a partially written microtile unknown,
 * and an unknown byte inside the envelope is exactly what a one-pixel
 * address experiment cannot carry.  GB_Z_PEQ_CONFIG stays at the
 * contract's zero, R300_GB_Z_PEQ_CONFIG_Z_PEQ_SIZE_4_4, the plane
 * equation configuration the R5xx acceleration guide requires while
 * compression is disabled; nothing after the contract writes 0x4028.
 * r300_zb_depth_discovery_check_state holds a stream to all of it.
 */

#ifndef R300_ZB_DEPTH_DISCOVERY_CELL_H
#define R300_ZB_DEPTH_DISCOVERY_CELL_H

#include "r300_zb_depth_discovery.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

struct r300_fragment_binary;
struct r300_first_draw_contract;

/* BO slots the cell references; the transport binds slot order to the
 * relocation-list order at submission. */
enum r300_zb_depth_discovery_slot {
   R300_ZB_DISCOVERY_SLOT_VERTEX = 0,
   R300_ZB_DISCOVERY_SLOT_COLOR = 1,
   R300_ZB_DISCOVERY_SLOT_DEPTH = 2,
   R300_ZB_DISCOVERY_SLOT_COUNT = 3,
};

/* Render extent and color allocation, the same 64x64 target the depth
 * ladder draws so the color attachment's row-major readback is the
 * arithmetic that already holds. */
#define R300_ZB_DISCOVERY_TARGET_WIDTH 64u
#define R300_ZB_DISCOVERY_TARGET_HEIGHT 64u
#define R300_ZB_DISCOVERY_PITCH_PIXELS 64u
#define R300_ZB_DISCOVERY_COLOR_ROWS 65u
#define R300_ZB_DISCOVERY_COLOR_BYTES \
   (R300_ZB_DISCOVERY_PITCH_PIXELS * R300_ZB_DISCOVERY_COLOR_ROWS * 4u)

/* Window-space depth of the covering primitive.  R300 stores a 24-bit
 * depth code, so 0.25 reaches memory as 0x00400000, the marker every
 * discovery scenario declares. */
#define R300_ZB_DISCOVERY_MARKER_Z 0.25f

/* Two triangles splitting the target along x + y = 64, walked as one
 * list.  Every pixel center of the extent falls strictly inside one of
 * them or on the shared diagonal; the campaign coordinate (37, 21) sums
 * to 59 at its center, strictly inside the first triangle, so its
 * coverage does not rest on the fill rule at any edge. */
#define R300_ZB_DISCOVERY_VERTEX_DWORDS 24
extern const float
   r300_zb_depth_discovery_vertices[R300_ZB_DISCOVERY_VERTEX_DWORDS];

struct r300_zb_depth_discovery_params {
   /* The declared experiment: surface, pixel, marker, seed. */
   const struct r300_zb_depth_discovery_scenario *scenario;
   uint32_t vertex_offset;
   uint32_t color_pitch_format;
   uint32_t depth_offset_bytes;
   /* Depth comparison, one of R300_ZS_NEVER through R300_ZS_ALWAYS.  The
    * discovery arm takes ALWAYS; the NEVER control takes NEVER. */
   uint32_t depth_function;
   /* Z_WRITE_ENABLE.  The discovery arm sets it; the depth-writes-
    * disabled control clears it. */
   bool depth_write;
   const struct r300_fragment_binary *fragment_binary;
   /* The contract owns the scissor that confines the draw, so the
    * confinement the poison checker verifies and the confinement the
    * cell emits are one value.  A null contract is refused. */
   const struct r300_first_draw_contract *first_draw_contract;
};

struct r300_zb_depth_discovery_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

#define R300_ZB_DISCOVERY_MAX_RELOC_SITES R300_ZB_DISCOVERY_SLOT_COUNT
static_assert(R300_ZB_DISCOVERY_SLOT_COUNT <= 32,
              "slot uniqueness is proven in a 32-bit mask");

struct r300_zb_depth_discovery_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_zb_depth_discovery_reloc_site
      reloc_sites[R300_ZB_DISCOVERY_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   bool owns_ib;
};

#define R300_ZB_DISCOVERY_MAX_DWORDS 512

/* Emits the cell into caller storage of exactly capacity dwords, or
 * refuses with -ENOSPC the moment an operation would pass that bound.
 * Returns 0 or a negative errno. */
int r300_zb_depth_discovery_emit_into(
   const struct r300_zb_depth_discovery_params *params, uint32_t *words,
   uint32_t capacity, struct r300_zb_depth_discovery_ib *out);

/* The same emission into an allocation this call owns and the caller
 * frees through r300_zb_depth_discovery_release. */
int r300_zb_depth_discovery_emit(
   const struct r300_zb_depth_discovery_params *params,
   struct r300_zb_depth_discovery_ib *out);

void r300_zb_depth_discovery_release(struct r300_zb_depth_discovery_ib *ib);

/* Resolves the contract for the 64x64 target and six vertices with the
 * texture block disabled, places the B8G8R8A8 output format, and narrows
 * SC_SCISSORS to the scenario's single pixel.  SC_CLIPRECT keeps the full
 * extent: the scissor is the register that confines the write, and a
 * clip rectangle narrower than it would make two registers answer the
 * same question.  Returns 0 or a negative errno. */
int r300_zb_depth_discovery_reference_contract(
   const struct r300_zb_depth_discovery_scenario *scenario,
   struct r300_first_draw_contract *out);

/* Emits the complete reference cell for one scenario and one control
 * arm: the triangle cell's compiled constant-color fragment binary, the
 * reference contract with the scenario's one-pixel scissor, vertex
 * offset zero, depth offset zero, linear 64-pixel B8G8R8A8 pitch.  Every
 * pre-hardware consumer takes its cell from here, so the discovery arm
 * and its two controls differ in the comparison and the write enable
 * alone.  Returns 0 or a negative errno; the caller owns the returned
 * IB.
 */
int r300_zb_depth_discovery_reference_emit(
   const struct r300_zb_depth_discovery_scenario *scenario,
   uint32_t depth_function, bool depth_write,
   struct r300_zb_depth_discovery_ib *out);

/* Checks the emitted relocation sites against the stream they index: one
 * site per slot in stream order, each inside the stream, and each naming
 * the slot whose payload sits at that index.  Returns 0 or a negative
 * errno. */
int r300_zb_depth_discovery_validate_reloc_sites(
   const struct r300_zb_depth_discovery_ib *ib);

/* The state the discovery run executes under, read back out of the
 * stream rather than asserted from the contract table.
 *
 * ZB_FORMAT is counted rather than found.  A grammar that writes the
 * format once makes the last write the effective one by construction,
 * and a checker that refuses a second write keeps that property a
 * property of the stream instead of a convention the reader has to
 * trust.  The ordinary format reader returns the first write it finds
 * and keeps that contract; this counts. */
struct r300_zb_depth_discovery_state {
   uint32_t zb_format_writes;
   uint32_t zb_format;
   uint32_t zb_cntl;
   uint32_t zb_zstencilcntl;
   uint32_t zb_bw_cntl;
   uint32_t gb_z_peq_config;
   uint32_t sc_scissors_tl;
   uint32_t sc_scissors_br;
   uint32_t sc_cliprect_tl;
   uint32_t sc_cliprect_br;
   uint32_t sc_screendoor;
};

/* Replays the stream's PACKET0 writes and reports the last value each
 * register received.  Returns 0, or -EINVAL for a null argument or a
 * malformed packet.  A register the stream never writes reports zero
 * with its write count, where one exists, left at zero. */
int r300_zb_depth_discovery_read_state(
   const uint32_t *ib, uint32_t dwords,
   struct r300_zb_depth_discovery_state *out);

/* Holds a stream to the state a discovery run requires: exactly one
 * ZB_FORMAT write carrying the scenario's format, Z_ENABLE set and
 * STENCIL_ENABLE clear,
 * Z_WRITE_ENABLE matching depth_write, the declared comparison,
 * ZB_BW_CNTL zero so HiZ, fast fill, read and write compression, and
 * ZB_CB_CLEAR are all clear, GB_Z_PEQ_CONFIG zero so the plane equations
 * are 4x4, SC_SCREENDOOR at its open value, the scissor at the declared
 * pixel, and a clip rectangle no narrower than that scissor.  Returns 0
 * or -EINVAL. */
int r300_zb_depth_discovery_check_state(
   const struct r300_zb_depth_discovery_params *params, const uint32_t *ib,
   uint32_t dwords);

#endif /* R300_ZB_DEPTH_DISCOVERY_CELL_H */
