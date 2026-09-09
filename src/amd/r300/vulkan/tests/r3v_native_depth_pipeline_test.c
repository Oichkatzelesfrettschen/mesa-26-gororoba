/*
 * SPDX-License-Identifier: MIT
 */

#include "../r3v_native_depth_pipeline.h"
#include "../r3v_native.h"
#include "../../common/r300_reg.h"
#include "vk_render_pass.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

int
main(void)
{
   struct r3v_native_depth_shader_flags shader = {0};
   struct r3v_native_depth_pipeline_state state;
   /* ZB_ZSTENCILCNTL comparison encodings in Vulkan comparison order. */
   const uint32_t encodings[] = {0u, 1u, 3u, 2u, 5u, 6u, 4u, 7u};
   for (VkCompareOp comparison = VK_COMPARE_OP_NEVER;
        comparison <= VK_COMPARE_OP_ALWAYS; comparison++) {
      VkPipelineDepthStencilStateCreateInfo info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = VK_TRUE,
         .depthWriteEnable = VK_TRUE,
         .depthCompareOp = comparison,
      };
      assert(r3v_native_depth_pipeline_lower(&info, &shader, false,
                                             &state) == 0);
      assert(state.hardware.depth_function == encodings[comparison]);
      assert(state.hardware.depth_write && state.early_fragment_tests);
   }

   VkPipelineDepthStencilStateCreateInfo disabled = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_FALSE,
      .depthWriteEnable = VK_TRUE,
      .depthCompareOp = VK_COMPARE_OP_LESS,
   };
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.hardware.depth_write && !state.early_fragment_tests);

   disabled.depthTestEnable = VK_TRUE;
   disabled.depthWriteEnable = VK_FALSE;
   disabled.depthCompareOp = VK_COMPARE_OP_EQUAL;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(state.hardware.depth_function == R300_ZS_EQUAL &&
          !state.hardware.depth_write && state.early_fragment_tests);

   shader.requested_early_fragment_tests = true;
   struct r3v_native_depth_pipeline_state before_early = state;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before_early, sizeof(state)) == 0);
   shader.requested_early_fragment_tests = false;
   shader.discards_fragments = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests);

   shader.discards_fragments = false;
   shader.has_observable_side_effects = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests);
   shader.has_observable_side_effects = false;
   struct r3v_native_depth_pipeline_state before = state;
   shader.writes_depth = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   shader.writes_depth = false;

   disabled.front.failOp = VK_STENCIL_OP_REPLACE;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(state.early_fragment_tests);
   before = state;
   disabled.front.failOp = VK_STENCIL_OP_KEEP;
   disabled.stencilTestEnable = VK_TRUE;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.stencilTestEnable = VK_FALSE;
   disabled.depthCompareOp = (VkCompareOp)-1;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.depthCompareOp = VK_COMPARE_OP_EQUAL;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, true, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.depthBoundsTestEnable = VK_TRUE;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   disabled.depthBoundsTestEnable = VK_FALSE;
   disabled.pNext = &disabled;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);

   struct vk_render_pass_attachment attachments[2] = {
      {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .store_op = VK_ATTACHMENT_STORE_OP_STORE,
      },
      {
         .format = VK_FORMAT_D24_UNORM_S8_UINT,
         .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .store_op = VK_ATTACHMENT_STORE_OP_STORE,
         .stencil_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE,
      },
   };
   struct vk_subpass_attachment color_ref = { .attachment = 0 };
   struct vk_subpass_attachment depth_ref = { .attachment = 1 };
   struct vk_subpass subpass = {
      .color_count = 1,
      .color_attachments = &color_ref,
      .depth_stencil_attachment = &depth_ref,
   };
   struct vk_render_pass pass = {
      .attachment_count = 2,
      .attachments = attachments,
      .subpass_count = 1,
      .subpasses = &subpass,
   };
   assert(r3v_native_render_pass_matches_cell(&pass));
   attachments[1].aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   attachments[1].aspects = VK_IMAGE_ASPECT_DEPTH_BIT |
                            VK_IMAGE_ASPECT_STENCIL_BIT;
   attachments[1].format = VK_FORMAT_D32_SFLOAT;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
   attachments[1].samples = VK_SAMPLE_COUNT_2_BIT;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
   attachments[1].load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   attachments[1].load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   attachments[1].stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   attachments[1].stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE;
   depth_ref.attachment = 0;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   depth_ref.attachment = 1;
   subpass.depth_stencil_attachment = NULL;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   return 0;
}
