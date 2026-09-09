/* SPDX-License-Identifier: MIT */

#include "r300_rb2d_copy.h"

#include "r300_pm4_builder.h"
#include "r300_rb2d_fill.h"
#include "r300_zb_tile_copy.h"
#include "radeon_legacy_2d_reg.h"

#include <errno.h>
#include <string.h>

/* Source-side register and control spellings from Linux radeon_reg.h.
 * r100_copy_blit establishes their complete copy operation, while R300
 * userspace command streams express the same state through PACKET0 because
 * r300_packet3_check does not admit PACKET3_BITBLT_MULTI.
 */
#define RB2D_COPY_SCISSOR_MAX R300_RB2D_SAFE_EXCLUSIVE_END
#define RB2D_COPY_RELOC_PAYLOAD(slot) ((slot) * 4u)

struct rb2d_copy_rect {
   uint32_t source_base;
   uint32_t destination_base;
   uint32_t source_y;
   uint32_t destination_y;
   uint32_t width;
   uint32_t height;
   uint32_t source_x;
   uint32_t destination_x;
};

const char *
r300_rb2d_copy_refusal_name(enum r300_rb2d_copy_refusal refusal)
{
   static const char *const names[R300_RB2D_COPY_REFUSAL_COUNT] = {
      "ok",
      "plan or segment storage is null",
      "segment count is outside one through four",
      "segment size is outside 512, 1024, and 2048 bytes",
      "segment offset is outside the 512-byte grid",
      "aligned base is outside the 22-bit 1 KiB offset field",
      "source rectangle reaches outside its buffer",
      "destination rectangle reaches outside its buffer",
      "source segments overlap",
      "destination segments overlap",
      "source and destination segments overlap in one buffer",
   };
   return (unsigned)refusal < R300_RB2D_COPY_REFUSAL_COUNT ? names[refusal]
                                                            : NULL;
}

static enum r300_rb2d_copy_refusal
segment_check(const struct r300_rb2d_copy_plan *plan,
              const struct r300_rb2d_copy_segment *segment)
{
   const uint32_t cpp = plan->byte_carrier ? 1u : 4u;
   const bool short_span = segment->byte_count <= 256u;
   if (short_span &&
       (segment->byte_count == 0u ||
        (!plan->byte_carrier && segment->byte_count % 4u != 0u) ||
        segment->source_offset_bytes % cpp != 0u ||
        segment->destination_offset_bytes % cpp != 0u ||
        segment->source_offset_bytes % 256u + segment->byte_count > 256u ||
        segment->destination_offset_bytes % 256u + segment->byte_count > 256u))
      return R300_RB2D_COPY_REFUSE_SEGMENT_SIZE;
   if (segment->byte_count != 512u && segment->byte_count != 1024u &&
       segment->byte_count != 2048u && !short_span)
      return R300_RB2D_COPY_REFUSE_SEGMENT_SIZE;
   if ((!short_span && (segment->source_offset_bytes % 512u != 0u ||
                        segment->destination_offset_bytes % 512u != 0u)) ||
       (short_span && !plan->byte_carrier &&
        (segment->source_offset_bytes % 4u != 0u ||
         segment->destination_offset_bytes % 4u != 0u)))
      return R300_RB2D_COPY_REFUSE_OFFSET_ALIGNMENT;

   const uint64_t source_base =
      segment->source_offset_bytes & ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);
   const uint64_t destination_base =
      segment->destination_offset_bytes &
      ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);
   if (source_base / R300_RB2D_OFFSET_GRANULARITY >
          R300_RB2D_MAX_OFFSET_UNITS ||
       destination_base / R300_RB2D_OFFSET_GRANULARITY >
          R300_RB2D_MAX_OFFSET_UNITS ||
       segment->source_offset_bytes > UINT32_MAX ||
       segment->byte_count >
          UINT32_MAX - segment->source_offset_bytes ||
       segment->destination_offset_bytes > UINT32_MAX ||
       segment->byte_count >
          UINT32_MAX - segment->destination_offset_bytes)
      return R300_RB2D_COPY_REFUSE_ADDRESS_WIDTH;
   if (segment->source_offset_bytes > plan->source_buffer_bytes ||
       segment->byte_count >
          plan->source_buffer_bytes - segment->source_offset_bytes)
      return R300_RB2D_COPY_REFUSE_SOURCE_BOUNDS;
   if (segment->destination_offset_bytes > plan->destination_buffer_bytes ||
       segment->byte_count >
          plan->destination_buffer_bytes - segment->destination_offset_bytes)
      return R300_RB2D_COPY_REFUSE_DESTINATION_BOUNDS;
   return R300_RB2D_COPY_OK;
}

enum r300_rb2d_copy_refusal
r300_rb2d_copy_plan_check(const struct r300_rb2d_copy_plan *plan)
{
   if (plan == NULL || plan->segments == NULL)
      return R300_RB2D_COPY_REFUSE_PLAN_NULL;
   if (plan->segment_count == 0u ||
       plan->segment_count > R300_RB2D_COPY_MAX_SEGMENTS)
      return R300_RB2D_COPY_REFUSE_SEGMENT_COUNT;
   for (uint32_t segment_index = 0; segment_index < plan->segment_count;
        segment_index++) {
      const enum r300_rb2d_copy_refusal refusal =
         segment_check(plan, &plan->segments[segment_index]);
      if (refusal != R300_RB2D_COPY_OK)
         return refusal;
   }
   for (uint32_t first = 0; first < plan->segment_count; first++) {
      const struct r300_rb2d_copy_segment *source_segment =
         &plan->segments[first];
      const uint64_t source_segment_end =
         source_segment->source_offset_bytes + source_segment->byte_count;
      const uint64_t destination_segment_end =
         source_segment->destination_offset_bytes + source_segment->byte_count;
      for (uint32_t second = first + 1u; second < plan->segment_count;
           second++) {
         const struct r300_rb2d_copy_segment *compared_segment =
            &plan->segments[second];
         const uint64_t compared_source_end =
            compared_segment->source_offset_bytes + compared_segment->byte_count;
         const uint64_t compared_destination_end =
            compared_segment->destination_offset_bytes +
            compared_segment->byte_count;
         if (source_segment->source_offset_bytes < compared_source_end &&
             compared_segment->source_offset_bytes < source_segment_end)
            return R300_RB2D_COPY_REFUSE_SOURCE_OVERLAP;
         if (source_segment->destination_offset_bytes <
                compared_destination_end &&
             compared_segment->destination_offset_bytes <
                destination_segment_end)
            return R300_RB2D_COPY_REFUSE_DESTINATION_OVERLAP;
      }
      if (plan->same_buffer) {
         for (uint32_t destination = 0; destination < plan->segment_count;
              destination++) {
            const struct r300_rb2d_copy_segment *destination_segment =
               &plan->segments[destination];
            const uint64_t compared_destination_end =
               destination_segment->destination_offset_bytes +
               destination_segment->byte_count;
            if (source_segment->source_offset_bytes <
                   compared_destination_end &&
                destination_segment->destination_offset_bytes <
                   source_segment_end)
               return R300_RB2D_COPY_REFUSE_COPY_OVERLAP;
         }
      }
   }
   return R300_RB2D_COPY_OK;
}

enum r300_rb2d_copy_refusal
r300_rb2d_copy_plan_from_zb_tile(
   const struct r300_zb_tile_copy_plan *tile_plan,
   uint64_t source_buffer_bytes, uint64_t destination_buffer_bytes,
   bool same_buffer,
   struct r300_rb2d_copy_segment
      segment_storage[R300_RB2D_COPY_MAX_SEGMENTS],
   struct r300_rb2d_copy_plan *out)
{
   if (tile_plan == NULL || segment_storage == NULL || out == NULL)
      return R300_RB2D_COPY_REFUSE_PLAN_NULL;
   if (tile_plan->segment_count == 0u ||
       tile_plan->segment_count > R300_RB2D_COPY_MAX_SEGMENTS)
      return R300_RB2D_COPY_REFUSE_SEGMENT_COUNT;

   struct r300_rb2d_copy_segment
      candidate_segments[R300_RB2D_COPY_MAX_SEGMENTS];
   for (uint32_t segment_index = 0;
        segment_index < tile_plan->segment_count; segment_index++) {
      candidate_segments[segment_index] = (struct r300_rb2d_copy_segment){
         .source_offset_bytes =
            tile_plan->segments[segment_index].source_offset_bytes,
         .destination_offset_bytes =
            tile_plan->segments[segment_index].destination_offset_bytes,
         .byte_count = tile_plan->segments[segment_index].byte_count,
      };
   }
   struct r300_rb2d_copy_plan candidate = {
      .source_buffer_bytes = source_buffer_bytes,
      .destination_buffer_bytes = destination_buffer_bytes,
      .same_buffer = same_buffer,
      .segments = candidate_segments,
      .segment_count = tile_plan->segment_count,
   };
   const enum r300_rb2d_copy_refusal refusal =
      r300_rb2d_copy_plan_check(&candidate);
   if (refusal != R300_RB2D_COPY_OK)
      return refusal;

   memcpy(segment_storage, candidate_segments,
          candidate.segment_count * sizeof(segment_storage[0]));
   candidate.segments = segment_storage;
   *out = candidate;
   return R300_RB2D_COPY_OK;
}

static struct rb2d_copy_rect
rect_for(const struct r300_rb2d_copy_segment *segment, bool byte_carrier)
{
   const uint32_t cpp = byte_carrier ? 1u : 4u;
   const uint64_t source_base =
      segment->source_offset_bytes & ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);
   const uint64_t destination_base =
      segment->destination_offset_bytes &
      ~(uint64_t)(R300_RB2D_OFFSET_GRANULARITY - 1u);
   return (struct rb2d_copy_rect){
      .source_base = (uint32_t)source_base,
      .destination_base = (uint32_t)destination_base,
      .source_y = (uint32_t)(segment->source_offset_bytes - source_base) /
                  R300_RB2D_COPY_CARRIER_PITCH_BYTES,
      .destination_y =
         (uint32_t)(segment->destination_offset_bytes - destination_base) /
         R300_RB2D_COPY_CARRIER_PITCH_BYTES,
      .width = segment->byte_count <= 256u
                  ? segment->byte_count / cpp
                  : R300_RB2D_COPY_CARRIER_PITCH_BYTES / cpp,
      .height = segment->byte_count <= 256u
                   ? 1u
                   : segment->byte_count / R300_RB2D_COPY_CARRIER_PITCH_BYTES,
      .source_x = ((uint32_t)(segment->source_offset_bytes - source_base) %
                   R300_RB2D_COPY_CARRIER_PITCH_BYTES) /
                  cpp,
      .destination_x = ((uint32_t)(segment->destination_offset_bytes -
                                   destination_base) %
                        R300_RB2D_COPY_CARRIER_PITCH_BYTES) /
                       cpp,
   };
}

static uint32_t
pitch_offset_word(uint32_t base)
{
   return ((R300_RB2D_COPY_CARRIER_PITCH_BYTES /
            R300_RB2D_PITCH_GRANULARITY) << RADEON_DST_PITCH_SHIFT) |
          (base / R300_RB2D_OFFSET_GRANULARITY);
}

static void
emit_relocation(struct r300_pm4_builder *builder,
                struct r300_rb2d_copy_ib *ib, uint32_t slot,
                enum r300_rb2d_copy_slot_role role)
{
   const uint32_t index =
      r300_pm4_reloc_nop(builder, RB2D_COPY_RELOC_PAYLOAD(slot));
   ib->reloc_sites[ib->reloc_site_count++] =
      (struct r300_rb2d_copy_reloc_site){
         .ib_index = index,
         .slot = slot,
         .role = role,
      };
}

int
r300_rb2d_copy_emit_into(const struct r300_rb2d_copy_plan *plan,
                         uint32_t *words, uint32_t capacity,
                         struct r300_rb2d_copy_ib *out)
{
   return r300_rb2d_copy_emit_masked_into(plan, UINT32_MAX, words, capacity,
                                          out);
}

int
r300_rb2d_copy_emit_masked_into(const struct r300_rb2d_copy_plan *plan,
                                uint32_t mask, uint32_t *words,
                                uint32_t capacity,
                                struct r300_rb2d_copy_ib *out)
{
   if (words == NULL || out == NULL ||
       r300_rb2d_copy_plan_check(plan) != R300_RB2D_COPY_OK)
      return -EINVAL;
   const uint32_t required = R300_RB2D_COPY_DWORDS(plan->segment_count);
   if (capacity < required)
      return -ENOSPC;

   struct r300_rb2d_copy_ib candidate;
   memset(&candidate, 0, sizeof(candidate));
   candidate.ib = words;
   struct r300_pm4_builder builder;
   r300_pm4_builder_init(&builder, words, capacity);

   const uint32_t scissor =
      RB2D_COPY_SCISSOR_MAX | (RB2D_COPY_SCISSOR_MAX << 16);
   r300_pm4_reg(&builder, RADEON_SC_TOP_LEFT, 0u);
   r300_pm4_reg(&builder, RADEON_SC_BOTTOM_RIGHT, scissor);
   r300_pm4_reg(&builder, RADEON_DEFAULT_SC_BOTTOM_RIGHT, scissor);
   r300_pm4_reg(&builder, RADEON_SRC_SC_BOTTOM_RIGHT, scissor);
   r300_pm4_reg(&builder, RADEON_DP_GUI_MASTER_CNTL,
                RADEON_GMC_SRC_PITCH_OFFSET_CNTL |
                   RADEON_GMC_DST_PITCH_OFFSET_CNTL |
                   RADEON_GMC_SRC_CLIPPING | RADEON_GMC_DST_CLIPPING |
                   RADEON_GMC_BRUSH_NONE |
                   ((plan->byte_carrier ? 9u : RADEON_COLOR_FORMAT_ARGB8888)
                    << 8) |
                   RADEON_GMC_SRC_DATATYPE_COLOR | RADEON_ROP3_S |
                   RADEON_DP_SRC_SOURCE_MEMORY |
                   RADEON_GMC_CLR_CMP_CNTL_DIS |
                   (mask == UINT32_MAX ? RADEON_GMC_WR_MSK_DIS : 0u));
   r300_pm4_reg(&builder, RADEON_DP_CNTL,
                RADEON_DST_X_LEFT_TO_RIGHT | RADEON_DST_Y_TOP_TO_BOTTOM);
   r300_pm4_reg(&builder, RADEON_DP_WRITE_MSK, mask);

   for (uint32_t segment_index = 0; segment_index < plan->segment_count;
        segment_index++) {
      const struct rb2d_copy_rect rect = rect_for(&plan->segments[segment_index],
                                                  plan->byte_carrier);
      uint32_t slot = segment_index * R300_RB2D_COPY_RELOCS_PER_SEGMENT;
      r300_pm4_reg(&builder, RADEON_SRC_PITCH_OFFSET,
                   pitch_offset_word(rect.source_base));
      emit_relocation(&builder, &candidate, slot++, R300_RB2D_COPY_SLOT_SOURCE);
      r300_pm4_reg(&builder, RADEON_DST_PITCH_OFFSET,
                   pitch_offset_word(rect.destination_base));
      emit_relocation(&builder, &candidate, slot,
                      R300_RB2D_COPY_SLOT_DESTINATION);
      r300_pm4_reg(&builder, RADEON_SRC_Y_X,
                   (rect.source_y << 16) | rect.source_x);
      r300_pm4_reg(&builder, RADEON_DST_Y_X,
                   (rect.destination_y << 16) | rect.destination_x);
      r300_pm4_reg(&builder, RADEON_DST_WIDTH_HEIGHT,
                   (rect.width << 16) | rect.height);
   }
   r300_pm4_reg(&builder, RADEON_DSTCACHE_CTLSTAT, RADEON_RB2D_DC_FLUSH_ALL);
   r300_pm4_reg(&builder, RADEON_WAIT_UNTIL,
                RADEON_WAIT_2D_IDLECLEAN | RADEON_WAIT_HOST_IDLECLEAN |
                   RADEON_WAIT_DMA_GUI_IDLE);

   const int result = r300_pm4_builder_finish(&builder,
                                               &candidate.ib_size_dwords);
   if (result != 0)
      return result;
   *out = candidate;
   return 0;
}

int
r300_rb2d_copy_validate_reloc_sites(const struct r300_rb2d_copy_ib *ib)
{
   if (ib == NULL || ib->ib == NULL || ib->reloc_site_count == 0u ||
       ib->reloc_site_count > R300_RB2D_COPY_MAX_RELOC_SITES ||
       ib->reloc_site_count % R300_RB2D_COPY_RELOCS_PER_SEGMENT != 0u)
      return -EINVAL;
   for (uint32_t site_index = 0; site_index < ib->reloc_site_count;
        site_index++) {
      const struct r300_rb2d_copy_reloc_site *site =
         &ib->reloc_sites[site_index];
      if (site->slot != site_index ||
          site->role != (site_index % 2u == 0u ? R300_RB2D_COPY_SLOT_SOURCE
                                               : R300_RB2D_COPY_SLOT_DESTINATION) ||
          site->ib_index == 0u || site->ib_index >= ib->ib_size_dwords ||
          ib->ib[site->ib_index] != RB2D_COPY_RELOC_PAYLOAD(site->slot) ||
          ib->ib[site->ib_index - 1u] !=
             (0xc0000000u | R300_PM4_PACKET3_NOP))
         return -EINVAL;
   }
   return 0;
}
