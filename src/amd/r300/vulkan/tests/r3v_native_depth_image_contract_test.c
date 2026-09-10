/*
 * SPDX-License-Identifier: MIT
 */

#include "../r3v_native.h"

#include "vk_alloc.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static struct r3v_native_depth_image_create_info
valid_info(void)
{
   return (struct r3v_native_depth_image_create_info){
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
}

int
main(void)
{
   struct r3v_native_depth_image_create_info info = valid_info();
   struct r3v_native_depth_image_contract contract;
   assert(r3v_native_depth_image_contract_init(&info, &contract) == 0);
   assert(contract.logical_extent.width == 64u &&
          contract.logical_extent.height == 64u);
   assert(contract.surface.address_resolver != NULL);
   assert(contract.surface.logical_image_readback);
   assert(contract.surface_base_bytes == 2048u);
   assert(!contract.physical_bo_placement_known);

   struct r3v_native_depth_image_contract before = contract;
   info.format = VK_FORMAT_D16_UNORM;
   assert(r3v_native_depth_image_contract_init(&info, &contract) == -EINVAL);
   info.format = VK_FORMAT_D24_UNORM_S8_UINT;
   info.image_type = VK_IMAGE_TYPE_1D;
   assert(r3v_native_depth_image_contract_init(&info, &contract) == -EINVAL);
   info.image_type = VK_IMAGE_TYPE_2D;
   info.extent.depth = 2u;
   assert(r3v_native_depth_image_contract_init(&info, &contract) == -EINVAL);
   info.extent.depth = 1u;
   info.pci_subsystem_device = 0x0000u;
   assert(r3v_native_depth_image_contract_init(&info, &contract) == -EINVAL);
   assert(memcmp(&contract, &before, sizeof(contract)) == 0);

   info.pci_subsystem_device = 0x022au;
   info.extent = (VkExtent3D){1u, 1u, 1u};
   assert(r3v_native_depth_image_contract_init(&info, &contract) == 0);
   assert(contract.surface.width == 64u && contract.surface.height == 64u);
   assert(contract.logical_extent.width == 1u &&
          contract.logical_extent.height == 1u);
   info.extent = (VkExtent3D){64u, 63u, 1u};
   assert(r3v_native_depth_image_contract_init(&info, &contract) == 0);
   assert(contract.surface.width == 64u && contract.surface.height == 64u);
   info.extent = (VkExtent3D){65u, 1u, 1u};
   assert(r3v_native_depth_image_contract_init(&info, &contract) == -EINVAL);
   info.extent = (VkExtent3D){1u, 65u, 1u};
   assert(r3v_native_depth_image_contract_init(&info, &contract) == -EINVAL);
   info.extent = (VkExtent3D){64u, 64u, 1u};
   assert(r3v_native_depth_image_contract_init(&info, &contract) == 0);

   struct r3v_native_depth_image_bound bound;
   assert(r3v_native_depth_image_contract_bind(&before, 0u,
                                                before.binding_bytes,
                                                &bound) == 0);
   assert(bound.surface_base_bytes == 2048u);
   assert(r3v_native_depth_image_contract_bind(&before, 4096u,
                                                before.binding_bytes + 4096u,
                                                &bound) == 0);
   assert(bound.binding_offset_bytes == 4096u &&
          bound.surface_base_bytes == 6144u);
   assert(r3v_native_depth_image_contract_bind(&before, 0u,
                                                before.binding_bytes,
                                                &bound) == 0);
   assert(r3v_native_depth_image_contract_bind(&before, 1u,
                                                before.binding_bytes + 1u,
                                                &bound) == -EINVAL);
   assert(r3v_native_depth_image_contract_bind(&before, 0u,
                                                before.binding_bytes - 1u,
                                                &bound) == -EINVAL);

   struct r300_zb_tile_copy_mapping mapping;
   assert(r3v_native_depth_image_contract_copy_mapping(&bound, 0u, 0u,
                                                        &mapping) == 0);
   assert(mapping.tile_offset_bytes == 2048u);
   assert(mapping.macro_width == 2u && mapping.macro_height == 4u);
   struct r300_zb_tile_copy_mapping destination;
   struct r300_zb_tile_copy_plan plan;
   assert(r3v_native_depth_image_contract_copy_mapping(&bound, 1u, 0u,
                                                        &destination) == 0);
   assert(r300_zb_tile_copy_plan_build(
             &(struct r300_zb_tile_copy_request){
                .source = mapping,
                .destination = destination,
                .same_format = true,
             }, &plan) == R300_ZB_TILE_COPY_OK);
   assert(plan.segment_count == 2u);
   assert(plan.segments[0].byte_count == 1024u &&
          plan.segments[1].byte_count == 1024u);
   assert(r3v_native_depth_image_contract_bind(&before, 4096u,
                                                before.binding_bytes + 4096u,
                                                &bound) == 0);
   assert(r3v_native_depth_image_contract_copy_mapping(&bound, 0u, 0u,
                                                        &mapping) == 0);
   assert(mapping.tile_offset_bytes == 6144u);
   assert(r3v_native_depth_image_contract_bind(&before, 0u,
                                                before.binding_bytes,
                                                &bound) == 0);
   assert(r3v_native_depth_image_contract_copy_mapping(&bound, 0u, 3u,
                                                        &mapping) == 0);
   struct r300_zb_depth_address_coordinate coordinate;
   assert(before.surface.address_resolver->coordinate(
             &before.surface, before.surface_base_bytes,
             before.surface_base_bytes + before.layout.storage_bytes,
             before.surface_base_bytes + before.layout.storage_bytes - 4u,
             &coordinate) == 0);
   assert(coordinate.region == R300_ZB_DEPTH_ADDRESS_PADDING);
   assert(before.surface.address_resolver->coordinate(
             &before.surface, before.surface_base_bytes,
             before.surface_base_bytes + before.layout.storage_bytes,
             before.surface_base_bytes, &coordinate) == 0);
   assert(coordinate.region == R300_ZB_DEPTH_ADDRESS_LOGICAL);
   assert(r3v_native_depth_image_contract_copy_mapping(&bound, 2u, 0u,
                                                        &mapping) == -EINVAL);
   assert(r3v_native_depth_image_contract_copy_mapping(&bound, 0u, 4u,
                                                        &mapping) == -EINVAL);

   /* The clear recorder installs one GPU RB2D stream and one exact BO
    * reference.  Mutating either identity must invalidate the geometry. */
   struct r3v_native_memory memory = {0};
   memory.bo.handle = 17u;
   memory.bo.size = before.binding_bytes;
   struct r3v_native_image image = {0};
   image.base.type = VK_OBJECT_TYPE_IMAGE;
   image.memory = &memory;
   image.depth_family = true;
   image.depth_contract = before;
   image.committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED;
   assert(r3v_native_depth_image_contract_bind(&before, 0u,
                                                before.binding_bytes,
                                                &image.depth_bound) == 0);
   struct r3v_native_cmd_buffer command = {0};
   struct vk_command_pool pool = {0};
   pool.alloc = *vk_default_allocator();
   command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   command.vk.pool = &pool;
   struct r3v_native_image stale_backing_image = image;
   stale_backing_image.committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR;
   stale_backing_image.committed_submission.zmask_metadata =
      (struct r3v_native_zmask_metadata_state){
         .status = R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR,
         .clear_depth_code = 0x400000u,
         .clear_stencil = 0x5au,
         .generation = 1u,
      };
   assert(r3v_native_record_depth_image_clear(
             r3v_native_cmd_buffer_to_handle(&command),
             r3v_native_image_to_handle(&stale_backing_image),
             R300_ZB_COMBINED_CLEAR_ASPECTS, 0x123456u, 0xa5u) ==
          VK_ERROR_INITIALIZATION_FAILED);
   assert(command.ib_size_dwords == 0u &&
          command.ordered_operation_count == 0u &&
          command.image_state_count == 0u);
   assert(r3v_native_record_depth_image_clear(
             r3v_native_cmd_buffer_to_handle(&command),
             r3v_native_image_to_handle(&image),
             R300_ZB_COMBINED_CLEAR_ASPECTS, 0x123456u, 0xa5u) == VK_SUCCESS);
   assert(r3v_native_depth_image_clear_geometry_valid(&command));
   command.ib[0] ^= 1u;
   assert(!r3v_native_depth_image_clear_geometry_valid(&command));
   command.ib[0] ^= 1u;
   command.references[0].read_domains ^= RADEON_GEM_DOMAIN_GTT;
   assert(!r3v_native_depth_image_clear_geometry_valid(&command));
   r3v_native_cmd_buffer_release_ib(&command);

   /* Logical load clears expand to one RB2D packet per resolved pixel.  The
    * packet count proves 1x1 and 64x63 extents stay inside their logical
    * domains instead of inheriting the physical 64x64 envelope. */
   struct r3v_native_device device = {0};
   device.vk.base.type = VK_OBJECT_TYPE_DEVICE;
   struct vk_command_pool logical_pool = {0};
   logical_pool.alloc = *vk_default_allocator();
   struct r3v_native_memory logical_memory = {0};
   logical_memory.bo.handle = 31u;
   struct r3v_native_cmd_buffer logical_command = {0};
   logical_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   logical_command.vk.base.device = &device.vk;
   logical_command.vk.pool = &logical_pool;

   info.extent = (VkExtent3D){1u, 1u, 1u};
   struct r3v_native_depth_image_contract logical_contract;
   assert(r3v_native_depth_image_contract_init(&info, &logical_contract) == 0);
   logical_memory.bo.size = logical_contract.binding_bytes;
   struct r3v_native_image logical_image = {0};
   logical_image.base.type = VK_OBJECT_TYPE_IMAGE;
   logical_image.memory = &logical_memory;
   logical_image.depth_family = true;
   logical_image.depth_contract = logical_contract;
   logical_image.committed_submission.representation =
      R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED;
   assert(r3v_native_depth_image_contract_bind(
             &logical_contract, 0u, logical_contract.binding_bytes,
             &logical_image.depth_bound) == 0);
   assert(r3v_native_record_depth_image_clear_logical_rect(
             r3v_native_cmd_buffer_to_handle(&logical_command),
             r3v_native_image_to_handle(&logical_image), 1u, 0u, 1u, 1u,
             R300_ZB_COMBINED_CLEAR_ASPECTS, 0x123456u, 0xa5u) ==
          VK_ERROR_INITIALIZATION_FAILED);
   assert(logical_command.ib == NULL && logical_command.ib_size_dwords == 0u &&
          logical_command.references == NULL && logical_command.reference_count == 0u &&
          logical_command.ordered_operation_count == 0u);
   assert(r3v_native_record_depth_image_clear_logical(
             r3v_native_cmd_buffer_to_handle(&logical_command),
             r3v_native_image_to_handle(&logical_image),
             R300_ZB_COMBINED_CLEAR_ASPECTS, 0x123456u, 0xa5u) == VK_SUCCESS);
   assert(logical_command.ordered_operation_count == 1u);
   assert(logical_command.ib_size_dwords == R300_RB2D_FILL_DWORDS(1u));
   r3v_native_cmd_buffer_release_ib(&logical_command);
   r3v_native_cmd_buffer_release_recording(&logical_command);

   logical_command = (struct r3v_native_cmd_buffer){0};
   logical_command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   logical_command.vk.base.device = &device.vk;
   logical_command.vk.pool = &logical_pool;
   info.extent = (VkExtent3D){64u, 63u, 1u};
   assert(r3v_native_depth_image_contract_init(&info, &logical_contract) == 0);
   logical_memory.bo.size = logical_contract.binding_bytes;
   logical_image.depth_contract = logical_contract;
   assert(r3v_native_depth_image_contract_bind(
             &logical_contract, 0u, logical_contract.binding_bytes,
             &logical_image.depth_bound) == 0);
   VkResult logical_result = r3v_native_record_depth_image_clear_logical_rect(
             r3v_native_cmd_buffer_to_handle(&logical_command),
             r3v_native_image_to_handle(&logical_image), 0u, 0u, 64u, 63u,
             R300_ZB_COMBINED_CLEAR_ASPECTS, 0x654321u, 0x5au);
   assert(logical_result == VK_SUCCESS);
   assert(logical_command.ordered_operation_count == 1u);
   assert(logical_command.ib_size_dwords <
          64u * 63u * R300_RB2D_FILL_DWORDS(1u));
   r3v_native_cmd_buffer_release_ib(&logical_command);
   r3v_native_cmd_buffer_release_recording(&logical_command);

   /* Secondary replay preserves a partial logical clear rectangle.  A
    * replay that falls back to the full-image helper would clear the
    * physical envelope and lose the recorded position. */
   struct r3v_native_cmd_buffer secondary = {0};
   secondary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   secondary.vk.base.device = &device.vk;
   secondary.vk.pool = &logical_pool;
   secondary.vk.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
   assert(r3v_native_record_depth_image_clear_logical_rect(
             r3v_native_cmd_buffer_to_handle(&secondary),
             r3v_native_image_to_handle(&logical_image), 7u, 9u, 5u, 3u,
             R300_ZB_COMBINED_CLEAR_ASPECTS, 0x234567u, 0x3cu) ==
          VK_SUCCESS);
   assert(secondary.ordered_operation_count == 1u);

   struct r3v_native_cmd_buffer primary = {0};
   primary.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
   primary.vk.base.device = &device.vk;
   primary.vk.pool = &logical_pool;
   primary.vk.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   VkCommandBuffer secondary_handle =
      r3v_native_cmd_buffer_to_handle(&secondary);
   r3v_CmdExecuteCommands(r3v_native_cmd_buffer_to_handle(&primary), 1u,
                          &secondary_handle);
   assert(primary.vk.record_result == VK_SUCCESS);
   assert(primary.ordered_operation_count == 1u);
   const struct r3v_native_ordered_operation *replayed =
      &primary.ordered_operations[0];
   assert(replayed->kind == R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR);
   assert(replayed->payload.rb2d_depth_clear.image == &logical_image);
   assert(replayed->payload.rb2d_depth_clear.x == 7u);
   assert(replayed->payload.rb2d_depth_clear.y == 9u);
   assert(replayed->payload.rb2d_depth_clear.width == 5u);
   assert(replayed->payload.rb2d_depth_clear.height == 3u);
   assert(replayed->payload.rb2d_depth_clear.aspect_mask ==
          R300_ZB_COMBINED_CLEAR_ASPECTS);
   assert(replayed->payload.rb2d_depth_clear.depth_code == 0x234567u);
   assert(replayed->payload.rb2d_depth_clear.stencil == 0x3cu);
   r3v_native_cmd_buffer_release_ib(&primary);
   r3v_native_cmd_buffer_release_recording(&primary);
   r3v_native_cmd_buffer_release_ib(&secondary);
   r3v_native_cmd_buffer_release_recording(&secondary);
   return 0;
}
