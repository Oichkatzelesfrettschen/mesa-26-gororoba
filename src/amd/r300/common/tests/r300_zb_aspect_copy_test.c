/* SPDX-License-Identifier: MIT */
#undef NDEBUG

#include "../r300_zb_aspect_copy.h"

#include <assert.h>
#include <string.h>

int
main(void)
{
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   const uint64_t base = 2048u;
   const uint64_t mapped = 2048u + 32768u;
   struct r300_rb2d_copy_segment segment;
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 0u, 0u, 0u, 0u, 0u, 256u, 64u * 256u,
             R300_ZB_ASPECT_COPY_DEPTH,
             R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER, &segment) ==
          R300_ZB_ASPECT_COPY_OK);
   assert(segment.byte_count == 3u && segment.destination_offset_bytes == 0u);
   assert(segment.source_offset_bytes == 2049u);

   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 0u, 0u, 0u, 0u, 0u, 256u, 64u * 256u,
             R300_ZB_ASPECT_COPY_STENCIL,
             R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) ==
          R300_ZB_ASPECT_COPY_OK);
   assert(segment.byte_count == 1u);
   assert(segment.source_offset_bytes == 0u &&
          segment.destination_offset_bytes == 2048u);
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 1u, 0u, 1u, 0u, 0u, 64u, 64u,
             R300_ZB_ASPECT_COPY_STENCIL,
             R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) ==
          R300_ZB_ASPECT_COPY_OK);
   assert(segment.source_offset_bytes == 1u);
   unsigned char surface_bytes[24576];
   unsigned char buffer_bytes[16384];
   for (unsigned aspect = 0; aspect < 2; aspect++) {
      const bool depth = aspect == 0;
      const uint32_t pitch = depth ? 256u : 64u;
      const uint64_t buffer_pixel = 21u * pitch + 37u * (depth ? 4u : 1u);
      memset(surface_bytes, 0x5a, sizeof(surface_bytes));
      memset(buffer_bytes, 0xa5, sizeof(buffer_bytes));
      surface_bytes[10036] = 0x19;
      surface_bytes[10037] = 0x23;
      surface_bytes[10038] = 0x45;
      surface_bytes[10039] = 0x67;
      assert(r300_zb_aspect_copy_plan(
                surface, base, sizeof(surface_bytes), 37, 21, 37, 21, 0, pitch,
                sizeof(buffer_bytes), depth ? R300_ZB_ASPECT_COPY_DEPTH :
                                             R300_ZB_ASPECT_COPY_STENCIL,
                R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER, &segment) == 0);
      memcpy(buffer_bytes + segment.destination_offset_bytes,
             surface_bytes + segment.source_offset_bytes, segment.byte_count);
      for (unsigned offset = 0; offset < sizeof(buffer_bytes); offset++) {
         unsigned char expected = 0xa5;
         if (depth && offset >= buffer_pixel && offset < buffer_pixel + 3)
            expected = (unsigned char[]){0x23, 0x45, 0x67}[offset - buffer_pixel];
         if (!depth && offset == buffer_pixel)
            expected = 0x19;
         assert(buffer_bytes[offset] == expected);
      }
      memset(surface_bytes, 0x5a, sizeof(surface_bytes));
      assert(r300_zb_aspect_copy_plan(
                surface, base, sizeof(surface_bytes), 37, 21, 37, 21, 0, pitch,
                sizeof(buffer_bytes), depth ? R300_ZB_ASPECT_COPY_DEPTH :
                                             R300_ZB_ASPECT_COPY_STENCIL,
                R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) == 0);
      memcpy(surface_bytes + segment.destination_offset_bytes,
             buffer_bytes + segment.source_offset_bytes, segment.byte_count);
      for (unsigned offset = 0; offset < sizeof(surface_bytes); offset++) {
         unsigned char expected = 0x5a;
         if (depth && offset >= 10037 && offset <= 10039)
            expected = (unsigned char[]){0x23, 0x45, 0x67}[offset - 10037];
         if (!depth && offset == 10036)
            expected = 0x19;
         assert(surface_bytes[offset] == expected);
      }
   }
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 37, 21, 0, 0, 0, 4, 4,
             R300_ZB_ASPECT_COPY_DEPTH,
             R300_ZB_ASPECT_COPY_SURFACE_TO_BUFFER, &segment) == 0);
   assert(segment.source_offset_bytes == 10037);
   assert(segment.destination_offset_bytes == 0 && segment.byte_count == 3);
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 37, 21, 0, 0, 0, 1, 1,
             R300_ZB_ASPECT_COPY_STENCIL,
             R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) == 0);
   assert(segment.source_offset_bytes == 0);
   assert(segment.destination_offset_bytes == 10036 && segment.byte_count == 1);
   const struct r300_rb2d_copy_segment before = segment;
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 64u, 0u, 64u, 0u, 0u, 256u, 64u * 256u,
             R300_ZB_ASPECT_COPY_DEPTH,
             R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) !=
          R300_ZB_ASPECT_COPY_OK);
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 0u, 0u, 0u, 0u, 0u, 256u, 2u,
             R300_ZB_ASPECT_COPY_DEPTH,
             R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) !=
          R300_ZB_ASPECT_COPY_OK);
   assert(r300_zb_aspect_copy_plan(
             surface, base, mapped, 0u, 0u, 0u, 0u, 0u, 256u, 64u * 256u,
             (enum r300_zb_aspect_copy_aspect)99,
             R300_ZB_ASPECT_COPY_BUFFER_TO_SURFACE, &segment) !=
          R300_ZB_ASPECT_COPY_OK);
   assert(memcmp(&segment, &before, sizeof(segment)) == 0);
   return 0;
}
