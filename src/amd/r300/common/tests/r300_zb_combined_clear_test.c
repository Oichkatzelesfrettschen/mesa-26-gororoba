/* SPDX-License-Identifier: MIT */

#include "../r300_zb_combined_clear.h"

#include <assert.h>
#include <string.h>

static struct r300_zb_combined_clear_request
valid_request(void)
{
   return (struct r300_zb_combined_clear_request){
      .surface = &r300_zb_depth_surface_rs485m_z24_macrotiled_logical,
      .surface_base_bytes = 2048u,
      .mapped_surface_bytes = 2048u + 24576u,
      .pitch_bytes = 256u,
      .format = R300_RB2D_FORMAT_ARGB8888,
      .aspect_mask = R300_ZB_COMBINED_CLEAR_ASPECTS,
      .depth_code = 0x123456u,
      .stencil = 0xa5u,
   };
}

int main(void)
{
   struct r300_zb_combined_clear_request request = valid_request();
   struct r300_zb_combined_clear_plan plan;
   memset(&plan, 0xa5, sizeof(plan));
   assert(r300_zb_combined_clear_plan(&request, &plan) ==
          R300_ZB_COMBINED_CLEAR_OK);
   assert(plan.rect.width == 64u && plan.rect.height == 64u);
   assert(plan.rect.value == 0x123456a5u);
   assert(plan.fill.surface.base_offset_bytes == 2048u);
   assert(plan.fill.surface.pitch_bytes == 256u);
   assert(plan.fill.rects == &plan.rect);

   struct r300_zb_depth_surface copied_surface = *request.surface;
   request.surface = &copied_surface;
   assert(r300_zb_combined_clear_plan(&request, &plan) ==
          R300_ZB_COMBINED_CLEAR_OK);
   request = valid_request();

   request.binding_offset_bytes = 4096u;
   request.mapped_surface_bytes += 4096u;
   assert(r300_zb_combined_clear_plan(&request, &plan) ==
          R300_ZB_COMBINED_CLEAR_OK);
   assert(plan.fill.surface.base_offset_bytes == 6144u);
   request = valid_request();

   const struct r300_zb_combined_clear_plan before = plan;
   const struct r300_zb_combined_clear_request bad[] = {
      {.surface = request.surface, .surface_base_bytes = 1024u,
       .mapped_surface_bytes = request.mapped_surface_bytes,
       .pitch_bytes = 256u, .format = request.format,
       .aspect_mask = request.aspect_mask, .depth_code = request.depth_code,
       .stencil = request.stencil},
      {.surface = request.surface, .surface_base_bytes = 2048u,
       .mapped_surface_bytes = request.mapped_surface_bytes,
       .pitch_bytes = 192u, .format = request.format,
       .aspect_mask = request.aspect_mask, .depth_code = request.depth_code,
       .stencil = request.stencil},
      {.surface = request.surface, .surface_base_bytes = 2048u,
       .mapped_surface_bytes = request.mapped_surface_bytes,
       .pitch_bytes = 256u, .format = R300_RB2D_FORMAT_RGB565,
       .aspect_mask = request.aspect_mask, .depth_code = request.depth_code,
       .stencil = request.stencil},
      {.surface = request.surface, .surface_base_bytes = 2048u,
       .mapped_surface_bytes = request.mapped_surface_bytes,
       .pitch_bytes = 256u, .format = request.format,
       .aspect_mask = R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH,
       .depth_code = request.depth_code, .stencil = request.stencil},
      {.surface = request.surface, .surface_base_bytes = 2048u,
       .binding_offset_bytes = UINT64_MAX,
       .mapped_surface_bytes = request.mapped_surface_bytes,
       .pitch_bytes = 256u, .format = request.format,
       .aspect_mask = request.aspect_mask, .depth_code = request.depth_code,
       .stencil = request.stencil},
      {.surface = request.surface, .surface_base_bytes = 2048u,
       .mapped_surface_bytes = 2048u + 24575u,
       .pitch_bytes = 256u, .format = request.format,
       .aspect_mask = request.aspect_mask, .depth_code = request.depth_code,
       .stencil = request.stencil},
   };
   for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      assert(r300_zb_combined_clear_plan(&bad[i], &plan) !=
             R300_ZB_COMBINED_CLEAR_OK);
      assert(memcmp(&plan, &before, sizeof(plan)) == 0);
   }
   return 0;
}
