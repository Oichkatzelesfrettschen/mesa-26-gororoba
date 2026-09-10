/* SPDX-License-Identifier: MIT */
#undef NDEBUG

#include "../r300_zb_tile_copy.h"
#include "../r300_zb_depth_layout.h"

#include <assert.h>
#include <string.h>

static struct r300_zb_tile_copy_request
request_for(uint32_t source_x, uint32_t source_y, uint32_t destination_x,
            uint32_t destination_y)
{
   const uint64_t source_tile_offset =
      2048u + 2048u * (source_y * 2u + source_x);
   const uint64_t destination_tile_offset =
      4096u + 2048u * (destination_y * 2u + destination_x);
   const struct r300_zb_tile_copy_mapping source = {
      .tile_offset_bytes = source_tile_offset,
      .buffer_bytes = 0x10000u,
      .macro_x = source_x, .macro_y = source_y,
      .macro_width = 2u, .macro_height = 4u, .macro_pitch = 2u,
   };
   const struct r300_zb_tile_copy_mapping destination = {
      .tile_offset_bytes = destination_tile_offset,
      .buffer_bytes = 0x10000u,
      .macro_x = destination_x, .macro_y = destination_y,
      .macro_width = 2u, .macro_height = 4u, .macro_pitch = 2u,
   };
   return (struct r300_zb_tile_copy_request){
      .source = source, .destination = destination,
      .same_buffer = false, .same_format = true,
   };
}

static void
test_all_parities(void)
{
   for (uint32_t sx = 0; sx < 2; sx++)
      for (uint32_t sy = 0; sy < 2; sy++)
         for (uint32_t dx = 0; dx < 2; dx++)
            for (uint32_t dy = 0; dy < 2; dy++) {
               struct r300_zb_tile_copy_request request =
                  request_for(sx, sy, dx, dy);
               struct r300_zb_tile_copy_plan plan;
               assert(r300_zb_tile_copy_plan_build(&request, &plan) ==
                      R300_ZB_TILE_COPY_OK);
               const uint32_t x_difference = sx ^ dx;
               const uint32_t y_difference = sy ^ dy;
               const uint32_t expected_count =
                  y_difference ? 4u : (x_difference ? 2u : 1u);
               const uint32_t expected_size = 2048u / expected_count;
               assert(plan.segment_count == expected_count);
               for (uint32_t i = 0; i < expected_count; i++) {
                  const uint32_t source_within = i * expected_size;
                  const uint32_t delta = (x_difference ? 1024u : 0u) |
                                         (y_difference ? 512u : 0u);
                  assert(plan.segments[i].source_offset_bytes ==
                         request.source.tile_offset_bytes + source_within);
                  assert(plan.segments[i].destination_offset_bytes ==
                         request.destination.tile_offset_bytes +
                         (source_within ^ delta));
                  assert(plan.segments[i].byte_count == expected_size);
               }

               struct r300_zb_depth_surface source_surface =
                  r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
               struct r300_zb_depth_surface destination_surface =
                  r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
               source_surface.pitch_pixels = 64u;
               destination_surface.pitch_pixels = 64u;
               source_surface.address_resolver =
                  &r300_zb_depth_address_rs485m_tiled;
               destination_surface.address_resolver =
                  &r300_zb_depth_address_rs485m_tiled;

               const uint64_t source_tile_base = 2048u + 2048u * (sy * 2u + sx);
               const uint64_t destination_tile_base = 4096u + 2048u * (dy * 2u + dx);
               /* The macro tile contains 32x16 four-byte pixels.  Checking
                * every resolver-derived pixel for every parity tuple
                * exercises the complete 8192-pixel mapping surface. */
               for (uint32_t y = 0; y < 16u; y++)
                  for (uint32_t x = 0; x < 32u; x++) {
                     uint64_t source_address;
                     uint64_t destination_address;
                     assert(r300_zb_depth_address_checked(
                               &source_surface, 2048u, 65536u,
                               sx * 32u + x, sy * 16u + y,
                               &source_address) == 0);
                     assert(r300_zb_depth_address_checked(
                               &destination_surface, 4096u, 65536u,
                               dx * 32u + x, dy * 16u + y,
                               &destination_address) == 0);
                     const uint32_t source_within =
                        (uint32_t)(source_address - source_tile_base);
                     const uint32_t destination_within =
                        (uint32_t)(destination_address - destination_tile_base);
                     bool found = false;
                     for (uint32_t i = 0; i < expected_count; i++) {
                        const struct r300_zb_tile_copy_segment *segment =
                           &plan.segments[i];
                        const uint32_t segment_start = i * expected_size;
                        if (source_within >= segment_start &&
                            source_within < segment_start + expected_size) {
                           assert(segment->destination_offset_bytes +
                                  source_within - segment_start ==
                                  destination_tile_base + destination_within);
                           found = true;
                           break;
                        }
                     }
                     assert(found);
                  }
            }
}

static void
test_refusal_preserves_output(void)
{
   struct r300_zb_tile_copy_request request = request_for(0u, 0u, 1u, 0u);
   struct r300_zb_tile_copy_plan plan;
   memset(&plan, 0xa5, sizeof(plan));
   struct r300_zb_tile_copy_plan before = plan;
   request.source.tile_offset_bytes = 0x2004u;
   assert(r300_zb_tile_copy_plan_build(&request, &plan) ==
          R300_ZB_TILE_COPY_REFUSE_OFFSET_ALIGNMENT);
   assert(memcmp(&plan, &before, sizeof(plan)) == 0);

   request = request_for(0u, 0u, 1u, 0u);
   before = plan;
   request.destination.macro_x = 4u;
   assert(r300_zb_tile_copy_plan_build(&request, &plan) ==
          R300_ZB_TILE_COPY_REFUSE_MAPPING_BOUNDS);
   assert(memcmp(&plan, &before, sizeof(plan)) == 0);

   request = request_for(0u, 0u, 1u, 0u);
   before = plan;
   request.same_buffer = true;
   request.destination.tile_offset_bytes = request.source.tile_offset_bytes;
   assert(r300_zb_tile_copy_plan_build(&request, &plan) ==
          R300_ZB_TILE_COPY_REFUSE_OVERLAP);
   assert(memcmp(&plan, &before, sizeof(plan)) == 0);
}

int
main(void)
{
   test_all_parities();
   test_refusal_preserves_output();
   return 0;
}
