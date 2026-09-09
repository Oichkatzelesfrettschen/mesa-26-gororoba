/* SPDX-License-Identifier: MIT */
#undef NDEBUG

#include "../r300_rb2d_copy.h"
#include "../r300_pm4_builder.h"
#include "../r300_rb2d_fill.h"
#include "../r300_zb_tile_copy.h"
#include "../radeon_legacy_2d_reg.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WORD_CAPACITY R300_RB2D_COPY_DWORDS(R300_RB2D_COPY_MAX_SEGMENTS)

static struct r300_rb2d_copy_plan
plan_for(const struct r300_rb2d_copy_segment *segments, uint32_t segment_count)
{
   return (struct r300_rb2d_copy_plan){
      .source_buffer_bytes = 65536u,
      .destination_buffer_bytes = 65536u,
      .same_buffer = false,
      .segments = segments,
      .segment_count = segment_count,
   };
}

static uint32_t
packet0(uint32_t reg)
{
   return reg >> 2;
}

static void
check_shape(const struct r300_rb2d_copy_segment *segments,
            uint32_t segment_count, uint32_t segment_bytes)
{
   const struct r300_rb2d_copy_plan plan = plan_for(segments, segment_count);
   struct r300_rb2d_copy_ib ib;
   uint32_t words[WORD_CAPACITY];

   assert(r300_rb2d_copy_plan_check(&plan) == R300_RB2D_COPY_OK);
   assert(r300_rb2d_copy_emit_into(&plan, words, WORD_CAPACITY, &ib) == 0);
   assert(ib.ib_size_dwords == R300_RB2D_COPY_DWORDS(segment_count));
   assert(ib.reloc_site_count == segment_count * 2u);
   assert(r300_rb2d_copy_validate_reloc_sites(&ib) == 0);

   const uint32_t expected_master =
      RADEON_GMC_SRC_PITCH_OFFSET_CNTL |
      RADEON_GMC_DST_PITCH_OFFSET_CNTL | RADEON_GMC_SRC_CLIPPING |
      RADEON_GMC_DST_CLIPPING | RADEON_GMC_BRUSH_NONE |
      (RADEON_COLOR_FORMAT_ARGB8888 << 8) |
      RADEON_GMC_SRC_DATATYPE_COLOR | RADEON_ROP3_S |
      RADEON_DP_SRC_SOURCE_MEMORY | RADEON_GMC_CLR_CMP_CNTL_DIS |
      RADEON_GMC_WR_MSK_DIS;
   assert(words[8] == packet0(RADEON_DP_GUI_MASTER_CNTL));
   assert(words[9] == expected_master);
   assert((words[9] & RADEON_ROP3_P) != RADEON_ROP3_P);

   for (uint32_t segment_index = 0; segment_index < segment_count;
        segment_index++) {
      const struct r300_rb2d_copy_segment *segment = &segments[segment_index];
      const uint32_t start = 14u + segment_index * 14u;
      const uint64_t source_base = segment->source_offset_bytes & ~UINT64_C(1023);
      const uint64_t destination_base =
         segment->destination_offset_bytes & ~UINT64_C(1023);
      const uint32_t pitch_word =
         (4u << RADEON_DST_PITCH_SHIFT);

      assert(words[start] == packet0(RADEON_SRC_PITCH_OFFSET));
      assert(words[start + 1u] == pitch_word + source_base / 1024u);
      assert(words[start + 4u] == packet0(RADEON_DST_PITCH_OFFSET));
      assert(words[start + 5u] == pitch_word + destination_base / 1024u);
      assert(words[start + 8u] == packet0(RADEON_SRC_Y_X));
      assert(words[start + 9u] ==
             ((segment->source_offset_bytes - source_base) / 256u) << 16);
      assert(words[start + 10u] == packet0(RADEON_DST_Y_X));
      assert(words[start + 11u] ==
             ((segment->destination_offset_bytes - destination_base) / 256u)
                << 16);
      assert(words[start + 12u] == packet0(RADEON_DST_WIDTH_HEIGHT));
      assert(words[start + 13u] ==
             ((64u << 16) | (segment_bytes / 256u)));
   }
}

static void
test_parity_shapes(void)
{
   static const struct r300_rb2d_copy_segment equal_parity[] = {
      { 2048u, 4096u, 2048u },
   };
   static const struct r300_rb2d_copy_segment x_parity[] = {
      { 2048u, 5120u, 1024u },
      { 3072u, 4096u, 1024u },
   };
   static const struct r300_rb2d_copy_segment y_parity[] = {
      { 2048u, 4608u, 512u },
      { 2560u, 4096u, 512u },
      { 3072u, 5632u, 512u },
      { 3584u, 5120u, 512u },
   };

   check_shape(equal_parity, 1u, 2048u);
   check_shape(x_parity, 2u, 1024u);
   check_shape(y_parity, 4u, 512u);
}

static void
test_refusals_preserve_outputs(void)
{
   const struct r300_rb2d_copy_segment valid_segment = {
      2048u, 4096u, 2048u,
   };
   struct r300_rb2d_copy_plan plan = plan_for(&valid_segment, 1u);
   struct r300_rb2d_copy_ib out;
   struct r300_rb2d_copy_ib before_out;
   uint32_t words[WORD_CAPACITY];
   uint32_t before_words[WORD_CAPACITY];

   memset(&out, 0xa5, sizeof(out));
   memset(words, 0x5a, sizeof(words));
   before_out = out;
   memcpy(before_words, words, sizeof(words));
   assert(r300_rb2d_copy_emit_into(&plan, words,
                                  R300_RB2D_COPY_DWORDS(1u) - 1u,
                                  &out) == -ENOSPC);
   assert(memcmp(&out, &before_out, sizeof(out)) == 0);
   assert(memcmp(words, before_words, sizeof(words)) == 0);

   struct r300_rb2d_copy_segment invalid_segment = valid_segment;
   invalid_segment.destination_offset_bytes = 4096u + 4u;
   plan = plan_for(&invalid_segment, 1u);
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_OFFSET_ALIGNMENT);
   assert(r300_rb2d_copy_emit_into(&plan, words, WORD_CAPACITY, &out) ==
          -EINVAL);
   assert(memcmp(&out, &before_out, sizeof(out)) == 0);
   assert(memcmp(words, before_words, sizeof(words)) == 0);

   invalid_segment = valid_segment;
   invalid_segment.source_offset_bytes = 65024u;
   plan = plan_for(&invalid_segment, 1u);
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_SOURCE_BOUNDS);
   invalid_segment = valid_segment;
   invalid_segment.destination_offset_bytes = 65024u;
   plan = plan_for(&invalid_segment, 1u);
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_DESTINATION_BOUNDS);

   invalid_segment = valid_segment;
   invalid_segment.source_offset_bytes = UINT64_C(0xfffffc00);
   plan = plan_for(&invalid_segment, 1u);
   plan.source_buffer_bytes = UINT64_MAX;
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_ADDRESS_WIDTH);
   invalid_segment = valid_segment;
   invalid_segment.destination_offset_bytes = UINT64_C(0xfffffc00);
   plan = plan_for(&invalid_segment, 1u);
   plan.destination_buffer_bytes = UINT64_MAX;
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_ADDRESS_WIDTH);

   invalid_segment = (struct r300_rb2d_copy_segment){
      UINT64_C(0xfffffc00), 4096u, 512u,
   };
   plan = plan_for(&invalid_segment, 1u);
   plan.source_buffer_bytes = UINT64_MAX;
   assert(r300_rb2d_copy_plan_check(&plan) == R300_RB2D_COPY_OK);
   invalid_segment.byte_count = 1024u;
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_ADDRESS_WIDTH);
}

static struct r300_zb_tile_copy_request
tile_request(uint32_t source_x, uint32_t source_y, uint32_t destination_x,
             uint32_t destination_y)
{
   return (struct r300_zb_tile_copy_request){
      .source = {
         .tile_offset_bytes = 2048u,
         .buffer_bytes = 65536u,
         .macro_x = source_x,
         .macro_y = source_y,
         .macro_width = 2u,
         .macro_height = 2u,
         .macro_pitch = 2u,
      },
      .destination = {
         .tile_offset_bytes = 4096u,
         .buffer_bytes = 65536u,
         .macro_x = destination_x,
         .macro_y = destination_y,
         .macro_width = 2u,
         .macro_height = 2u,
         .macro_pitch = 2u,
      },
      .same_format = true,
   };
}

static void
test_tile_adapter_all_parities(void)
{
   for (uint32_t source_x = 0; source_x < 2u; source_x++)
      for (uint32_t source_y = 0; source_y < 2u; source_y++)
         for (uint32_t destination_x = 0; destination_x < 2u;
              destination_x++)
            for (uint32_t destination_y = 0; destination_y < 2u;
                 destination_y++) {
               const struct r300_zb_tile_copy_request request =
                  tile_request(source_x, source_y, destination_x,
                               destination_y);
               struct r300_zb_tile_copy_plan tile_plan;
               assert(r300_zb_tile_copy_plan_build(&request, &tile_plan) ==
                      R300_ZB_TILE_COPY_OK);
               struct r300_rb2d_copy_segment
                  segment_storage[R300_RB2D_COPY_MAX_SEGMENTS];
               struct r300_rb2d_copy_plan copy_plan;
               assert(r300_rb2d_copy_plan_from_zb_tile(
                         &tile_plan, 65536u, 65536u, false, segment_storage,
                         &copy_plan) == R300_RB2D_COPY_OK);
               assert(copy_plan.segment_count == tile_plan.segment_count);

               uint8_t source_seen[R300_ZB_TILE_COPY_BYTES] = {0};
               uint8_t destination_seen[R300_ZB_TILE_COPY_BYTES] = {0};
               const uint32_t destination_xor =
                  ((source_x ^ destination_x) * 1024u) |
                  ((source_y ^ destination_y) * 512u);
               for (uint32_t segment_index = 0;
                    segment_index < copy_plan.segment_count; segment_index++) {
                  const struct r300_rb2d_copy_segment *segment =
                     &copy_plan.segments[segment_index];
                  for (uint32_t byte_index = 0;
                       byte_index < segment->byte_count; byte_index++) {
                     const uint32_t source_within =
                        (uint32_t)(segment->source_offset_bytes - 2048u) +
                        byte_index;
                     const uint32_t destination_within =
                        (uint32_t)(segment->destination_offset_bytes - 4096u) +
                        byte_index;
                     assert(source_within < R300_ZB_TILE_COPY_BYTES);
                     assert(destination_within < R300_ZB_TILE_COPY_BYTES);
                     assert(destination_within ==
                            (source_within ^ destination_xor));
                     assert(source_seen[source_within] == 0u);
                     source_seen[source_within]++;
                     assert(destination_seen[destination_within] == 0u);
                     destination_seen[destination_within]++;
                  }
               }
               for (uint32_t byte_index = 0;
                    byte_index < R300_ZB_TILE_COPY_BYTES; byte_index++) {
                  assert(source_seen[byte_index] == 1u);
                  assert(destination_seen[byte_index] == 1u);
               }
            }
}

static void
test_complete_overlap_rules(void)
{
   struct r300_rb2d_copy_segment segments[2] = {
      { 2048u, 8192u, 1024u },
      { 2048u, 9216u, 1024u },
   };
   struct r300_rb2d_copy_plan plan = plan_for(segments, 2u);
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_SOURCE_OVERLAP);

   segments[1] = (struct r300_rb2d_copy_segment){ 3072u, 8192u, 1024u };
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_DESTINATION_OVERLAP);

   segments[1] = (struct r300_rb2d_copy_segment){ 3072u, 9216u, 1024u };
   plan.same_buffer = true;
   segments[0].destination_offset_bytes = 3072u;
   segments[1].destination_offset_bytes = 4096u;
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_COPY_OVERLAP);

   plan.same_buffer = false;
   assert(r300_rb2d_copy_plan_check(&plan) == R300_RB2D_COPY_OK);
}

static void
test_register_authority(void)
{
   assert(RADEON_SRC_PITCH_OFFSET == 0x1428u);
   assert(RADEON_SRC_Y_X == 0x1434u);
   assert(RADEON_SRC_SC_BOTTOM_RIGHT == 0x16f4u);
   assert(RADEON_GMC_SRC_PITCH_OFFSET_CNTL == 0x00000001u);
   assert(RADEON_GMC_BRUSH_NONE == 0x000000f0u);
   assert(RADEON_GMC_SRC_DATATYPE_COLOR == 0x00003000u);
   assert(RADEON_ROP3_S == 0x00cc0000u);
   assert(RADEON_DP_SRC_SOURCE_MEMORY == 0x02000000u);
}

static void
test_masked_streams(void)
{
   const struct r300_rb2d_copy_segment segment = {2048u, 4096u, 2048u};
   const struct r300_rb2d_copy_plan plan = plan_for(&segment, 1u);
   uint32_t full_words[WORD_CAPACITY], wrapped_words[WORD_CAPACITY];
   struct r300_rb2d_copy_ib full, wrapped;
   assert(r300_rb2d_copy_emit_into(&plan, wrapped_words, WORD_CAPACITY,
                                   &wrapped) == 0);
   assert(r300_rb2d_copy_emit_masked_into(&plan, UINT32_MAX, full_words,
                                          WORD_CAPACITY, &full) == 0);
   assert(full.ib_size_dwords == wrapped.ib_size_dwords &&
          memcmp(full_words, wrapped_words,
                 full.ib_size_dwords * sizeof(uint32_t)) == 0);
   const uint32_t masks[] = {0u, 0xffu, 0xffffff00u, 0x12345678u};
   for (unsigned mask_index = 0; mask_index < 4; mask_index++) {
      uint32_t words[WORD_CAPACITY];
      struct r300_rb2d_copy_ib ib;
      assert(r300_rb2d_copy_emit_masked_into(&plan, masks[mask_index], words,
                                             WORD_CAPACITY, &ib) == 0);
      assert(words[13] == masks[mask_index]);
      assert((words[9] & RADEON_GMC_WR_MSK_DIS) == 0u);
      assert(r300_rb2d_copy_validate_reloc_sites(&ib) == 0);
   }
}

static void
test_short_carrier_spans(void)
{
   struct r300_rb2d_copy_segment segment = {1027u, 2055u, 3u};
   struct r300_rb2d_copy_plan plan = plan_for(&segment, 1u);
   plan.byte_carrier = true;
   uint32_t words[WORD_CAPACITY];
   struct r300_rb2d_copy_ib ib;
   assert(r300_rb2d_copy_emit_masked_into(&plan, 0xffu, words,
                                          WORD_CAPACITY, &ib) == 0);
   assert(words[23] == 3u);
   assert(words[25] == 7u);
   assert(words[27] == ((3u << 16) | 1u));
   assert(words[9] == (RADEON_GMC_DST_8BPP_RGB | (words[9] & ~0x0f00u)));

   struct r300_rb2d_copy_ib before = ib;
   segment = (struct r300_rb2d_copy_segment){255u, 511u, 2u};
   plan.segments = &segment;
   assert(r300_rb2d_copy_emit_into(&plan, words, WORD_CAPACITY, &ib) ==
          -EINVAL);
   assert(memcmp(&ib, &before, sizeof(ib)) == 0);

   plan.byte_carrier = false;
   segment = (struct r300_rb2d_copy_segment){4u, 8u, 4u};
   assert(r300_rb2d_copy_plan_check(&plan) == R300_RB2D_COPY_OK);
   segment.source_offset_bytes = 3u;
   assert(r300_rb2d_copy_plan_check(&plan) ==
          R300_RB2D_COPY_REFUSE_SEGMENT_SIZE);
}

int
main(void)
{
   test_register_authority();
   test_masked_streams();
   test_short_carrier_spans();
   test_parity_shapes();
   test_tile_adapter_all_parities();
   test_complete_overlap_rules();
   test_refusals_preserve_outputs();
   for (unsigned refusal = 0; refusal < R300_RB2D_COPY_REFUSAL_COUNT;
        refusal++)
      assert(r300_rb2d_copy_refusal_name(
                (enum r300_rb2d_copy_refusal)refusal) != NULL);
   assert(r300_rb2d_copy_refusal_name(R300_RB2D_COPY_REFUSAL_COUNT) == NULL);
   printf("r300_rb2d_copy_test: all checks passed\n");
   return 0;
}
