/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V image surface: the qualified linear render-target family
 * with its identity view, and the transfer family, which executes one
 * linear layout under either VK_IMAGE_TILING_LINEAR or
 * VK_IMAGE_TILING_OPTIMAL.
 */

#include "r3v_native.h"

#include "r3v_entrypoints.h"

#include "vk_log.h"
#include "vk_util.h"

#include <assert.h>
#include <string.h>

/* Creation admits two families over one common shape -- 2D, one mip,
 * one layer, one sample, exclusive sharing. The render family carries
 * color-attachment usage alone at any extent inside the 64x64 maximum
 * over the fixed 64-pixel row pitch, tiled VK_IMAGE_TILING_LINEAR
 * only; readback of the rendered pixels rides the host mapping of the
 * bound memory. The transfer family carries transfer usage alone at
 * any extent inside 2048 per axis over a width-derived 64-byte-aligned
 * pitch, admitting either VK_IMAGE_TILING_LINEAR or
 * VK_IMAGE_TILING_OPTIMAL as the same executing linear span; its
 * copies execute through host mappings, and the attachment paths never
 * see it because usage without the color-attachment bit admits no
 * view. Usage mixing the families refuses: a render target the copy
 * commands could write would put two writers on the qualified cell's
 * output. Every other shape refuses with a cleared handle, so no
 * image exists whose lowering the implementation cannot record.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateImage(VkDevice _device, const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);

   *pImage = VK_NULL_HANDLE;

   /* VK_IMAGE_CREATE_ALIAS_BIT admits the transfer family alone.  The
    * flag's rule is that images created with identical parameters over
    * one memory range read that range's contents consistently, and the
    * family's linear layout makes the aliasing window exact:
    * row_pitch_bytes * height from the bind offset, the same footprint
    * r3v_GetImageMemoryRequirements reports, so two such images over one
    * offset cover the same bytes texel for texel.  The render family
    * reports a required dedicated allocation and binds at offset zero
    * alone (the `!image->transfer_family && memoryOffset != 0` refusal
    * in r3v_BindImageMemory), so its window starts where the allocation
    * starts and a second render image over it would need the same base;
    * the flag refuses there.  The transfer family's copies move bytes
    * through host mappings of the bound allocation and the queue
    * completes each submission before vkQueueSubmit returns
    * (r3v_native_transfer.c, r3v_native_queue.c; rg --fixed-strings
    * memory_offset src/amd/r300/vulkan/), so a read through one alias
    * observes every write the record order places before it.
    */
   const uint32_t texel_bytes =
      r3v_native_transfer_texel_bytes(pCreateInfo->format);
   if ((pCreateInfo->flags & ~(VkImageCreateFlags)VK_IMAGE_CREATE_ALIAS_BIT) ||
       pCreateInfo->imageType != VK_IMAGE_TYPE_2D || texel_bytes == 0 ||
       pCreateInfo->extent.width < 1 || pCreateInfo->extent.height < 1 ||
       pCreateInfo->extent.depth != 1 || pCreateInfo->mipLevels != 1 ||
       pCreateInfo->arrayLayers != 1 ||
       pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT ||
       (pCreateInfo->tiling != VK_IMAGE_TILING_LINEAR &&
        pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL) ||
       pCreateInfo->sharingMode != VK_SHARING_MODE_EXCLUSIVE)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   /* VK_IMAGE_TILING_OPTIMAL grants the implementation an opaque byte
    * layout the application makes no assumptions about
    * (VUID-vkGetImageSubresourceLayout-image-07790, cited in full at
    * r3v_GetImageSubresourceLayout below). The transfer family already
    * executes its bytes as one linear span over the GEM BO
    * (r3v_native_transfer_footprint_bytes), so an OPTIMAL request over
    * that family's usage and extent vocabulary is the same executing
    * layout under the opaque contract; the image only stops answering
    * r3v_GetImageSubresourceLayout. The render family keeps its
    * dedicated relocation-addressed cell LINEAR-only: it has never
    * executed any other layout, and admitting OPTIMAL there would
    * advertise a capability this driver has not built.
    */
   const VkImageUsageFlags transfer_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   bool transfer_family;
   uint32_t row_pitch_bytes;
   if (pCreateInfo->usage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      if (pCreateInfo->flags != 0 ||
          pCreateInfo->tiling != VK_IMAGE_TILING_LINEAR ||
          pCreateInfo->format != R3V_NATIVE_TARGET_FORMAT ||
          pCreateInfo->extent.width > R3V_NATIVE_TARGET_WIDTH ||
          pCreateInfo->extent.height > R3V_NATIVE_TARGET_HEIGHT)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      transfer_family = false;
      row_pitch_bytes = R3V_NATIVE_TARGET_ROW_BYTES;
   } else if (pCreateInfo->usage != 0 &&
              (pCreateInfo->usage & ~transfer_usage) == 0) {
      if (pCreateInfo->extent.width > R3V_NATIVE_TRANSFER_DIMENSION_MAX ||
          pCreateInfo->extent.height > R3V_NATIVE_TRANSFER_DIMENSION_MAX)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      transfer_family = true;
      row_pitch_bytes = r3v_native_transfer_row_pitch_bytes(
         pCreateInfo->extent.width, texel_bytes);
   } else {
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   }

   struct r3v_native_image *image =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*image),
                       VK_OBJECT_TYPE_IMAGE);
   if (image == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   image->width = pCreateInfo->extent.width;
   image->height = pCreateInfo->extent.height;
   image->format = pCreateInfo->format;
   image->texel_bytes = texel_bytes;
   image->row_pitch_bytes = row_pitch_bytes;
   image->usage = pCreateInfo->usage;
   image->transfer_family = transfer_family;
   image->optimal_tiling = pCreateInfo->tiling == VK_IMAGE_TILING_OPTIMAL;
   image->memory = NULL;
   image->memory_offset = 0;
   *pImage = r3v_native_image_to_handle(image);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyImage(VkDevice _device, VkImage _image,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_image, image, _image);

   if (image == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, image);
}

/* The requirement carries each family's footprint.  The render family
 * holds the cell's memory contract: one row past the render extent as
 * implementation padding, so the recorded cell's output oracle always
 * has headroom to prove the device wrote inside the extent alone.  The
 * transfer family holds its rows alone; the copies move bytes through
 * host mappings and read nothing past the last row.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageMemoryRequirements(VkDevice _device, VkImage _image,
                               VkMemoryRequirements *pMemoryRequirements)
{
   VK_FROM_HANDLE(r3v_native_image, image, _image);

   /* Type 0 alone: the draw's load-op clear executes through a CPU
    * mapping of the bound allocation, and type 1 allocates with
    * RADEON_GEM_NO_CPU_ACCESS, so an allocation the requirement admits
    * is always one the clear can map.
    */
   *pMemoryRequirements = (VkMemoryRequirements){
      .size = image->transfer_family
                 ? r3v_native_transfer_footprint_bytes(image->width,
                                                       image->height,
                                                       image->texel_bytes)
                 : r3v_native_image_footprint_bytes(image->height),
      .alignment = R3V_NATIVE_MEMORY_ALIGNMENT,
      .memoryTypeBits = 0x1,
   };
}

/* VK_KHR_get_memory_requirements2 resolves through this entry; the
 * requirement is the per-family contract above.  The render family
 * reports a dedicated allocation required: the cell's color reference
 * resolves to the BO base through the relocation payload the qualified
 * digest freezes, so the image binds at offset zero, and a required
 * dedicated allocation is the vocabulary that tells a conformant
 * allocator exactly that.  The transfer family reaches no relocation
 * payload -- its copies address the host mapping directly -- so it
 * carries no dedicated requirement.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageMemoryRequirements2(VkDevice _device,
                                const VkImageMemoryRequirementsInfo2 *pInfo,
                                VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r3v_native_image, image, pInfo->image);

   r3v_GetImageMemoryRequirements(_device, pInfo->image,
                                  &pMemoryRequirements->memoryRequirements);

   vk_foreach_struct(ext, pMemoryRequirements->pNext) {
      if (ext->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
         VkMemoryDedicatedRequirements *dedicated = (void *)ext;
         const VkBool32 render_family =
            image != NULL && !image->transfer_family;
         dedicated->prefersDedicatedAllocation = render_family;
         dedicated->requiresDedicatedAllocation = render_family;
      }
   }
}

/* VK_KHR_get_memory_requirements2 includes the sparse form of the
 * query, and the device exposes no sparse residency, so an image's
 * sparse requirement set is empty: a zero count with the caller's
 * array untouched.
 */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageSparseMemoryRequirements2(VkDevice _device,
   const VkImageSparseMemoryRequirementsInfo2 *pInfo,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   (void)_device;
   (void)pInfo;
   (void)pSparseMemoryRequirements;

   *pSparseMemoryRequirementCount = 0;
}

/* The render family binds at offset zero because its color reference names
 * the BO base and its dedicated-allocation requirement keeps that address
 * stable.  The transfer family binds any aligned suballocation whose
 * footprint fits and carries that offset into every host copy address.  The
 * complete field consumer set is enumerated by `(rg --fixed-strings
 * memory_offset src/amd/r300/vulkan/)`.  Binding happens exactly once
 * per image, to the one host-visible type the requirement admits: the
 * load-op clear and the host copies execute through a CPU mapping, and
 * type 1 allocates with RADEON_GEM_NO_CPU_ACCESS.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_BindImageMemory(VkDevice _device, VkImage _image, VkDeviceMemory _memory,
                    VkDeviceSize memoryOffset)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_image, image, _image);
   VK_FROM_HANDLE(r3v_native_memory, memory, _memory);

   const uint64_t footprint =
      image != NULL
         ? (image->transfer_family
               ? r3v_native_transfer_footprint_bytes(image->width,
                                                     image->height,
                                                     image->texel_bytes)
               : r3v_native_image_footprint_bytes(image->height))
         : 0;
   if (image == NULL || memory == NULL || image->memory != NULL ||
       memory->vk.memory_type_index != 0 ||
       memoryOffset % R3V_NATIVE_MEMORY_ALIGNMENT != 0 ||
       (!image->transfer_family && memoryOffset != 0) ||
       memoryOffset > memory->bo.size ||
       footprint > memory->bo.size - memoryOffset)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   image->memory = memory;
   image->memory_offset = memoryOffset;
   return VK_SUCCESS;
}

/* VK_KHR_bind_memory2 resolves through this entry; each bind runs the
 * same admission as r3v_BindImageMemory, and the first refusal reports
 * after every remaining bind has been attempted, matching the
 * all-or-report batch contract.
 */
VKAPI_ATTR VkResult VKAPI_CALL
r3v_BindImageMemory2(VkDevice _device, uint32_t bindInfoCount,
                     const VkBindImageMemoryInfo *pBindInfos)
{
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VkResult bind = r3v_BindImageMemory(_device, pBindInfos[i].image,
                                          pBindInfos[i].memory,
                                          pBindInfos[i].memoryOffset);
      if (bind != VK_SUCCESS && result == VK_SUCCESS)
         result = bind;
   }
   return result;
}

VKAPI_ATTR void VKAPI_CALL
r3v_GetImageSubresourceLayout(VkDevice _device, VkImage _image,
                              const VkImageSubresource *pSubresource,
                              VkSubresourceLayout *pLayout)
{
   VK_FROM_HANDLE(r3v_native_image, image, _image);

   /* VUID-vkGetImageSubresourceLayout-image-07790: calling this entry
    * on a VK_IMAGE_TILING_OPTIMAL image is invalid usage the
    * validation layers catch; the entry point is void, so the driver
    * has no return value to report it through. The assert holds the
    * same refusal in a debug build (b_ndebug=false) that runs without
    * validation; a release build (NDEBUG, assert() a no-op) computes
    * the transfer family's linear-span layout below regardless of
    * tiling, since VK_IMAGE_TILING_OPTIMAL executes that identical
    * span (r3v_native_transfer_footprint_bytes) -- the answer is
    * correct for the bytes this driver actually writes, even though
    * the call remains invalid application usage under the spec.
    */
   assert(!image->optimal_tiling);

   *pLayout = (VkSubresourceLayout){
      .offset = 0,
      .size = (VkDeviceSize)image->row_pitch_bytes * image->height,
      .rowPitch = image->row_pitch_bytes,
   };
}

/* The view admits the identity reading of the one image shape: same
 * format, 2D, the full single-mip single-layer color subresource, and
 * identity component mapping in either spelling.
 */
static bool
swizzle_is_identity(VkComponentSwizzle swizzle, VkComponentSwizzle identity)
{
   return swizzle == VK_COMPONENT_SWIZZLE_IDENTITY || swizzle == identity;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateImageView(VkDevice _device,
                    const VkImageViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkImageView *pView)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_image, image, pCreateInfo->image);

   *pView = VK_NULL_HANDLE;

   /* A view serves the attachment path, and the transfer family
    * carries no attachment usage, so no view admits it.
    */
   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
   if (image == NULL || image->transfer_family ||
       pCreateInfo->flags != 0 ||
       pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_2D ||
       pCreateInfo->format != R3V_NATIVE_TARGET_FORMAT ||
       !swizzle_is_identity(pCreateInfo->components.r,
                            VK_COMPONENT_SWIZZLE_R) ||
       !swizzle_is_identity(pCreateInfo->components.g,
                            VK_COMPONENT_SWIZZLE_G) ||
       !swizzle_is_identity(pCreateInfo->components.b,
                            VK_COMPONENT_SWIZZLE_B) ||
       !swizzle_is_identity(pCreateInfo->components.a,
                            VK_COMPONENT_SWIZZLE_A) ||
       range->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
       range->baseMipLevel != 0 ||
       (range->levelCount != 1 &&
        range->levelCount != VK_REMAINING_MIP_LEVELS) ||
       range->baseArrayLayer != 0 ||
       (range->layerCount != 1 &&
        range->layerCount != VK_REMAINING_ARRAY_LAYERS))
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_image_view *view =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*view),
                       VK_OBJECT_TYPE_IMAGE_VIEW);
   if (view == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   view->image = image;
   *pView = r3v_native_image_view_to_handle(view);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_DestroyImageView(VkDevice _device, VkImageView _view,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_native_device, device, _device);
   VK_FROM_HANDLE(r3v_native_image_view, view, _view);

   if (view == NULL)
      return;
   vk_object_free(&device->vk, pAllocator, view);
}
