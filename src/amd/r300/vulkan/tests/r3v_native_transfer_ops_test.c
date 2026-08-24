/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V transfer-op fixture: dword-pattern fills, inline updates,
 * and unit-scale blits through host mappings at submission, with the
 * overlap, scale, flip, alignment, and unsupported-layout refusals
 * that keep the recorded surface fail-closed under the drm-shim
 * transport.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

static unsigned failures;

enum mutation_mode {
   MUTATION_NONE,
   /* A scaling blit is reported as admitted. */
   MUTATION_SCALED_BLIT_ADMITS,
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

static void
destroy_transfer_image(const struct fixture *f, struct transfer_image *img)
{
   vkUnmapMemory(f->device, img->memory);
   vkDestroyImage(f->device, img->image, NULL);
   vkFreeMemory(f->device, img->memory, NULL);
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
   const VkResult scaled_end = vkEndCommandBuffer(f->cmd);
   if (mutation == MUTATION_SCALED_BLIT_ADMITS)
      CHECK(scaled_end == VK_SUCCESS,
            "mutation: scaling blit reported admitted");
   else
      CHECK(scaled_end != VK_SUCCESS,
            "a scaling blit poisons the recording: the host row mover "
            "has no filter");

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

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--inject-scaled-blit-admits") == 0) {
         mutation = MUTATION_SCALED_BLIT_ADMITS;
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
   int fatal = check_fill_and_update(&f) || check_copy_overlap(&f) ||
               check_blit(&f) || check_texel_formats(&f);
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
