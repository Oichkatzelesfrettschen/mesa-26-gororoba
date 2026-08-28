/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V image surface: the render-shape target family with its
 * identity view, and the transfer family.  Both execute one linear
 * layout under either VK_IMAGE_TILING_LINEAR or
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
 * the color-attachment bit at either 32-bpp lane order, at any extent
 * inside R3V_NATIVE_RENDER_MAX_EXTENT per axis over the eight-pixel
 * aligned 32-bpp row pitch, under either tiling; the render-shape cell
 * places each of those parameters through one named register class, so
 * every admitted shape emits from the reference cell's construction.
 * The family also takes the transfer bits, and their copies move the
 * rendered bytes through the same host mapping the readback rides. The
 * transfer family carries transfer usage alone at any extent inside
 * 2048 per axis over a width-derived 64-byte-aligned pitch; its copies
 * execute through host mappings, and the attachment paths never see it
 * because usage without the color-attachment bit admits no view. Every
 * other shape refuses with a cleared handle, so no image exists whose
 * lowering the implementation cannot record.
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
    * reports a required dedicated allocation, which holds one image per
    * allocation whatever offset it binds at, so a second render image
    * over the same range has no allocation to bind to and the flag
    * refuses there.  The transfer family's copies move bytes
    * through host mappings of the bound allocation and the queue
    * completes each submission before vkQueueSubmit returns
    * (r3v_native_transfer.c, r3v_native_queue.c; rg --fixed-strings
    * memory_offset src/amd/r300/vulkan/), so a read through one alias
    * observes every write the record order places before it.
    */
   const uint32_t texel_bytes =
      r3v_native_transfer_texel_bytes(pCreateInfo->format);
   /* VK_IMAGE_TYPE_1D is the height-one member of the same linear
    * layout: one row the TX block addresses with its height mask at
    * zero and the render cell scissors to a single scanline, so the two
    * types share every register class.  VK_IMAGE_TYPE_3D stacks depth
    * slices at the same stride the array layout places its layers at,
    * so creation, the memory requirement, and the bind all describe it
    * exactly; a view then names one slice, and the executing routes
    * take the one-slice view types alone
    * (r3v_native_view_type_executes), since no TX program indexes a
    * slice from the shader coordinate.
    */
   const bool one_dimensional = pCreateInfo->imageType == VK_IMAGE_TYPE_1D;
   const bool volumetric = pCreateInfo->imageType == VK_IMAGE_TYPE_3D;
   /* VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT names a square 2D image with
    * six layers per cube (VUID-VkImageCreateInfo-flags-00949 and
    * -00954); the layout is the array layout, and the flag only widens
    * which view types the image's own geometry supports.
    */
   const bool cube_compatible =
      (pCreateInfo->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0;
   const uint32_t slice_count =
      volumetric ? pCreateInfo->extent.depth : pCreateInfo->arrayLayers;
   if ((pCreateInfo->flags &
        ~(VkImageCreateFlags)(VK_IMAGE_CREATE_ALIAS_BIT |
                              VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) ||
       (pCreateInfo->imageType != VK_IMAGE_TYPE_2D && !one_dimensional &&
        !volumetric) ||
       texel_bytes == 0 ||
       pCreateInfo->extent.width < 1 || pCreateInfo->extent.height < 1 ||
       (one_dimensional && pCreateInfo->extent.height != 1) ||
       (volumetric && pCreateInfo->arrayLayers != 1) ||
       (!volumetric && pCreateInfo->extent.depth != 1) ||
       pCreateInfo->extent.depth < 1 ||
       (cube_compatible &&
        (pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
         pCreateInfo->extent.width != pCreateInfo->extent.height ||
         pCreateInfo->arrayLayers % 6 != 0)) ||
       pCreateInfo->mipLevels != 1 ||
       pCreateInfo->arrayLayers < 1 ||
       slice_count > R3V_NATIVE_MAX_ARRAY_LAYERS ||
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
    * r3v_GetImageSubresourceLayout. The render family's cell addresses
    * its target through one RB3D_COLORPITCH0 word over the same linear
    * span (r3v_native_render_footprint_bytes), so OPTIMAL is that
    * identical layout there too and the image likewise stops answering
    * r3v_GetImageSubresourceLayout.
    */
   const VkImageUsageFlags transfer_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   bool transfer_family;
   uint32_t row_pitch_bytes;
   enum r300_triangle_lane_order lanes = R300_TRIANGLE_LANES_B8G8R8A8;
   if (pCreateInfo->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
      /* The sampled bit joins the attachment bit over one layout: the
       * 64-byte row alignment the sampling family already executes is a
       * multiple of the eight 32-bpp pixels the render cell's
       * RB3D_COLORPITCH0 addresses, so a union image carries the
       * stricter pitch and both routes read the same rows.  The extent
       * then answers to whichever ceiling is lower.
       */
      const bool sampled = (pCreateInfo->usage &
                            VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
      const uint32_t extent_max =
         sampled ? MIN2(R3V_NATIVE_RENDER_MAX_EXTENT,
                        R3V_NATIVE_TRANSFER_DIMENSION_MAX)
                 : R3V_NATIVE_RENDER_MAX_EXTENT;
      if (pCreateInfo->flags != 0 || volumetric ||
          (pCreateInfo->usage &
           ~(VkImageUsageFlags)(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT |
                                transfer_usage)) != 0 ||
          !r3v_native_render_lane_order(pCreateInfo->format, &lanes) ||
          pCreateInfo->extent.width > extent_max ||
          pCreateInfo->extent.height > extent_max)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      if (slice_count > R3V_NATIVE_RENDER_MAX_ARRAY_LAYERS)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      transfer_family = false;
      row_pitch_bytes =
         sampled ? r3v_native_transfer_row_pitch_bytes(
                      pCreateInfo->extent.width, texel_bytes)
                 : r3v_native_render_row_pitch_bytes(pCreateInfo->extent.width);
   } else if (pCreateInfo->usage != 0 &&
              (pCreateInfo->usage &
               ~(transfer_usage | VK_IMAGE_USAGE_SAMPLED_BIT)) == 0) {
      /* The sampled bit joins the linear transfer family for the two
       * 32-bpp lane orders; FORMAT1's per-channel selects route each
       * order's memory bytes to shader R/G/B/A, so the sampling cell
       * converts nothing, and a sampled image keeps the family's extent
       * ceiling, which is the TX width/height mask ceiling.
       */
      enum r300_triangle_lane_order sampled_lanes;
      if ((pCreateInfo->usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
          !r3v_native_render_lane_order(pCreateInfo->format, &sampled_lanes))
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      if (pCreateInfo->extent.width > R3V_NATIVE_TRANSFER_DIMENSION_MAX ||
          pCreateInfo->extent.height > R3V_NATIVE_TRANSFER_DIMENSION_MAX)
         return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
      transfer_family = true;
      row_pitch_bytes = r3v_native_transfer_row_pitch_bytes(
         pCreateInfo->extent.width, texel_bytes);
   } else {
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);
   }

   /* The reported maxResourceSize is the 32-bit span every offset the
    * routes place travels in, so a footprint past it has no register
    * payload and refuses here.
    */
   const uint32_t layer_pitch_bytes = r3v_native_image_layer_pitch_bytes(
      row_pitch_bytes, pCreateInfo->extent.height, !transfer_family);
   const uint64_t footprint_bytes =
      (uint64_t)layer_pitch_bytes * slice_count;
   if (footprint_bytes > UINT32_MAX)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_image *image =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*image),
                       VK_OBJECT_TYPE_IMAGE);
   if (image == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   image->width = pCreateInfo->extent.width;
   image->height = pCreateInfo->extent.height;
   image->format = pCreateInfo->format;
   image->texel_bytes = texel_bytes;
   image->lanes = lanes;
   image->row_pitch_bytes = row_pitch_bytes;
   image->array_layers = pCreateInfo->arrayLayers;
   image->one_dimensional = one_dimensional;
   image->image_type = pCreateInfo->imageType;
   image->depth = pCreateInfo->extent.depth;
   image->cube_compatible = cube_compatible;
   image->slice_count = slice_count;
   image->layer_pitch_bytes = layer_pitch_bytes;
   image->footprint_bytes = footprint_bytes;
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
      .size = image->footprint_bytes,
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

/* Both families bind any aligned suballocation whose footprint fits from
 * the bind offset.  The render family carries that offset into the cell's
 * RB3D_COLOROFFSET0 payload, which the kernel biases by the relocation
 * base (r300_packet0_check, radeon r300.c), and into the host writes the
 * load-op clear and the in-pass clears make; the transfer family carries
 * it into every host copy address.  The alignment the requirement
 * reports is one page, so an admitted offset already satisfies the
 * register's 32-byte base granularity
 * (R300_TRIANGLE_TARGET_OFFSET_ALIGNMENT).  The
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

   const uint64_t footprint = image != NULL ? image->footprint_bytes
         : 0;
   if (image == NULL || memory == NULL || image->memory != NULL ||
       memory->vk.memory_type_index != 0 ||
       memoryOffset % R3V_NATIVE_MEMORY_ALIGNMENT != 0 ||
       /* The render cell places the last layer's base in
        * RB3D_COLOROFFSET0, so the bind offset plus that layer's stride
        * answers to the register's ceiling, not the bind offset alone.
        */
       (!image->transfer_family &&
        memoryOffset + (uint64_t)image->layer_pitch_bytes *
                          (image->array_layers - 1) >
           R300_TRIANGLE_MAX_TARGET_OFFSET) ||
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

   /* Layers and depth slices stack at the one stride, so the queried
    * layer's span starts there.  arrayPitch names that stride for a
    * layered image and depthPitch names it for a volume image, each
    * staying zero where the VkSubresourceLayout contract leaves it
    * undefined.
    */
   assert(pSubresource->mipLevel == 0 &&
          pSubresource->arrayLayer < image->array_layers);
   *pLayout = (VkSubresourceLayout){
      .offset = (VkDeviceSize)image->layer_pitch_bytes *
                pSubresource->arrayLayer,
      .size = (VkDeviceSize)image->row_pitch_bytes * image->height,
      .rowPitch = image->row_pitch_bytes,
      .arrayPitch = image->array_layers > 1 ? image->layer_pitch_bytes : 0,
      .depthPitch = image->image_type == VK_IMAGE_TYPE_3D
                       ? image->layer_pitch_bytes
                       : 0,
   };
}

/* The view admits the identity reading of one layer: same format, the
 * view type its image's own type names, one mip, one array layer at any
 * index the image holds, and identity component mapping in either
 * spelling.  The layer resolves to a byte offset the sampling and
 * attachment routes add to the bind offset, so every admitted view has
 * an executing route; the array, cube, and volume view types index a
 * layer from the shader instead, which the TX program has no route for,
 * and they refuse here.
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

   /* A view serves the attachment and sampling paths; a transfer-only
    * image carries neither usage, so no view admits it.
    */
   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;
   /* The view types the image's own geometry supports.  A one-slice
    * type resolves its slice at creation and the executing routes bind
    * it; the multi-slice types name a coordinate axis no cell indexes,
    * so they create and destroy while the draw and attachment paths
    * refuse them (r3v_native_view_type_executes).
    */
   const VkImageViewType view_type = pCreateInfo->viewType;
   bool view_type_ok = false;
   uint32_t required_layers = 1;
   if (image != NULL) {
      switch (view_type) {
      case VK_IMAGE_VIEW_TYPE_1D:
      case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
         view_type_ok = image->one_dimensional;
         break;
      case VK_IMAGE_VIEW_TYPE_2D:
      case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
         view_type_ok = image->image_type == VK_IMAGE_TYPE_2D;
         break;
      case VK_IMAGE_VIEW_TYPE_CUBE:
         /* Exactly one cube's six faces
          * (VUID-VkImageViewCreateInfo-viewType-02960).
          */
         view_type_ok = image->cube_compatible;
         required_layers = 6;
         break;
      case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
         /* A whole number of cubes
          * (VUID-VkImageViewCreateInfo-viewType-02961).
          */
         view_type_ok = image->cube_compatible;
         required_layers = 0;
         break;
      case VK_IMAGE_VIEW_TYPE_3D:
         view_type_ok = image->image_type == VK_IMAGE_TYPE_3D;
         break;
      default:
         break;
      }
   }
   /* A 3D view names the whole volume, so its subresource range carries
    * the image's one array layer (VUID-VkImageViewCreateInfo-image-01584).
    */
   const uint32_t resolved_layers =
      range->layerCount == VK_REMAINING_ARRAY_LAYERS
         ? image != NULL ? image->array_layers - range->baseArrayLayer : 0
         : range->layerCount;
   bool layer_count_ok;
   switch (view_type) {
   case VK_IMAGE_VIEW_TYPE_CUBE:
      layer_count_ok = resolved_layers == required_layers;
      break;
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      layer_count_ok = resolved_layers >= 6 && resolved_layers % 6 == 0;
      break;
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      layer_count_ok = resolved_layers >= 1;
      break;
   default:
      layer_count_ok = resolved_layers == 1;
      break;
   }
   if (image == NULL ||
       (image->transfer_family &&
        (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) ||
       !view_type_ok || !layer_count_ok ||
       pCreateInfo->flags != 0 ||
       pCreateInfo->format != image->format ||
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
       range->baseArrayLayer >= image->array_layers ||
       resolved_layers < 1 ||
       range->baseArrayLayer + resolved_layers > image->array_layers)
      return vk_error(device, R3V_NATIVE_REFUSAL_RESULT);

   struct r3v_native_image_view *view =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*view),
                       VK_OBJECT_TYPE_IMAGE_VIEW);
   if (view == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   view->image = image;
   view->base_array_layer = range->baseArrayLayer;
   view->layer_offset_bytes = image->layer_pitch_bytes * range->baseArrayLayer;
   view->view_type = view_type;
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
