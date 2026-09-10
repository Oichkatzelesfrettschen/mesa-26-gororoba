/*
 * SPDX-License-Identifier: MIT
 */

#include "../r3v_native.h"

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
   assert(r3v_native_depth_image_contract_bind(&before, 0u,
                                                before.binding_bytes,
                                                &image.depth_bound) == 0);
   struct r3v_native_cmd_buffer command = {0};
   command.vk.base.type = VK_OBJECT_TYPE_COMMAND_BUFFER;
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
   return 0;
}
