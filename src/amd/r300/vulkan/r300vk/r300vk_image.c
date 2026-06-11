/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_image.h"
#include "r300vk_format.h"
#include "r300vk_device.h"

/* r300g internals: a linear image reads its row stride from the backing
 * r300_resource (r300_texture_desc::stride_in_bytes), which the Vulkan layer
 * must report verbatim so a CPU readback maps the same pitch the transfer map
 * returns.  The gallium-backed ICD statically links r300g, so reaching the
 * r300_resource() cast here matches how r300vk_pipeline.c uses r300_screen.h. */
#include "r300/r300_context.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/u_inlines.h"

/* Split one image axis into r300g tiles no wider than max_tile.  max_tile is
 * the per-tile dimension ceiling: a tile is both rendered to (CB limit, 2560 on
 * r300-class) and sampled as a blit source (sampler cap max_texture_2d_size,
 * 2048 on r300-class / 4096 on r500), so the binding limit is the smaller of
 * the two.  r300 samples the whole source resource, taking TX_WIDTH from the
 * resource width and wrapping past the 11-bit field, so a tile wider than the
 * sampler cap cannot be sampled at all.  Splitting at the cap keeps every tile
 * directly samplable, which is what lets a blit source larger than the cap be
 * blitted tile-by-tile.  The Vulkan floor maxImageDimension2D is 4096 and the
 * cap is at least 2048, so an axis is at most two tiles and tiles[2] suffices. */
static uint32_t
r300vk_split_image_axis(uint32_t extent, uint32_t max_tile, uint32_t tiles[2])
{
   if (extent == 0 || extent > R300VK_VK10_MIN_IMAGE_DIMENSION_2D)
      return 0;

   if (extent <= max_tile) {
      tiles[0] = extent;
      tiles[1] = 0;
      return 1;
   }

   tiles[0] = max_tile;
   tiles[1] = extent - max_tile;
   return 2;
}

static void
r300vk_image_release_resources(struct r300vk_image *img)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(img->tiles); i++)
      pipe_resource_reference(&img->tiles[i], NULL);
   img->resource = NULL;
}

static bool
r300vk_screen_supports_format(struct r300vk_device *device,
                              enum pipe_format format,
                              unsigned bindings)
{
   return device->screen &&
          device->screen->is_format_supported(device->screen, format,
                                              PIPE_TEXTURE_2D, 0, 0,
                                              bindings);
}

static bool
r300vk_format_supports_transfer_dst(enum pipe_format pipe_fmt)
{
   if (util_format_is_compressed(pipe_fmt) ||
       util_format_is_depth_or_stencil(pipe_fmt) ||
       util_format_is_snorm(pipe_fmt))
      return false;

   const struct util_format_description *desc =
      util_format_description(pipe_fmt);
   return desc && desc->nr_channels > 0 &&
          (desc->channel[0].type != UTIL_FORMAT_TYPE_FLOAT ||
           desc->channel[0].size < 32);
}

static VkImageUsageFlags
r300vk_supported_image_usage(struct r300vk_device *device,
                             enum pipe_format pipe_fmt)
{
   VkImageUsageFlags usage = 0;

   if (r300vk_screen_supports_format(device, pipe_fmt, PIPE_BIND_SAMPLER_VIEW)) {
      /* Input attachments ride the sampled path: r300vk lowers subpassLoad to
       * a normalized texture read, so sampled capability is input-attachment
       * capability.  zink requests INPUT_ATTACHMENT on every swapchain image. */
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   }
   if (r300vk_screen_supports_format(device, pipe_fmt, PIPE_BIND_RENDER_TARGET))
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (r300vk_screen_supports_format(device, pipe_fmt, PIPE_BIND_DEPTH_STENCIL))
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

   /* Image transfers walk r300g's mappable tile resources after submit.  Every
    * image format that has a real hardware use can be read back as a transfer
    * source.  Transfer destinations are narrower because the same feature bit
    * also enables image-to-image copy; expose it only for formats covered by the
    * staging copy and CPU upload paths. */
   if (usage) {
      usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      if (r300vk_format_supports_transfer_dst(pipe_fmt))
         usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   }

   return usage;
}

static unsigned
r300vk_image_pipe_bind(struct r300vk_device *device,
                       enum pipe_format pipe_fmt,
                       VkImageUsageFlags usage)
{
   unsigned bind = 0;

   if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
      bind |= PIPE_BIND_SAMPLER_VIEW;
   if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
      bind |= PIPE_BIND_RENDER_TARGET;
   if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
      bind |= PIPE_BIND_DEPTH_STENCIL;

   /* Transfer-only images still need a real r300g texture bind class.  Use the
    * strongest supported image role so transfer sources and destinations are
    * backed by mappable tile resources. */
   if (bind == 0 &&
       (usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT))) {
      if (r300vk_screen_supports_format(device, pipe_fmt,
                                        PIPE_BIND_DEPTH_STENCIL))
         bind = PIPE_BIND_DEPTH_STENCIL;
      else if (r300vk_screen_supports_format(device, pipe_fmt,
                                             PIPE_BIND_RENDER_TARGET))
         bind = PIPE_BIND_RENDER_TARGET;
      else
         bind = PIPE_BIND_SAMPLER_VIEW;
   }

   return bind;
}

static VkResult
r300vk_image_create_tile_resources(struct r300vk_device *device,
                                   struct r300vk_image *img,
                                   const VkImageCreateInfo *info,
                                   enum pipe_format pipe_fmt,
                                   unsigned pipe_bind)
{
   /* A linear image is one row-major tile (the accept gate bounds its extent to
    * one tile), allocated PIPE_USAGE_STAGING so r300g keeps it CPU-mappable.
    * Optimal images keep the up-to-2x2 spatial-subdivision tiling. */
   const bool is_linear = (pipe_bind & PIPE_BIND_LINEAR) != 0;
   if (is_linear) {
      img->tile_cols = 1;
      img->tile_rows = 1;
      img->tile_width[0] = info->extent.width;
      img->tile_height[0] = info->extent.height;
   } else {
      /* Bound a tile by the smaller of the sampler cap and the render limit so
       * every optimal tile can be both a blit source (sampled) and a render
       * target.  On r300-class the sampler cap (2048) is the smaller; on r500
       * both are 4096. */
      const uint32_t max_tile =
         MIN2(device->screen->caps.max_texture_2d_size,
              R300VK_R3XX_MAX_RENDER_DIMENSION);
      img->tile_cols = r300vk_split_image_axis(info->extent.width,
                                               max_tile, img->tile_width);
      img->tile_rows = r300vk_split_image_axis(info->extent.height,
                                               max_tile, img->tile_height);
   }
   if (img->tile_cols == 0 || img->tile_rows == 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r300vk: image extent %ux%u exceeds the 4096 floor",
                       info->extent.width, info->extent.height);

   for (uint32_t y = 0; y < img->tile_rows; y++) {
      for (uint32_t x = 0; x < img->tile_cols; x++) {
         struct pipe_resource tmpl = {
            .target     = PIPE_TEXTURE_2D,
            .format     = pipe_fmt,
            .bind       = pipe_bind,
            .usage      = is_linear ? PIPE_USAGE_STAGING : PIPE_USAGE_DEFAULT,
            .width0     = img->tile_width[x],
            .height0    = img->tile_height[y],
            .depth0     = 1,
            .array_size = 1,
            .last_level = 0,
            .nr_samples = info->samples,
         };

         const uint32_t tile_index = y * img->tile_cols + x;
         img->tiles[tile_index] =
            device->screen->resource_create(device->screen, &tmpl);
         if (!img->tiles[tile_index]) {
            r300vk_image_release_resources(img);
            return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         }
      }
   }

   /* Report exactly the stride r300g chose for the linear tile.  The transfer
    * map returns this pitch, so GetImageSubresourceLayout and the readback walk
    * the same row layout. */
   if (is_linear)
      img->linear_row_pitch = r300_resource(img->tiles[0])->tex.stride_in_bytes[0];

   img->resource = img->tiles[0];
   return VK_SUCCESS;
}

VkResult
r300vk_CreateImage(VkDevice _device,
                   const VkImageCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkImage *pImage)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_image *img;

   /* r300vk renders into r300g-tiled (optimal) resources.  A linear image
    * exists only as transfer staging: deqp's draw readback copies an optimal
    * render target into a VK_IMAGE_TILING_LINEAR TRANSFER_DST image, then maps
    * it and walks rowPitch-sized rows.  r300g produces a row-major single tile
    * when PIPE_BIND_LINEAR is set, so accept linear here and constrain it to
    * that staging shape below (2D, transfer-only usage, within one tile). */
   const bool is_linear = pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR;
   if (pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL && !is_linear)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r300vk: image tiling %d unsupported",
                       pCreateInfo->tiling);

   img = vk_zalloc2(&device->vk.alloc, pAllocator,
                    sizeof(*img), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!img)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_init(&device->vk, &img->vk, pCreateInfo);

   /* R300-class hardware only supports flat 2D images: one layer, one mip
    * level, and a single sample.  The 4096 Vulkan 1.0 floor is represented as
    * one, two, or four hardware-sized 2D resources.  r300vk has no multisample
    * path (no MSAA resolve), and r300vk_get_image_format_properties reports
    * VK_SAMPLE_COUNT_1_BIT, so reject more than one sample here: a 4x image
    * would otherwise reach the unimplemented CmdResolveImage destination path
    * and crash.  Reject unsupported shapes so callers see a clear error rather
    * than silently incorrect behavior. */
   if (pCreateInfo->arrayLayers > 1) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: arrayLayers %u > 1 unsupported",
                       pCreateInfo->arrayLayers);
   }
   if (pCreateInfo->mipLevels > 1) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: mipLevels %u > 1 unsupported",
                       pCreateInfo->mipLevels);
   }
   if (pCreateInfo->samples != VK_SAMPLE_COUNT_1_BIT) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: samples 0x%x unsupported (single-sample only)",
                       pCreateInfo->samples);
   }
   /* Every tile resource is PIPE_TEXTURE_2D with depth0 == 1, so a 3D image's
    * depth slices have no storage.  r300vk_get_image_format_properties already
    * reports VK_IMAGE_TYPE_3D unsupported; reject it here too so a caller that
    * skips the format query fails cleanly instead of silently collapsing the
    * depth to one slice. */
   if (pCreateInfo->imageType == VK_IMAGE_TYPE_3D) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r300vk: image type 3D unsupported (no depth-slice storage)");
   }

   enum pipe_format pipe_fmt = r300vk_vk_format_to_pipe_format(pCreateInfo->format);
   if (pipe_fmt == PIPE_FORMAT_NONE) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r300vk: unsupported image format %d", pCreateInfo->format);
   }

   VkImageUsageFlags supported_usage =
      r300vk_supported_image_usage(device, pipe_fmt);

   /* A linear image lives inside a single r300g tile: transfer staging, plus
    * colour-attachment and sampled use when the format's oracle bits allow
    * them -- r300 renders to and samples from row-major surfaces (scanout on
    * this silicon is linear), and the common WSI's software swapchain creates
    * exactly such images (linear, COLOR_ATTACHMENT | TRANSFER, one tile).
    * Reject any shape the single-tile row-major path cannot back: non-2D, an
    * extent past one tile, or a format without a lossless transfer-destination
    * byte layout (the same predicate that gates linearTilingFeatures). */
   if (is_linear) {
      /* INPUT_ATTACHMENT rides the sampled path: r300vk lowers subpassLoad to
       * a normalized texture read, so any sampled-capable image serves as an
       * input attachment.  zink requests it on every swapchain image, and the
       * software swapchain's images are linear, so excluding it here rejected
       * vkCreateSwapchainKHR outright. */
      supported_usage &= (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      if (pCreateInfo->imageType != VK_IMAGE_TYPE_2D ||
          pCreateInfo->extent.width > R300VK_R3XX_MAX_RENDER_DIMENSION ||
          pCreateInfo->extent.height > R300VK_R3XX_MAX_RENDER_DIMENSION ||
          !r300vk_format_supports_transfer_dst(pipe_fmt)) {
         vk_image_finish(&img->vk);
         vk_free2(&device->vk.alloc, pAllocator, img);
         return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "r300vk: linear image unsupported (format %d type %d %ux%u)",
                          pCreateInfo->format, pCreateInfo->imageType,
                          pCreateInfo->extent.width, pCreateInfo->extent.height);
      }
   }

   if ((pCreateInfo->usage & ~supported_usage) != 0) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r300vk: unsupported image usage 0x%x for format %d",
                       pCreateInfo->usage & ~supported_usage,
                       pCreateInfo->format);
   }

   unsigned pipe_bind =
      r300vk_image_pipe_bind(device, pipe_fmt, pCreateInfo->usage);
   if (is_linear)
      pipe_bind |= PIPE_BIND_LINEAR;

   VkResult result =
      r300vk_image_create_tile_resources(device, img, pCreateInfo, pipe_fmt,
                                         pipe_bind);
   if (result != VK_SUCCESS) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return result;
   }

   *pImage = r300vk_image_to_handle(img);
   return VK_SUCCESS;
}

void
r300vk_DestroyImage(VkDevice _device,
                    VkImage _image,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_image, img, _image);
   if (!img)
      return;

   r300vk_image_release_resources(img);
   vk_image_finish(&img->vk);
   vk_free2(&device->vk.alloc, pAllocator, img);
}

VkDeviceSize
r300vk_image_memory_size(const struct r300vk_image *img)
{
   const VkExtent3D *ext = &img->vk.extent;

   /* A linear image's bytes are r300g's row-major tile: stride times height.
    * Reporting the same value GetImageSubresourceLayout returns keeps the
    * BindImageMemory2 bound check and the deqp readback walk consistent. */
   if (img->vk.tiling == VK_IMAGE_TILING_LINEAR)
      return (VkDeviceSize)img->linear_row_pitch * ext->height;

   return (VkDeviceSize)ext->width * ext->height *
          MAX2(1u, img->vk.samples) *
          util_format_get_blocksize(r300vk_vk_format_to_pipe_format(img->vk.format));
}

static void
r300vk_image_subresource_layout(VkImage _image,
                                VkSubresourceLayout2 *pLayout)
{
   VK_FROM_HANDLE(r300vk_image, img, _image);
   VkSubresourceLayout *layout = &pLayout->subresourceLayout;

   /* r300vk images are single mip level and single array layer (r300vk_CreateImage
    * rejects mipLevels > 1 and arrayLayers > 1), so the queried subresource is
    * always level 0 / layer 0 and pSubresource selects no other layout. */
   if (img->vk.tiling == VK_IMAGE_TILING_LINEAR) {
      /* The single linear tile is row-major from offset 0.  rowPitch is the
       * stride r300g's transfer map returns, so the mapped readback walks the
       * same rows.  One 2D layer, no depth, so array/depth pitch are the layer
       * size. */
      layout->offset = 0;
      layout->rowPitch = img->linear_row_pitch;
      layout->size = (VkDeviceSize)img->linear_row_pitch * img->vk.extent.height;
      layout->arrayPitch = layout->size;
      layout->depthPitch = layout->size;
   } else {
      /* vkGetImageSubresourceLayout is defined only for linear tiling; an
       * optimal image carries an implementation-private swizzle.  deqp queries
       * layout only on its linear staging copy, so report a zeroed layout. */
      layout->offset = 0;
      layout->rowPitch = 0;
      layout->size = 0;
      layout->arrayPitch = 0;
      layout->depthPitch = 0;
   }
}

/* GetImageSubresourceLayout2, ...2KHR (KHR_maintenance5) and ...2EXT
 * (EXT_host_image_copy) are aliases of one entrypoint: the generated device
 * entrypoint table maps all three names to a single dispatch slot.  Defining a
 * driver function for more than one alias makes
 * vk_device_dispatch_table_from_entrypoints fill that slot twice and trip its
 * assert(disp[disp_index] == NULL) at vkCreateDevice, aborting every r300vk
 * device.  Define only the core form, as every other Mesa driver does; the
 * table dispatches the KHR/EXT names to it. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetImageSubresourceLayout2(VkDevice device, VkImage image,
                                  const VkImageSubresource2 *pSubresource,
                                  VkSubresourceLayout2 *pLayout)
{
   r300vk_image_subresource_layout(image, pLayout);
}

/* VK_KHR_maintenance5: the subresource layout of an image that has not been
 * created (VkDeviceImageSubresourceInfo carries the VkImageCreateInfo).  r300g
 * derives the row pitch from the realized pipe_resource, so the honest answer
 * realizes the image transiently, reads its layout, and frees it -- no device
 * memory is bound, only the resource metadata is allocated.  A create the chip
 * cannot honour (multi-mip, array, or 3D, which r300vk_CreateImage rejects)
 * yields a zeroed layout, the same as the optimal-tiling case. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetDeviceImageSubresourceLayout(VkDevice _device,
                                       const VkDeviceImageSubresourceInfo *pInfo,
                                       VkSubresourceLayout2 *pLayout)
{
   VkImage image;
   if (r300vk_CreateImage(_device, pInfo->pCreateInfo, NULL, &image) != VK_SUCCESS) {
      pLayout->subresourceLayout = (VkSubresourceLayout){0};
      return;
   }
   r300vk_image_subresource_layout(image, pLayout);
   r300vk_DestroyImage(_device, image, NULL);
}

void
r300vk_GetImageMemoryRequirements2(VkDevice _device,
                                    const VkImageMemoryRequirementsInfo2 *pInfo,
                                    VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r300vk_image, img, pInfo->image);

   /* 4096-byte alignment satisfies r300g tiling requirements.  The reported
    * size must match the bind-time bound check in BindImageMemory2, so both
    * read it from r300vk_image_memory_size(). */
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size           = r300vk_image_memory_size(img),
      .alignment      = 4096,
      /* r300g places a single-sample render-target texture in
       * RADEON_DOMAIN_VRAM | RADEON_DOMAIN_GTT (r300_texture.c domain
       * selection), so both advertised memory types back it validly: type 0
       * (host-visible GTT) and type 1 (device-local).  The bound BO domain is
       * fixed by the pipe_resource template, not by the type the app picks, so
       * a device-local color target binds correctly.  Reporting only type 0
       * rejected a conformant device-local color-image allocation. */
      .memoryTypeBits = 0x3,
   };
}

VkResult
r300vk_CreateImageView(VkDevice _device,
                       const VkImageViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkImageView *pView)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   struct r300vk_image_view *iv;

   iv = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*iv), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!iv)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_view_init(&device->vk, &iv->vk, pCreateInfo);

   *pView = r300vk_image_view_to_handle(iv);
   return VK_SUCCESS;
}

void
r300vk_DestroyImageView(VkDevice _device,
                         VkImageView _view,
                         const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r300vk_device, device, _device);
   VK_FROM_HANDLE(r300vk_image_view, iv, _view);
   if (!iv)
      return;

   vk_image_view_finish(&iv->vk);
   vk_free2(&device->vk.alloc, pAllocator, iv);
}
