/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V object-lifetime fixture: sampler creation as recorded
 * state with its use refused at the descriptor write, buffer-view
 * admission and refusal under the advertised texel-buffer format
 * table, and the once-only host-visible memory-binding admission for
 * buffers and images under the drm-shim transport.
 */

#include "r3v_native_reference_spirv.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

static unsigned failures;

enum mutation_mode {
   MUTATION_NONE,
   /* A second bind of an already-bound buffer is reported as admitted. */
   MUTATION_REBIND_ADMITS,
   /* A bind to the device-local CPU-inaccessible type is reported as
    * admitted. */
   MUTATION_WRONG_TYPE_BIND_ADMITS,
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
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkCommandPool cmd_pool;
   VkCommandBuffer cmd;
};

static VkSamplerCreateInfo
basic_sampler_info(void)
{
   return (VkSamplerCreateInfo){
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.25f,
      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
   };
}

static int
check_sampler_lifetime(const struct fixture *f)
{
   VkSampler sampler = VK_NULL_HANDLE;
   const VkSamplerCreateInfo info = basic_sampler_info();
   CHECK(vkCreateSampler(f->device, &info, NULL, &sampler) == VK_SUCCESS &&
            sampler != VK_NULL_HANDLE,
         "a basic sampler creates as recorded state");
   vkDestroySampler(f->device, sampler, NULL);
   vkDestroySampler(f->device, VK_NULL_HANDLE, NULL);

   VkSamplerCreateInfo anisotropic = basic_sampler_info();
   anisotropic.anisotropyEnable = VK_TRUE;
   anisotropic.maxAnisotropy = 2.0f;
   sampler = (VkSampler)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateSampler(f->device, &anisotropic, NULL, &sampler) !=
            VK_SUCCESS &&
         sampler == VK_NULL_HANDLE,
         "enabled anisotropy refuses with a cleared handle: the feature "
         "is withheld");
   return 0;
}

/* VK_FORMAT_R32_SFLOAT carries VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT alone
 * (r3v_get_format_properties), so a view over it refuses whatever the
 * buffer's own usage bits are.  VK_FORMAT_R32_UINT sits in the texel
 * table r3v_native_transfer_texel_bytes names and carries
 * VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT alone -- the RS480 die
 * withholds the storage-texel-buffer bit on every format
 * (tests/r3v_conformance_nonpass_ledger.tsv row
 * mandatory_format_feature_absent) -- so a uniform-usage view over it
 * constructs and a storage-usage view over it refuses.
 */
static int
check_buffer_view_lifetime(const struct fixture *f)
{
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 256,
      .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   REQUIRE(vkCreateBuffer(f->device, &buffer_info, NULL, &buffer) ==
              VK_SUCCESS,
           "texel-usage buffer object creation");

   /* VUID-VkBufferViewCreateInfo-buffer-00935: buffer must have a
    * non-sparse, complete memory binding before a view over it
    * constructs, so this fixture binds real host-visible memory (type
    * 0, r3v_native_memory_properties_fill) before any vkCreateBufferView
    * call below.
    */
   VkMemoryRequirements mem_reqs;
   vkGetBufferMemoryRequirements(f->device, buffer, &mem_reqs);
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_reqs.size,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   REQUIRE(vkAllocateMemory(f->device, &memory_info, NULL, &memory) ==
              VK_SUCCESS,
           "buffer-view backing memory allocation");
   REQUIRE(vkBindBufferMemory(f->device, buffer, memory, 0) == VK_SUCCESS,
           "buffer-view backing memory binding");

   const VkBufferViewCreateInfo unadmitted_format = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_SFLOAT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &unadmitted_format, NULL, &view) !=
            VK_SUCCESS &&
         view == VK_NULL_HANDLE,
         "buffer-view creation over an unadmitted format refuses with a "
         "cleared handle: R32_SFLOAT advertises no texel-buffer feature");
   vkDestroyBufferView(f->device, VK_NULL_HANDLE, NULL);

   const VkBufferViewCreateInfo whole_size = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView whole_size_view = VK_NULL_HANDLE;
   CHECK(vkCreateBufferView(f->device, &whole_size, NULL,
                            &whole_size_view) == VK_SUCCESS &&
            whole_size_view != VK_NULL_HANDLE,
         "buffer-view creation over the admitted texel table constructs: "
         "R32_UINT VK_WHOLE_SIZE from offset 0");
   vkDestroyBufferView(f->device, whole_size_view, NULL);

   /* 16 is minTexelBufferOffsetAlignment (r3v_physical_device_init_limits);
    * a loader-level fixture has no internal header to name the driver's
    * own macro, so the aligned and misaligned offsets below are the
    * literal value and one that is not a multiple of it.
    */
   const VkBufferViewCreateInfo explicit_range = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 16,
      .range = 64,
   };
   VkBufferView explicit_range_view = VK_NULL_HANDLE;
   CHECK(vkCreateBufferView(f->device, &explicit_range, NULL,
                            &explicit_range_view) == VK_SUCCESS &&
            explicit_range_view != VK_NULL_HANDLE,
         "buffer-view creation admits an aligned offset with an "
         "explicit range a multiple of the texel size");
   vkDestroyBufferView(f->device, explicit_range_view, NULL);

   /* Calibration: an offset that is not a multiple of
    * minTexelBufferOffsetAlignment (16) is the known-bad input; a driver
    * that admitted it here would pass a view an application cannot
    * legally present, and this assertion is what catches that defect.
    */
   const VkBufferViewCreateInfo misaligned_offset = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 4,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView misaligned_view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &misaligned_offset, NULL,
                            &misaligned_view) != VK_SUCCESS &&
            misaligned_view == VK_NULL_HANDLE,
         "known-bad: a misaligned offset (4, not a multiple of 16) "
         "refuses with a cleared handle rather than admitting");

   /* VUID-VkBufferViewCreateInfo-offset-00925: offset must be less than
    * the buffer's size; offset == size leaves zero remaining bytes and
    * refuses rather than resolving VK_WHOLE_SIZE to an empty view.
    */
   const VkBufferViewCreateInfo offset_at_size = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = buffer_info.size,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView offset_at_size_view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &offset_at_size, NULL,
                            &offset_at_size_view) != VK_SUCCESS &&
            offset_at_size_view == VK_NULL_HANDLE,
         "an offset equal to the buffer size refuses: offset must be "
         "strictly less than the buffer size");

   /* A buffer with neither texel-buffer usage bit names no route a
    * view can admit, whatever format or range it presents.
    */
   const VkBufferCreateInfo vertex_only_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 256,
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer vertex_only_buffer;
   REQUIRE(vkCreateBuffer(f->device, &vertex_only_info, NULL,
                          &vertex_only_buffer) == VK_SUCCESS,
           "vertex-only buffer object creation");
   const VkBufferViewCreateInfo neither_usage = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = vertex_only_buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView neither_usage_view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &neither_usage, NULL,
                            &neither_usage_view) != VK_SUCCESS &&
            neither_usage_view == VK_NULL_HANDLE,
         "a buffer with neither texel-buffer usage bit refuses every "
         "view");
   vkDestroyBuffer(f->device, vertex_only_buffer, NULL);

   /* A storage-usage buffer over an admitted format still refuses: the
    * queried format grants VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT
    * alone, so the storage-usage route names a bit no format carries.
    */
   const VkBufferCreateInfo storage_only_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 256,
      .usage = VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer storage_only_buffer;
   REQUIRE(vkCreateBuffer(f->device, &storage_only_info, NULL,
                          &storage_only_buffer) == VK_SUCCESS,
           "storage-usage buffer object creation");
   const VkBufferViewCreateInfo storage_over_uniform_format = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = storage_only_buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView storage_view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &storage_over_uniform_format, NULL,
                            &storage_view) != VK_SUCCESS &&
            storage_view == VK_NULL_HANDLE,
         "a storage-usage buffer over a uniform-only format refuses: "
         "no format grants VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT");
   vkDestroyBuffer(f->device, storage_only_buffer, NULL);

   /* VUID-VkBufferViewCreateInfo-range-00928-adjacent: an explicit range
    * not a multiple of the format's texel size (63, R32_UINT's 4-byte
    * texel) refuses.
    */
   const VkBufferViewCreateInfo range_not_texel_multiple = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 0,
      .range = 63,
   };
   VkBufferView misaligned_range_view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &range_not_texel_multiple, NULL,
                            &misaligned_range_view) != VK_SUCCESS &&
            misaligned_range_view == VK_NULL_HANDLE,
         "an explicit range that is not a multiple of the texel size "
         "refuses");

   /* An explicit range past the buffer's remainder from offset refuses:
    * buffer_info.size is 256, so 260 exceeds it.
    */
   const VkBufferViewCreateInfo range_past_remainder = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 0,
      .range = 260,
   };
   VkBufferView oversize_range_view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &range_past_remainder, NULL,
                            &oversize_range_view) != VK_SUCCESS &&
            oversize_range_view == VK_NULL_HANDLE,
         "an explicit range past the buffer's remainder from offset "
         "refuses");

   vkDestroyBuffer(f->device, buffer, NULL);
   vkFreeMemory(f->device, memory, NULL);
   return 0;
}

/* VK_WHOLE_SIZE rounding, finding 1: a buffer whose size is not a
 * multiple of the requested format's texel size resolves VK_WHOLE_SIZE
 * down to the nearest texel multiple rather than refusing, per
 * VkBufferViewCreateInfo prose (no VU governs this rounding).
 */
static int
check_buffer_view_whole_size_rounds_down(const struct fixture *f)
{
   const VkBufferCreateInfo odd_size_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 257,
      .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   REQUIRE(vkCreateBuffer(f->device, &odd_size_info, NULL, &buffer) ==
              VK_SUCCESS,
           "odd-size texel-usage buffer object creation");

   VkMemoryRequirements mem_reqs;
   vkGetBufferMemoryRequirements(f->device, buffer, &mem_reqs);
   const VkMemoryAllocateInfo memory_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = mem_reqs.size,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   REQUIRE(vkAllocateMemory(f->device, &memory_info, NULL, &memory) ==
              VK_SUCCESS,
           "odd-size buffer-view backing memory allocation");
   REQUIRE(vkBindBufferMemory(f->device, buffer, memory, 0) == VK_SUCCESS,
           "odd-size buffer-view backing memory binding");

   const VkBufferViewCreateInfo whole_size = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_UINT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView view = VK_NULL_HANDLE;
   CHECK(vkCreateBufferView(f->device, &whole_size, NULL, &view) ==
            VK_SUCCESS && view != VK_NULL_HANDLE,
         "VK_WHOLE_SIZE over a 257-byte buffer with a 4-byte texel "
         "rounds the remainder down to 256 bytes and admits, rather "
         "than refusing the non-multiple remainder");
   vkDestroyBufferView(f->device, view, NULL);
   vkDestroyBuffer(f->device, buffer, NULL);
   vkFreeMemory(f->device, memory, NULL);
   return 0;
}

static int
allocate_memory(const struct fixture *f, VkDeviceSize bytes,
                uint32_t type_index, VkDeviceMemory *out)
{
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = bytes,
      .memoryTypeIndex = type_index,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocate_info, NULL, out) ==
              VK_SUCCESS,
           "memory allocation of type %u", type_index);
   return 0;
}

static int
check_buffer_binding(const struct fixture *f)
{
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 4096,
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   REQUIRE(vkCreateBuffer(f->device, &buffer_info, NULL, &buffer) ==
              VK_SUCCESS,
           "buffer object creation");
   VkDeviceMemory memory;
   if (allocate_memory(f, 2 * 4096, 0, &memory))
      return 1;

   CHECK(vkBindBufferMemory(f->device, buffer, memory, 4) != VK_SUCCESS,
         "a page-misaligned bind offset refuses");
   CHECK(vkBindBufferMemory(f->device, buffer, memory, 4096) == VK_SUCCESS,
         "an aligned bind whose footprint closes inside the allocation "
         "admits");

   /* VK_KHR_bind_memory2 resolves through the vk_common bridge onto the
    * same once-per-buffer binding, so the *2 form admits an aligned
    * bind and refuses a rebind exactly as the core form does.
    */
   PFN_vkBindBufferMemory2KHR bind_buffer2 =
      (PFN_vkBindBufferMemory2KHR)vkGetDeviceProcAddr(
         f->device, "vkBindBufferMemory2KHR");
   CHECK(bind_buffer2 != NULL, "vkBindBufferMemory2KHR resolves");
   if (bind_buffer2 != NULL) {
      VkBuffer buffer2;
      REQUIRE(vkCreateBuffer(f->device, &buffer_info, NULL, &buffer2) ==
                 VK_SUCCESS,
              "second buffer object creation");
      VkBindBufferMemoryInfoKHR bind2 = {
         .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO_KHR,
         .buffer = buffer2,
         .memory = memory,
         .memoryOffset = 0,
      };
      CHECK(bind_buffer2(f->device, 1, &bind2) == VK_SUCCESS,
            "vkBindBufferMemory2KHR admits an aligned bind");
      CHECK(bind_buffer2(f->device, 1, &bind2) != VK_SUCCESS,
            "vkBindBufferMemory2KHR refuses a rebind");
      vkDestroyBuffer(f->device, buffer2, NULL);
   }

   const VkResult rebind =
      vkBindBufferMemory(f->device, buffer, memory, 0);
   if (mutation == MUTATION_REBIND_ADMITS)
      CHECK(rebind == VK_SUCCESS, "mutation: rebind reported admitted");
   else
      CHECK(rebind != VK_SUCCESS,
            "a second bind of a bound buffer refuses: binding happens "
            "exactly once");

   const VkBufferCreateInfo oversize_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 8192,
      .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer oversize;
   REQUIRE(vkCreateBuffer(f->device, &oversize_info, NULL, &oversize) ==
              VK_SUCCESS,
           "second buffer object creation");
   CHECK(vkBindBufferMemory(f->device, oversize, memory, 4096) !=
            VK_SUCCESS,
         "a footprint past the allocation end refuses");

   VkDeviceMemory device_local;
   if (allocate_memory(f, 4096, 1, &device_local))
      return 1;
   const VkResult wrong_type =
      vkBindBufferMemory(f->device, oversize, device_local, 0);
   if (mutation == MUTATION_WRONG_TYPE_BIND_ADMITS)
      CHECK(wrong_type == VK_SUCCESS,
            "mutation: wrong-type bind reported admitted");
   else
      CHECK(wrong_type != VK_SUCCESS,
            "a bind to the CPU-inaccessible type refuses: the gather "
            "reads bound buffers through a host mapping");

   vkDestroyBuffer(f->device, oversize, NULL);
   vkDestroyBuffer(f->device, buffer, NULL);
   vkFreeMemory(f->device, device_local, NULL);
   vkFreeMemory(f->device, memory, NULL);
   return 0;
}

static int
check_image_binding(const struct fixture *f)
{
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 64, 64, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkImage image;
   REQUIRE(vkCreateImage(f->device, &image_info, NULL, &image) ==
              VK_SUCCESS,
           "render-family image creation");
   VkMemoryRequirements requirements;
   vkGetImageMemoryRequirements(f->device, image, &requirements);
   VkDeviceMemory memory;
   if (allocate_memory(f, requirements.size, 0, &memory))
      return 1;

   CHECK(vkBindImageMemory(f->device, image, memory, 0) == VK_SUCCESS,
         "the render-family image binds at offset zero");
   CHECK(vkBindImageMemory(f->device, image, memory, 0) != VK_SUCCESS,
         "a second bind of a bound image refuses: binding happens "
         "exactly once");
   PFN_vkBindImageMemory2KHR bind_image2 =
      (PFN_vkBindImageMemory2KHR)vkGetDeviceProcAddr(
         f->device, "vkBindImageMemory2KHR");
   CHECK(bind_image2 != NULL, "vkBindImageMemory2KHR resolves");
   if (bind_image2 != NULL) {
      VkImage image2;
      REQUIRE(vkCreateImage(f->device, &image_info, NULL, &image2) ==
                 VK_SUCCESS,
              "second render-family image creation");
      VkDeviceMemory memory2;
      if (allocate_memory(f, requirements.size, 0, &memory2))
         return 1;
      VkBindImageMemoryInfoKHR bind2 = {
         .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO_KHR,
         .image = image2,
         .memory = memory2,
         .memoryOffset = 0,
      };
      CHECK(bind_image2(f->device, 1, &bind2) == VK_SUCCESS,
            "vkBindImageMemory2KHR binds the render-family image at "
            "offset zero");
      CHECK(bind_image2(f->device, 1, &bind2) != VK_SUCCESS,
            "vkBindImageMemory2KHR refuses a rebind");
      vkDestroyImage(f->device, image2, NULL);
      vkFreeMemory(f->device, memory2, NULL);
   }

   vkDestroyImage(f->device, image, NULL);
   vkFreeMemory(f->device, memory, NULL);
   return 0;
}

/* Records one dispatch over the set and returns vkEndCommandBuffer's
 * result: VK_SUCCESS for an admitted recording, the refusal result for
 * a poisoned one.
 */
static VkResult
record_dispatch(const struct fixture *f, VkDescriptorSet set)
{
   if (vkResetCommandPool(f->device, f->cmd_pool, 0) != VK_SUCCESS)
      return VK_ERROR_UNKNOWN;
   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   if (vkBeginCommandBuffer(f->cmd, &begin_info) != VK_SUCCESS)
      return VK_ERROR_UNKNOWN;
   vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, f->pipeline);
   vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                           f->pipeline_layout, 0, 1, &set, 0, NULL);
   vkCmdDispatch(f->cmd, 1, 1, 1);
   return vkEndCommandBuffer(f->cmd);
}

/* A descriptor write naming a sampler poisons its set, and the poisoned
 * set refuses the dispatch recording: the sampler's only route to
 * execution is fail-closed at the point of use.
 */
static int
check_sampler_use_fail_closed(const struct fixture *f)
{
   VkSampler sampler;
   const VkSamplerCreateInfo sampler_info = basic_sampler_info();
   REQUIRE(vkCreateSampler(f->device, &sampler_info, NULL, &sampler) ==
              VK_SUCCESS,
           "sampler for the descriptor-use leg");

   const VkDescriptorPoolSize pool_size = {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 2,
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &pool_size,
   };
   VkDescriptorPool pool;
   REQUIRE(vkCreateDescriptorPool(f->device, &pool_info, NULL, &pool) ==
              VK_SUCCESS,
           "descriptor pool");
   const VkDescriptorSetAllocateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &f->set_layout,
   };
   VkDescriptorSet set;
   REQUIRE(vkAllocateDescriptorSets(f->device, &set_info, &set) ==
              VK_SUCCESS,
           "descriptor set allocation");

   const VkDescriptorImageInfo image_info = { .sampler = sampler };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .pImageInfo = &image_info,
   };
   vkUpdateDescriptorSets(f->device, 1, &write, 0, NULL);
   CHECK(record_dispatch(f, set) != VK_SUCCESS,
         "the sampler write poisoned the set and the recording refuses");

   vkDestroyDescriptorPool(f->device, pool, NULL);
   vkDestroySampler(f->device, sampler, NULL);
   return 0;
}

/* The occlusion query lifecycle over the zero-fragment span: results
 * read NOT_READY before the span submits, the submitted end publishes
 * the exact zero with availability, the recorded reset returns the
 * query to unavailable, and the fail-closed edges poison -- an end
 * without a begin, an open query at vkEndCommandBuffer, and a pass
 * begun inside an active span.
 */
static int
check_query_zero_span(const struct fixture *f)
{
   const VkQueryPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
      .queryType = VK_QUERY_TYPE_OCCLUSION,
      .queryCount = 4,
   };
   VkQueryPool pool;
   REQUIRE(vkCreateQueryPool(f->device, &pool_info, NULL, &pool) ==
              VK_SUCCESS,
           "occlusion pool creation");

   uint64_t words[2] = { 0xdeadbeefdeadbeefull, 0xdeadbeefdeadbeefull };
   CHECK(vkGetQueryPoolResults(f->device, pool, 1, 1, sizeof(words), words,
                               sizeof(words), VK_QUERY_RESULT_64_BIT) ==
            VK_NOT_READY,
         "an unsubmitted query reads NOT_READY");

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "query pool span reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "query span begin");
   vkCmdBeginQuery(f->cmd, pool, 1, 0);
   vkCmdEndQuery(f->cmd, pool, 1);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "the zero-fragment span records");
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &f->cmd,
   };
   REQUIRE(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
              VK_SUCCESS,
           "query span submit");
   REQUIRE(vkQueueWaitIdle(f->queue) == VK_SUCCESS, "query span idle");

   CHECK(vkGetQueryPoolResults(f->device, pool, 1, 1, sizeof(words), words,
                               sizeof(words),
                               VK_QUERY_RESULT_64_BIT |
                                  VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
            VK_SUCCESS &&
         words[0] == 0 && words[1] == 1,
         "the submitted end publishes the exact zero, available");

   /* The recorded reset returns the query to unavailable. */
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "query reset span reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "query reset begin");
   vkCmdResetQueryPool(f->cmd, pool, 0, 4);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "the reset records");
   REQUIRE(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
              VK_SUCCESS,
           "query reset submit");
   REQUIRE(vkQueueWaitIdle(f->queue) == VK_SUCCESS, "query reset idle");
   CHECK(vkGetQueryPoolResults(f->device, pool, 1, 1, sizeof(words), words,
                               sizeof(words), VK_QUERY_RESULT_64_BIT) ==
            VK_NOT_READY,
         "the submitted reset returns the query to unavailable");

   /* Fail-closed edges. */
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "query edge reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "query edge begin");
   vkCmdEndQuery(f->cmd, pool, 0);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "an end without a begin poisons the recording");

   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "open query reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "open query begin");
   vkCmdBeginQuery(f->cmd, pool, 0, 0);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "a query left active poisons vkEndCommandBuffer");

   vkDestroyQueryPool(f->device, pool, NULL);
   return 0;
}

/* vkCmdExecuteCommands replays a secondary's recorded ops into the
 * primary: the empty sequence as a no-op, a recorded transfer op
 * executing at the primary's submission; dynamic state and a
 * primary-level buffer in the list each poison.
 */
static int
check_empty_secondary_execution(const struct fixture *f)
{
   const VkCommandBufferAllocateInfo secondary_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = f->cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer secondary;
   REQUIRE(vkAllocateCommandBuffers(f->device, &secondary_info,
                                    &secondary) == VK_SUCCESS,
           "secondary allocation");
   const VkCommandBufferInheritanceInfo inheritance = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
   };
   const VkCommandBufferBeginInfo secondary_begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .pInheritanceInfo = &inheritance,
   };
   REQUIRE(vkBeginCommandBuffer(secondary, &secondary_begin) ==
              VK_SUCCESS,
           "secondary begin");
   REQUIRE(vkEndCommandBuffer(secondary) == VK_SUCCESS,
           "empty secondary end");

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   /* A pool reset would return the live secondary too, so the primary
    * begins directly (an implicit reset of the primary alone). */
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "primary begin for execute");
   vkCmdExecuteCommands(f->cmd, 1, &secondary);
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "executing the empty secondary records");
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &f->cmd,
   };
   CHECK(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
            VK_SUCCESS &&
         vkQueueWaitIdle(f->queue) == VK_SUCCESS,
         "the primary with the no-op execute submits");

   /* A fill recorded into the secondary executes at the primary's
    * submission. */
   const VkBufferCreateInfo fill_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 64,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer fill_buffer;
   REQUIRE(vkCreateBuffer(f->device, &fill_buffer_info, NULL,
                          &fill_buffer) == VK_SUCCESS,
           "replay fill buffer creation");
   VkDeviceMemory fill_memory;
   if (allocate_memory(f, 4096, 0, &fill_memory))
      return 1;
   REQUIRE(vkBindBufferMemory(f->device, fill_buffer, fill_memory, 0) ==
              VK_SUCCESS,
           "replay fill buffer bind");
   REQUIRE(vkBeginCommandBuffer(secondary, &secondary_begin) ==
              VK_SUCCESS,
           "fill secondary begin");
   vkCmdFillBuffer(secondary, fill_buffer, 0, 64, 0xa1b2c3d4u);
   REQUIRE(vkEndCommandBuffer(secondary) == VK_SUCCESS,
           "fill secondary end");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "primary begin for the fill replay");
   vkCmdExecuteCommands(f->cmd, 1, &secondary);
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "executing the fill-bearing secondary records");
   CHECK(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
            VK_SUCCESS &&
         vkQueueWaitIdle(f->queue) == VK_SUCCESS,
         "the primary with the replayed fill submits");
   void *fill_map = NULL;
   REQUIRE(vkMapMemory(f->device, fill_memory, 0, 64, 0, &fill_map) ==
              VK_SUCCESS,
           "replay fill mapping");
   {
      const uint32_t *words = fill_map;
      bool filled = true;
      for (uint32_t w = 0; w < 16; w++)
         filled = filled && words[w] == 0xa1b2c3d4u;
      CHECK(filled, "the replayed fill wrote every dword");
   }
   vkUnmapMemory(f->device, fill_memory);

   /* An update recorded into the secondary replays with its own copy
    * of the data, so freeing the secondary before the primary leaves
    * one owner per recording. */
   const uint32_t update_words[4] = { 1, 2, 3, 4 };
   VkCommandBuffer update_secondary;
   REQUIRE(vkAllocateCommandBuffers(f->device, &secondary_info,
                                    &update_secondary) == VK_SUCCESS,
           "update secondary allocation");
   REQUIRE(vkBeginCommandBuffer(update_secondary, &secondary_begin) ==
              VK_SUCCESS,
           "update secondary begin");
   vkCmdUpdateBuffer(update_secondary, fill_buffer, 0,
                     sizeof(update_words), update_words);
   REQUIRE(vkEndCommandBuffer(update_secondary) == VK_SUCCESS,
           "update secondary end");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "primary begin for the update replay");
   vkCmdExecuteCommands(f->cmd, 1, &update_secondary);
   CHECK(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
         "executing the update-bearing secondary records");
   vkFreeCommandBuffers(f->device, f->cmd_pool, 1, &update_secondary);
   CHECK(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
            VK_SUCCESS &&
         vkQueueWaitIdle(f->queue) == VK_SUCCESS,
         "the primary submits after the secondary is freed");
   REQUIRE(vkMapMemory(f->device, fill_memory, 0, 64, 0, &fill_map) ==
              VK_SUCCESS,
           "replay update mapping");
   CHECK(memcmp(fill_map, update_words, sizeof(update_words)) == 0,
         "the replayed update wrote its bytes");
   vkUnmapMemory(f->device, fill_memory);
   vkDestroyBuffer(f->device, fill_buffer, NULL);
   vkFreeMemory(f->device, fill_memory, NULL);

   /* A primary-level buffer in the list poisons. */
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "primary begin for the level refusal");
   vkCmdExecuteCommands(f->cmd, 1, &f->cmd);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "executing a primary-level buffer poisons");

   /* A secondary carrying recorded dynamic state poisons the execute. */
   REQUIRE(vkBeginCommandBuffer(secondary, &secondary_begin) ==
              VK_SUCCESS,
           "secondary re-begin");
   const VkViewport viewport = { .width = 64.0f, .height = 64.0f,
                                 .maxDepth = 1.0f };
   vkCmdSetViewport(secondary, 0, 1, &viewport);
   REQUIRE(vkEndCommandBuffer(secondary) == VK_SUCCESS,
           "stateful secondary end");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "primary begin for the stateful refusal");
   vkCmdExecuteCommands(f->cmd, 1, &secondary);
   CHECK(vkEndCommandBuffer(f->cmd) != VK_SUCCESS,
         "executing a secondary with recorded state poisons");

   vkFreeCommandBuffers(f->device, f->cmd_pool, 1, &secondary);
   return 0;
}

/* The host event path: immediate host set/reset/status, the recorded
 * set publishing at submission, a wait after a recorded set
 * submitting, and a wait on an unsignaled event losing the device --
 * observed as the refused submit under the runtime's loss folding.
 */
static int
check_host_events(const struct fixture *f)
{
   const VkEventCreateInfo event_info = {
      .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
   };
   VkEvent event;
   REQUIRE(vkCreateEvent(f->device, &event_info, NULL, &event) ==
              VK_SUCCESS,
           "event creation");
   CHECK(vkGetEventStatus(f->device, event) == VK_EVENT_RESET,
         "a fresh event reads unsignaled");

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &f->cmd,
   };
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "event span pool reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "event span begin");
   vkCmdSetEvent(f->cmd, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   vkCmdWaitEvents(f->cmd, 1, &event,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL,
                   0, NULL);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "set-then-wait records");
   CHECK(vkQueueSubmit(f->queue, 1, &submit_info, VK_NULL_HANDLE) ==
            VK_SUCCESS &&
         vkQueueWaitIdle(f->queue) == VK_SUCCESS,
         "the recorded set satisfies the recorded wait");
   CHECK(vkGetEventStatus(f->device, event) == VK_EVENT_SET,
         "the recorded set published to the host view");

   REQUIRE(vkResetEvent(f->device, event) == VK_SUCCESS, "host reset");
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "unsatisfied wait pool reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "unsatisfied wait begin");
   vkCmdWaitEvents(f->cmd, 1, &event,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL,
                   0, NULL);
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS,
           "the bare wait records");
   VkResult lost = vkQueueSubmit(f->queue, 1, &submit_info,
                                 VK_NULL_HANDLE);
   CHECK(lost != VK_SUCCESS,
         "a wait on an unsignaled event loses the submission");

   /* A wait carrying barriers poisons: barrier work travels through
    * vkCmdPipelineBarrier.  The lost queue above ends the fixture, so
    * this recording check closes the leg.
    */
   vkDestroyEvent(f->device, event, NULL);
   return 0;
}

/* Fences, semaphores, and idle over the CPU sync type: an unsignaled
 * fence signals with its submission and waits complete at once (the
 * submission executed synchronously), reset returns it, a pre-signaled
 * fence waits immediately, a semaphore signaled by one submission
 * satisfies the next submission's wait, and both idle calls return on
 * the drained queue.
 */
static int
check_sync_primitives(const struct fixture *f)
{
   VkFence fence;
   const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   REQUIRE(vkCreateFence(f->device, &fence_info, NULL, &fence) ==
              VK_SUCCESS,
           "fence creation");
   CHECK(vkGetFenceStatus(f->device, fence) == VK_NOT_READY,
         "a fresh fence reads unsignaled");

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   REQUIRE(vkResetCommandPool(f->device, f->cmd_pool, 0) == VK_SUCCESS,
           "sync pool reset");
   REQUIRE(vkBeginCommandBuffer(f->cmd, &begin_info) == VK_SUCCESS,
           "sync begin");
   REQUIRE(vkEndCommandBuffer(f->cmd) == VK_SUCCESS, "empty sync end");

   VkSemaphore semaphore;
   const VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
   };
   REQUIRE(vkCreateSemaphore(f->device, &semaphore_info, NULL,
                             &semaphore) == VK_SUCCESS,
           "semaphore creation");

   const VkSubmitInfo signal_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &f->cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &semaphore,
   };
   CHECK(vkQueueSubmit(f->queue, 1, &signal_submit, fence) == VK_SUCCESS,
         "the signaling submission");
   CHECK(vkWaitForFences(f->device, 1, &fence, VK_TRUE, UINT64_MAX) ==
            VK_SUCCESS &&
         vkGetFenceStatus(f->device, fence) == VK_SUCCESS,
         "the fence signaled with its submission");
   CHECK(vkResetFences(f->device, 1, &fence) == VK_SUCCESS &&
         vkGetFenceStatus(f->device, fence) == VK_NOT_READY,
         "the reset returns the fence");

   const VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   const VkSubmitInfo wait_submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &semaphore,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &f->cmd,
   };
   CHECK(vkQueueSubmit(f->queue, 1, &wait_submit, fence) == VK_SUCCESS &&
         vkWaitForFences(f->device, 1, &fence, VK_TRUE, UINT64_MAX) ==
            VK_SUCCESS,
         "the semaphore signaled by the first submission satisfies the "
         "second");

   CHECK(vkQueueWaitIdle(f->queue) == VK_SUCCESS &&
         vkDeviceWaitIdle(f->device) == VK_SUCCESS,
         "both idle calls return on the drained queue");

   VkFence signaled_fence;
   const VkFenceCreateInfo signaled_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
   };
   REQUIRE(vkCreateFence(f->device, &signaled_info, NULL,
                         &signaled_fence) == VK_SUCCESS,
           "pre-signaled fence creation");
   CHECK(vkWaitForFences(f->device, 1, &signaled_fence, VK_TRUE, 0) ==
            VK_SUCCESS,
         "a pre-signaled fence waits immediately");

   vkDestroyFence(f->device, signaled_fence, NULL);
   vkDestroyFence(f->device, fence, NULL);
   vkDestroySemaphore(f->device, semaphore, NULL);
   vkDestroyFence(f->device, VK_NULL_HANDLE, NULL);
   vkDestroySemaphore(f->device, VK_NULL_HANDLE, NULL);
   return 0;
}

static int
create_fixture(struct fixture *f)
{
   setenv("R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL", "1", 1);

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
   const char *const device_extensions[] = {
      VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
      VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 2,
      .ppEnabledExtensionNames = device_extensions,
   };
   REQUIRE(vkCreateDevice(f->pdev, &device_info, NULL, &f->device) ==
              VK_SUCCESS,
           "device creation");
   vkGetDeviceQueue(f->device, 0, 0, &f->queue);

   const VkDescriptorSetLayoutBinding bindings[2] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };
   const VkDescriptorSetLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings,
   };
   REQUIRE(vkCreateDescriptorSetLayout(f->device, &layout_info, NULL,
                                       &f->set_layout) == VK_SUCCESS,
           "descriptor set layout");

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(r3v_reference_identity_map_spirv),
      .pCode = r3v_reference_identity_map_spirv,
   };
   VkShaderModule module;
   REQUIRE(vkCreateShaderModule(f->device, &module_info, NULL, &module) ==
              VK_SUCCESS,
           "reference shader module");
   const VkPipelineLayoutCreateInfo pipeline_layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &f->set_layout,
   };
   REQUIRE(vkCreatePipelineLayout(f->device, &pipeline_layout_info, NULL,
                                  &f->pipeline_layout) == VK_SUCCESS,
           "pipeline layout");
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = f->pipeline_layout,
   };
   REQUIRE(vkCreateComputePipelines(f->device, VK_NULL_HANDLE, 1,
                                    &pipeline_info, NULL,
                                    &f->pipeline) == VK_SUCCESS,
           "reference compute pipeline");
   vkDestroyShaderModule(f->device, module, NULL);

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

static void
destroy_fixture(struct fixture *f)
{
   vkDestroyCommandPool(f->device, f->cmd_pool, NULL);
   vkDestroyPipeline(f->device, f->pipeline, NULL);
   vkDestroyPipelineLayout(f->device, f->pipeline_layout, NULL);
   vkDestroyDescriptorSetLayout(f->device, f->set_layout, NULL);
   vkDestroyDevice(f->device, NULL);
   vkDestroyInstance(f->instance, NULL);
}

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--inject-rebind-admits") == 0) {
         mutation = MUTATION_REBIND_ADMITS;
      } else if (strcmp(argv[i], "--inject-wrong-type-bind-admits") == 0) {
         mutation = MUTATION_WRONG_TYPE_BIND_ADMITS;
      } else {
         fprintf(stderr, "unknown argument: %s\n", argv[i]);
         return 1;
      }
   }

   struct fixture f = { 0 };
   if (create_fixture(&f))
      return 1;

   int fatal = check_sampler_lifetime(&f) ||
               check_buffer_view_lifetime(&f) ||
               check_buffer_view_whole_size_rounds_down(&f) ||
               check_buffer_binding(&f) ||
               check_image_binding(&f) ||
               check_sampler_use_fail_closed(&f) ||
               check_query_zero_span(&f) ||
               check_empty_secondary_execution(&f) ||
               check_sync_primitives(&f) ||
               check_host_events(&f);

   destroy_fixture(&f);
   if (fatal || failures) {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("native object lifetime contract holds\n");
   return 0;
}
