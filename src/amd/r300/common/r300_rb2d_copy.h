/* SPDX-License-Identifier: MIT */

#ifndef R300_RB2D_COPY_H
#define R300_RB2D_COPY_H

#include <stdbool.h>
#include <stdint.h>

#define R300_RB2D_COPY_CARRIER_PITCH_BYTES 256u
#define R300_RB2D_COPY_BYTES_PER_PIXEL 4u
#define R300_RB2D_COPY_MAX_SEGMENTS 4u
#define R300_RB2D_COPY_RELOCS_PER_SEGMENT 2u
#define R300_RB2D_COPY_MAX_RELOC_SITES \
   (R300_RB2D_COPY_MAX_SEGMENTS * R300_RB2D_COPY_RELOCS_PER_SEGMENT)

enum r300_rb2d_copy_slot_role {
   R300_RB2D_COPY_SLOT_SOURCE = 0,
   R300_RB2D_COPY_SLOT_DESTINATION,
};

struct r300_rb2d_copy_segment {
   uint64_t source_offset_bytes;
   uint64_t destination_offset_bytes;
   uint32_t byte_count;
};

struct r300_rb2d_copy_plan {
   uint64_t source_buffer_bytes;
   uint64_t destination_buffer_bytes;
   bool same_buffer;
   const struct r300_rb2d_copy_segment *segments;
   uint32_t segment_count;
   bool byte_carrier;
};

struct r300_rb2d_copy_reloc_site {
   uint32_t ib_index;
   uint32_t slot;
   enum r300_rb2d_copy_slot_role role;
};

struct r300_rb2d_copy_ib {
   uint32_t *ib;
   uint32_t ib_size_dwords;
   struct r300_rb2d_copy_reloc_site
      reloc_sites[R300_RB2D_COPY_MAX_RELOC_SITES];
   uint32_t reloc_site_count;
};

struct r300_zb_tile_copy_plan;

enum r300_rb2d_copy_refusal {
   R300_RB2D_COPY_OK = 0,
   R300_RB2D_COPY_REFUSE_PLAN_NULL,
   R300_RB2D_COPY_REFUSE_SEGMENT_COUNT,
   R300_RB2D_COPY_REFUSE_SEGMENT_SIZE,
   R300_RB2D_COPY_REFUSE_OFFSET_ALIGNMENT,
   R300_RB2D_COPY_REFUSE_ADDRESS_WIDTH,
   R300_RB2D_COPY_REFUSE_SOURCE_BOUNDS,
   R300_RB2D_COPY_REFUSE_DESTINATION_BOUNDS,
   R300_RB2D_COPY_REFUSE_SOURCE_OVERLAP,
   R300_RB2D_COPY_REFUSE_DESTINATION_OVERLAP,
   R300_RB2D_COPY_REFUSE_COPY_OVERLAP,
   R300_RB2D_COPY_REFUSAL_COUNT,
};

const char *r300_rb2d_copy_refusal_name(enum r300_rb2d_copy_refusal refusal);

enum r300_rb2d_copy_refusal
r300_rb2d_copy_plan_check(const struct r300_rb2d_copy_plan *plan);

/* Converts a parity-resolved depth-tile plan into the checked RB2D copy
 * vocabulary.  The caller supplies four segment slots and owns their
 * lifetime.  Refusal preserves both the segment storage and output plan.
 */
enum r300_rb2d_copy_refusal r300_rb2d_copy_plan_from_zb_tile(
   const struct r300_zb_tile_copy_plan *tile_plan,
   uint64_t source_buffer_bytes, uint64_t destination_buffer_bytes,
   bool same_buffer,
   struct r300_rb2d_copy_segment
      segment_storage[R300_RB2D_COPY_MAX_SEGMENTS],
   struct r300_rb2d_copy_plan *out);

#define R300_RB2D_COPY_DWORDS(segment_count) \
   (18u + 14u * (uint32_t)(segment_count))

/* Emits one complete, standalone memory-source PACKET0 stream at words[0],
 * replacing out on success alone.  Relocation slots start at zero and
 * alternate SOURCE then DESTINATION for each segment; callers that append
 * the stream must rebase both IB indices and relocation slots.
 *
 * The source
 * and destination offsets are represented as independent 1 KiB-aligned
 * bases plus coordinates on the silicon-receipted 256-byte ARGB8888
 * carrier.  Validation and capacity checks complete before the first write,
 * so every refusal preserves both caller outputs.
 */
int r300_rb2d_copy_emit_into(const struct r300_rb2d_copy_plan *plan,
                             uint32_t *words, uint32_t capacity,
                             struct r300_rb2d_copy_ib *out);

int r300_rb2d_copy_emit_masked_into(const struct r300_rb2d_copy_plan *plan,
                                    uint32_t mask, uint32_t *words,
                                    uint32_t capacity,
                                    struct r300_rb2d_copy_ib *out);

int r300_rb2d_copy_validate_reloc_sites(const struct r300_rb2d_copy_ib *ib);

#endif /* R300_RB2D_COPY_H */
