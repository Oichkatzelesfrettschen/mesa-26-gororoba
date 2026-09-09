/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_native_depth_image_contract.h"

#include <errno.h>

#define R3V_RS485M_VENDOR 0x1002u
#define R3V_RS485M_DEVICE 0x5974u
#define R3V_RS485M_SUBSYSTEM_VENDOR 0x1028u
#define R3V_RS485M_SUBSYSTEM_DEVICE 0x022au
#define R3V_DEPTH_SURFACE_BASE_BYTES 2048u
#define R3V_DEPTH_BINDING_ALIGNMENT 4096u

int
r3v_native_depth_image_contract_init(
   const struct r3v_native_depth_image_create_info *info,
   struct r3v_native_depth_image_contract *out)
{
   if (info == NULL || out == NULL ||
       info->pci_vendor != R3V_RS485M_VENDOR ||
       info->pci_device != R3V_RS485M_DEVICE ||
       info->pci_subsystem_vendor != R3V_RS485M_SUBSYSTEM_VENDOR ||
       info->pci_subsystem_device != R3V_RS485M_SUBSYSTEM_DEVICE ||
       info->format != VK_FORMAT_D24_UNORM_S8_UINT ||
       info->image_type != VK_IMAGE_TYPE_2D || info->extent.width != 64u ||
       info->extent.height != 64u || info->extent.depth != 1u ||
       info->mip_levels != 1u ||
       info->array_layers != 1u || info->samples != 1u || !info->optimal_tiling ||
       info->compressed)
      return -EINVAL;

   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_layout_compute(surface, R3V_DEPTH_SURFACE_BASE_BYTES,
                                    &layout) != 0)
      return -EINVAL;

   struct r3v_native_depth_image_contract candidate = {
      .surface = *surface,
      .layout = layout,
      .surface_base_bytes = layout.base_offset_bytes,
      /* Match r3v_native_zb_depth_surface_bytes: the extra tail observes
       * writes beyond both the tiled storage and its suffix guard. */
      .binding_bytes = layout.total_bytes + 4096u,
      .physical_bo_placement_known = false,
   };
   *out = candidate;
   return 0;
}

int
r3v_native_depth_image_contract_bind(
   const struct r3v_native_depth_image_contract *contract,
   uint64_t binding_offset, uint64_t bo_bytes,
   struct r3v_native_depth_image_bound *out)
{
   if (contract == NULL || out == NULL ||
       binding_offset % R3V_DEPTH_BINDING_ALIGNMENT != 0u ||
       binding_offset > bo_bytes ||
       contract->binding_bytes > bo_bytes - binding_offset)
      return -EINVAL;

   const uint64_t surface_base = binding_offset + contract->surface_base_bytes;
   if (surface_base < binding_offset || surface_base > bo_bytes)
      return -ERANGE;
   *out = (struct r3v_native_depth_image_bound){
      .contract = contract,
      .binding_offset_bytes = binding_offset,
      .surface_base_bytes = surface_base,
      .bo_bytes = bo_bytes,
   };
   return 0;
}

int
r3v_native_depth_image_contract_copy_mapping(
   const struct r3v_native_depth_image_bound *bound,
   uint32_t tile_x, uint32_t tile_y,
   struct r300_zb_tile_copy_mapping *out)
{
   if (bound == NULL || bound->contract == NULL || out == NULL)
      return -EINVAL;
   const struct r3v_native_depth_image_contract *contract = bound->contract;
   if (contract->layout.macrotile_width == 0u ||
       contract->layout.macrotile_height == 0u ||
       contract->surface.width == 0u || contract->surface.height == 0u ||
       contract->surface.pitch_pixels < contract->surface.width)
      return -EINVAL;

   const uint32_t macro_width =
      (contract->surface.width + contract->layout.macrotile_width - 1u) /
      contract->layout.macrotile_width;
   const uint32_t macro_height =
      (contract->surface.height + contract->layout.macrotile_height - 1u) /
      contract->layout.macrotile_height;
   if (tile_x >= macro_width || tile_y >= macro_height)
      return -EINVAL;
   const uint64_t offset = bound->surface_base_bytes +
                           (uint64_t)tile_y * contract->layout.pitch_bytes *
                              contract->layout.macrotile_height +
                           (uint64_t)tile_x * R300_ZB_MACROTILE_BYTES;
   const uint64_t storage_end = bound->surface_base_bytes +
                                contract->layout.storage_bytes;
   if (storage_end < bound->surface_base_bytes ||
       offset < bound->surface_base_bytes || offset > storage_end ||
       offset > bound->bo_bytes ||
       R300_ZB_MACROTILE_BYTES > storage_end - offset ||
       R300_ZB_MACROTILE_BYTES > bound->bo_bytes - offset)
      return -ERANGE;

   *out = (struct r300_zb_tile_copy_mapping){
      .tile_offset_bytes = offset,
      .buffer_bytes = bound->bo_bytes,
      .macro_x = tile_x,
      .macro_y = tile_y,
      .macro_width = macro_width,
      .macro_height = macro_height,
      .macro_pitch = contract->surface.pitch_pixels /
                     contract->layout.macrotile_width,
   };
   return 0;
}
