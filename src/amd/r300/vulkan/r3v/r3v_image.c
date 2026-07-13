/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r3v_image.h"
#include "r3v_format.h"
#include "frontend/winsys_handle.h"
#include "r3v_device.h"

/* r300g internals: a linear image reads its row stride from the backing
 * r300_resource (r300_texture_desc::stride_in_bytes), which the Vulkan layer
 * must report verbatim so a CPU readback maps the same pitch the transfer map
 * returns.  The gallium-backed ICD statically links r300g, so reaching the
 * r300_resource() cast here matches how r3v_pipeline.c uses r300_screen.h. */
#include "r300/r300_context.h"

#include "vk_alloc.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

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
r3v_split_image_axis(uint32_t extent, uint32_t max_tile, uint32_t tiles[2])
{
   if (extent == 0 || extent > R3V_VK10_MIN_IMAGE_DIMENSION_2D)
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
r3v_image_release_resources(struct r3v_image *img)
{
   for (uint32_t i = 0; i < ARRAY_SIZE(img->tiles); i++)
      pipe_resource_reference(&img->tiles[i], NULL);
   for (uint32_t i = 0; i < ARRAY_SIZE(img->sampler_atlas.tiles); i++)
      pipe_resource_reference(&img->sampler_atlas.tiles[i], NULL);
   img->sampler_atlas.valid = false;
   img->resource = NULL;
}

/* Build the lazy overlapped sampler atlas for a split image (Tier-2 LINEAR
 * tile-stitch).  The render/copy tiles[] stay a disjoint partition; here each of
 * up to four sampler charts is at most the 2048 sampler cap and the seam region
 * is duplicated into both neighbouring charts, so a bilinear footprint near the
 * logical seam stays inside one chart.  Two-chart cover charts span only in-image
 * logical coordinates (origins >= 0, extents within W/H), so no edge clamp is
 * needed; W==4096 (zero overlap) is excluded by the eligibility gate.  Returns
 * false (atlas left invalid) on any failure so the caller refuses the split
 * sampled image rather than sampling wrong. */
static bool
r3v_image_build_sampler_atlas(struct r3v_device *device,
                                 struct r3v_image *img)
{
   struct pipe_screen *screen = device->screen;
   struct pipe_context *pipe = device->pipe;
   if (!img->resource || !img->tile_cols || !img->tile_rows)
      return false;
   const enum pipe_format fmt = img->resource->format;
   const unsigned bpp = util_format_get_blocksize(fmt);

   const uint32_t W = img->tile_width[0] +
                      (img->tile_cols > 1 ? img->tile_width[1] : 0);
   const uint32_t H = img->tile_height[0] +
                      (img->tile_rows > 1 ? img->tile_height[1] : 0);
   /* A chart must fit the hardware sampler cap, which is the real
    * max_texture_2d_size (2048 on r3xx, 4096 on r500) -- not the r3xx constant.
    * On r500 the render split uses a 2560 tile, so deriving the cap from the
    * sampler limit keeps charts sampleable and the cover consistent with the
    * render split that produced tiles[]. */
   const uint32_t cap = device->screen->caps.max_texture_2d_size;

   /* The overlapped two-chart cover and the 1x1-block texel copy are correct only
    * when each source render tile and each axis fit within two cap-sized charts
    * and the format is not block-compressed (DXT walks rows in 4x4 blocks the
    * texel-granular copy below cannot honour).  Refuse otherwise -- the caller
    * then leaves the split image unbound rather than sampling a coverage hole or
    * a mis-strided compressed copy. */
   /* Two charts span an axis only up to 2*cap, and a bilinear tap at the logical
    * seam needs one texel of overlap on each side, so the cover is correct only
    * for W <= 2*cap - 2 (overlap = 2*cap - W >= 2).  W in (2*cap-2, 2*cap] (e.g.
    * 4095/4096 on r3xx) has sub-texel margin and is deferred to a future 3-chart
    * cover; refuse it here. */
   if (img->tile_width[0] > cap || img->tile_height[0] > cap ||
       W + 2 > 2 * cap || H + 2 > 2 * cap ||
       util_format_get_blockwidth(fmt) != 1 ||
       util_format_get_blockheight(fmt) != 1)
      return false;

   const uint32_t cols = (W > cap) ? 2u : 1u;
   const uint32_t rows = (H > cap) ? 2u : 1u;

   for (uint32_t i = 0; i < ARRAY_SIZE(img->sampler_atlas.tiles); i++)
      pipe_resource_reference(&img->sampler_atlas.tiles[i], NULL);

   img->sampler_atlas.cols = cols;
   img->sampler_atlas.rows = rows;
   img->sampler_atlas.origin_x[0] = 0;
   img->sampler_atlas.width[0]    = (cols > 1) ? cap : W;
   img->sampler_atlas.origin_x[1] = (cols > 1) ? (W - cap) : 0;
   img->sampler_atlas.width[1]    = (cols > 1) ? cap : 0;
   img->sampler_atlas.origin_y[0] = 0;
   img->sampler_atlas.height[0]   = (rows > 1) ? cap : H;
   img->sampler_atlas.origin_y[1] = (rows > 1) ? (H - cap) : 0;
   img->sampler_atlas.height[1]   = (rows > 1) ? cap : 0;

   for (uint32_t cy = 0; cy < rows; cy++) {
      for (uint32_t cx = 0; cx < cols; cx++) {
         const uint32_t cw  = img->sampler_atlas.width[cx];
         const uint32_t ch  = img->sampler_atlas.height[cy];
         const uint32_t cox = img->sampler_atlas.origin_x[cx];
         const uint32_t coy = img->sampler_atlas.origin_y[cy];

         struct pipe_resource tmpl = {
            .target = PIPE_TEXTURE_2D, .format = fmt,
            .bind = PIPE_BIND_SAMPLER_VIEW, .usage = PIPE_USAGE_DEFAULT,
            .width0 = cw, .height0 = ch, .depth0 = 1, .array_size = 1,
            .last_level = 0, .nr_samples = 1,
         };
         struct pipe_resource *chart = screen->resource_create(screen, &tmpl);
         if (!chart)
            goto fail;
         img->sampler_atlas.tiles[cy * cols + cx] = chart;

         struct pipe_box dbox = { .x = 0, .y = 0, .z = 0,
                                  .width = (int)cw, .height = (int)ch, .depth = 1 };
         struct pipe_transfer *dxfer = NULL;
         uint8_t *dmap = pipe->texture_map(pipe, chart, 0, PIPE_MAP_WRITE,
                                           &dbox, &dxfer);
         if (!dmap)
            goto fail;

         /* Fill the chart from each overlapping source render tile.  The chart's
          * logical region [cox,cox+cw) x [coy,coy+ch) intersects up to four source
          * tiles at the seam corner; copy each intersection. */
         for (uint32_t sr = 0; sr < img->tile_rows; sr++) {
            for (uint32_t sc = 0; sc < img->tile_cols; sc++) {
               const uint32_t sox = sc ? img->tile_width[0] : 0;
               const uint32_t soy = sr ? img->tile_height[0] : 0;
               const uint32_t ix0 = MAX2(cox, sox);
               const uint32_t ix1 = MIN2(cox + cw, sox + img->tile_width[sc]);
               const uint32_t iy0 = MAX2(coy, soy);
               const uint32_t iy1 = MIN2(coy + ch, soy + img->tile_height[sr]);
               if (ix0 >= ix1 || iy0 >= iy1)
                  continue;
               struct pipe_box sbox = { .x = (int)(ix0 - sox), .y = (int)(iy0 - soy),
                                        .z = 0, .width = (int)(ix1 - ix0),
                                        .height = (int)(iy1 - iy0), .depth = 1 };
               struct pipe_transfer *sxfer = NULL;
               const uint8_t *smap = pipe->texture_map(
                  pipe, img->tiles[sr * img->tile_cols + sc], 0, PIPE_MAP_READ,
                  &sbox, &sxfer);
               if (!smap) {
                  pipe->texture_unmap(pipe, dxfer);
                  goto fail;
               }
               uint8_t *drow = dmap + (size_t)(iy0 - coy) * dxfer->stride +
                               (size_t)(ix0 - cox) * bpp;
               const unsigned wbytes = (ix1 - ix0) * bpp;
               for (uint32_t r = 0; r < (iy1 - iy0); r++)
                  memcpy(drow + (size_t)r * dxfer->stride,
                         smap + (size_t)r * sxfer->stride, wbytes);
               pipe->texture_unmap(pipe, sxfer);
            }
         }
         pipe->texture_unmap(pipe, dxfer);
      }
   }

   img->sampler_atlas.serial = img->content_serial;
   img->sampler_atlas.valid = true;
   return true;

fail:
   for (uint32_t i = 0; i < ARRAY_SIZE(img->sampler_atlas.tiles); i++)
      pipe_resource_reference(&img->sampler_atlas.tiles[i], NULL);
   img->sampler_atlas.valid = false;
   return false;
}

/* Ensure the split image's sampler atlas reflects its current content, rebuilding
 * it when the content serial has advanced past the atlas build serial.  Returns
 * false if no valid atlas could be built (caller then refuses the split sample). */
bool
r3v_image_ensure_sampler_atlas(struct r3v_device *device,
                                  struct r3v_image *img)
{
   if (img->sampler_atlas.valid &&
       img->sampler_atlas.serial == img->content_serial)
      return true;
   return r3v_image_build_sampler_atlas(device, img);
}

static bool
r3v_screen_supports_format(struct r3v_device *device,
                              enum pipe_format format,
                              unsigned bindings)
{
   return device->screen &&
          device->screen->is_format_supported(device->screen, format,
                                              PIPE_TEXTURE_2D, 0, 0,
                                              bindings);
}

static VkImageUsageFlags
r3v_supported_image_usage(struct r3v_device *device,
                             enum pipe_format pipe_fmt)
{
   VkImageUsageFlags usage = 0;

   if (r3v_screen_supports_format(device, pipe_fmt, PIPE_BIND_SAMPLER_VIEW)) {
      /* Input attachments ride the sampled path: r3v lowers subpassLoad to
       * a normalized texture read, so sampled capability is input-attachment
       * capability.  zink requests INPUT_ATTACHMENT on every swapchain image. */
      usage |= VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
   }
   if (r3v_screen_supports_format(device, pipe_fmt, PIPE_BIND_RENDER_TARGET))
      usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if (r3v_screen_supports_format(device, pipe_fmt, PIPE_BIND_DEPTH_STENCIL))
      usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

   /* Image transfers walk r300g's mappable tile resources after submit.  Every
    * image format that has a real hardware use can be read back as a transfer
    * source.  Transfer destinations are narrower because the same feature bit
    * also enables image-to-image copy; expose it only for formats covered by the
    * staging copy and CPU upload paths. */
   if (usage) {
      usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      if (r3v_format_supports_transfer_dst(pipe_fmt))
         usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   }

   return usage;
}

static unsigned
r3v_image_pipe_bind(struct r3v_device *device,
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
      if (r3v_screen_supports_format(device, pipe_fmt,
                                        PIPE_BIND_DEPTH_STENCIL))
         bind = PIPE_BIND_DEPTH_STENCIL;
      else if (r3v_screen_supports_format(device, pipe_fmt,
                                             PIPE_BIND_RENDER_TARGET))
         bind = PIPE_BIND_RENDER_TARGET;
      else
         bind = PIPE_BIND_SAMPLER_VIEW;
   }

   return bind;
}

static VkResult
r3v_image_create_tile_resources(struct r3v_device *device,
                                   struct r3v_image *img,
                                   const VkImageCreateInfo *info,
                                   enum pipe_format pipe_fmt,
                                   unsigned pipe_bind)
{
   /* Every tile is row-major (PIPE_BIND_LINEAR is universal since the
    * transfer-exactness change), so the linear bit selects only the staging
    * memory class for API-linear images.  The spatial split ALWAYS applies:
    * keying the no-split shortcut on the linear bit collapsed every image
    * into one tile once the bit became universal, and a 4096 single tile is
    * unbindable as a framebuffer (r300_set_framebuffer_state refuses past
    * 2560) -- zink's surfaceless backbuffer then renders into nothing.  Linear
    * and optimal images share the same spatial cuts, so render-pass replay sees
    * one tile-origin grid across mixed tiling modes. */
   const bool is_linear = info->tiling == VK_IMAGE_TILING_LINEAR;
   {
      /* Bound a tile by the smaller of the sampler cap and the render limit so
       * every optimal tile can be both a blit source (sampled) and a render
       * target.  On r300-class the sampler cap (2048) is the smaller; on r500
       * the render limit can be the smaller. */
      const uint32_t max_tile =
         MIN2(device->screen->caps.max_texture_2d_size,
              R3V_R3XX_MAX_RENDER_DIMENSION);
      img->tile_cols = r3v_split_image_axis(info->extent.width,
                                               max_tile, img->tile_width);
      img->tile_rows = r3v_split_image_axis(info->extent.height,
                                               max_tile, img->tile_height);
   }
   if (img->tile_cols == 0 || img->tile_rows == 0)
      return vk_errorf(device, VK_ERROR_INITIALIZATION_FAILED,
                       "r3v: image extent %ux%u exceeds the 4096 floor",
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
            /* The accept gate admits mipLevels > 1 only when one tile backs
             * the image, so a multi-tile split always sees last_level 0. */
            .last_level = info->mipLevels - 1,
            .nr_samples = info->samples,
         };

         const uint32_t tile_index = y * img->tile_cols + x;
         img->tiles[tile_index] =
            device->screen->resource_create(device->screen, &tmpl);
         if (!img->tiles[tile_index]) {
            r3v_image_release_resources(img);
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

static VkResult
r3v_image_validate_shape(struct r3v_device *device,
                            const VkImageCreateInfo *info,
                            bool is_linear,
                            bool external)
{
   if (info->tiling != VK_IMAGE_TILING_OPTIMAL && !is_linear)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r3v: image tiling %d unsupported", info->tiling);

   /* R300-class hardware only supports flat 2D images: one layer, one mip
    * level, and a single sample.  The 4096 Vulkan 1.0 floor is represented as
    * one, two, or four hardware-sized 2D resources.  r3v has no multisample
    * path (no MSAA resolve), and r3v_get_image_format_properties reports
    * VK_SAMPLE_COUNT_1_BIT, so reject more than one sample here: a 4x image
    * would otherwise reach the unimplemented CmdResolveImage destination path
    * and crash.  Reject unsupported shapes so callers see a clear error rather
    * than silently incorrect behavior. */
   if (info->arrayLayers > 1)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: arrayLayers %u > 1 unsupported",
                       info->arrayLayers);
   /* Mip chains ride a single r300g resource (last_level > 0), so they are
    * accepted exactly where one tile backs the whole image: optimal tiling
    * within the sampler dimension.  A larger image splits into tiles whose
    * seams no per-tile chain can cross, and a linear image is transfer
    * staging with no mip consumer. */
   if (info->mipLevels > 1 &&
       (info->tiling != VK_IMAGE_TILING_OPTIMAL ||
        info->extent.width  > R3V_R3XX_MAX_TEXTURE_DIMENSION ||
        info->extent.height > R3V_R3XX_MAX_TEXTURE_DIMENSION))
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: mipLevels %u unsupported for this shape "
                       "(mips need one optimal tile)",
                       info->mipLevels);
   if (info->samples != VK_SAMPLE_COUNT_1_BIT)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: samples 0x%x unsupported (single-sample only)",
                       info->samples);
   /* Every tile resource is PIPE_TEXTURE_2D with depth0 == 1, so a 3D image's
    * depth slices have no storage.  r3v_get_image_format_properties already
    * reports VK_IMAGE_TYPE_3D unsupported; reject it here too so a caller that
    * skips the format query fails cleanly instead of silently collapsing the
    * depth to one slice. */
   if (info->imageType == VK_IMAGE_TYPE_3D)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "r3v: image type 3D unsupported (no depth-slice storage)");

   /* dma-buf export: the backing resource must be a real, non-suballocated BO
    * with KMS-visible layout, so the extent is bounded to one tile (the
    * oracle contract: GEM_CREATE + SET_TILING + PRIME_HANDLE_TO_FD once, then
    * nothing but CS per frame). */
   if (external &&
       (info->extent.width  > R3V_R3XX_MAX_TEXTURE_DIMENSION ||
        info->extent.height > R3V_R3XX_MAX_TEXTURE_DIMENSION))
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r3v: external image %ux%u exceeds the single-tile export bound",
                       info->extent.width, info->extent.height);

   return VK_SUCCESS;
}

/* Resolve the backing pipe format and the usage set the format supports;
 * a linear image narrows usage to what its single row-major tile can back. */
static VkResult
r3v_image_resolve_format_usage(struct r3v_device *device,
                                  const VkImageCreateInfo *info,
                                  bool is_linear,
                                  enum pipe_format *pipe_fmt_out,
                                  VkImageUsageFlags *supported_usage_out)
{
   enum pipe_format pipe_fmt = r3v_vk_format_to_pipe_format(info->format);
   if (pipe_fmt == PIPE_FORMAT_NONE)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r3v: unsupported image format %d", info->format);

   VkImageUsageFlags supported_usage =
      r3v_supported_image_usage(device, pipe_fmt);

   /* A linear image lives inside a single r300g tile: transfer staging, plus
    * colour-attachment and sampled use when the format's oracle bits allow
    * them -- r300 renders to and samples from row-major surfaces (scanout on
    * this silicon is linear), and the common WSI's software swapchain creates
    * exactly such images (linear, COLOR_ATTACHMENT | TRANSFER, one tile).
    * Reject any shape the single-tile row-major path cannot back: 3D, an
    * extent past one tile, or a format without a lossless transfer-destination
    * byte layout (the same predicate that gates linearTilingFeatures).  A 1D
    * image is a single row, so it always fits the row-major tile. */
   if (is_linear) {
      /* INPUT_ATTACHMENT rides the sampled path: r3v lowers subpassLoad to
       * a normalized texture read, so any sampled-capable image serves as an
       * input attachment.  zink requests it on every swapchain image, and the
       * software swapchain\'s images are linear, so excluding it here rejected
       * vkCreateSwapchainKHR outright. */
      supported_usage &= (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
      if ((info->imageType != VK_IMAGE_TYPE_2D &&
           info->imageType != VK_IMAGE_TYPE_1D) ||
          info->extent.width > R3V_R3XX_MAX_RENDER_DIMENSION ||
          info->extent.height > R3V_R3XX_MAX_RENDER_DIMENSION ||
          !r3v_format_supports_transfer_dst(pipe_fmt))
         return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                          "r3v: linear image unsupported (format %d type %d %ux%u)",
                          info->format, info->imageType,
                          info->extent.width, info->extent.height);
   }

   if ((info->usage & ~supported_usage) != 0)
      return vk_errorf(device, VK_ERROR_FORMAT_NOT_SUPPORTED,
                       "r3v: unsupported image usage 0x%x for format %d",
                       info->usage & ~supported_usage, info->format);

   *pipe_fmt_out = pipe_fmt;
   *supported_usage_out = supported_usage;
   return VK_SUCCESS;
}

/* Record the BO\'s real row pitch for vkGetImageSubresourceLayout: the KMS
 * handle query fills stride without exporting an fd.  Single-tile images
 * carry the BO in tiles[0] with resource NULL (the replay\'s own
 * tiles[0]-or-resource pattern); export must follow it. */
static void
r3v_image_capture_external_stride(struct r3v_device *device,
                                     struct r3v_image *img)
{
   struct pipe_resource *bo = img->tiles[0] ? img->tiles[0] : img->resource;
   struct winsys_handle wh = { .type = WINSYS_HANDLE_TYPE_KMS };
   if (bo &&
       device->screen->resource_get_handle(device->screen, NULL, bo, &wh,
                                           PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE))
      img->external_stride = wh.stride;
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_CreateImage(VkDevice _device,
                   const VkImageCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkImage *pImage)
{
   VK_FROM_HANDLE(r3v_device, device, _device);

   /* r3v renders into r300g-tiled (optimal) resources.  A linear image
    * exists only as transfer staging plus the software swapchain\'s
    * attachment shape; the resolve stage below constrains it to one tile. */
   const bool is_linear = pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR;
   /* dma-buf export: wsi-drm swapchain images arrive with
    * VkExternalMemoryImageCreateInfo. */
   const VkExternalMemoryImageCreateInfo *ext_info =
      vk_find_struct_const(pCreateInfo->pNext, EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   const bool external = ext_info && ext_info->handleTypes != 0;

   VkResult result =
      r3v_image_validate_shape(device, pCreateInfo, is_linear, external);
   if (result != VK_SUCCESS)
      return result;

   enum pipe_format pipe_fmt = PIPE_FORMAT_NONE;
   VkImageUsageFlags supported_usage;
   result = r3v_image_resolve_format_usage(device, pCreateInfo, is_linear,
                                              &pipe_fmt, &supported_usage);
   if (result != VK_SUCCESS)
      return result;

   struct r3v_image *img =
      vk_zalloc2(&device->vk.alloc, pAllocator, sizeof(*img), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!img)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_init(&device->vk, &img->vk, pCreateInfo);

   unsigned pipe_bind =
      r3v_image_pipe_bind(device, pipe_fmt, pCreateInfo->usage);
   /* Every tile is row-major: the replay's transfers, clears, and copies are
    * CPU passes through pipe texture_map, and r300g maps a tiled texture by
    * DETILING THROUGH A BLIT (r300_transfer.c) whose copy path reinterprets
    * formats by block size (r300_resource_copy_region: 8-byte texels ride
    * R16G16B16A16_UNORM through the FP24 ALU, 16-byte texels have no remap at
    * all) -- exact for 8-bit channels, lossy or undefined past them.  Linear
    * tiles make every map a direct mapping, so the byte-exact transfer
    * contract (r3v_format_supports_transfer_dst) holds for every format,
    * including 32-bit float and block-compressed data.  r300 renders to and
    * samples from row-major surfaces (scanout is linear), trading sampler
    * cache locality for transfer correctness on this CPU-replay device. */
   pipe_bind |= PIPE_BIND_LINEAR;
   if (external)
      pipe_bind |= PIPE_BIND_SHARED | PIPE_BIND_SCANOUT;

   result = r3v_image_create_tile_resources(device, img, pCreateInfo,
                                               pipe_fmt, pipe_bind);
   if (result != VK_SUCCESS) {
      vk_image_finish(&img->vk);
      vk_free2(&device->vk.alloc, pAllocator, img);
      return result;
   }

   img->external = external;
   if (external)
      r3v_image_capture_external_stride(device, img);

   *pImage = r3v_image_to_handle(img);
   return VK_SUCCESS;
}

void
r3v_DestroyImage(VkDevice _device,
                    VkImage _image,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_image, img, _image);
   if (!img)
      return;

   r3v_image_release_resources(img);
   vk_image_finish(&img->vk);
   vk_free2(&device->vk.alloc, pAllocator, img);
}

VkDeviceSize
r3v_image_memory_size(const struct r3v_image *img)
{
   const VkExtent3D *ext = &img->vk.extent;

   const enum pipe_format pipe_fmt =
      r3v_vk_format_to_pipe_format(img->vk.format);

   /* A linear image's bytes are r300g's row-major tile: stride times the row
    * count.  Rows are block rows (util_format_get_blocksize is bytes per
    * block, and r300g's stride spans one row of blocks), so a block-compressed
    * format counts nblocksy rows, not texel rows; for plain formats the two
    * are equal.  Reporting the same value GetImageSubresourceLayout returns
    * keeps the BindImageMemory2 bound check and the deqp readback walk
    * consistent. */
   if (img->vk.tiling == VK_IMAGE_TILING_LINEAR)
      return (VkDeviceSize)img->linear_row_pitch *
             util_format_get_nblocksy(pipe_fmt, ext->height);

   const unsigned bytes_per_block = util_format_get_blocksize(pipe_fmt);
   VkDeviceSize total = 0;
   for (uint32_t level = 0; level < img->vk.mip_levels; level++) {
      const uint32_t w = MAX2(ext->width >> level, 1u);
      const uint32_t h = MAX2(ext->height >> level, 1u);
      total += (VkDeviceSize)util_format_get_nblocksx(pipe_fmt, w) *
               util_format_get_nblocksy(pipe_fmt, h) *
               MAX2(1u, img->vk.samples) * bytes_per_block;
   }
   return total;
}

static void
r3v_image_subresource_layout(VkImage _image,
                                VkSubresourceLayout2 *pLayout)
{
   VK_FROM_HANDLE(r3v_image, img, _image);
   VkSubresourceLayout *layout = &pLayout->subresourceLayout;

   /* r3v images are single mip level and single array layer (r3v_CreateImage
    * rejects mipLevels > 1 and arrayLayers > 1), so the queried subresource is
    * always level 0 / layer 0 and pSubresource selects no other layout. */
   if (img->external && img->external_stride) {
      /* Exported BO: the winsys-reported pitch is the layout KMS/DRI3 sees. */
      layout->offset = 0;
      layout->rowPitch = img->external_stride;
      layout->size = (VkDeviceSize)img->external_stride * img->vk.extent.height;
      layout->arrayPitch = layout->size;
      layout->depthPitch = layout->size;
      return;
   }

   if (img->vk.tiling == VK_IMAGE_TILING_LINEAR) {
      /* The single linear tile is row-major from offset 0.  rowPitch is the
       * stride r300g's transfer map returns, so the mapped readback walks the
       * same rows.  Rows are block rows (for a block-compressed format
       * rowPitch spans one row of blocks, per the Vulkan compressed-layout
       * rule), so size counts nblocksy rows -- equal to texel rows for plain
       * formats -- matching r3v_image_memory_size.  One 2D layer, no depth,
       * so array/depth pitch are the layer size. */
      layout->offset = 0;
      layout->rowPitch = img->linear_row_pitch;
      layout->size = (VkDeviceSize)img->linear_row_pitch *
                     util_format_get_nblocksy(
                        r3v_vk_format_to_pipe_format(img->vk.format),
                        img->vk.extent.height);
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
 * assert(disp[disp_index] == NULL) at vkCreateDevice, aborting every r3v
 * device.  Define only the core form, as every other Mesa driver does; the
 * table dispatches the KHR/EXT names to it. */
VKAPI_ATTR void VKAPI_CALL
r3v_GetImageSubresourceLayout2(VkDevice device, VkImage image,
                                  const VkImageSubresource2 *pSubresource,
                                  VkSubresourceLayout2 *pLayout)
{
   r3v_image_subresource_layout(image, pLayout);
}

/* VK_KHR_maintenance5: the subresource layout of an image that has not been
 * created (VkDeviceImageSubresourceInfo carries the VkImageCreateInfo).  r300g
 * derives the row pitch from the realized pipe_resource, so the honest answer
 * realizes the image transiently, reads its layout, and frees it -- no device
 * memory is bound, only the resource metadata is allocated.  A create the chip
 * cannot honour (multi-mip, array, or 3D, which r3v_CreateImage rejects)
 * yields a zeroed layout, the same as the optimal-tiling case. */
VKAPI_ATTR void VKAPI_CALL
r3v_GetDeviceImageSubresourceLayout(VkDevice _device,
                                       const VkDeviceImageSubresourceInfo *pInfo,
                                       VkSubresourceLayout2 *pLayout)
{
   VkImage image;
   if (r3v_CreateImage(_device, pInfo->pCreateInfo, NULL, &image) != VK_SUCCESS) {
      pLayout->subresourceLayout = (VkSubresourceLayout){0};
      return;
   }
   r3v_image_subresource_layout(image, pLayout);
   r3v_DestroyImage(_device, image, NULL);
}

void
r3v_GetImageMemoryRequirements2(VkDevice _device,
                                    const VkImageMemoryRequirementsInfo2 *pInfo,
                                    VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(r3v_image, img, pInfo->image);

   /* 4096-byte alignment satisfies r300g tiling requirements.  The reported
    * size must match the bind-time bound check in BindImageMemory2, so both
    * read it from r3v_image_memory_size(). */
   pMemoryRequirements->memoryRequirements = (VkMemoryRequirements){
      .size           = r3v_image_memory_size(img),
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

   /* VK_KHR_dedicated_allocation: an external image is backed by a single
    * SHARED|SCANOUT BO that the PRIME export path (r3v_GetMemoryFdKHR)
    * reaches through the memory's dedicated_image, so a dedicated allocation
    * is preferred for it.  It is deliberately not reported as *required*:
    * requiresDedicatedAllocation is a hard constraint on the caller, and
    * suballocation is not proven to break export, so the honest report is the
    * preference only. */
   VkMemoryDedicatedRequirements *dedicated =
      vk_find_struct(pMemoryRequirements->pNext, MEMORY_DEDICATED_REQUIREMENTS);
   if (dedicated) {
      dedicated->prefersDedicatedAllocation  = img->external;
      dedicated->requiresDedicatedAllocation = VK_FALSE;
   }
}

/* VK_KHR_get_memory_requirements2: the device exposes no sparse residency
 * (sparseAddressSpaceSize == 0 and r3v_CreateImage rejects every sparse
 * VkImage), so an image has no sparse memory requirements -- report a zero
 * count and leave the caller's array untouched. */
void
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

VkResult
r3v_CreateImageView(VkDevice _device,
                       const VkImageViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkImageView *pView)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   struct r3v_image_view *iv;

   iv = vk_zalloc2(&device->vk.alloc, pAllocator,
                   sizeof(*iv), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!iv)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_view_init(&device->vk, &iv->vk, pCreateInfo);

   *pView = r3v_image_view_to_handle(iv);
   return VK_SUCCESS;
}

void
r3v_DestroyImageView(VkDevice _device,
                         VkImageView _view,
                         const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(r3v_device, device, _device);
   VK_FROM_HANDLE(r3v_image_view, iv, _view);
   if (!iv)
      return;

   vk_image_view_finish(&iv->vk);
   vk_free2(&device->vk.alloc, pAllocator, iv);
}
