/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V transfer-op fixture: dword-pattern fills, inline updates,
 * and nearest scaling and unit-scale blits through host mappings at
 * submission, with the overlap, scale, flip, alignment, and unsupported-layout refusals
 * that keep the recorded surface fail-closed under the drm-shim
 * transport.
 */

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "../r3v_native.h"
#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/radeon_legacy_2d_reg.h"

static unsigned failures;

enum mutation_mode {
   MUTATION_NONE,
   /* A scaling blit is reported as admitted. */
   MUTATION_SCALED_LINEAR_BLIT_ADMITS,
   /* A same-buffer overlapping copy is reported as admitted. */
   MUTATION_OVERLAP_COPY_ADMITS,
};

static enum mutation_mode mutation;

#define CHECK(condition, ...)                                                \
   do {                                                                      \
      if (!(condition)) {                                                    \
         failures++;                                                         \
         fprintf(stderr, "FAIL: ");                                         \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                            \
      }                                                                      \
   } while (0)

#define REQUIRE(condition, ...)                                              \
   do {                                                                      \
      if (!(condition)) {                                                    \
         failures++;                                                         \
         fprintf(stderr, "FAIL: ");                                         \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                            \
         return 1;                                                           \
      }                                                                      \
   } while (0)

struct fixture {
   VkInstance instance;
   VkPhysicalDevice pdev;
   VkDevice device;
   VkQueue queue;
   VkCommandPool cmd_pool;
   VkCommandBuffer cmd;
};

static int
begin(const struct fixture *f)
{
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "command pool reset");
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "command buffer begin");
   return 0;
}

static int
submit(const struct fixture *f)
{
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &f->cmd,
   };
   REQUIRE(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
              VK_SUCCESS,
           "queue submit");
   REQUIRE(vkQueueWaitIdle(f->queue) == VK_SUCCESS, "queue wait idle");
   return 0;
}

struct staging {
   VkBuffer buffer;
   VkDeviceMemory memory;
   uint8_t *map;
};

static int
create_staging(const struct fixture *f, VkDeviceSize bytes,
               VkBufferUsageFlags usage, struct staging *out)
{
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   REQUIRE(vkCreateBuffer(f->device, &buffer_info, NULL, &out->buffer) ==
              VK_SUCCESS,
           "staging buffer creation");
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = (bytes + 4095) & ~(VkDeviceSize)4095,
      .memoryTypeIndex = 0,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocate_info, NULL,
                            &out->memory) == VK_SUCCESS,
           "staging memory allocation");
   REQUIRE(vkBindBufferMemory(f->device, out->buffer, out->memory, 0) ==
              VK_SUCCESS,
           "staging buffer bind");
   void *map = NULL;
   REQUIRE(vkMapMemory(f->device, out->memory, 0, VK_WHOLE_SIZE, 0,
                       &map) == VK_SUCCESS,
           "staging memory map");
   out->map = map;
   return 0;
}

static void
destroy_staging(const struct fixture *f, struct staging *s)
{
   vkUnmapMemory(f->device, s->memory);
   vkDestroyBuffer(f->device, s->buffer, NULL);
   vkFreeMemory(f->device, s->memory, NULL);
}

static int
check_fill_and_update(const struct fixture *f)
{
   struct staging s;
   if (create_staging(f, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s))
      return 1;
   memset(s.map, 0xa5, 256);

   if (begin(f))
      return 1;
   vkCmdFillBuffer(f->cmd, s.buffer, 16, 32, 0x11223344);
   /* VK_WHOLE_SIZE runs from the offset to the buffer end. */
   vkCmdFillBuffer(f->cmd, s.buffer, 192, VK_WHOLE_SIZE, 0xcafef00d);
   const uint32_t words[3] = { 0x00000001, 0x00000002, 0x00000003 };
   vkCmdUpdateBuffer(f->cmd, s.buffer, 64, sizeof(words), words);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "fill and update recording admits");
   if (submit(f))
      return 1;

   uint32_t word;
   memcpy(&word, s.map + 16, 4);
   CHECK(word == 0x11223344, "the fill pattern landed at the offset");
   memcpy(&word, s.map + 44, 4);
   CHECK(word == 0x11223344, "the fill pattern landed at the range end");
   CHECK(s.map[12] == 0xa5 && s.map[48] == 0xa5,
         "bytes outside the fill range are untouched");
   memcpy(&word, s.map + 192, 4);
   CHECK(word == 0xcafef00d, "the whole-size fill runs to the buffer end");
   memcpy(&word, s.map + 252, 4);
   CHECK(word == 0xcafef00d, "the whole-size fill covers the last dword");
   memcpy(&word, s.map + 64, 4);
   CHECK(word == 0x00000001, "the update bytes landed");
   memcpy(&word, s.map + 72, 4);
   CHECK(word == 0x00000003, "the update covers its full size");

   /* Refusals: a misaligned offset, a non-dword size, and a
    * destination without transfer usage each poison the recording.
    */
   if (begin(f))
      return 1;
   vkCmdFillBuffer(f->cmd, s.buffer, 2, 8, 0);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "a misaligned fill offset poisons the recording");
   if (begin(f))
      return 1;
   vkCmdFillBuffer(f->cmd, s.buffer, 0, 6, 0);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "a non-dword fill size poisons the recording");

   struct staging plain;
   if (create_staging(f, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &plain))
      return 1;
   if (begin(f))
      return 1;
   vkCmdFillBuffer(f->cmd, plain.buffer, 0, 64, 0);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "a fill destination without transfer usage poisons the "
         "recording");
   destroy_staging(f, &plain);
   destroy_staging(f, &s);
   return 0;
}

static int
check_copy_overlap(const struct fixture *f)
{
   struct staging s;
   if (create_staging(f, 256,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      &s))
      return 1;

   if (begin(f))
      return 1;
   const VkBufferCopy overlapping = {
      .srcOffset = 0,
      .dstOffset = 32,
      .size = 64,
   };
   vkCmdCopyBuffer(f->cmd, s.buffer, s.buffer, 1, &overlapping);
   const VkResult overlap_end = vkEndCommandBuffer(f->cmd);
   if (mutation == MUTATION_OVERLAP_COPY_ADMITS)
      CHECK(overlap_end == VK_SUCCESS,
            "mutation: overlapping copy reported admitted");
   else
      CHECK(overlap_end != VK_SUCCESS,
            "a same-buffer overlapping copy poisons the recording");

   for (unsigned i = 0; i < 64; i++)
      s.map[i] = (uint8_t)i;
   if (begin(f))
      return 1;
   const VkBufferCopy disjoint = {
      .srcOffset = 0,
      .dstOffset = 64,
      .size = 64,
   };
   vkCmdCopyBuffer(f->cmd, s.buffer, s.buffer, 1, &disjoint);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "a same-buffer disjoint copy admits");
   if (submit(f))
      return 1;
   CHECK(memcmp(s.map, s.map + 64, 64) == 0,
         "the disjoint same-buffer copy moved the bytes");
   destroy_staging(f, &s);
   return 0;
}

struct transfer_image {
   VkImage image;
   VkDeviceMemory memory;
   uint8_t *map;
   uint32_t row_pitch;
};

static int
create_transfer_image_format(const struct fixture *f, uint32_t width,
                             uint32_t height, VkFormat format,
                             struct transfer_image *out)
{
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   REQUIRE(vkCreateImage(f->device, &image_info, NULL, &out->image) ==
              VK_SUCCESS,
           "transfer image creation");
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(f->device, out->image, &requirements);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = 0,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocate_info, NULL,
                            &out->memory) == VK_SUCCESS,
           "transfer image memory");
   REQUIRE(vkBindImageMemory(f->device, out->image, out->memory, 0) ==
              VK_SUCCESS,
           "transfer image bind");
   const VkImageSubresource subresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
   };
   VkSubresourceLayout layout;
   vkGetImageSubresourceLayout(f->device, out->image, &subresource,
                               &layout);
   out->row_pitch = (uint32_t)layout.rowPitch;
   void *map = NULL;
   REQUIRE(vkMapMemory(f->device, out->memory, 0, VK_WHOLE_SIZE, 0,
                       &map) == VK_SUCCESS,
           "transfer image map");
   out->map = map;
   return 0;
}

static int
create_transfer_image(const struct fixture *f, uint32_t width,
                      uint32_t height, struct transfer_image *out)
{
   return create_transfer_image_format(f, width, height,
                                       VK_FORMAT_B8G8R8A8_UNORM, out);
}

/* An OPTIMAL-tiled transfer image executes the identical linear span
 * over the GEM BO as the LINEAR cell (r3v_native_transfer_footprint_bytes),
 * so this fixture derives the row pitch from the same 64-byte-aligned
 * formula instead of vkGetImageSubresourceLayout, which the driver
 * refuses to answer for a VK_IMAGE_TILING_OPTIMAL image in a debug
 * build (VUID-vkGetImageSubresourceLayout-image-07790; see
 * check_optimal_tiling below for the branched oracle).
 */
static int
create_transfer_image_optimal(const struct fixture *f, uint32_t width,
                              uint32_t height, struct transfer_image *out)
{
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   REQUIRE(vkCreateImage(f->device, &image_info, NULL, &out->image) ==
              VK_SUCCESS,
           "optimal-tiled transfer image creation");
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(f->device, out->image, &requirements);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = 0,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocate_info, NULL,
                            &out->memory) == VK_SUCCESS,
           "optimal-tiled transfer image memory");
   REQUIRE(vkBindImageMemory(f->device, out->image, out->memory, 0) ==
              VK_SUCCESS,
           "optimal-tiled transfer image bind");
   out->row_pitch = (width * 4u + 63u) & ~63u;
   void *map = NULL;
   REQUIRE(vkMapMemory(f->device, out->memory, 0, VK_WHOLE_SIZE, 0,
                       &map) == VK_SUCCESS,
           "optimal-tiled transfer image map");
   out->map = map;
   return 0;
}

static void
destroy_transfer_image(const struct fixture *f, struct transfer_image *img)
{
   vkUnmapMemory(f->device, img->memory);
   vkDestroyImage(f->device, img->image, NULL);
   vkFreeMemory(f->device, img->memory, NULL);
}

/* bufferImageHeight supplies the distance between slices.  A depth-one copy
 * reads imageExtent.height rows from the current slice, so a large stride does
 * not require storage for the unused rows between slices. */
static int
check_buffer_image_height_stride(const struct fixture *f)
{
   struct transfer_image image;
   if (create_transfer_image(f, 4, 4, &image))
      return 1;
   struct staging staging;
   if (create_staging(f, 32, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging))
      return 1;
   for (uint32_t index = 0u; index < 32u; index++)
      staging.map[index] = (uint8_t)(index + 1u);

   const VkBufferImageCopy region = {
      .bufferRowLength = 4u,
      .bufferImageHeight = 100u,
      .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u},
      .imageExtent = {4u, 2u, 1u},
   };
   if (begin(f))
      return 1;
   vkCmdCopyBufferToImage(f->cmd, staging.buffer, image.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "depth-one copy uses imageExtent.height rows with a large slice stride");
   if (submit(f))
      return 1;

   CHECK(memcmp(image.map, staging.map, 16u) == 0,
         "the first depth-one copy row span is transferred");
   CHECK(memcmp(image.map + image.row_pitch, staging.map + 16u, 16u) == 0,
         "the second depth-one copy row span is transferred");
   destroy_staging(f, &staging);
   destroy_transfer_image(f, &image);
   return 0;
}

static uint32_t
texel(const struct transfer_image *img, uint32_t x, uint32_t y)
{
   uint32_t word;
   memcpy(&word, img->map + (uint64_t)y * img->row_pitch + x * 4, 4);
   return word;
}

static int
check_blit(const struct fixture *f)
{
   struct transfer_image src, dst;
   if (create_transfer_image(f, 16, 16, &src) ||
       create_transfer_image(f, 16, 16, &dst))
      return 1;
   for (uint32_t y = 0; y < 16; y++)
      for (uint32_t x = 0; x < 16; x++) {
         const uint32_t value = (y << 16) | x;
         memcpy(src.map + (uint64_t)y * src.row_pitch + x * 4, &value, 4);
      }
   memset(dst.map, 0, (uint64_t)16 * dst.row_pitch);

   const VkImageSubresourceLayers layers = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .layerCount = 1,
   };

   if (begin(f))
      return 1;
   const VkImageBlit unit = {
      .srcSubresource = layers,
      .srcOffsets = { { 2, 3, 0 }, { 10, 11, 1 } },
      .dstSubresource = layers,
      .dstOffsets = { { 5, 6, 0 }, { 13, 14, 1 } },
   };
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &unit, VK_FILTER_LINEAR);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "a unit-scale blit admits under either filter");
   if (submit(f))
      return 1;
   CHECK(texel(&dst, 5, 6) == texel(&src, 2, 3),
         "the blit moved the rectangle origin");
   CHECK(texel(&dst, 12, 13) == texel(&src, 9, 10),
         "the blit moved the rectangle end");
   CHECK(texel(&dst, 4, 6) == 0 && texel(&dst, 13, 14) == 0,
         "texels outside the blit rectangle are untouched");

   if (begin(f))
      return 1;
   const VkImageBlit scaled = {
      .srcSubresource = layers,
      .srcOffsets = { { 0, 0, 0 }, { 8, 8, 1 } },
      .dstSubresource = layers,
      .dstOffsets = { { 0, 0, 0 }, { 16, 16, 1 } },
   };
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &scaled, VK_FILTER_NEAREST);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "a nearest scaling blit admits");
   if (submit(f))
      return 1;
   CHECK(texel(&dst, 0, 0) == texel(&src, 0, 0) &&
         texel(&dst, 1, 0) == texel(&src, 0, 0) &&
         texel(&dst, 8, 3) == texel(&src, 4, 1) &&
         texel(&dst, 15, 15) == texel(&src, 7, 7),
         "the 2x resample reads the nearest sample at (d + 0.5) / 2");

   if (begin(f))
      return 1;
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &scaled, VK_FILTER_LINEAR);
   const VkResult linear_end = vkEndCommandBuffer(f->cmd);
   if (mutation == MUTATION_SCALED_LINEAR_BLIT_ADMITS)
      CHECK(linear_end == VK_SUCCESS,
            "mutation: linear scaling blit reported admitted");
   else
      CHECK(linear_end != VK_SUCCESS,
            "a linear scaling blit poisons the recording: the resample "
            "executor is nearest only");

   if (begin(f))
      return 1;
   const VkImageBlit flipped = {
      .srcSubresource = layers,
      .srcOffsets = { { 8, 8, 0 }, { 0, 0, 1 } },
      .dstSubresource = layers,
      .dstOffsets = { { 0, 0, 0 }, { 8, 8, 1 } },
   };
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, dst.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &flipped, VK_FILTER_NEAREST);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "an axis-flipped blit poisons the recording");

   if (begin(f))
      return 1;
   const VkImageBlit self_overlap = {
      .srcSubresource = layers,
      .srcOffsets = { { 0, 0, 0 }, { 8, 8, 1 } },
      .dstSubresource = layers,
      .dstOffsets = { { 4, 4, 0 }, { 12, 12, 1 } },
   };
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, src.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &self_overlap,
                  VK_FILTER_NEAREST);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "a same-image overlapping blit poisons the recording");

   if (begin(f))
      return 1;
   const VkImageBlit self_disjoint = {
      .srcSubresource = layers,
      .srcOffsets = { { 0, 0, 0 }, { 4, 4, 1 } },
      .dstSubresource = layers,
      .dstOffsets = { { 8, 8, 0 }, { 12, 12, 1 } },
   };
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_GENERAL, src.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &self_disjoint,
                  VK_FILTER_NEAREST);
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "a same-image disjoint blit admits");

   if (begin(f))
      return 1;
   const VkImageBlit copy_shape = {
      .srcSubresource = layers,
      .srcOffsets = { { 0, 0, 0 }, { 4, 4, 1 } },
      .dstSubresource = layers,
      .dstOffsets = { { 0, 0, 0 }, { 4, 4, 1 } },
   };
   vkCmdBlitImage(f->cmd, src.image, VK_IMAGE_LAYOUT_UNDEFINED, dst.image,
                  VK_IMAGE_LAYOUT_GENERAL, 1, &copy_shape,
                  VK_FILTER_NEAREST);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "an unsupported source layout poisons the recording");

   destroy_transfer_image(f, &src);
   destroy_transfer_image(f, &dst);
   return 0;
}


/* The texel table beyond four bytes: an 8- and a 16-byte texel image
 * round-trip buffer -> image -> buffer through a padded buffer row
 * length and a non-zero image offset byte for byte, and the format
 * clear lands each format's packed texel across the full extent.
 */
static int
check_texel_formats(const struct fixture *f)
{
   static const struct {
      VkFormat format;
      uint32_t texel_bytes;
   } formats[] = {
      { VK_FORMAT_R16G16B16A16_UINT, 8 },
      { VK_FORMAT_R32G32B32A32_UINT, 16 },
   };
   for (unsigned i = 0; i < 2; i++) {
      const uint32_t tb = formats[i].texel_bytes;
      const uint32_t w = 6, h = 5, row_length = 9;
      struct transfer_image img;
      if (create_transfer_image_format(f, 16, 12, formats[i].format, &img))
         return 1;
      struct staging src, dst;
      if (create_staging(f, (VkDeviceSize)row_length * h * tb,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src) ||
          create_staging(f, (VkDeviceSize)row_length * h * tb,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst))
         return 1;
      for (uint32_t b = 0; b < row_length * h * tb; b++) {
         src.map[b] = (uint8_t)(b * 7u + i);
         dst.map[b] = 0xee;
      }
      const VkBufferImageCopy region = {
         .bufferRowLength = row_length,
         .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
         .imageOffset = { 3, 2, 0 },
         .imageExtent = { w, h, 1 },
      };
      if (begin(f))
         return 1;
      vkCmdCopyBufferToImage(f->cmd, src.buffer, img.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                             &region);
      vkCmdCopyImageToBuffer(f->cmd, img.image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             dst.buffer, 1, &region);
      REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
              "texel-format round-trip recording");
      if (submit(f))
         return 1;
      unsigned mismatches = 0;
      for (uint32_t y = 0; y < h; y++) {
         for (uint32_t b = 0; b < w * tb; b++) {
            const uint64_t at = (uint64_t)y * row_length * tb + b;
            if (dst.map[at] != src.map[at])
               mismatches++;
            const uint8_t *in_image =
               img.map + (uint64_t)(2 + y) * img.row_pitch + 3 * tb + b;
            if (*in_image != src.map[at])
               mismatches++;
         }
         /* The padding past the copied row stays untouched. */
         for (uint32_t b = w * tb; b < row_length * tb; b++) {
            if (dst.map[(uint64_t)y * row_length * tb + b] != 0xee)
               mismatches++;
         }
      }
      CHECK(mismatches == 0,
            "%u-byte texel round-trip through a %u-texel row length at "
            "offset (3, 2): %u byte mismatches",
            tb, row_length, mismatches);

      const VkClearColorValue color = {
         .uint32 = { 0x00010203u, 0x8000fffeu, 0x00000001u, 0xdeadbeefu },
      };
      const VkImageSubresourceRange whole = {
         VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
      };
      if (begin(f))
         return 1;
      vkCmdClearColorImage(f->cmd, img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                           &whole);
      REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
              "texel-format clear recording");
      if (submit(f))
         return 1;
      uint8_t expect[16];
      if (tb == 8) {
         const uint16_t lanes[4] = { 0x0203, 0xfffe, 0x0001, 0xbeef };
         for (unsigned c = 0; c < 4; c++) {
            expect[2 * c] = (uint8_t)(lanes[c] & 0xff);
            expect[2 * c + 1] = (uint8_t)(lanes[c] >> 8);
         }
      } else {
         for (unsigned c = 0; c < 4; c++) {
            const uint32_t v = color.uint32[c];
            expect[4 * c] = (uint8_t)v;
            expect[4 * c + 1] = (uint8_t)(v >> 8);
            expect[4 * c + 2] = (uint8_t)(v >> 16);
            expect[4 * c + 3] = (uint8_t)(v >> 24);
         }
      }
      mismatches = 0;
      for (uint32_t y = 0; y < 12; y++) {
         for (uint32_t x = 0; x < 16; x++) {
            if (memcmp(img.map + (uint64_t)y * img.row_pitch + x * tb,
                       expect, tb) != 0)
               mismatches++;
         }
      }
      CHECK(mismatches == 0,
            "%u-byte texel clear lands the packed texel on every texel: "
            "%u mismatches", tb, mismatches);
      destroy_staging(f, &src);
      destroy_staging(f, &dst);
      destroy_transfer_image(f, &img);
   }
   return 0;
}

static volatile sig_atomic_t wait_timed_out;

static void
mark_wait_timeout(int signum)
{
   (void)signum;
   wait_timed_out = 1;
}

/* Child setup exit codes, distinct from the oracle result: 2 is
 * vkCreateInstance, 3 is vkEnumeratePhysicalDevices, 4 is
 * vkCreateDevice, 5 is vkCreateImage. A setup failure means this
 * fixture could not reach the known-bad call at all, and it reports
 * as its own defect rather than folding into the oracle check.
 */
static const char *
optimal_probe_setup_step(int code)
{
   switch (code) {
   case 2: return "vkCreateInstance";
   case 3: return "vkEnumeratePhysicalDevices";
   case 4: return "vkCreateDevice";
   case 5: return "vkCreateImage";
   default: return NULL;
   }
}

/* vkGetImageSubresourceLayout on a VK_IMAGE_TILING_OPTIMAL image is
 * invalid application usage (VUID-vkGetImageSubresourceLayout-image-
 * 07790); r3v_native_image.c enforces it with assert(), which is live
 * only when this build's b_ndebug leaves NDEBUG undefined. The known-
 * bad oracle branches at compile time on the same macro the driver
 * checks, since the test binary shares the project-wide b_ndebug
 * setting: a live assert must abort, and a compiled-out assert must
 * return an empty layout rather than exposing opaque storage. The call runs in a forked child with its
 * own fresh instance and device, since reusing the parent's live
 * device across fork risks the drm-shim mock's per-process handle
 * bookkeeping, which this probe does not need to share; the wait is
 * bounded by SIGALRM because the child re-enters the loader and
 * driver after fork.
 */
static int
check_optimal_subresource_layout_oracle(const struct fixture *f)
{
   (void)f;
   int pipefd[2];
   REQUIRE(pipe(pipefd) == 0,
           "pipe for the subresource-layout known-bad leg");

   pid_t child = fork();
   REQUIRE(child >= 0, "fork for the subresource-layout known-bad leg");
   if (child == 0) {
      close(pipefd[0]);
      VkInstance instance = VK_NULL_HANDLE;
      const VkApplicationInfo app_info = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .apiVersion = VK_API_VERSION_1_0,
      };
      const VkInstanceCreateInfo instance_info = {
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo = &app_info,
      };
      if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS)
         _exit(2);
      uint32_t count = 1;
      VkPhysicalDevice pdev = VK_NULL_HANDLE;
      VkResult enumerated = vkEnumeratePhysicalDevices(instance, &count,
                                                        &pdev);
      if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) ||
          count != 1)
         _exit(3);
      const float priority = 1.0f;
      const VkDeviceQueueCreateInfo queue_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0,
         .queueCount = 1,
         .pQueuePriorities = &priority,
      };
      const VkDeviceCreateInfo device_info = {
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &queue_info,
      };
      VkDevice device = VK_NULL_HANDLE;
      if (vkCreateDevice(pdev, &device_info, NULL, &device) != VK_SUCCESS)
         _exit(4);
      const VkImageCreateInfo probe_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { 4, 4, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      };
      VkImage probe_image = VK_NULL_HANDLE;
      if (vkCreateImage(device, &probe_info, NULL, &probe_image) !=
          VK_SUCCESS)
         _exit(5);
      const VkImageSubresource subresource = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      };
      VkSubresourceLayout layout;
      vkGetImageSubresourceLayout(device, probe_image, &subresource,
                                  &layout);
      /* Reached only when NDEBUG compiles the assert out; report the
       * computed layout so the parent can check it against the
       * transfer family's linear span. */
      ssize_t written = write(pipefd[1], &layout, sizeof(layout));
      (void)written;
      _exit(0);
   }
   close(pipefd[1]);

   struct sigaction alarm_action = { .sa_handler = mark_wait_timeout };
   struct sigaction old_action;
   sigemptyset(&alarm_action.sa_mask);
   sigaction(SIGALRM, &alarm_action, &old_action);
   wait_timed_out = 0;
   alarm(5);
   int status = 0;
   pid_t waited = waitpid(child, &status, 0);
   int wait_errno = errno;
   alarm(0);
   sigaction(SIGALRM, &old_action, NULL);

   if (waited != child) {
      if (wait_timed_out || wait_errno == EINTR) {
         kill(child, SIGKILL);
         waitpid(child, &status, 0);
         CHECK(false,
               "the subresource-layout known-bad child did not exit "
               "within 5s; killed");
      } else {
         CHECK(false,
               "waiting for the subresource-layout known-bad child "
               "failed: errno %d", wait_errno);
      }
      close(pipefd[0]);
      return 0;
   }

   const char *setup_step =
      WIFEXITED(status) ? optimal_probe_setup_step(WEXITSTATUS(status))
                        : NULL;
   if (setup_step != NULL) {
      CHECK(false,
            "the subresource-layout known-bad child's setup failed at "
            "%s (exit code %d) before reaching the oracle", setup_step,
            WEXITSTATUS(status));
      close(pipefd[0]);
      return 0;
   }

#ifdef NDEBUG
   CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "under NDEBUG (assert compiled out), the known-bad child "
         "exits cleanly instead of terminating on the oracle call "
         "(status 0x%x)", status);
   if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      VkSubresourceLayout layout = { 0 };
      ssize_t n = read(pipefd[0], &layout, sizeof(layout));
      CHECK(n == (ssize_t)sizeof(layout),
            "the known-bad child reports its computed layout (read %zd "
            "of %zu bytes)", n, sizeof(layout));
      if (n == (ssize_t)sizeof(layout)) {
         CHECK(layout.offset == 0 && layout.rowPitch == 0 && layout.size == 0 &&
                  layout.arrayPitch == 0 && layout.depthPitch == 0,
               "optimal image query returns an empty layout under NDEBUG");
      }
   }
#else
   CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
         "vkGetImageSubresourceLayout on an OPTIMAL image aborts the "
         "live debug assert (VUID-vkGetImageSubresourceLayout-image-"
         "07790) instead of answering a layout (status 0x%x)", status);
#endif
   close(pipefd[0]);
   return 0;
}

/* An OPTIMAL-tiled transfer image round-trips buffer->image->buffer
 * byte for byte, matching the LINEAR cell's contract, and vkCreateImage
 * still refuses an OPTIMAL request over the render family's
 * color-attachment usage (r3v_native_image.c admits OPTIMAL on the
 * transfer family alone).
 */
static int
check_optimal_tiling(const struct fixture *f)
{
   struct transfer_image img;
   if (create_transfer_image_optimal(f, 16, 12, &img))
      return 1;
   struct staging src, dst;
   const uint32_t w = 16, h = 12, tb = 4;
   if (create_staging(f, (VkDeviceSize)w * h * tb,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &src) ||
       create_staging(f, (VkDeviceSize)w * h * tb,
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, &dst))
      return 1;
   for (uint32_t b = 0; b < w * h * tb; b++) {
      src.map[b] = (uint8_t)(b * 11u + 5u);
      dst.map[b] = 0xcc;
   }
   const VkBufferImageCopy region = {
      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .imageExtent = { w, h, 1 },
   };
   if (begin(f))
      return 1;
   vkCmdCopyBufferToImage(f->cmd, src.buffer, img.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
   vkCmdCopyImageToBuffer(f->cmd, img.image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buffer,
                          1, &region);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "optimal-tiled round-trip recording");
   if (submit(f))
      return 1;
   CHECK(memcmp(src.map, dst.map, (size_t)w * h * tb) == 0,
         "the optimal-tiled buffer->image->buffer round trip moved the "
         "bytes");
   destroy_staging(f, &src);
   destroy_staging(f, &dst);
   destroy_transfer_image(f, &img);

   const VkImageCreateInfo optimal_attachment_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 16, 16, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   /* The render family executes one linear span whichever tiling the
    * application asks for, so an OPTIMAL color-attachment request
    * creates and stops answering vkGetImageSubresourceLayout alone.
    */
   VkImage optimal_attachment = VK_NULL_HANDLE;
   CHECK(vkCreateImage(f->device, &optimal_attachment_info, NULL,
                       &optimal_attachment) == VK_SUCCESS &&
            optimal_attachment != VK_NULL_HANDLE,
         "an OPTIMAL-tiled color-attachment request creates: the render "
         "family executes the one linear span under either tiling");
   vkDestroyImage(f->device, optimal_attachment, NULL);

   /* A swapchain-shaped presentable image--TRANSFER_DST for the
    * present blit plus COLOR_ATTACHMENT for the render target, OPTIMAL
    * tiling for scanout, an application-chosen extent above the render
    * family's R3V_NATIVE_RENDER_MAX_EXTENT ceiling--carries no route
    * through either admitted family: the extent fails the
    * color-attachment branch's ceiling and the usage mix fails the
    * transfer branch's usage-subset test (r3v_native_image.c
    * r3v_CreateImage), so it falls to the shared refusal.
    * docs/hardware/r3v-wsi-denominator.md names this as the gate a
    * swapchain-shaped image meets before any WSI callback executes;
    * pinning it here holds that gate through future image-admission
    * refactors.
    */
   const VkImageCreateInfo swapchain_shaped_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 512, 512, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkImage swapchain_refused = VK_NULL_HANDLE;
   CHECK(vkCreateImage(f->device, &swapchain_shaped_info, NULL,
                       &swapchain_refused) != VK_SUCCESS &&
            swapchain_refused == VK_NULL_HANDLE,
         "a swapchain-shaped TRANSFER_DST|COLOR_ATTACHMENT OPTIMAL "
         "image above the render family's extent ceiling refuses: no "
         "WSI-aware exception widens either admitted family");

   return check_optimal_subresource_layout_oracle(f);
}

static int
create_fixture(struct fixture *f)
{
   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };
   REQUIRE(vkCreateInstance(&instance_info, NULL, &f->instance) ==
              VK_SUCCESS,
           "instance creation");
   uint32_t count = 1;
   VkResult enumerated =
      vkEnumeratePhysicalDevices(f->instance, &count, &f->pdev);
   REQUIRE((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
              count == 1,
           "physical device enumeration");
   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   REQUIRE(vkCreateDevice(f->pdev, &device_info, NULL, &f->device) ==
              VK_SUCCESS,
           "device creation");
   vkGetDeviceQueue(f->device, 0, 0, &f->queue);
   const VkCommandPoolCreateInfo cmd_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   REQUIRE(vkCreateCommandPool(f->device, &cmd_pool_info, NULL,
                               &f->cmd_pool) == VK_SUCCESS,
           "command pool");
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = f->cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   REQUIRE(vkAllocateCommandBuffers(f->device, &cmd_info, &f->cmd) ==
              VK_SUCCESS,
           "command buffer allocation");
   return 0;
}

static bool
command_packet0_value(const struct r3v_native_cmd_buffer *cmd, uint32_t reg,
                      uint32_t *value)
{
   const uint32_t header = reg >> 2;
   for (uint32_t index = 0u; index + 1u < cmd->ib_size_dwords; index++) {
      if (cmd->ib[index] == header) {
         *value = cmd->ib[index + 1u];
         return true;
      }
   }
   return false;
}

static int
check_depth_clear_recording(const struct fixture *f, VkImage image)
{
   static const struct {
      VkImageAspectFlags aspects;
      VkClearDepthStencilValue clear;
      uint32_t internal_aspects;
      uint32_t write_mask;
      bool preserves_component;
   } cases[] = {
      {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
       {.depth = 0.25f, .stencil = 0xa5u}, R300_ZB_COMBINED_CLEAR_ASPECTS,
       UINT32_MAX, false},
      {VK_IMAGE_ASPECT_DEPTH_BIT, {.depth = 0.25f, .stencil = 0x1ffu},
       R300_ZB_COMBINED_CLEAR_ASPECT_DEPTH, 0xffffff00u, true},
      {VK_IMAGE_ASPECT_STENCIL_BIT, {.depth = NAN, .stencil = 0xa5u},
       R300_ZB_COMBINED_CLEAR_ASPECT_STENCIL, 0x000000ffu, true},
   };

   for (unsigned case_index = 0u; case_index < ARRAY_SIZE(cases);
        case_index++) {
      if (begin(f))
         return 1;
      const VkImageSubresourceRange range = {
         .aspectMask = cases[case_index].aspects,
         .baseMipLevel = 0u,
         .levelCount = 1u,
         .baseArrayLayer = 0u,
         .layerCount = 1u,
      };
      vkCmdClearDepthStencilImage(f->cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                                  &cases[case_index].clear, 1u, &range);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cmd, f->cmd);
      CHECK(native_cmd->cell_kind == R3V_NATIVE_CELL_KIND_ZB_DEPTH_CLEAR &&
               native_cmd->zb_depth_clear_configured &&
               native_cmd->zb_depth_clear_aspect_mask ==
                  cases[case_index].internal_aspects,
            "depth clear case %u records a GPU cell and aspect identity",
            case_index);
      CHECK(native_cmd->reference_count == 1u &&
               native_cmd->references[0].write_domain ==
                  RADEON_GEM_DOMAIN_GTT &&
               native_cmd->references[0].read_domains ==
                  (cases[case_index].preserves_component
                      ? RADEON_GEM_DOMAIN_GTT
                      : 0u),
            "depth clear case %u records exact destination access",
            case_index);
      uint32_t master = 0u, write_mask = 0u, extent = 0u;
      CHECK(command_packet0_value(native_cmd, RADEON_DP_GUI_MASTER_CNTL,
                                  &master) &&
               command_packet0_value(native_cmd, RADEON_DP_WRITE_MSK,
                                     &write_mask) &&
               command_packet0_value(native_cmd, RADEON_DST_WIDTH_HEIGHT,
                                     &extent) &&
               write_mask == cases[case_index].write_mask &&
               ((master & RADEON_GMC_WR_MSK_DIS) != 0u) ==
                  !cases[case_index].preserves_component &&
               extent == ((512u << 16) | 8u),
            "depth clear case %u emits the logical macrotile span and mask",
            case_index);
      CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
            "depth clear case %u records successfully", case_index);
   }

   static const struct {
      uint32_t api_value;
      uint8_t packed_value;
   } stencil_values[] = {
      {0x0000005au, 0x5au},
      {0x00000100u, 0x00u},
      {0x0000015au, 0x5au},
      {UINT32_MAX, 0xffu},
   };
   static const VkImageAspectFlags conversion_aspects[] = {
      VK_IMAGE_ASPECT_STENCIL_BIT,
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
   };
   for (uint32_t aspect_index = 0u;
        aspect_index < ARRAY_SIZE(conversion_aspects); aspect_index++) {
      for (uint32_t value_index = 0u;
           value_index < ARRAY_SIZE(stencil_values); value_index++) {
         if (begin(f))
            return 1;
         const VkClearDepthStencilValue clear = {
            .depth = 0.5f,
            .stencil = stencil_values[value_index].api_value,
         };
         const VkImageSubresourceRange range = {
            .aspectMask = conversion_aspects[aspect_index],
            .baseMipLevel = 0u,
            .levelCount = 1u,
            .baseArrayLayer = 0u,
            .layerCount = 1u,
         };
         vkCmdClearDepthStencilImage(f->cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                                     &clear, 1u, &range);
         VK_FROM_HANDLE(r3v_native_cmd_buffer, converted_cmd, f->cmd);
         uint32_t write_mask = 0u;
         CHECK(converted_cmd->zb_depth_clear_stencil ==
                  stencil_values[value_index].packed_value &&
                  converted_cmd->cell_kind ==
                     R3V_NATIVE_CELL_KIND_ZB_DEPTH_CLEAR &&
                  command_packet0_value(converted_cmd, RADEON_DP_WRITE_MSK,
                                        &write_mask) &&
                  write_mask ==
                     (conversion_aspects[aspect_index] ==
                            VK_IMAGE_ASPECT_STENCIL_BIT
                         ? 0x000000ffu
                         : UINT32_MAX),
               "stencil conversion case %u/%u records the low byte and aspect mask",
               aspect_index, value_index);
         CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
               "stencil conversion case %u/%u records successfully",
               aspect_index, value_index);
      }
   }

   if (begin(f))
      return 1;
   const VkClearDepthStencilValue split_clear = {
      .depth = 0.5f,
      .stencil = 0x15au,
   };
   const VkImageSubresourceRange split_ranges[] = {
      {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .baseMipLevel = 0u,
         .levelCount = VK_REMAINING_MIP_LEVELS,
         .baseArrayLayer = 0u,
         .layerCount = VK_REMAINING_ARRAY_LAYERS,
      },
      {
         .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
         .baseMipLevel = 0u,
         .levelCount = 1u,
         .baseArrayLayer = 0u,
         .layerCount = 1u,
      },
   };
   vkCmdClearDepthStencilImage(f->cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                               &split_clear, ARRAY_SIZE(split_ranges),
                               split_ranges);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, split_cmd, f->cmd);
   CHECK(split_cmd->zb_depth_clear_aspect_mask ==
            R300_ZB_COMBINED_CLEAR_ASPECTS &&
            split_cmd->zb_depth_clear_stencil == 0x5au,
         "separate depth and stencil ranges normalize into one combined clear");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "separate depth and stencil ranges record successfully");

   if (begin(f))
      return 1;
   VkImageSubresourceRange invalid_ranges[] = {
      split_ranges[0],
      split_ranges[1],
   };
   invalid_ranges[1].baseMipLevel = 1u;
   vkCmdClearDepthStencilImage(f->cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                               &split_clear, ARRAY_SIZE(invalid_ranges),
                               invalid_ranges);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, invalid_cmd, f->cmd);
   CHECK(invalid_cmd->cell_kind == R3V_NATIVE_CELL_KIND_UNDECLARED &&
            invalid_cmd->ib == NULL && invalid_cmd->references == NULL &&
            !invalid_cmd->zb_depth_clear_configured,
         "an invalid later range preserves the command payload state");
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "an invalid later range poisons the complete request");
   return 0;
}

static int
check_depth_image_copy_recording(const struct fixture *f,
                                 const VkImageCreateInfo *image_info,
                                 VkImage source_image,
                                 VkDeviceSize allocation_size)
{
   VkImage destination_image = VK_NULL_HANDLE;
   VkDeviceMemory destination_memory = VK_NULL_HANDLE;
   REQUIRE(vkCreateImage(f->device, image_info, NULL, &destination_image) ==
              VK_SUCCESS,
           "depth copy destination image creation");
   const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = allocation_size,
      .memoryTypeIndex = 0,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocation, NULL,
                            &destination_memory) == VK_SUCCESS,
           "depth copy destination allocation");
   REQUIRE(vkBindImageMemory(f->device, destination_image,
                             destination_memory, 0u) == VK_SUCCESS,
           "depth copy destination binding");

   static const struct {
      VkOffset3D destination;
      uint32_t segment_count;
   } cases[] = {
      {{0, 0, 0}, 1u},
      {{32, 0, 0}, 2u},
      {{0, 16, 0}, 4u},
   };
   for (uint32_t case_index = 0u; case_index < ARRAY_SIZE(cases);
        case_index++) {
      if (begin(f))
         return 1;
      const VkImageCopy region = {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .srcOffset = {0, 0, 0},
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .dstOffset = cases[case_index].destination,
         .extent = {32u, 16u, 1u},
      };
      vkCmdCopyImage(f->cmd, source_image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     destination_image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cmd, f->cmd);
      CHECK(native_cmd->deferred_copy_count == 0u &&
               native_cmd->rb2d_copy_operation_count == 1u &&
               native_cmd->rb2d_copy_operations[0].segment_count ==
                  cases[case_index].segment_count &&
               native_cmd->references[0].read_domains ==
                  RADEON_GEM_DOMAIN_GTT &&
               native_cmd->references[0].write_domain == 0u &&
               native_cmd->references[1].read_domains ==
                  RADEON_GEM_DOMAIN_GTT &&
               native_cmd->references[1].write_domain ==
                  RADEON_GEM_DOMAIN_GTT,
            "depth image copy case %u records the parity span plan",
            case_index);
      CHECK(native_cmd->rb2d_copy_operations[0]
                   .segments[0].source_offset_bytes == 6144u &&
               native_cmd->rb2d_copy_operations[0]
                      .segments[0].destination_offset_bytes >= 2048u,
            "depth image copy case %u includes both image bindings",
            case_index);
      CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
            "depth image copy case %u records successfully", case_index);
   }

   static const struct {
      VkImageAspectFlags aspect;
      uint32_t write_mask;
   } aspect_cases[] = {
      {VK_IMAGE_ASPECT_DEPTH_BIT, 0xffffff00u},
      {VK_IMAGE_ASPECT_STENCIL_BIT, 0x000000ffu},
   };
   for (uint32_t case_index = 0u; case_index < ARRAY_SIZE(aspect_cases);
        case_index++) {
      if (begin(f))
         return 1;
      const VkImageCopy region = {
         .srcSubresource = {
            .aspectMask = aspect_cases[case_index].aspect,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = aspect_cases[case_index].aspect,
            .layerCount = 1u,
         },
         .extent = {32u, 16u, 1u},
      };
      vkCmdCopyImage(f->cmd, source_image, VK_IMAGE_LAYOUT_GENERAL,
                     destination_image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                     &region);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, aspect_cmd, f->cmd);
      CHECK(aspect_cmd->rb2d_copy_operation_count == 1u &&
               aspect_cmd->rb2d_copy_operations[0].write_mask ==
                  aspect_cases[case_index].write_mask &&
               aspect_cmd->references[1].read_domains ==
                  RADEON_GEM_DOMAIN_GTT &&
               aspect_cmd->references[1].write_domain ==
                  RADEON_GEM_DOMAIN_GTT,
            "aspect image copy case %u records masked destination read-modify-write",
            case_index);
      CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
            "aspect image copy case %u records successfully", case_index);
   }

   if (begin(f))
      return 1;
   const VkImageCopy regions[] = {
      {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .srcOffset = {32, 0, 0},
         .extent = {32u, 16u, 1u},
      },
      {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .layerCount = 1u,
         },
         .dstOffset = {32, 0, 0},
         .extent = {32u, 16u, 1u},
      },
   };
   vkCmdCopyImage(f->cmd, source_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination_image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ARRAY_SIZE(regions),
                  regions);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, multi_region_cmd, f->cmd);
   CHECK(multi_region_cmd->rb2d_copy_operation_count == ARRAY_SIZE(regions) &&
            multi_region_cmd->rb2d_copy_operations[0].segment_count == 2u &&
            multi_region_cmd->rb2d_copy_operations[1].segment_count == 2u,
         "depth image copy appends independent regions to one command");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "multiple non-overlapping depth image copy regions record");

   if (begin(f))
      return 1;
   VkImageCopy invalid_later_regions[] = {regions[0], regions[1]};
   invalid_later_regions[1].srcOffset.x = 33;
   if (invalid_later_regions[1].srcOffset.x +
          invalid_later_regions[1].extent.width <= 64u)
      invalid_later_regions[1].srcOffset.x = 64;
   vkCmdCopyImage(f->cmd, source_image, VK_IMAGE_LAYOUT_GENERAL,
                  destination_image, VK_IMAGE_LAYOUT_GENERAL,
                  ARRAY_SIZE(invalid_later_regions), invalid_later_regions);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, invalid_later_cmd, f->cmd);
   CHECK(invalid_later_cmd->rb2d_copy_operation_count == 0u &&
            invalid_later_cmd->ordered_operation_count == 0u &&
            invalid_later_cmd->image_state_count == 0u &&
            invalid_later_cmd->ib == NULL &&
            invalid_later_cmd->references == NULL,
         "an invalid later image region leaves the copy transaction empty");
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "an invalid later image region poisons the complete request");

   if (begin(f))
      return 1;
   VkImageMemoryBarrier source_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = source_image,
      .subresourceRange = {
         .aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
         .levelCount = 1u,
         .layerCount = 1u,
      },
   };
   VkImageMemoryBarrier destination_barrier = source_barrier;
   destination_barrier.image = destination_image;
   destination_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                        NULL, 1u, &source_barrier);
   const VkClearDepthStencilValue composition_clear = {
      .depth = 0.25f,
      .stencil = 0x15au,
   };
   const VkImageSubresourceRange composition_range =
      source_barrier.subresourceRange;
   vkCmdClearDepthStencilImage(f->cmd, source_image, VK_IMAGE_LAYOUT_GENERAL,
                               &composition_clear, 1u, &composition_range);
   source_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
   source_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   source_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   source_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                        NULL, 1u, &source_barrier);
   vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL, 0u,
                        NULL, 1u, &destination_barrier);
   const VkImageCopy composition_copy = {
      .srcSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layerCount = 1u,
      },
      .dstSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layerCount = 1u,
      },
      .extent = {32u, 16u, 1u},
   };
   vkCmdCopyImage(f->cmd, source_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination_image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u,
                  &composition_copy);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, composition_cmd, f->cmd);
   CHECK(composition_cmd->ordered_operation_count == 5u &&
            composition_cmd->ordered_operations[0].kind ==
               R3V_NATIVE_ORDERED_OPERATION_IMAGE_BARRIER &&
            composition_cmd->ordered_operations[1].kind ==
               R3V_NATIVE_ORDERED_OPERATION_RB2D_DEPTH_CLEAR &&
            composition_cmd->ordered_operations[2].kind ==
               R3V_NATIVE_ORDERED_OPERATION_IMAGE_BARRIER &&
            composition_cmd->ordered_operations[3].kind ==
               R3V_NATIVE_ORDERED_OPERATION_IMAGE_BARRIER &&
            composition_cmd->ordered_operations[4].kind ==
               R3V_NATIVE_ORDERED_OPERATION_RB2D_COPY,
         "clear barriers and copy retain Vulkan recording order");
   CHECK(composition_cmd->cell_kind ==
            R3V_NATIVE_CELL_KIND_ORDERED_IMAGE_COMPOSITION &&
            composition_cmd->ib != NULL &&
            composition_cmd->reference_count == 2u,
         "ordered depth composition carries a bounded native geometry");
   if (composition_cmd->ordered_operation_count == 5u) {
      const uint32_t dependency_position =
         composition_cmd->ordered_operations[2].ib_position_dwords;
      CHECK(composition_cmd->ib[dependency_position] ==
               CP_PACKET0(R300_ZB_ZCACHE_CTLSTAT, 0) &&
               composition_cmd->ib[dependency_position + 2u] ==
                  CP_PACKET0(R300_RB3D_DSTCACHE_CTLSTAT, 0) &&
               composition_cmd->ib[dependency_position + 4u] ==
                  CP_PACKET0(RADEON_DSTCACHE_CTLSTAT, 0) &&
               composition_cmd->ib[dependency_position + 6u] ==
                  CP_PACKET0(RADEON_WAIT_UNTIL, 0),
            "depth barrier places ZB RB3D and RB2D publication before the copy");
      CHECK(composition_cmd->ordered_operations[4].ib_position_dwords >
               dependency_position + 7u,
            "copy stream follows the complete cross-engine dependency");
   }
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "clear barrier and copy composition records successfully");

   if (begin(f))
      return 1;
   const VkImageCopy full_image_regions[] = {
      {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .extent = {64u, 64u, 1u},
      },
      {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .layerCount = 1u,
         },
         .extent = {64u, 64u, 1u},
      },
   };
   vkCmdCopyImage(f->cmd, source_image,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination_image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  ARRAY_SIZE(full_image_regions), full_image_regions);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, full_cmd, f->cmd);
   CHECK(full_cmd->deferred_copy_count == 0u &&
            full_cmd->rb2d_copy_operation_count == 16u,
         "complete depth and stencil image copies decompose into sixteen tiles");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "complete depth image copy records successfully");

   if (begin(f))
      return 1;
   const VkImageCopy partial = {
      .srcSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layerCount = 1u,
      },
      .dstSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layerCount = 1u,
      },
      .extent = {31u, 16u, 1u},
   };
   vkCmdCopyImage(f->cmd, source_image, VK_IMAGE_LAYOUT_GENERAL,
                  destination_image, VK_IMAGE_LAYOUT_GENERAL, 1u, &partial);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, partial_cmd, f->cmd);
   CHECK(partial_cmd->deferred_copy_count == 0u &&
            partial_cmd->rb2d_copy_operation_count == 124u &&
            partial_cmd->rb2d_copy_operations[0].segment_count == 4u &&
            partial_cmd->rb2d_copy_operations[0].byte_carrier == true,
            "a partial depth image copy resolves each depth component");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "a partial depth image copy records successfully");

   if (begin(f))
      return 1;
   const VkImageCopy combined_aspect = {
      .srcSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
         .layerCount = 1u,
      },
      .dstSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
         .layerCount = 1u,
      },
      .extent = {32u, 16u, 1u},
   };
   vkCmdCopyImage(f->cmd, source_image, VK_IMAGE_LAYOUT_GENERAL,
                  destination_image, VK_IMAGE_LAYOUT_GENERAL, 1u,
                  &combined_aspect);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, combined_cmd, f->cmd);
   CHECK(combined_cmd->rb2d_copy_operation_count == 0u &&
            combined_cmd->ib == NULL && combined_cmd->references == NULL,
         "a combined depth-stencil image aspect refuses as one copy region");
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "a combined depth-stencil image aspect poisons the command");

   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "release depth image copy references before object destruction");
   vkDestroyImage(f->device, destination_image, NULL);
   vkFreeMemory(f->device, destination_memory, NULL);
   return 0;
}

static int
check_depth_storage(const struct fixture *f, bool refuse_platform)
{
   VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D24_UNORM_S8_UINT,
      .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkImage image = VK_NULL_HANDLE;
   VkResult result = vkCreateImage(f->device, &info, NULL, &image);
   if (refuse_platform) {
      CHECK(result != VK_SUCCESS && image == VK_NULL_HANDLE,
            "unresolved board refuses depth storage");
      return 0;
   }
   REQUIRE(result == VK_SUCCESS, "qualified depth storage creation");
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(f->device, image, &requirements);
   CHECK(requirements.size == 28672 && requirements.alignment == 4096,
         "depth requirements include tiled padding, guards and tail");
   VkDeviceMemory memory = VK_NULL_HANDLE;
   const VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size + 4096, .memoryTypeIndex = 0,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocation, NULL, &memory) == VK_SUCCESS,
           "depth backing allocation");
   CHECK(vkBindImageMemory(f->device, image, memory, 1) != VK_SUCCESS,
         "unaligned depth binding refuses");
   CHECK(vkBindImageMemory(f->device, image, memory, 8192) != VK_SUCCESS,
         "depth binding requires complete storage envelope");
   CHECK(vkBindImageMemory(f->device, image, memory, 4096) == VK_SUCCESS,
         "nonzero aligned depth binding");
   CHECK(vkBindImageMemory(f->device, image, memory, 4096) != VK_SUCCESS,
         "depth rebinding refuses");
   if (check_depth_clear_recording(f, image))
      return 1;
   if (check_depth_image_copy_recording(f, &info, image, requirements.size))
      return 1;
   const VkImageAspectFlags view_aspects[] = {
      VK_IMAGE_ASPECT_DEPTH_BIT,
      VK_IMAGE_ASPECT_STENCIL_BIT,
      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
   };
   for (unsigned aspect_index = 0;
        aspect_index < sizeof(view_aspects) / sizeof(view_aspects[0]);
        aspect_index++) {
      const VkImageViewCreateInfo view_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_D24_UNORM_S8_UINT,
         .subresourceRange = {view_aspects[aspect_index], 0, 1, 0, 1},
      };
      VkImageView view = VK_NULL_HANDLE;
      CHECK(vkCreateImageView(f->device, &view_info, NULL, &view) == VK_SUCCESS,
            "depth image view aspect mask %u", view_aspects[aspect_index]);
      vkDestroyImageView(f->device, view, NULL);
   }
   const VkImageViewCreateInfo color_view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_D24_UNORM_S8_UINT,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   VkImageView color_view = VK_NULL_HANDLE;
   CHECK(vkCreateImageView(f->device, &color_view_info, NULL, &color_view) !=
            VK_SUCCESS && color_view == VK_NULL_HANDLE,
         "depth image refuses a color aspect view");
   struct staging staging;
   /* The effective bufferImageHeight contributes to the admitted footprint;
    * keep enough storage for the explicit 100-row image height below. */
   if (create_staging(f, 400, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging))
      return 1;
   struct staging undersized_staging;
   if (create_staging(f, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      &undersized_staging))
      return 1;
   VkBufferImageCopy region = {
      .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
      .imageOffset = {37, 21, 0}, .imageExtent = {1, 1, 1},
      .bufferImageHeight = 100,
   };
   if (begin(f))
      return 1;
   VkImageMemoryBarrier depth_barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
         0, 1, 0, 1,
      },
   };
   vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                        1, &depth_barrier);
   vkCmdCopyBufferToImage(f->cmd, staging.buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
   depth_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   depth_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   depth_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   depth_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                        1, &depth_barrier);
   vkCmdCopyImageToBuffer(f->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         staging.buffer, 1, &region);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_depth_copy, f->cmd);
   CHECK(native_depth_copy->rb2d_copy_operation_count == 2u,
         "depth transfer records two ordered operations");
   if (native_depth_copy->rb2d_copy_operation_count == 2u) {
      const struct r3v_native_rb2d_copy_operation *to_image =
         &native_depth_copy->rb2d_copy_operations[0];
      const struct r3v_native_rb2d_copy_operation *to_buffer =
         &native_depth_copy->rb2d_copy_operations[1];
      CHECK(to_image->segment_count == 1u &&
               to_image->segments[0].source_offset_bytes == 0u &&
               to_image->segments[0].destination_offset_bytes == 14133u &&
               to_image->segments[0].byte_count == 3u,
            "depth upload binds layout-local offset to image memory placement");
      CHECK(to_buffer->segment_count == 1u &&
               to_buffer->segments[0].source_offset_bytes == 14133u &&
               to_buffer->segments[0].destination_offset_bytes == 0u &&
               to_buffer->segments[0].byte_count == 3u,
            "depth download preserves the bound image and buffer coordinates");
   }
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "depth copies retain independent image/buffer origins in both directions");
   VkBufferImageCopy undersized_region = region;
   undersized_region.bufferImageHeight = 100;
   if (begin(f))
      return 1;
   vkCmdCopyBufferToImage(f->cmd, undersized_staging.buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                         &undersized_region);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "bufferImageHeight footprint refuses an undersized buffer");
   if (begin(f))
      return 1;
   depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
   vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                        1, &depth_barrier);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "combined D24S8 layout transition refuses a single-aspect range");
   for (unsigned invalid = 0; invalid < 4; invalid++) {
      VkBufferImageCopy bad = region;
      VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
      if (invalid == 0)
         bad.bufferOffset = 1;
      if (invalid == 1)
         bad.imageExtent.width = 2;
      if (invalid == 2)
         bad.imageSubresource.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
      if (invalid == 3)
         layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      if (begin(f))
         return 1;
      vkCmdCopyImageToBuffer(f->cmd, image, layout, staging.buffer, 1, &bad);
      CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
            "invalid depth copy case %u refuses", invalid);
   }
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
   if (begin(f))
      return 1;
   vkCmdCopyBufferToImage(f->cmd, staging.buffer, image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &region);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_stencil_copy, f->cmd);
   CHECK(native_stencil_copy->rb2d_copy_operation_count == 1u &&
            native_stencil_copy->rb2d_copy_operations[0].segment_count == 1u &&
            native_stencil_copy->rb2d_copy_operations[0]
                  .segments[0].source_offset_bytes == 0u &&
            native_stencil_copy->rb2d_copy_operations[0]
                  .segments[0].destination_offset_bytes == 14132u &&
            native_stencil_copy->rb2d_copy_operations[0]
                  .segments[0].byte_count == 1u,
         "stencil upload addresses the packed low byte independently");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "single stencil byte records independently");
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "release depth copy references before object destruction");
   destroy_staging(f, &staging);
   destroy_staging(f, &undersized_staging);
   vkDestroyImage(f->device, image, NULL);
   vkFreeMemory(f->device, memory, NULL);

   VkImage shared_source = VK_NULL_HANDLE;
   VkImage shared_destination = VK_NULL_HANDLE;
   REQUIRE(vkCreateImage(f->device, &info, NULL, &shared_source) == VK_SUCCESS,
           "shared-allocation source image creation");
   REQUIRE(vkCreateImage(f->device, &info, NULL, &shared_destination) ==
              VK_SUCCESS,
           "shared-allocation destination image creation");
   const VkMemoryAllocateInfo shared_allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = 65536u,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory shared_memory = VK_NULL_HANDLE;
   REQUIRE(vkAllocateMemory(f->device, &shared_allocation, NULL,
                            &shared_memory) == VK_SUCCESS,
           "shared-allocation memory allocation");
   REQUIRE(vkBindImageMemory(f->device, shared_source, shared_memory, 4096u) ==
              VK_SUCCESS,
           "shared-allocation source binding");
   REQUIRE(vkBindImageMemory(f->device, shared_destination, shared_memory,
                             32768u) == VK_SUCCESS,
           "shared-allocation destination binding");
   const VkImageCopy shared_full_regions[] = {
      {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .layerCount = 1u,
         },
         .extent = {64u, 64u, 1u},
      },
      {
         .srcSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .layerCount = 1u,
         },
         .dstSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
            .layerCount = 1u,
         },
         .extent = {64u, 64u, 1u},
      },
   };
   if (begin(f))
      return 1;
   vkCmdCopyImage(f->cmd, shared_source, VK_IMAGE_LAYOUT_GENERAL,
                  shared_destination, VK_IMAGE_LAYOUT_GENERAL,
                  ARRAY_SIZE(shared_full_regions), shared_full_regions);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, shared_cmd, f->cmd);
   CHECK(shared_cmd->rb2d_copy_operation_count == 16u &&
            shared_cmd->reference_count == 1u,
         "disjoint image suballocations share one BO reference safely");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "disjoint image suballocations record successfully");
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "release shared-allocation image references");

   const VkBufferCreateInfo shared_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 4u,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer shared_buffer = VK_NULL_HANDLE;
   REQUIRE(vkCreateBuffer(f->device, &shared_buffer_info, NULL,
                          &shared_buffer) == VK_SUCCESS,
           "shared-allocation buffer creation");
   REQUIRE(vkBindBufferMemory(f->device, shared_buffer, shared_memory,
                              61440u) == VK_SUCCESS,
           "shared-allocation buffer binding");
   const VkBufferImageCopy shared_buffer_region = {
      .bufferOffset = 0u,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
         .layerCount = 1u,
      },
      .imageOffset = {37, 21, 0},
      .imageExtent = {1u, 1u, 1u},
   };
   if (begin(f))
      return 1;
   vkCmdCopyBufferToImage(f->cmd, shared_buffer, shared_source,
                          VK_IMAGE_LAYOUT_GENERAL, 1u,
                          &shared_buffer_region);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, shared_buffer_cmd, f->cmd);
   CHECK(shared_buffer_cmd->rb2d_copy_operation_count == 1u &&
            shared_buffer_cmd->reference_count == 1u,
         "disjoint image and buffer suballocations share one BO safely");
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "disjoint image and buffer suballocations record successfully");
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "release shared-allocation buffer references");
   vkDestroyBuffer(f->device, shared_buffer, NULL);

   const VkBufferCreateInfo overlapping_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 16384u,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer overlapping_buffer = VK_NULL_HANDLE;
   REQUIRE(vkCreateBuffer(f->device, &overlapping_buffer_info, NULL,
                          &overlapping_buffer) == VK_SUCCESS,
           "overlapping-allocation buffer creation");
   REQUIRE(vkBindBufferMemory(f->device, overlapping_buffer, shared_memory,
                              4096u) == VK_SUCCESS,
           "overlapping-allocation buffer binding");
   const VkBufferImageCopy overlapping_region = {
      .bufferOffset = 10036u,
      .imageSubresource = {
         .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
         .layerCount = 1u,
      },
      .imageOffset = {37, 21, 0},
      .imageExtent = {1u, 1u, 1u},
   };
   if (begin(f))
      return 1;
   vkCmdCopyBufferToImage(f->cmd, overlapping_buffer, shared_source,
                          VK_IMAGE_LAYOUT_GENERAL, 1u,
                          &overlapping_region);
   VK_FROM_HANDLE(r3v_native_cmd_buffer, overlapping_cmd, f->cmd);
   CHECK(overlapping_cmd->rb2d_copy_operation_count == 0u &&
            overlapping_cmd->ib == NULL && overlapping_cmd->references == NULL,
         "overlapping image and buffer ranges refuse before payload mutation");
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "overlapping image and buffer ranges refuse");
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "release overlapping-allocation buffer references");
   vkDestroyBuffer(f->device, overlapping_buffer, NULL);

   vkDestroyImage(f->device, shared_destination, NULL);
   vkDestroyImage(f->device, shared_source, NULL);
   vkFreeMemory(f->device, shared_memory, NULL);

   info.extent.height = 65;
   image = VK_NULL_HANDLE;
   CHECK(vkCreateImage(f->device, &info, NULL, &image) != VK_SUCCESS &&
         image == VK_NULL_HANDLE, "storage padding remains nonrenderable");
   return 0;
}

int
main(int argc, char **argv)
{
   bool depth_storage = false, refuse_platform = false;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--depth-storage") == 0) {
         depth_storage = true;
      } else if (strcmp(argv[i], "--depth-storage-refuse-platform") == 0) {
         depth_storage = true;
         refuse_platform = true;
      } else if (strcmp(argv[i], "--inject-scaled-linear-blit-admits") == 0) {
         mutation = MUTATION_SCALED_LINEAR_BLIT_ADMITS;
      } else if (strcmp(argv[i], "--inject-overlap-copy-admits") == 0) {
         mutation = MUTATION_OVERLAP_COPY_ADMITS;
      } else {
         fprintf(stderr, "unknown argument: %s\n", argv[i]);
         return 1;
      }
   }

   struct fixture f = { 0 };
   if (create_fixture(&f))
      return 1;
   int fatal = depth_storage ? check_depth_storage(&f, refuse_platform) :
               check_fill_and_update(&f) || check_copy_overlap(&f) ||
               check_buffer_image_height_stride(&f) ||
               check_blit(&f) || check_texel_formats(&f) ||
               check_optimal_tiling(&f);
   vkDestroyCommandPool(f.device, f.cmd_pool, NULL);
   vkDestroyDevice(f.device, NULL);
   vkDestroyInstance(f.instance, NULL);
   if (fatal || failures) {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("native transfer-op contract holds\n");
   return 0;
}
