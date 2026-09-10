/*
 * SPDX-License-Identifier: MIT
 */

#include "../r3v_native_depth_pipeline.h"
#include "../r3v_native.h"
#include "../../common/r300_reg.h"
#include "vk_render_pass.h"
#include "vk_alloc.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static uint32_t
f_bits(float value)
{
   uint32_t bits;
   memcpy(&bits, &value, sizeof(bits));
   return bits;
}

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
          0);
   assert(state.early_fragment_tests && !state.late_fragment_tests);
   assert(memcmp(&state.hardware, &before_early.hardware,
                 sizeof(state.hardware)) == 0);
   shader.requested_early_fragment_tests = false;
   shader.discards_fragments = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests && state.late_fragment_tests);

   shader.discards_fragments = false;
   shader.has_observable_side_effects = true;
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(!state.early_fragment_tests && state.late_fragment_tests);
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
          0);
   assert(state.hardware.stencil.enabled);
   before = state;
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
   disabled.pNext = NULL;

   /* R300 carries front and back operation fields in ZB_ZSTENCILCNTL.
    * RS485M has one reference/mask register, so differing face values are
    * surfaced as a draw-layer split requirement. */
   disabled.stencilTestEnable = VK_TRUE;
   disabled.front = (VkStencilOpState){
      .failOp = VK_STENCIL_OP_ZERO,
      .passOp = VK_STENCIL_OP_REPLACE,
      .depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP,
      .compareOp = VK_COMPARE_OP_LESS,
      .compareMask = 0xf0f0u,
      .writeMask = 0x0fffu,
      .reference = 0x15au,
   };
   disabled.back = (VkStencilOpState){
      .failOp = VK_STENCIL_OP_DECREMENT_AND_WRAP,
      .passOp = VK_STENCIL_OP_INVERT,
      .depthFailOp = VK_STENCIL_OP_INCREMENT_AND_WRAP,
      .compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
      .compareMask = 0x55u,
      .writeMask = 0xaau,
      .reference = 0xa5u,
   };
   assert(r3v_native_depth_pipeline_lower(&disabled, &shader, false, &state) ==
          0);
   assert(state.hardware.stencil.enabled);
   assert(state.hardware.stencil.two_sided);
   assert(state.hardware.stencil.back_reference_requires_draw_split);
   assert(state.hardware.stencil.front.reference == 0x5a);
   assert(state.hardware.stencil.front.compare_mask == 0xf0);
   assert(state.hardware.stencil.front.write_mask == 0xff);
   assert(state.hardware.stencil.back.reference == 0xa5);
   assert(state.hardware.stencil.back.compare_mask == 0x55);
   assert(state.hardware.stencil.back.write_mask == 0xaa);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_FRONT_FUNC_SHIFT) & R300_ZS_MASK) == R300_ZS_LESS);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_FRONT_SFAIL_OP_SHIFT) & R300_ZS_MASK) == R300_ZS_ZERO);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_FRONT_ZPASS_OP_SHIFT) & R300_ZS_MASK) == R300_ZS_REPLACE);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_FRONT_ZFAIL_OP_SHIFT) & R300_ZS_MASK) == R300_ZS_INCR);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_BACK_FUNC_SHIFT) & R300_ZS_MASK) == R300_ZS_GEQUAL);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_BACK_SFAIL_OP_SHIFT) & R300_ZS_MASK) == R300_ZS_DECR_WRAP);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_BACK_ZPASS_OP_SHIFT) & R300_ZS_MASK) == R300_ZS_INVERT);
   assert(((state.hardware.stencil.zstencil_control >>
            R300_S_BACK_ZFAIL_OP_SHIFT) & R300_ZS_MASK) == R300_ZS_INCR_WRAP);

   /* FragDepth selects the shader W path only when the selected binary
    * proves that export.  D24 polygon bias uses the R300 scale and constant
    * units inherited by the Gallium state setup. */
   shader.fragment_depth_export = true;
   shader.writes_depth = true;
   VkPipelineRasterizationStateCreateInfo rasterization = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .cullMode = VK_CULL_MODE_NONE,
      .depthBiasEnable = VK_TRUE,
      .depthBiasConstantFactor = 3.0f,
      .depthBiasSlopeFactor = 2.0f,
      .depthBiasClamp = 0.0f,
   };
   assert(r3v_native_depth_pipeline_lower_rasterization(
             &disabled, &rasterization, &shader, &state) == 0);
   assert(state.hardware.fragment_depth_source == R300_FG_DEPTH_SRC_SHADER);
   assert(state.hardware.fragment_depth_format ==
          (R300_W_FMT_W24 | R300_W_SRC_US));
   assert(state.hardware.polygon_offset.enabled);
   assert(state.hardware.polygon_offset.front_scale == f_bits(24.0f));
   assert(state.hardware.polygon_offset.front_offset == f_bits(6.0f));
   rasterization.depthBiasClamp = 1.0f;
   before = state;
   assert(r3v_native_depth_pipeline_lower_rasterization(
             &disabled, &rasterization, &shader, &state) == -EINVAL);
   assert(memcmp(&state, &before, sizeof(state)) == 0);
   shader.writes_depth = false;
   shader.fragment_depth_export = false;

   struct vk_render_pass_attachment attachments[2] = {
      {
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .store_op = VK_ATTACHMENT_STORE_OP_STORE,
         .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
         .final_layout = VK_IMAGE_LAYOUT_GENERAL,
      },
      {
         .format = VK_FORMAT_D24_UNORM_S8_UINT,
         .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .store_op = VK_ATTACHMENT_STORE_OP_STORE,
         .stencil_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE,
         .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
         .final_layout = VK_IMAGE_LAYOUT_GENERAL,
      },
   };
   struct vk_subpass_attachment color_ref = {
      .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   struct vk_subpass_attachment depth_ref = {
      .attachment = 1,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
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
   attachments[1].stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
   assert(r3v_native_render_pass_matches_cell(&pass));
   attachments[1].stencil_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   assert(r3v_native_render_pass_matches_cell(&pass));
   attachments[1].load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
   attachments[1].stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   assert(r3v_native_render_pass_matches_cell(&pass));
   attachments[1].load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   attachments[1].stencil_load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   attachments[1].store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   assert(r3v_native_render_pass_matches_cell(&pass));
   attachments[1].stencil_store_op = VK_ATTACHMENT_STORE_OP_STORE;
   depth_ref.attachment = 0;
   assert(!r3v_native_render_pass_matches_cell(&pass));
   depth_ref.attachment = 1;
   subpass.depth_stencil_attachment = NULL;
   assert(!r3v_native_render_pass_matches_cell(&pass));

   struct vk_render_pass_attachment depth_only_attachment = {
      .format = VK_FORMAT_D24_UNORM_S8_UINT,
      .aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .load_op = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .store_op = VK_ATTACHMENT_STORE_OP_STORE,
      .stencil_load_op = VK_ATTACHMENT_LOAD_OP_LOAD,
      .stencil_store_op = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initial_layout = VK_IMAGE_LAYOUT_UNDEFINED,
      .final_layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   struct vk_subpass_attachment depth_only_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
   };
   struct vk_subpass depth_only_subpass = {
      .color_count = 0,
      .color_attachments = NULL,
      .depth_stencil_attachment = &depth_only_ref,
   };
   struct vk_render_pass depth_only_pass = {
      .attachment_count = 1,
      .attachments = &depth_only_attachment,
      .subpass_count = 1,
      .subpasses = &depth_only_subpass,
   };
   assert(r3v_native_render_pass_matches_cell(&depth_only_pass));

   /* Read-only depth/stencil layout admits the ZB read dependency while a
    * write producer must leave the image-state record unchanged. */
   struct vk_command_pool pool = {0};
   pool.alloc = *vk_default_allocator();
   struct r3v_native_cmd_buffer command = {0};
   command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   command.vk.pool = &pool;
   struct r3v_native_image read_only_image = {
      .depth_family = true,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
   };
   assert(r3v_native_packed_depth_stencil_layout(
             VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
   assert(r3v_native_packed_depth_stencil_layout(
             VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL) ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
   assert(r3v_native_packed_depth_stencil_layout(
             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL) ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
   assert(r3v_native_packed_depth_stencil_layout(
             VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL) ==
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
   assert(r3v_native_cmd_buffer_require_image_layout(
             &command, &read_only_image,
             VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
             R3V_NATIVE_IMAGE_PRODUCER_ZB, false) == VK_SUCCESS);
   assert(command.image_state_count == 1u);
   const struct r3v_native_cmd_image_state before_read_only_write =
      command.image_states[0];
   assert(r3v_native_cmd_buffer_require_image_layout(
             &command, &read_only_image,
             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
             R3V_NATIVE_IMAGE_PRODUCER_ZB, true) != VK_SUCCESS);
   assert(memcmp(&command.image_states[0], &before_read_only_write,
                 sizeof(before_read_only_write)) == 0);

   struct r3v_native_cmd_buffer representation_command = {0};
   representation_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   representation_command.vk.pool = &pool;
   struct r3v_native_image representation_image = {
      .committed_submission = {
         .representation = R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED,
      },
   };
   assert(r3v_native_cmd_buffer_transition_image_representation(
             &representation_command, &representation_image,
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED,
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR) == VK_SUCCESS);
   assert(representation_command.image_state_count == 1u);
   assert(representation_command.image_states[0].required_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(representation_command.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   assert(representation_command.image_states[0].required_representation_set);
   assert(representation_command.image_states[0].current_representation_set);
   assert(representation_image.committed_submission.representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);

   assert(r3v_native_cmd_buffer_transition_image_representation(
             &representation_command, &representation_image,
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED) == VK_SUCCESS);
   const struct r3v_native_cmd_image_state before_refusal =
      representation_command.image_states[0];
   assert(r3v_native_cmd_buffer_transition_image_representation(
             &representation_command, &representation_image,
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED,
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED) != VK_SUCCESS);
   assert(memcmp(&representation_command.image_states[0], &before_refusal,
                 sizeof(before_refusal)) == 0);

   const struct r3v_native_zmask_metadata_state retired_metadata = {0};
   const struct r3v_native_zmask_metadata_state initialized_metadata = {
      .status = R3V_NATIVE_ZMASK_METADATA_INITIALIZED,
      .generation = 1u,
   };
   const struct r3v_native_zmask_metadata_state fast_clear_metadata = {
      .status = R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR,
      .clear_depth_code = 0x400000u,
      .clear_stencil = 0xa5u,
      .generation = 2u,
   };
   assert(r3v_native_cmd_buffer_transition_zmask_metadata(
             &representation_command, &representation_image,
             &retired_metadata, &initialized_metadata) == VK_SUCCESS);
   assert(r3v_native_cmd_buffer_transition_zmask_metadata(
             &representation_command, &representation_image,
             &initialized_metadata, &fast_clear_metadata) == VK_SUCCESS);
   assert(r3v_native_zmask_metadata_equal(
      &representation_command.image_states[0].required_zmask_metadata,
      &retired_metadata));
   assert(r3v_native_zmask_metadata_equal(
      &representation_command.image_states[0].current_zmask_metadata,
      &fast_clear_metadata));
   assert(representation_command.image_states[0]
             .required_zmask_metadata_set);
   assert(representation_command.image_states[0]
             .current_zmask_metadata_set);
   assert(r3v_native_zmask_metadata_equal(
      &representation_image.committed_submission.zmask_metadata,
      &retired_metadata));

   const struct r3v_native_cmd_image_state before_metadata_refusal =
      representation_command.image_states[0];
   assert(r3v_native_cmd_buffer_transition_zmask_metadata(
             &representation_command, &representation_image,
             &retired_metadata, &fast_clear_metadata) != VK_SUCCESS);
   assert(memcmp(&representation_command.image_states[0],
                 &before_metadata_refusal,
                 sizeof(before_metadata_refusal)) == 0);
   struct r3v_native_zmask_metadata_state invalid_metadata =
      fast_clear_metadata;
   invalid_metadata.clear_stencil = 0x100u;
   assert(r3v_native_cmd_buffer_transition_zmask_metadata(
             &representation_command, &representation_image,
             &fast_clear_metadata, &invalid_metadata) != VK_SUCCESS);
   assert(memcmp(&representation_command.image_states[0],
                 &before_metadata_refusal,
                 sizeof(before_metadata_refusal)) == 0);

   struct r3v_native_image owner_image_a = {
      .zmask_layout = {
         .stride_in_pixels = 64u,
         .dwords = 16u,
         .fits_zmask_ram = true,
      },
      .zmask_layout_admitted = true,
   };
   struct r3v_native_image owner_image_b = owner_image_a;
   struct r3v_native_zmask_owner_state owner_a = {0};
   struct r3v_native_zmask_owner_state owner_b = {0};
   const struct r3v_native_zmask_owner_state no_owner = {0};
   assert(r3v_native_zmask_owner_from_image(
             &owner_image_a, &initialized_metadata, &owner_a) == VK_SUCCESS);
   struct r3v_native_zmask_metadata_state image_b_metadata =
      initialized_metadata;
   image_b_metadata.generation = 3u;
   assert(r3v_native_zmask_owner_from_image(
             &owner_image_b, &image_b_metadata, &owner_b) == VK_SUCCESS);
   assert(owner_a.image == &owner_image_a && owner_a.dword_count == 16u &&
          owner_a.stride_in_pixels == 64u);

   assert(r3v_native_cmd_buffer_transition_zmask_owner(
             &representation_command, &no_owner, &owner_a) == VK_SUCCESS);
   assert(r3v_native_cmd_buffer_transition_zmask_owner(
             &representation_command, &owner_a, &no_owner) == VK_SUCCESS);
   assert(r3v_native_cmd_buffer_transition_zmask_owner(
             &representation_command, &no_owner, &owner_b) == VK_SUCCESS);
   assert(r3v_native_cmd_buffer_transition_zmask_owner(
             &representation_command, &owner_b, &no_owner) == VK_SUCCESS);
   assert(r3v_native_cmd_buffer_transition_zmask_owner(
             &representation_command, &no_owner, &owner_a) == VK_SUCCESS);
   const struct r3v_native_zmask_owner_state owner_before_refusal =
      representation_command.current_zmask_owner;
   assert(r3v_native_cmd_buffer_transition_zmask_owner(
             &representation_command, &owner_a, &owner_b) != VK_SUCCESS);
   assert(r3v_native_zmask_owner_equal(
      &representation_command.current_zmask_owner, &owner_before_refusal));
   r3v_native_cmd_buffer_release_recording(&representation_command);
   r3v_native_cmd_buffer_release_recording(&command);
   return 0;
}
