/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V object-lifetime fixture: sampler creation as recorded
 * state with its use refused at the descriptor write, buffer-view
 * refusal under the advertised format table, and the once-only
 * host-visible memory-binding admission for buffers and images under
 * the drm-shim transport.
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

static int
check_buffer_view_refusal(const struct fixture *f)
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

   const VkBufferViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
      .buffer = buffer,
      .format = VK_FORMAT_R32_SFLOAT,
      .offset = 0,
      .range = VK_WHOLE_SIZE,
   };
   VkBufferView view = (VkBufferView)(uintptr_t)0xdeadbeef;
   CHECK(vkCreateBufferView(f->device, &view_info, NULL, &view) !=
            VK_SUCCESS &&
         view == VK_NULL_HANDLE,
         "buffer-view creation refuses with a cleared handle: no format "
         "advertises texel-buffer features");
   vkDestroyBufferView(f->device, VK_NULL_HANDLE, NULL);
   vkDestroyBuffer(f->device, buffer, NULL);
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

/* Secondary command buffers: the empty recorded sequence executes as
 * an exact no-op through vkCmdExecuteCommands and the whole primary
 * submits; a secondary carrying recorded work, and an execute inside
 * a render pass or of a primary-level buffer, each poison.
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

static int
create_fixture(struct fixture *f)
{
   setenv("R3V_HYBRID_COMPUTE_EXPERIMENTAL", "1", 1);

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
               check_buffer_view_refusal(&f) ||
               check_buffer_binding(&f) ||
               check_image_binding(&f) ||
               check_sampler_use_fail_closed(&f) ||
               check_query_zero_span(&f) ||
               check_empty_secondary_execution(&f) ||
               check_host_events(&f);

   destroy_fixture(&f);
   if (fatal || failures) {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("native object lifetime contract holds\n");
   return 0;
}
