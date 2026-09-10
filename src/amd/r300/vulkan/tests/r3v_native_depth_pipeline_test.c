/*
 * SPDX-License-Identifier: MIT
 */

#include "../r3v_native_depth_pipeline.h"
#include "../r3v_native.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/r300/common/r300_zmask_clear_plan.h"
#include "amd/r300/common/r300_zmask_materialize_plan.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "../../common/r300_reg.h"
#include "vk_render_pass.h"
#include "vk_alloc.h"
#include "vk_physical_device.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
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

   struct r3v_native_cmd_buffer materialize_command = {0};
   struct r3v_native_device materialize_device = {0};
   struct vk_instance materialize_instance = {0};
   struct vk_physical_device materialize_physical = {0};
   materialize_instance.base.type = VK_OBJECT_TYPE_INSTANCE;
   list_inithead(&materialize_instance.debug_utils.instance_callbacks);
   materialize_physical.base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
   materialize_physical.instance = &materialize_instance;
   materialize_device.vk.base.type = VK_OBJECT_TYPE_DEVICE;
   materialize_device.vk.base.device = &materialize_device.vk;
   materialize_device.vk.physical = &materialize_physical;
   materialize_device.zmask_materialize_vertex.bo.handle = 1u;
   materialize_device.zmask_materialize_vertex.bo.size =
      R3V_NATIVE_MEMORY_ALIGNMENT;
   materialize_device.zmask_materialize_color.bo.handle = 2u;
   materialize_device.zmask_materialize_color.bo.size =
      R300_ZB_DEPTH_CONTROL_COLOR_BYTES;
   materialize_device.zmask_materialize_scratch_initialized = true;
   materialize_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   materialize_command.vk.base.device = &materialize_device.vk;
   materialize_command.vk.pool = &pool;
   struct r3v_native_memory materialize_memory = {0};
   materialize_memory.bo.handle = 3u;
   materialize_memory.bo.size = 32768u;
   const struct r3v_native_depth_image_create_info materialize_create_info = {
      .pci_vendor = 0x1002u,
      .pci_device = 0x5974u,
      .pci_subsystem_vendor = 0x1028u,
      .pci_subsystem_device = 0x022au,
      .format = VK_FORMAT_D24_UNORM_S8_UINT,
      .image_type = VK_IMAGE_TYPE_2D,
      .extent = {64u, 64u, 1u},
      .mip_levels = 1u,
      .array_layers = 1u,
      .samples = 1u,
      .optimal_tiling = true,
      .compressed = false,
   };
   struct r3v_native_image materialize_image = {
      .base = { .type = VK_OBJECT_TYPE_IMAGE },
      .memory = &materialize_memory,
      .committed_submission = {
         .representation = R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
         .zmask_metadata = fast_clear_metadata,
      },
      .zmask_layout = {
         .stride_in_pixels = 64u,
         .dwords = 16u,
         .zmask_ram_dwords = 5120u,
         .fits_zmask_ram = true,
         .zcomp8x8 = false,
      },
      .zmask_layout_admitted = true,
      .depth_family = true,
   };
   assert(r3v_native_depth_image_contract_init(
             &materialize_create_info,
             &materialize_image.depth_contract) == 0);
   assert(r3v_native_depth_image_contract_bind(
             &materialize_image.depth_contract, 4096u,
             materialize_memory.bo.size, &materialize_image.depth_bound) == 0);

   struct r3v_native_image fast_clear_image = materialize_image;
   fast_clear_image.committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED;
   fast_clear_image.committed_submission.zmask_metadata = retired_metadata;
   fast_clear_image.depth_bound.contract = &fast_clear_image.depth_contract;

   struct r3v_native_cmd_buffer initialize_command = {0};
   initialize_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   initialize_command.vk.base.device = &materialize_device.vk;
   initialize_command.vk.pool = &pool;
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&initialize_command),
             r3v_native_image_to_handle(&fast_clear_image)) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
   assert(initialize_command.ib_size_dwords == 0u);
   assert(initialize_command.reference_count == 0u);
   assert(initialize_command.ordered_operation_count == 0u);
   assert(initialize_command.image_state_count == 0u);

   assert(unsetenv("R3V_NATIVE_ZMASK_INITIALIZE_EXPERIMENTAL") == 0);
   r3v_native_device_refresh_delivery_gates(&materialize_device);
   assert(materialize_device.zmask_initialize_gate == NULL);
   assert(setenv("R3V_NATIVE_ZMASK_INITIALIZE_EXPERIMENTAL", "0", 1) == 0);
   r3v_native_device_refresh_delivery_gates(&materialize_device);
   assert(materialize_device.zmask_initialize_gate == NULL);
   assert(setenv("R3V_NATIVE_ZMASK_INITIALIZE_EXPERIMENTAL", "1", 1) == 0);
   r3v_native_device_refresh_delivery_gates(&materialize_device);
   assert(materialize_device.zmask_initialize_gate != NULL);
   assert(unsetenv("R3V_NATIVE_ZMASK_INITIALIZE_EXPERIMENTAL") == 0);
   materialize_device.zmask_initialize_gate = "1";
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&initialize_command),
             r3v_native_image_to_handle(&fast_clear_image)) == VK_SUCCESS);
   assert(initialize_command.ordered_operation_count == 1u);
   assert(initialize_command.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_ZMASK_INITIALIZE);
   assert(initialize_command.reference_count == 1u);
   assert(initialize_command.references[0].memory == &materialize_memory);
   assert(initialize_command.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   const struct r3v_native_zmask_metadata_state first_initialized_metadata =
      initialize_command.image_states[0].current_zmask_metadata;
   assert(first_initialized_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_INITIALIZED);
   assert(first_initialized_metadata.clear_depth_code == 0u);
   assert(first_initialized_metadata.clear_stencil == 0u);
   assert(first_initialized_metadata.generation != 0u);
   assert(initialize_command.current_zmask_owner.image == &fast_clear_image);
   assert(r3v_native_ordered_image_composition_geometry_valid(
      &initialize_command));
   initialize_command.ordered_operations[0]
      .payload.image_zmask_initialize.image = NULL;
   assert(!r3v_native_ordered_image_composition_geometry_valid(
      &initialize_command));
   initialize_command.ordered_operations[0]
      .payload.image_zmask_initialize.image = &fast_clear_image;
   assert(fast_clear_image.committed_submission.representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(fast_clear_image.committed_submission.zmask_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   struct r300_zmask_clear_plan expected_initialize;
   assert(r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
                                      &fast_clear_image.zmask_layout,
                                      &expected_initialize) == 0);
   assert(initialize_command.ib_size_dwords ==
          expected_initialize.dword_count);
   assert(memcmp(initialize_command.ib, expected_initialize.words,
                 (size_t)expected_initialize.dword_count *
                    sizeof(expected_initialize.words[0])) == 0);

   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&initialize_command),
             r3v_native_image_to_handle(&fast_clear_image)) == VK_SUCCESS);
   assert(initialize_command.ordered_operation_count == 2u);
   assert(initialize_command.ordered_operations[1]
             .payload.image_zmask_initialize.source_metadata.generation ==
          first_initialized_metadata.generation);
   assert(initialize_command.image_states[0]
             .current_zmask_metadata.generation !=
          first_initialized_metadata.generation);

   struct r3v_native_cmd_buffer initialize_secondary = {0};
   initialize_secondary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   initialize_secondary.vk.base.device = &materialize_device.vk;
   initialize_secondary.vk.pool = &pool;
   initialize_secondary.vk.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&initialize_secondary),
             r3v_native_image_to_handle(&fast_clear_image)) == VK_SUCCESS);
   const uint64_t frozen_initialize_generation =
      initialize_secondary.ordered_operations[0]
         .payload.image_zmask_initialize.resulting_metadata.generation;
   const uint64_t initialize_counter_before_replay =
      materialize_device.zmask_metadata_generation_counter;
   struct r3v_native_cmd_buffer initialize_primary = {0};
   initialize_primary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   initialize_primary.vk.base.device = &materialize_device.vk;
   initialize_primary.vk.pool = &pool;
   initialize_primary.vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   VkCommandBuffer initialize_secondary_handle =
      r3v_native_cmd_buffer_to_handle(&initialize_secondary);
   r3v_CmdExecuteCommands(r3v_native_cmd_buffer_to_handle(&initialize_primary),
                          1u, &initialize_secondary_handle);
   assert(initialize_primary.vk.record_result == VK_SUCCESS);
   assert(initialize_primary.ordered_operation_count == 1u);
   assert(initialize_primary.ordered_operations[0]
             .payload.image_zmask_initialize.resulting_metadata.generation ==
          frozen_initialize_generation);
   assert(materialize_device.zmask_metadata_generation_counter ==
          initialize_counter_before_replay);
   assert(initialize_primary.ib_size_dwords ==
          initialize_secondary.ib_size_dwords);
   assert(memcmp(initialize_primary.ib, initialize_secondary.ib,
                 (size_t)initialize_primary.ib_size_dwords *
                    sizeof(initialize_primary.ib[0])) == 0);

   struct r3v_native_memory initialized_owner_memory = materialize_memory;
   initialized_owner_memory.bo.handle = 4u;
   struct r3v_native_image initialized_owner_image = fast_clear_image;
   initialized_owner_image.memory = &initialized_owner_memory;
   initialized_owner_image.committed_submission.zmask_metadata =
      initialized_metadata;
   initialized_owner_image.depth_bound.contract =
      &initialized_owner_image.depth_contract;
   assert(r3v_native_zmask_owner_from_image(
             &initialized_owner_image, &initialized_metadata,
             &materialize_device.zmask_owner) == VK_SUCCESS);
   struct r3v_native_cmd_buffer initialized_switch_command = {0};
   initialized_switch_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   initialized_switch_command.vk.base.device = &materialize_device.vk;
   initialized_switch_command.vk.pool = &pool;
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&initialized_switch_command),
             r3v_native_image_to_handle(&fast_clear_image)) == VK_SUCCESS);
   assert(initialized_switch_command.ordered_operation_count == 1u);
   assert(initialized_switch_command.image_state_count == 2u);
   assert(initialized_switch_command.image_states[0].image ==
          &fast_clear_image);
   assert(initialized_switch_command.image_states[0]
             .current_zmask_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_INITIALIZED);
   assert(initialized_switch_command.image_states[1].image ==
          &initialized_owner_image);
   assert(initialized_switch_command.image_states[1]
             .current_zmask_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   assert(initialized_switch_command.current_zmask_owner.image ==
          &fast_clear_image);
   materialize_device.zmask_owner = no_owner;

   materialize_device.zmask_fast_clear_gate = "1";
   struct r3v_native_cmd_buffer fast_clear_command = {0};
   fast_clear_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   fast_clear_command.vk.base.device = &materialize_device.vk;
   fast_clear_command.vk.pool = &pool;
   assert(r3v_native_record_zmask_fast_clear(
             r3v_native_cmd_buffer_to_handle(&fast_clear_command),
             r3v_native_image_to_handle(&fast_clear_image), 0x400000u,
             0xa5u) == VK_SUCCESS);
   assert(fast_clear_command.ordered_operation_count == 1u);
   assert(fast_clear_command.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR);
   assert(fast_clear_command.reference_count == 1u);
   assert(fast_clear_command.references[0].memory == &materialize_memory);
   assert(fast_clear_command.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   const struct r3v_native_zmask_metadata_state first_fast_clear_metadata =
      fast_clear_command.image_states[0].current_zmask_metadata;
   assert(first_fast_clear_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR);
   assert(first_fast_clear_metadata.clear_depth_code == 0x400000u);
   assert(first_fast_clear_metadata.clear_stencil == 0xa5u);
   assert(first_fast_clear_metadata.generation != 0u);
   assert(fast_clear_command.current_zmask_owner.image == &fast_clear_image);
   assert(fast_clear_image.committed_submission.representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(fast_clear_image.committed_submission.zmask_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   struct r300_zmask_clear_plan expected_fast_clear;
   assert(r300_zmask_fast_clear_plan_build(
             &fast_clear_image.depth_contract.surface,
             &fast_clear_image.zmask_layout, 0x400000u, 0xa5u,
             &expected_fast_clear) == 0);
   assert(fast_clear_command.ib_size_dwords ==
          expected_fast_clear.dword_count);
   assert(memcmp(fast_clear_command.ib, expected_fast_clear.words,
                 (size_t)expected_fast_clear.dword_count *
                    sizeof(expected_fast_clear.words[0])) == 0);

   assert(r3v_native_record_zmask_fast_clear(
             r3v_native_cmd_buffer_to_handle(&fast_clear_command),
             r3v_native_image_to_handle(&fast_clear_image), 0x200000u,
             0x5au) == VK_SUCCESS);
   assert(fast_clear_command.ordered_operation_count == 2u);
   assert(fast_clear_command.ordered_operations[1]
             .payload.image_fast_clear.source_metadata.generation ==
          first_fast_clear_metadata.generation);
   assert(fast_clear_command.image_states[0]
             .current_zmask_metadata.clear_depth_code == 0x200000u);
   assert(fast_clear_command.image_states[0]
             .current_zmask_metadata.clear_stencil == 0x5au);
   assert(fast_clear_command.image_states[0]
             .current_zmask_metadata.generation !=
          first_fast_clear_metadata.generation);

   struct r3v_native_cmd_buffer fast_clear_secondary = {0};
   fast_clear_secondary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   fast_clear_secondary.vk.base.device = &materialize_device.vk;
   fast_clear_secondary.vk.pool = &pool;
   fast_clear_secondary.vk.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   assert(r3v_native_record_zmask_fast_clear(
             r3v_native_cmd_buffer_to_handle(&fast_clear_secondary),
             r3v_native_image_to_handle(&fast_clear_image), 0x600000u,
             0x3cu) == VK_SUCCESS);
   const uint64_t frozen_fast_clear_generation =
      fast_clear_secondary.ordered_operations[0]
         .payload.image_fast_clear.resulting_metadata.generation;
   const uint64_t generation_counter_before_replay =
      materialize_device.zmask_metadata_generation_counter;
   struct r3v_native_cmd_buffer fast_clear_primary = {0};
   fast_clear_primary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   fast_clear_primary.vk.base.device = &materialize_device.vk;
   fast_clear_primary.vk.pool = &pool;
   fast_clear_primary.vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   VkCommandBuffer fast_clear_secondary_handle =
      r3v_native_cmd_buffer_to_handle(&fast_clear_secondary);
   r3v_CmdExecuteCommands(r3v_native_cmd_buffer_to_handle(&fast_clear_primary),
                          1u, &fast_clear_secondary_handle);
   assert(fast_clear_primary.vk.record_result == VK_SUCCESS);
   assert(fast_clear_primary.ordered_operation_count == 1u);
   assert(fast_clear_primary.ordered_operations[0]
             .payload.image_fast_clear.resulting_metadata.generation ==
          frozen_fast_clear_generation);
   assert(materialize_device.zmask_metadata_generation_counter ==
          generation_counter_before_replay);
   assert(fast_clear_primary.ib_size_dwords ==
          fast_clear_secondary.ib_size_dwords);
   assert(memcmp(fast_clear_primary.ib, fast_clear_secondary.ib,
                 (size_t)fast_clear_primary.ib_size_dwords *
                    sizeof(fast_clear_primary.ib[0])) == 0);

   struct r3v_native_memory switching_owner_memory = materialize_memory;
   switching_owner_memory.bo.handle = 4u;
   struct r3v_native_image switching_owner_image = fast_clear_image;
   switching_owner_image.memory = &switching_owner_memory;
   switching_owner_image.committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR;
   switching_owner_image.committed_submission.zmask_metadata =
      fast_clear_metadata;
   switching_owner_image.depth_bound.contract =
      &switching_owner_image.depth_contract;
   assert(r3v_native_zmask_owner_from_image(
             &switching_owner_image, &fast_clear_metadata,
             &materialize_device.zmask_owner) == VK_SUCCESS);
   struct r3v_native_cmd_buffer owner_switch_command = {0};
   owner_switch_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   owner_switch_command.vk.base.device = &materialize_device.vk;
   owner_switch_command.vk.pool = &pool;
   assert(r3v_native_record_zmask_fast_clear(
             r3v_native_cmd_buffer_to_handle(&owner_switch_command),
             r3v_native_image_to_handle(&fast_clear_image), 0x400000u,
             0xa5u) == VK_SUCCESS);
   assert(owner_switch_command.ordered_operation_count == 2u);
   assert(owner_switch_command.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
   assert(owner_switch_command.ordered_operations[0]
             .payload.image_materialize.image == &switching_owner_image);
   assert(owner_switch_command.ordered_operations[1].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR);
   assert(owner_switch_command.ordered_operations[1]
             .payload.image_fast_clear.image == &fast_clear_image);
   assert(owner_switch_command.current_zmask_owner.image ==
          &fast_clear_image);
   assert(r3v_native_record_zmask_fast_clear(
             r3v_native_cmd_buffer_to_handle(&owner_switch_command),
             r3v_native_image_to_handle(&switching_owner_image), 0x600000u,
             0x3cu) == VK_SUCCESS);
   assert(owner_switch_command.ordered_operation_count == 4u);
   assert(owner_switch_command.ordered_operations[2].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
   assert(owner_switch_command.ordered_operations[2]
             .payload.image_materialize.image == &fast_clear_image);
   assert(owner_switch_command.ordered_operations[3].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR);
   assert(owner_switch_command.ordered_operations[3]
             .payload.image_fast_clear.image == &switching_owner_image);
   assert(owner_switch_command.current_zmask_owner.image ==
          &switching_owner_image);
   assert(owner_switch_command.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   assert(owner_switch_command.image_states[1].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);

   struct r3v_native_cmd_buffer initialize_fast_clear_owner = {0};
   initialize_fast_clear_owner.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   initialize_fast_clear_owner.vk.base.device = &materialize_device.vk;
   initialize_fast_clear_owner.vk.pool = &pool;
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&initialize_fast_clear_owner),
             r3v_native_image_to_handle(&fast_clear_image)) == VK_SUCCESS);
   assert(initialize_fast_clear_owner.ordered_operation_count == 2u);
   assert(initialize_fast_clear_owner.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
   assert(initialize_fast_clear_owner.ordered_operations[0]
             .payload.image_materialize.image == &switching_owner_image);
   assert(initialize_fast_clear_owner.ordered_operations[1].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_ZMASK_INITIALIZE);
   assert(initialize_fast_clear_owner.ordered_operations[1]
             .payload.image_zmask_initialize.image == &fast_clear_image);
   assert(initialize_fast_clear_owner.current_zmask_owner.image ==
          &fast_clear_image);

   struct r3v_native_image invalid_switch_destination = fast_clear_image;
   invalid_switch_destination.zmask_layout_admitted = false;
   invalid_switch_destination.depth_bound.contract =
      &invalid_switch_destination.depth_contract;
   struct r3v_native_cmd_buffer failed_owner_switch = {0};
   failed_owner_switch.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   failed_owner_switch.vk.base.device = &materialize_device.vk;
   failed_owner_switch.vk.pool = &pool;
   assert(r3v_native_record_zmask_fast_clear(
             r3v_native_cmd_buffer_to_handle(&failed_owner_switch),
             r3v_native_image_to_handle(&invalid_switch_destination),
             0x400000u, 0xa5u) != VK_SUCCESS);
   assert(failed_owner_switch.ib_size_dwords == 0u);
   assert(failed_owner_switch.reference_count == 0u);
   assert(failed_owner_switch.ordered_operation_count == 0u);
   assert(failed_owner_switch.image_state_count == 0u);
   assert(!failed_owner_switch.required_zmask_owner_set);
   assert(!failed_owner_switch.current_zmask_owner_set);

   struct r3v_native_cmd_buffer failed_initialize_switch = {0};
   failed_initialize_switch.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   failed_initialize_switch.vk.base.device = &materialize_device.vk;
   failed_initialize_switch.vk.pool = &pool;
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&failed_initialize_switch),
             r3v_native_image_to_handle(&invalid_switch_destination)) !=
          VK_SUCCESS);
   assert(failed_initialize_switch.ib_size_dwords == 0u);
   assert(failed_initialize_switch.reference_count == 0u);
   assert(failed_initialize_switch.ordered_operation_count == 0u);
   assert(failed_initialize_switch.image_state_count == 0u);
   assert(!failed_initialize_switch.required_zmask_owner_set);
   assert(!failed_initialize_switch.current_zmask_owner_set);

   struct r3v_native_zmask_metadata_state compressed_metadata = {
      .status = R3V_NATIVE_ZMASK_METADATA_COMPRESSED,
      .clear_depth_code = 0x400000u,
      .clear_stencil = 0xa5u,
      .generation = 9u,
   };
   switching_owner_image.committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_COMPRESSED;
   switching_owner_image.committed_submission.zmask_metadata =
      compressed_metadata;
   assert(r3v_native_zmask_owner_from_image(
             &switching_owner_image, &compressed_metadata,
             &materialize_device.zmask_owner) == VK_SUCCESS);
   struct r3v_native_cmd_buffer compressed_owner_initialize = {0};
   compressed_owner_initialize.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   compressed_owner_initialize.vk.base.device = &materialize_device.vk;
   compressed_owner_initialize.vk.pool = &pool;
   assert(r3v_native_record_zmask_initialize(
             r3v_native_cmd_buffer_to_handle(&compressed_owner_initialize),
             r3v_native_image_to_handle(&fast_clear_image)) ==
          VK_ERROR_FEATURE_NOT_PRESENT);
   assert(compressed_owner_initialize.ib_size_dwords == 0u);
   assert(compressed_owner_initialize.reference_count == 0u);
   assert(compressed_owner_initialize.ordered_operation_count == 0u);
   assert(compressed_owner_initialize.image_state_count == 0u);
   materialize_device.zmask_owner = no_owner;

   assert(r3v_native_record_zmask_materialize(
             r3v_native_cmd_buffer_to_handle(&materialize_command),
             r3v_native_image_to_handle(&materialize_image),
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
             &fast_clear_metadata) == VK_SUCCESS);
   assert(materialize_command.ordered_operation_count == 1u);
   assert(materialize_command.ib != NULL &&
          materialize_command.ib_size_dwords != 0u);
   assert(materialize_command.reference_count ==
          R300_ZB_DEPTH_CONTROL_SLOT_COUNT);
   assert(materialize_command.references[0].memory ==
             &materialize_device.zmask_materialize_vertex &&
          materialize_command.references[0].read_domains ==
             RADEON_GEM_DOMAIN_GTT &&
          materialize_command.references[0].write_domain == 0u);
   assert(materialize_command.references[1].memory ==
             &materialize_device.zmask_materialize_color &&
          materialize_command.references[1].read_domains == 0u &&
          materialize_command.references[1].write_domain ==
             RADEON_GEM_DOMAIN_GTT);
   assert(materialize_command.references[2].memory == &materialize_memory &&
          materialize_command.references[2].read_domains ==
             RADEON_GEM_DOMAIN_GTT &&
          materialize_command.references[2].write_domain ==
             RADEON_GEM_DOMAIN_GTT);
   assert(materialize_command.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
   assert(materialize_command.ordered_operations[0].ib_position_dwords ==
          materialize_command.ib_size_dwords);
   assert(materialize_command.ordered_operations[0]
             .payload.image_materialize.image == &materialize_image);
   struct r300_zmask_materialize_plan expected_plan;
   assert(r300_zmask_materialize_prefix(
             &materialize_image.depth_contract.surface,
             &materialize_image.zmask_layout,
             fast_clear_metadata.clear_depth_code,
             fast_clear_metadata.clear_stencil, &expected_plan) == 0);
   assert(r300_zmask_materialize_suffix(&expected_plan) == 0);
   struct r300_zb_depth_control_ib expected_cell;
   assert(r300_zb_depth_zmask_materialize_emit(
             &expected_plan, 6144u, 0u,
             r300_rb3d_colorpitch0_pack_argb8888(64u), &expected_cell) == 0);
   assert(expected_cell.reloc_site_count != 0u);
   assert(materialize_command.ib_size_dwords == expected_cell.ib_size_dwords);
   for (uint32_t word = 0u; word < expected_cell.ib_size_dwords; word++) {
      uint32_t expected_word = expected_cell.ib[word];
      for (uint32_t index = 0u; index < expected_cell.reloc_site_count;
           index++) {
         if (expected_cell.reloc_sites[index].ib_index == word)
            expected_word = expected_cell.reloc_sites[index].slot * 4u;
      }
      assert(materialize_command.ib[word] == expected_word);
   }
   r300_zb_depth_control_release(&expected_cell);
   assert(materialize_command.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(materialize_command.image_states[0]
             .current_zmask_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   assert(materialize_command.current_zmask_owner_set);
   assert(materialize_command.current_zmask_owner.image == NULL);
   assert(materialize_image.committed_submission.representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   assert(materialize_image.committed_submission.zmask_metadata.generation ==
          fast_clear_metadata.generation);
   assert(r3v_native_cmd_buffer_require_ordinary_depth_backing(
             &materialize_command, &materialize_image) == VK_SUCCESS);

   struct r3v_native_cmd_buffer stale_backing_command = {0};
   stale_backing_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   stale_backing_command.vk.base.device = &materialize_device.vk;
   stale_backing_command.vk.pool = &pool;
   const VkResult stale_backing_result = r3v_native_record_depth_image_clear(
      r3v_native_cmd_buffer_to_handle(&stale_backing_command),
      r3v_native_image_to_handle(&materialize_image),
      R300_ZB_COMBINED_CLEAR_ASPECTS, 0x200000u, 0x5au);
   assert(stale_backing_result == VK_SUCCESS);
   assert(stale_backing_command.ib_size_dwords != 0u);
   assert(stale_backing_command.ordered_operation_count == 2u);
   assert(stale_backing_command.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
   assert(stale_backing_command.ordered_operations[1].kind ==
          R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR);
   assert(stale_backing_command.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(stale_backing_command.image_states[0]
             .current_zmask_metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);

   struct r3v_native_cmd_buffer materialize_secondary = {0};
   materialize_secondary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   materialize_secondary.vk.base.device = &materialize_device.vk;
   materialize_secondary.vk.pool = &pool;
   materialize_secondary.vk.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   assert(r3v_native_record_zmask_materialize(
             r3v_native_cmd_buffer_to_handle(&materialize_secondary),
             r3v_native_image_to_handle(&materialize_image),
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
             &fast_clear_metadata) == VK_SUCCESS);
   struct r3v_native_cmd_buffer materialize_primary = {0};
   materialize_primary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   materialize_primary.vk.base.device = &materialize_device.vk;
   materialize_primary.vk.pool = &pool;
   materialize_primary.vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   const struct r3v_native_ordered_operation memory_dependency = {
      .kind = R3V_NATIVE_ORDERED_OPERATION_MEMORY_BARRIER,
   };
   assert(r3v_native_cmd_buffer_append_ordered_operation(
             &materialize_primary, &memory_dependency) == VK_SUCCESS);
   VkCommandBuffer materialize_secondary_handle =
      r3v_native_cmd_buffer_to_handle(&materialize_secondary);
   r3v_CmdExecuteCommands(r3v_native_cmd_buffer_to_handle(&materialize_primary),
                          1u, &materialize_secondary_handle);
   assert(materialize_primary.vk.record_result == VK_SUCCESS);
   assert(r3v_native_cmd_buffer_append_ordered_operation(
             &materialize_primary, &memory_dependency) == VK_SUCCESS);
   assert(materialize_primary.ordered_operation_count == 3u);
   assert(materialize_primary.ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_MEMORY_BARRIER);
   assert(materialize_primary.ordered_operations[1].kind ==
          R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
   assert(materialize_primary.ordered_operations[2].kind ==
          R3V_NATIVE_ORDERED_OPERATION_MEMORY_BARRIER);
   assert(materialize_primary.ib_size_dwords ==
          materialize_secondary.ib_size_dwords);
   assert(materialize_primary.reference_count ==
          R300_ZB_DEPTH_CONTROL_SLOT_COUNT);
   assert(materialize_primary.ordered_operations[1].ib_position_dwords ==
          materialize_primary.ib_size_dwords);
   assert(materialize_primary.image_states[0].current_representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
   assert(materialize_primary.current_zmask_owner.image == NULL);

   const uint32_t materialize_operation_count =
      materialize_command.ordered_operation_count;
   const uint32_t materialize_state_count =
      materialize_command.image_state_count;
   const struct r3v_native_cmd_image_state materialize_state_before_refusal =
      materialize_command.image_states[0];
   assert(r3v_native_record_zmask_materialize(
             r3v_native_cmd_buffer_to_handle(&materialize_command),
             r3v_native_image_to_handle(&materialize_image),
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
             &fast_clear_metadata) != VK_SUCCESS);
   assert(materialize_command.ordered_operation_count ==
          materialize_operation_count);
   assert(materialize_command.image_state_count == materialize_state_count);
   assert(memcmp(&materialize_command.image_states[0],
                 &materialize_state_before_refusal,
                 sizeof(materialize_state_before_refusal)) == 0);

   struct r3v_native_cmd_buffer conflicting_reference_command = {0};
   conflicting_reference_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   conflicting_reference_command.vk.base.device = &materialize_device.vk;
   conflicting_reference_command.vk.pool = &pool;
   struct r3v_native_memory conflicting_memory = materialize_memory;
   conflicting_memory.bo.handle =
      materialize_device.zmask_materialize_vertex.bo.handle;
   struct r3v_native_image conflicting_image = materialize_image;
   conflicting_image.memory = &conflicting_memory;
   conflicting_image.depth_bound.contract = &conflicting_image.depth_contract;
   assert(r3v_native_record_zmask_materialize(
             r3v_native_cmd_buffer_to_handle(&conflicting_reference_command),
             r3v_native_image_to_handle(&conflicting_image),
             R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
             &fast_clear_metadata) != VK_SUCCESS);
   assert(conflicting_reference_command.ib == NULL);
   assert(conflicting_reference_command.ib_size_dwords == 0u);
   assert(conflicting_reference_command.references == NULL);
   assert(conflicting_reference_command.reference_count == 0u);
   assert(conflicting_reference_command.ordered_operation_count == 0u);
   assert(conflicting_reference_command.image_state_count == 0u);
   assert(!conflicting_reference_command.required_zmask_owner_set);
   assert(!conflicting_reference_command.current_zmask_owner_set);
   assert(conflicting_image.committed_submission.representation ==
          R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
   assert(conflicting_image.committed_submission.zmask_metadata.generation ==
          fast_clear_metadata.generation);
   r3v_native_cmd_buffer_release_recording(&conflicting_reference_command);
   r3v_native_cmd_buffer_release_recording(&compressed_owner_initialize);
   r3v_native_cmd_buffer_release_recording(&failed_initialize_switch);
   r3v_native_cmd_buffer_release_recording(&failed_owner_switch);
   r3v_native_cmd_buffer_release_recording(&initialize_fast_clear_owner);
   r3v_native_cmd_buffer_release_recording(&owner_switch_command);
   r3v_native_cmd_buffer_release_recording(&fast_clear_primary);
   r3v_native_cmd_buffer_release_recording(&fast_clear_secondary);
   r3v_native_cmd_buffer_release_recording(&fast_clear_command);
   r3v_native_cmd_buffer_release_recording(&materialize_command);
   r3v_native_cmd_buffer_release_recording(&stale_backing_command);
   r3v_native_cmd_buffer_release_recording(&materialize_primary);
   r3v_native_cmd_buffer_release_recording(&materialize_secondary);
   r3v_native_cmd_buffer_release_recording(&initialized_switch_command);
   r3v_native_cmd_buffer_release_recording(&initialize_primary);
   r3v_native_cmd_buffer_release_recording(&initialize_secondary);
   r3v_native_cmd_buffer_release_recording(&initialize_command);
   r3v_native_cmd_buffer_release_recording(&representation_command);
   r3v_native_cmd_buffer_release_recording(&command);
   return 0;
}
