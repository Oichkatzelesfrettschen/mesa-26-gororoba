/*
 * SPDX-License-Identifier: MIT
 *
 * ZMASK read-group discovery cell: one submission that asks which
 * ZB_BW_CNTL group makes a depth-test-only draw consume a ZMASK fast
 * clear on RS485M (Radeon Xpress 1150, CHIP_RS480, R300-class US/PFS
 * fixed VLIW).
 *
 * The surface is the 64x64 Z24S8 macro+micro-tiled level the public
 * lifecycle binds, at the same ZB_DEPTHOFFSET, with every storage word
 * seeded to one backing word.  One 3D_CLEAR_ZMASK covers the whole level
 * with ZB_DEPTHCLEARVALUE above the backing, and four read-only LESS
 * draws follow, each confined by SC_SCISSORS to one 32x32 quadrant --
 * sixteen whole 8x8 compression tiles -- and each under its own group:
 *
 *    quadrant  origin    ZB_BW_CNTL             depth code  question
 *    A         (0, 0)    0x00, 4x4 equations    0x100000    ordinary path
 *    B         (32, 0)   0x1c FF|RD_COMP|WR_COMP 0x400000   full group
 *    C         (0, 32)   0x04 FAST_FILL          0x400000   fast fill alone
 *    D         (32, 32)  0x0c FF|RD_COMP        0x400000   decompression group
 *
 * The near arm draws where the two candidate reference values disagree
 * in the pass direction: a B, C or D fragment at 0x400000 passes LESS
 * against the 0x800000 clear code and fails against the 0x200000
 * backing, and A at 0x100000 passes against the backing.  The far arm
 * draws where every candidate fails: B, C and D at 0xc00000 fail against
 * the clear code and the backing alike, and A at 0x300000 fails against
 * the backing, so a colored far quadrant names a test that did not gate
 * the write.  The two arms emit one stream shape and select their vertex
 * sets through the LOAD_VBPNTR offsets, so each arm has its own digest.  No draw writes depth, so the
 * cleared metadata and the backing both stay as seeded, and the four
 * quadrants are independent readings of one clear.
 *
 * B, C and D run inside the lifecycle's read wrapper at their own group:
 * the bind prefix at 8x8 plane equations, the draw, then the suffix that
 * flushes and frees the Z cache and restores ZB_BW_CNTL 0 with 4x4
 * plane equations, so no line cached under one group serves the next
 * draw.  A runs directly under that restored state.
 */

#ifndef R300_ZMASK_READ_QUADRANT_CELL_H
#define R300_ZMASK_READ_QUADRANT_CELL_H

#include "r300_zb_depth_layout.h"
#include "r300_zb_depth_surface.h"
#include "r300_zmask_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct r300_fragment_binary;
struct r300_first_draw_contract;

enum r300_zmask_read_quadrant_slot {
   R300_ZMASK_READ_QUADRANT_SLOT_VERTEX = 0,
   R300_ZMASK_READ_QUADRANT_SLOT_COLOR = 1,
   R300_ZMASK_READ_QUADRANT_SLOT_DEPTH = 2,
   R300_ZMASK_READ_QUADRANT_SLOT_COUNT = 3,
};

enum r300_zmask_read_quadrant {
   R300_ZMASK_READ_QUADRANT_A = 0,
   R300_ZMASK_READ_QUADRANT_B,
   R300_ZMASK_READ_QUADRANT_C,
   R300_ZMASK_READ_QUADRANT_D,
   R300_ZMASK_READ_QUADRANT_COUNT,
};

#define R300_ZMASK_READ_QUADRANT_TARGET_WIDTH 64u
#define R300_ZMASK_READ_QUADRANT_TARGET_HEIGHT 64u
#define R300_ZMASK_READ_QUADRANT_EXTENT 32u
#define R300_ZMASK_READ_QUADRANT_PITCH_PIXELS 64u
/* One row past the target, so a write beyond the scissored extent lands
 * in bytes the color oracle inspects. */
#define R300_ZMASK_READ_QUADRANT_COLOR_ROWS 65u
#define R300_ZMASK_READ_QUADRANT_COLOR_BYTES                                \
   (R300_ZMASK_READ_QUADRANT_PITCH_PIXELS *                                 \
    R300_ZMASK_READ_QUADRANT_COLOR_ROWS * 4u)

/* The lifecycle's depth codes and stencils, so the cell and the public
 * route read one experiment: the backing every storage word carries, and
 * the fast-clear word the ZMASK substitutes. */
#define R300_ZMASK_READ_QUADRANT_BACKING_DEPTH_CODE 0x200000u
#define R300_ZMASK_READ_QUADRANT_BACKING_STENCIL 0x5au
#define R300_ZMASK_READ_QUADRANT_CLEAR_DEPTH_CODE 0x800000u
#define R300_ZMASK_READ_QUADRANT_CLEAR_STENCIL 0x3cu
/* The lifecycle places the storage envelope 2048 bytes into its buffer
 * object, which is the ZB_DEPTHOFFSET its read draw executed under. */
#define R300_ZMASK_READ_QUADRANT_DEPTH_GUARD_BYTES 2048u
#define R300_ZMASK_READ_QUADRANT_DEPTH_GUARD_FILL 0xa3u

/* Six vertices per quadrant, two triangles covering the whole target at
 * the quadrant's depth; the scissor confines the write. */
#define R300_ZMASK_READ_QUADRANT_VERTEX_COUNT 6u
#define R300_ZMASK_READ_QUADRANT_VERTEX_STRIDE_BYTES 16u
#define R300_ZMASK_READ_QUADRANT_VERTEX_SET_BYTES                           \
   (R300_ZMASK_READ_QUADRANT_VERTEX_COUNT *                                 \
    R300_ZMASK_READ_QUADRANT_VERTEX_STRIDE_BYTES)
enum r300_zmask_read_quadrant_arm {
   R300_ZMASK_READ_QUADRANT_ARM_NEAR = 0,
   R300_ZMASK_READ_QUADRANT_ARM_FAR,
   R300_ZMASK_READ_QUADRANT_ARM_COUNT,
};

#define R300_ZMASK_READ_QUADRANT_VERTEX_BYTES                               \
   (R300_ZMASK_READ_QUADRANT_ARM_COUNT * R300_ZMASK_READ_QUADRANT_COUNT *    \
    R300_ZMASK_READ_QUADRANT_VERTEX_SET_BYTES)

struct r300_zmask_read_quadrant_draw {
   const char *name;
   uint32_t origin_x;
   uint32_t origin_y;
   /* The group the draw executes under.  wrapped says the draw runs
    * inside the bind prefix and suffix; A runs under the restored state
    * with neither. */
   uint32_t zb_bw_cntl;
   bool wrapped;
   /* Window-space z of the covering primitive and the depth code it
    * reaches, z * 2^24, per arm. */
   float z[R300_ZMASK_READ_QUADRANT_ARM_COUNT];
   uint32_t depth_code[R300_ZMASK_READ_QUADRANT_ARM_COUNT];
   /* PFS_PARAM_0 in FP24 (s1e7m16, 1.0 = 0x3f0000), and the B8G8R8A8
    * word the US_OUT_FMT_0 swizzle stores for it: x lands in bits 16-23,
    * y in 8-15, z in 0-7 and w in 24-31. */
   uint32_t constant[4];
   uint32_t color;
};

extern const struct r300_zmask_read_quadrant_draw
   r300_zmask_read_quadrant_draws[R300_ZMASK_READ_QUADRANT_COUNT];

/* The vertex sets in arm then quadrant order, each at its quadrant's z
 * for that arm; the set for (arm, quadrant) starts at
 * r300_zmask_read_quadrant_vertex_offset(arm, quadrant). */
extern const float r300_zmask_read_quadrant_vertices
   [R300_ZMASK_READ_QUADRANT_ARM_COUNT][R300_ZMASK_READ_QUADRANT_COUNT]
   [R300_ZMASK_READ_QUADRANT_VERTEX_COUNT * 4];

static inline uint32_t
r300_zmask_read_quadrant_vertex_offset(enum r300_zmask_read_quadrant_arm arm,
                                       uint32_t quadrant)
{
   return ((uint32_t)arm * R300_ZMASK_READ_QUADRANT_COUNT + quadrant) *
          R300_ZMASK_READ_QUADRANT_VERTEX_SET_BYTES;
}

const char *
r300_zmask_read_quadrant_arm_name(enum r300_zmask_read_quadrant_arm arm);

#define R300_ZMASK_READ_QUADRANT_COLOR_SENTINEL 0xa5a5a5a5u

struct r300_zmask_read_quadrant_params {
   enum r300_zmask_read_quadrant_arm arm;
   const struct r300_zb_depth_surface *surface;
   const struct r300_zmask_layout *zmask_layout;
   uint32_t depth_offset_bytes;
   uint32_t color_pitch_format;
   const struct r300_fragment_binary *fragment_binary;
   /* The full-extent contract; each draw narrows SC_SCISSORS after it. */
   const struct r300_first_draw_contract *first_draw_contract;
};

struct r300_zmask_read_quadrant_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
};

/* One depth binding, one color binding, one vertex binding per draw. */
#define R300_ZMASK_READ_QUADRANT_MAX_RELOC_SITES                            \
   (2u + R300_ZMASK_READ_QUADRANT_COUNT)
#define R300_ZMASK_READ_QUADRANT_MAX_DWORDS 1024u

struct r300_zmask_read_quadrant_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_zmask_read_quadrant_reloc_site
      reloc_sites[R300_ZMASK_READ_QUADRANT_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
   bool owns_ib;
};

/* The Z24 surface and ZMASK layout the lifecycle's 64x64 D24S8 image
 * resolves: r300_zb_depth_surface_rs485m_z24_macrotiled_logical and its
 * layout at the level's own compression block.  Returns 0 or a negative
 * errno. */
int r300_zmask_read_quadrant_surface(
   const struct r300_zb_depth_surface **surface_out,
   struct r300_zb_depth_layout *depth_layout_out,
   struct r300_zmask_layout *zmask_layout_out);

/* Bytes the depth buffer object holds: both guards and the envelope. */
uint64_t r300_zmask_read_quadrant_depth_bytes(void);

int r300_zmask_read_quadrant_emit(
   const struct r300_zmask_read_quadrant_params *params,
   struct r300_zmask_read_quadrant_ib *out);

/* The cell every consumer takes: the triangle cell's compiled
 * constant-color fragment binary, the full-extent contract, the linear
 * 64-pixel B8G8R8A8 color pitch, and the surface above at its guard
 * offset.  The caller releases the IB. */
int r300_zmask_read_quadrant_reference_emit(
   enum r300_zmask_read_quadrant_arm arm,
   struct r300_zmask_read_quadrant_ib *out);

void r300_zmask_read_quadrant_release(struct r300_zmask_read_quadrant_ib *ib);

/* One site per color and depth slot, one vertex site per draw, in
 * rising stream order, each naming the slot its payload selects. */
int r300_zmask_read_quadrant_validate_reloc_sites(
   const struct r300_zmask_read_quadrant_ib *ib);

/* The state standing at each draw, read out of the stream. */
struct r300_zmask_read_quadrant_draw_state {
   uint32_t zb_bw_cntl;
   uint32_t gb_z_peq_config;
   uint32_t zb_cntl;
   uint32_t zb_zstencilcntl;
   uint32_t zb_depthclearvalue;
   uint32_t zb_depthoffset;
   uint32_t zb_depthpitch;
   uint32_t zb_zmask_pitch;
   uint32_t sc_scissors_tl;
   uint32_t sc_scissors_br;
   uint32_t pfs_param_0[4];
   uint32_t vertex_offset;
   /* ZB_ZCACHE_CTLSTAT writes of flush-and-free since the previous draw
    * (or the clear, for the first draw). */
   uint32_t zcache_flushes_before;
};

struct r300_zmask_read_quadrant_stream_state {
   uint32_t zmask_clears;
   uint32_t zmask_clear_payload[3];
   uint32_t draw_count;
   struct r300_zmask_read_quadrant_draw_state
      draws[R300_ZMASK_READ_QUADRANT_COUNT];
   /* The group and plane equations the stream leaves behind. */
   uint32_t final_zb_bw_cntl;
   uint32_t final_gb_z_peq_config;
};

/* Walks PACKET0 writes and the draw, clear and vertex-pointer packets.
 * Returns 0, or -EINVAL for a malformed stream or more draws than the
 * cell carries. */
int r300_zmask_read_quadrant_read_state(
   const uint32_t *ib, uint32_t dwords,
   struct r300_zmask_read_quadrant_stream_state *out);

/* Holds a stream to the experiment: one whole-level 3D_CLEAR_ZMASK,
 * exactly four draws, each at its table group, plane equations, quadrant
 * scissor, constant, the declared arm's vertex set, LESS test with no
 * depth write, the
 * clear word and the depth binding, at least one Z-cache flush ahead of
 * every draw, and a stream that ends with compression disabled.  Returns
 * 0 or -EINVAL. */
int r300_zmask_read_quadrant_check_state(
   const struct r300_zmask_read_quadrant_params *params, const uint32_t *ib,
   uint32_t dwords);

/* Seeds a depth allocation: guard fill outside the envelope and the
 * packed backing word in every envelope slot.  Returns 0 or -EINVAL. */
int r300_zmask_read_quadrant_fill_depth(
   const struct r300_zb_depth_layout *layout, uint8_t *bytes,
   uint64_t size);

/* Seeds the color allocation with the sentinel. */
void r300_zmask_read_quadrant_fill_color(uint8_t *bytes, uint64_t size);

enum r300_zmask_read_quadrant_reading {
   /* Every pixel of the quadrant holds the draw's color. */
   R300_ZMASK_READ_QUADRANT_COLORED = 0,
   /* Every pixel still holds the sentinel. */
   R300_ZMASK_READ_QUADRANT_SENTINEL,
   /* Anything else: a partial or foreign result, itself a finding. */
   R300_ZMASK_READ_QUADRANT_MIXED,
};

const char *r300_zmask_read_quadrant_reading_name(
   enum r300_zmask_read_quadrant_reading reading);

struct r300_zmask_read_quadrant_color_observation {
   bool judged;
   uint32_t colored[R300_ZMASK_READ_QUADRANT_COUNT];
   uint32_t sentinel[R300_ZMASK_READ_QUADRANT_COUNT];
   uint32_t foreign[R300_ZMASK_READ_QUADRANT_COUNT];
   enum r300_zmask_read_quadrant_reading
      reading[R300_ZMASK_READ_QUADRANT_COUNT];
   /* Pixels past the target (the guard row) that left the sentinel. */
   uint32_t guard_changed;
};

void r300_zmask_read_quadrant_observe_color(
   const uint8_t *bytes, uint64_t size,
   struct r300_zmask_read_quadrant_color_observation *out);

/* Bytes of the depth allocation that differ between two images. */
uint64_t r300_zmask_read_quadrant_depth_changed_bytes(const uint8_t *before,
                                                      const uint8_t *after,
                                                      uint64_t size);

#endif /* R300_ZMASK_READ_QUADRANT_CELL_H */
