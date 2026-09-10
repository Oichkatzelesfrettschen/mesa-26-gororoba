/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_NATIVE_DEPTH_IMAGE_CONTRACT_H
#define R3V_NATIVE_DEPTH_IMAGE_CONTRACT_H

#include "amd/r300/common/r300_zb_depth_layout.h"
#include "amd/r300/common/r300_zb_tile_copy.h"

#include <vulkan/vulkan_core.h>

#include <stdbool.h>
#include <stdint.h>

struct r3v_native_depth_image_create_info {
   uint32_t pci_vendor;
   uint32_t pci_device;
   uint32_t pci_subsystem_vendor;
   uint32_t pci_subsystem_device;
   VkFormat format;
   VkImageType image_type;
   VkExtent3D extent;
   uint32_t mip_levels;
   uint32_t array_layers;
   uint32_t samples;
   bool optimal_tiling;
   bool compressed;
};

struct r3v_native_depth_image_contract {
   struct r300_zb_depth_surface surface;
   struct r300_zb_depth_layout layout;
   /* The RS485M allocation keeps the qualified 64x64 physical surface.
    * Vulkan bounds each image operation to the extent selected at creation. */
   VkExtent3D logical_extent;
   uint64_t surface_base_bytes;
   uint64_t binding_bytes;
   bool physical_bo_placement_known;
};

struct r3v_native_depth_image_bound {
   const struct r3v_native_depth_image_contract *contract;
   uint64_t binding_offset_bytes;
   uint64_t surface_base_bytes;
   uint64_t bo_bytes;
};

int r3v_native_depth_image_contract_init(
   const struct r3v_native_depth_image_create_info *info,
   struct r3v_native_depth_image_contract *out);

int r3v_native_depth_image_contract_bind(
   const struct r3v_native_depth_image_contract *contract,
   uint64_t binding_offset, uint64_t bo_bytes,
   struct r3v_native_depth_image_bound *out);

int r3v_native_depth_image_contract_copy_mapping(
   const struct r3v_native_depth_image_bound *bound,
   uint32_t tile_x, uint32_t tile_y,
   struct r300_zb_tile_copy_mapping *out);

#endif
