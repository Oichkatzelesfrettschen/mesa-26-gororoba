/* SPDX-License-Identifier: MIT */

#include "radeon_drm_vk_device.h"

#include "util/hash_table.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <xf86drm.h>
#include <radeon_drm.h>

int
radeon_drm_vk_device_init(struct radeon_drm_vk_device *device, int fd,
                          const struct radeon_drm_vk_ioctl_ops *ops)
{
   device->fd = fd;
   device->ops = ops != NULL ? ops : &radeon_drm_vk_ioctl_ops_drm;
   atomic_init(&device->cache_sync_count, 0);
   atomic_init(&device->submit_boundary_sync_count, 0);
   device->cache_event_sequence = 0;
   device->cache_sync_last = (struct radeon_drm_vk_cache_event){ 0 };
   device->bo_close_last = (struct radeon_drm_vk_close_event){ 0 };
   device->shared_bo_reference_counts =
      _mesa_hash_table_create(NULL, _mesa_hash_pointer,
                              _mesa_key_pointer_equal);
   if (device->shared_bo_reference_counts == NULL) {
      return -ENOMEM;
   }
   if (mtx_init(&device->shared_bo_mutex, mtx_plain) != thrd_success) {
      _mesa_hash_table_destroy(device->shared_bo_reference_counts, NULL);
      device->shared_bo_reference_counts = NULL;
      return -ENOMEM;
   }
   if (mtx_init(&device->cache_event_mutex, mtx_plain) != thrd_success) {
      mtx_destroy(&device->shared_bo_mutex);
      _mesa_hash_table_destroy(device->shared_bo_reference_counts, NULL);
      device->shared_bo_reference_counts = NULL;
      return -ENOMEM;
   }
   return 0;
}

void
radeon_drm_vk_device_finish(struct radeon_drm_vk_device *device)
{
   mtx_destroy(&device->cache_event_mutex);
   mtx_destroy(&device->shared_bo_mutex);
   _mesa_hash_table_destroy(device->shared_bo_reference_counts, NULL);
   device->shared_bo_reference_counts = NULL;
   device->fd = -1;
}

int
radeon_drm_vk_device_getparam(struct radeon_drm_vk_device *device,
                              uint32_t param, uint32_t *value)
{
   /* The kernel writes through the value pointer, so the wrapper keeps the
    * out-parameter alive across the ioctl.
    */
   int kernel_value = 0;
   struct drm_radeon_getparam arguments = {
      .param = (int)param,
      .value = &kernel_value,
   };
   int result = device->ops->command_write_read(device->fd,
                                                DRM_RADEON_GETPARAM,
                                                &arguments,
                                                sizeof(arguments));
   if (result == 0) {
      *value = (uint32_t)kernel_value;
   }
   return result;
}

enum radeon_drm_vk_info_layout {
   RADEON_DRM_VK_INFO_INVALID = 0,
   RADEON_DRM_VK_INFO_U32_OUTPUT,
   RADEON_DRM_VK_INFO_U32_INPUT_OUTPUT,
   RADEON_DRM_VK_INFO_U64_OUTPUT,
   RADEON_DRM_VK_INFO_U32_16_OUTPUT,
   RADEON_DRM_VK_INFO_U32_32_OUTPUT,
};

static enum radeon_drm_vk_info_layout
radeon_drm_vk_info_layout(uint32_t request)
{
   switch (request) {
   case RADEON_INFO_TIMESTAMP:
   case RADEON_INFO_NUM_BYTES_MOVED:
   case RADEON_INFO_VRAM_USAGE:
   case RADEON_INFO_GTT_USAGE:
      return RADEON_DRM_VK_INFO_U64_OUTPUT;
   case RADEON_INFO_SI_TILE_MODE_ARRAY:
      return RADEON_DRM_VK_INFO_U32_32_OUTPUT;
   case RADEON_INFO_CIK_MACROTILE_MODE_ARRAY:
      return RADEON_DRM_VK_INFO_U32_16_OUTPUT;
   case RADEON_INFO_CRTC_FROM_ID:
   case RADEON_INFO_WANT_HYPERZ:
   case RADEON_INFO_WANT_CMASK:
   case RADEON_INFO_RING_WORKING:
   case RADEON_INFO_READ_REG:
      return RADEON_DRM_VK_INFO_U32_INPUT_OUTPUT;
   case RADEON_INFO_DEVICE_ID:
   case RADEON_INFO_NUM_GB_PIPES:
   case RADEON_INFO_NUM_Z_PIPES:
   case RADEON_INFO_ACCEL_WORKING:
   case RADEON_INFO_ACCEL_WORKING2:
   case RADEON_INFO_TILING_CONFIG:
   case RADEON_INFO_CLOCK_CRYSTAL_FREQ:
   case RADEON_INFO_NUM_BACKENDS:
   case RADEON_INFO_NUM_TILE_PIPES:
   case RADEON_INFO_FUSION_GART_WORKING:
   case RADEON_INFO_BACKEND_MAP:
   case RADEON_INFO_VA_START:
   case RADEON_INFO_IB_VM_MAX_SIZE:
   case RADEON_INFO_MAX_PIPES:
   case RADEON_INFO_MAX_SE:
   case RADEON_INFO_MAX_SH_PER_SE:
   case RADEON_INFO_FASTFB_WORKING:
   case RADEON_INFO_SI_CP_DMA_COMPUTE:
   case RADEON_INFO_SI_BACKEND_ENABLED_MASK:
   case RADEON_INFO_MAX_SCLK:
   case RADEON_INFO_VCE_FW_VERSION:
   case RADEON_INFO_VCE_FB_VERSION:
   case RADEON_INFO_ACTIVE_CU_COUNT:
   case RADEON_INFO_CURRENT_GPU_TEMP:
   case RADEON_INFO_CURRENT_GPU_SCLK:
   case RADEON_INFO_CURRENT_GPU_MCLK:
   case RADEON_INFO_VA_UNMAP_WORKING:
   case RADEON_INFO_GPU_RESET_COUNTER:
      return RADEON_DRM_VK_INFO_U32_OUTPUT;
   default:
      return RADEON_DRM_VK_INFO_INVALID;
   }
}

static size_t
radeon_drm_vk_info_layout_size(enum radeon_drm_vk_info_layout layout)
{
   switch (layout) {
   case RADEON_DRM_VK_INFO_U32_OUTPUT:
   case RADEON_DRM_VK_INFO_U32_INPUT_OUTPUT:
      return sizeof(uint32_t);
   case RADEON_DRM_VK_INFO_U64_OUTPUT:
      return sizeof(uint64_t);
   case RADEON_DRM_VK_INFO_U32_16_OUTPUT:
      return 16 * sizeof(uint32_t);
   case RADEON_DRM_VK_INFO_U32_32_OUTPUT:
      return 32 * sizeof(uint32_t);
   case RADEON_DRM_VK_INFO_INVALID:
      return 0;
   }
   return 0;
}

static int
radeon_drm_vk_device_info_sized(struct radeon_drm_vk_device *device,
                                uint32_t request, void *value,
                                size_t value_size)
{
   const enum radeon_drm_vk_info_layout layout =
      radeon_drm_vk_info_layout(request);
   if (value == NULL || layout == RADEON_DRM_VK_INFO_INVALID ||
       radeon_drm_vk_info_layout_size(layout) != value_size)
      return -EINVAL;

   union {
      uint32_t words[32];
      uint64_t alignment;
   } kernel_value = {0};
   if (layout == RADEON_DRM_VK_INFO_U32_INPUT_OUTPUT)
      memcpy(kernel_value.words, value, sizeof(uint32_t));
   struct drm_radeon_info arguments = {
      .request = request,
      .value = (uintptr_t)kernel_value.words,
   };
   int result = device->ops->command_write_read(device->fd, DRM_RADEON_INFO,
                                                &arguments,
                                                sizeof(arguments));
   if (result == 0)
      memcpy(value, kernel_value.words, value_size);
   return result;
}

int
radeon_drm_vk_device_info_u32(struct radeon_drm_vk_device *device,
                              uint32_t request, uint32_t *value)
{
   const enum radeon_drm_vk_info_layout layout =
      radeon_drm_vk_info_layout(request);
   if (layout != RADEON_DRM_VK_INFO_U32_OUTPUT &&
       layout != RADEON_DRM_VK_INFO_U32_INPUT_OUTPUT)
      return -EINVAL;

   return radeon_drm_vk_device_info_sized(device, request, value,
                                          sizeof(*value));
}

int
radeon_drm_vk_device_info_u64(struct radeon_drm_vk_device *device,
                              uint32_t request, uint64_t *value)
{
   if (radeon_drm_vk_info_layout(request) != RADEON_DRM_VK_INFO_U64_OUTPUT)
      return -EINVAL;

   return radeon_drm_vk_device_info_sized(device, request, value,
                                          sizeof(*value));
}

int
radeon_drm_vk_device_info_u32_array(
   struct radeon_drm_vk_device *device, uint32_t request, uint32_t *values,
   size_t value_count)
{
   const enum radeon_drm_vk_info_layout layout =
      radeon_drm_vk_info_layout(request);
   if (layout != RADEON_DRM_VK_INFO_U32_16_OUTPUT &&
       layout != RADEON_DRM_VK_INFO_U32_32_OUTPUT)
      return -EINVAL;
   if (value_count > SIZE_MAX / sizeof(*values))
      return -EINVAL;

   return radeon_drm_vk_device_info_sized(
      device, request, values, value_count * sizeof(*values));
}
