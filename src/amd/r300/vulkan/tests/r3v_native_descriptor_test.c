/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V descriptor fixture: the storage-buffer descriptor contract
 * through the public layout, pool, set, update, bind, and dispatch
 * entry points under the drm-shim transport.  vkUpdateDescriptorSets
 * returns void, so every out-of-contract write or copy poisons its set
 * and the dispatch recording refuses it; a descriptor offset shifts the
 * executed base; VK_WHOLE_SIZE clips at the buffer end; a pool admits
 * exactly its declared sets across free and reset.
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
   /* The over-range write is reported as admitted at recording. */
   MUTATION_UNPOISONED_OVERSIZE_WRITE,
   /* The allocation past the pool capacity is reported as admitted. */
   MUTATION_POOL_OVERFLOW_ADMITS,
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

/* Setup that the later verdicts depend on: a failure ends the run. */
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

#define DISPATCH_GROUPS 2u
#define WORKGROUP_SIZE 64u
#define DISPATCH_WORDS (DISPATCH_GROUPS * WORKGROUP_SIZE)
#define DISPATCH_BYTES (DISPATCH_WORDS * 4u)

struct fixture {
   VkInstance instance;
   VkPhysicalDevice pdev;
   VkDevice device;
   VkQueue queue;
   VkDeviceSize storage_alignment;
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkCommandPool cmd_pool;
   VkCommandBuffer cmd;
};

struct storage {
   VkBuffer buffer;
   VkDeviceMemory memory;
   uint32_t *map;
   VkDeviceSize bytes;
};

static int
create_storage(const struct fixture *f, VkDeviceSize bytes,
               struct storage *out)
{
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = bytes,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   REQUIRE(vkCreateBuffer(f->device, &buffer_info, NULL, &out->buffer) ==
              VK_SUCCESS,
           "storage buffer creation");
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(f->device, out->buffer, &requirements);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      /* Type 0 is the one host-visible native type. */
      .memoryTypeIndex = 0,
   };
   REQUIRE(vkAllocateMemory(f->device, &allocate_info, NULL,
                            &out->memory) == VK_SUCCESS,
           "storage memory allocation");
   REQUIRE(vkBindBufferMemory(f->device, out->buffer, out->memory, 0) ==
              VK_SUCCESS,
           "storage buffer bind");
   void *map = NULL;
   REQUIRE(vkMapMemory(f->device, out->memory, 0, VK_WHOLE_SIZE, 0,
                       &map) == VK_SUCCESS,
           "storage memory map");
   out->map = map;
   out->bytes = bytes;
   return 0;
}

static VkDescriptorSetLayoutBinding
storage_binding(uint32_t binding)
{
   return (VkDescriptorSetLayoutBinding){
      .binding = binding,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
}

static VkResult
create_layout(const struct fixture *f,
              const VkDescriptorSetLayoutBinding *bindings, uint32_t count,
              VkDescriptorSetLayoutCreateFlags flags,
              VkDescriptorSetLayout *layout)
{
   const VkDescriptorSetLayoutCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = flags,
      .bindingCount = count,
      .pBindings = bindings,
   };
   *layout = (VkDescriptorSetLayout)(uintptr_t)0x1;
   return vkCreateDescriptorSetLayout(f->device, &info, NULL, layout);
}

static VkResult
create_pool(const struct fixture *f, uint32_t max_sets,
            VkDescriptorPoolCreateFlags flags, VkDescriptorType size_type,
            VkDescriptorPool *pool)
{
   const VkDescriptorPoolSize size = {
      .type = size_type,
      .descriptorCount = 2 * max_sets,
   };
   const VkDescriptorPoolCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .flags = flags,
      .maxSets = max_sets,
      .poolSizeCount = 1,
      .pPoolSizes = &size,
   };
   *pool = (VkDescriptorPool)(uintptr_t)0x1;
   return vkCreateDescriptorPool(f->device, &info, NULL, pool);
}

static VkResult
allocate_set(const struct fixture *f, VkDescriptorPool pool,
             VkDescriptorSet *set)
{
   const VkDescriptorSetAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &f->set_layout,
   };
   *set = (VkDescriptorSet)(uintptr_t)0x1;
   return vkAllocateDescriptorSets(f->device, &info, set);
}

static void
write_storage(const struct fixture *f, VkDescriptorSet set,
              uint32_t binding, uint32_t array_element,
              VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset,
              VkDeviceSize range)
{
   const VkDescriptorBufferInfo buffer_info = {
      .buffer = buffer,
      .offset = offset,
      .range = range,
   };
   const VkWriteDescriptorSet write = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .dstArrayElement = array_element,
      .descriptorCount = 1,
      .descriptorType = type,
      .pBufferInfo = &buffer_info,
   };
   vkUpdateDescriptorSets(f->device, 1, &write, 0, NULL);
}

/* Records one dispatch of the reference identity-map pipeline over the
 * set and returns vkEndCommandBuffer's result: VK_SUCCESS for an
 * admitted recording, the refusal result for a poisoned one.
 */
static VkResult
record_dispatch(const struct fixture *f, VkDescriptorSet set,
                uint32_t groups)
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
   vkCmdDispatch(f->cmd, groups, 1, 1);
   return vkEndCommandBuffer(f->cmd);
}

static int
submit_and_wait(const struct fixture *f)
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

   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(f->pdev, &properties);
   f->storage_alignment =
      properties.limits.minStorageBufferOffsetAlignment;
   REQUIRE(f->storage_alignment >= 4 && f->storage_alignment <= 256,
           "advertised storage offset alignment is a small power-of-two "
           "byte count");

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
      storage_binding(0), storage_binding(1),
   };
   REQUIRE(create_layout(f, bindings, 2, 0, &f->set_layout) == VK_SUCCESS,
           "two storage bindings under the compute stage admit");

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(r3v_reference_identity_map_spirv),
      .pCode = r3v_reference_identity_map_spirv,
   };
   VkShaderModule module;
   REQUIRE(vkCreateShaderModule(f->device, &module_info, NULL, &module) ==
              VK_SUCCESS,
           "reference shader module");
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &f->set_layout,
   };
   REQUIRE(vkCreatePipelineLayout(f->device, &layout_info, NULL,
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
           "command buffer");
   return 0;
}

/* Layout admission: the storage-only, one-descriptor, compute-stage
 * shape admits; every other shape refuses with the handle cleared.
 */
static void
check_layout_admission(const struct fixture *f)
{
   VkDescriptorSetLayout layout;
   VkDescriptorSetLayoutBinding binding = storage_binding(0);

   binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   CHECK(create_layout(f, &binding, 1, 0, &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "a uniform-buffer binding refuses");

   binding = storage_binding(0);
   binding.descriptorCount = 2;
   CHECK(create_layout(f, &binding, 1, 0, &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "a descriptor array refuses");

   binding = storage_binding(0);
   binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
   CHECK(create_layout(f, &binding, 1, 0, &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "a vertex-stage binding refuses");

   binding = storage_binding(0);
   const VkSampler sampler = (VkSampler)(uintptr_t)0x404;
   binding.pImmutableSamplers = &sampler;
   CHECK(create_layout(f, &binding, 1, 0, &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "an immutable sampler pointer refuses");

   const VkDescriptorSetLayoutBinding duplicate[2] = {
      storage_binding(1), storage_binding(1),
   };
   CHECK(create_layout(f, duplicate, 2, 0, &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "a duplicate binding number refuses");

   binding = storage_binding(4);
   CHECK(create_layout(f, &binding, 1, 0, &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "a binding number past the four-binding bound refuses");

   binding = storage_binding(0);
   CHECK(create_layout(
            f, &binding, 1,
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            &layout) != VK_SUCCESS &&
            layout == VK_NULL_HANDLE,
         "an update-after-bind layout refuses");

   CHECK(create_layout(f, &binding, 1, 0, &layout) == VK_SUCCESS &&
            layout != VK_NULL_HANDLE,
         "the storage binding alone admits");
   if (layout != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(f->device, layout, NULL);
}

/* Pool lifetime: admission by flags and capacity, allocation up to the
 * declared set count, refusal past it, and reuse after free and reset.
 */
static void
check_pool_lifetime(const struct fixture *f)
{
   VkDescriptorPool pool;
   CHECK(create_pool(f, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pool) !=
            VK_SUCCESS &&
            pool == VK_NULL_HANDLE,
         "a zero-capacity pool refuses");
   CHECK(create_pool(f, 17, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pool) !=
            VK_SUCCESS &&
            pool == VK_NULL_HANDLE,
         "a pool past sixteen sets refuses");
   CHECK(create_pool(f, 2, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &pool) !=
            VK_SUCCESS &&
            pool == VK_NULL_HANDLE,
         "a uniform-buffer pool size refuses");
   CHECK(create_pool(f, 2, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pool) !=
            VK_SUCCESS &&
            pool == VK_NULL_HANDLE,
         "an update-after-bind pool refuses");

   CHECK(create_pool(f, 2, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pool) ==
            VK_SUCCESS &&
            pool != VK_NULL_HANDLE,
         "a two-set pool with free-descriptor-set admits");
   if (pool == VK_NULL_HANDLE)
      return;

   VkDescriptorSet sets[3];
   CHECK(allocate_set(f, pool, &sets[0]) == VK_SUCCESS &&
            allocate_set(f, pool, &sets[1]) == VK_SUCCESS,
         "the pool allocates its two declared sets");
   VkResult third = allocate_set(f, pool, &sets[2]);
   if (mutation == MUTATION_POOL_OVERFLOW_ADMITS) {
      third = VK_SUCCESS;
      sets[2] = (VkDescriptorSet)(uintptr_t)0x1;
   }
   CHECK(third == VK_ERROR_OUT_OF_POOL_MEMORY && sets[2] == VK_NULL_HANDLE,
         "the third allocation refuses with out-of-pool-memory and a "
         "cleared handle");

   CHECK(vkFreeDescriptorSets(f->device, pool, 1, &sets[0]) == VK_SUCCESS,
         "freeing one set succeeds");
   CHECK(allocate_set(f, pool, &sets[0]) == VK_SUCCESS,
         "the freed capacity allocates again");

   CHECK(vkResetDescriptorPool(f->device, pool, 0) == VK_SUCCESS,
         "pool reset succeeds");
   CHECK(allocate_set(f, pool, &sets[0]) == VK_SUCCESS &&
            allocate_set(f, pool, &sets[1]) == VK_SUCCESS &&
            allocate_set(f, pool, &sets[2]) == VK_ERROR_OUT_OF_POOL_MEMORY,
         "a reset pool allocates its full capacity once more");
   vkDestroyDescriptorPool(f->device, pool, NULL);
}

/* Descriptor range semantics: the descriptor offset shifts the executed
 * base, VK_WHOLE_SIZE resolves to the bytes past the offset, and a
 * dispatch past the resolved range refuses at recording.
 */
static int
check_offset_and_range(const struct fixture *f)
{
   VkDescriptorPool pool;
   REQUIRE(create_pool(f, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pool) ==
              VK_SUCCESS,
           "range pool");
   VkDescriptorSet set;
   REQUIRE(allocate_set(f, pool, &set) == VK_SUCCESS, "range set");

   const VkDeviceSize offset = f->storage_alignment;
   const VkDeviceSize bytes = offset + DISPATCH_BYTES + 4;
   struct storage input, output;
   if (create_storage(f, bytes, &input) != 0 ||
       create_storage(f, bytes, &output) != 0)
      return 1;
   const uint32_t words = (uint32_t)(bytes / 4);
   const uint32_t lead = (uint32_t)(offset / 4);
   for (uint32_t i = 0; i < words; i++) {
      input.map[i] = 0x2000u ^ (i * 0x9e3779b9u);
      output.map[i] = 0xdeadbeefu;
   }

   write_storage(f, set, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 input.buffer, offset, DISPATCH_BYTES);
   write_storage(f, set, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 output.buffer, offset, VK_WHOLE_SIZE);
   CHECK(record_dispatch(f, set, DISPATCH_GROUPS) == VK_SUCCESS,
         "a dispatch inside both ranges records");
   if (submit_and_wait(f) != 0)
      return 1;
   CHECK(memcmp(&output.map[lead], &input.map[lead], DISPATCH_BYTES) == 0,
         "the executed words start at the descriptor offset");
   bool prefix_untouched = true;
   for (uint32_t i = 0; i < lead; i++)
      prefix_untouched &= output.map[i] == 0xdeadbeefu;
   CHECK(prefix_untouched,
         "words before the descriptor offset keep their sentinel");
   CHECK(output.map[lead + DISPATCH_WORDS] == 0xdeadbeefu,
         "the word past the dispatch keeps its sentinel");

   /* VK_WHOLE_SIZE at the offset resolves to DISPATCH_BYTES + 4, so one
    * more group exceeds it and the input's explicit range alike.
    */
   CHECK(record_dispatch(f, set, DISPATCH_GROUPS + 1) != VK_SUCCESS,
         "a dispatch past the resolved range refuses at recording");

   vkDestroyDescriptorPool(f->device, pool, NULL);
   return 0;
}

/* Poison on void update: each out-of-contract write or copy poisons its
 * set, and the dispatch recording refuses the poisoned set; a fresh set
 * with the same buffers records.
 */
static int
check_update_poison(const struct fixture *f)
{
   VkDescriptorPool pool;
   REQUIRE(create_pool(f, 8, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &pool) ==
              VK_SUCCESS,
           "poison pool");
   struct storage input, output;
   if (create_storage(f, DISPATCH_BYTES, &input) != 0 ||
       create_storage(f, DISPATCH_BYTES, &output) != 0)
      return 1;

   struct arm {
      const char *label;
      uint32_t binding;
      uint32_t array_element;
      VkDescriptorType type;
      VkDeviceSize offset;
      VkDeviceSize range;
   };
   const struct arm arms[] = {
      { "an offset past the buffer", 0, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DISPATCH_BYTES + 4,
        VK_WHOLE_SIZE },
      { "a range past the buffer end", 0, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16, DISPATCH_BYTES },
      { "a nonzero array element", 0, 1,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, VK_WHOLE_SIZE },
      { "a binding absent from the layout", 3, 0,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, VK_WHOLE_SIZE },
      { "a uniform-buffer write", 0, 0,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, VK_WHOLE_SIZE },
   };
   for (unsigned i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
      VkDescriptorSet set;
      REQUIRE(allocate_set(f, pool, &set) == VK_SUCCESS, "poison-arm set");
      /* Both bindings valid first, so the poison alone decides. */
      write_storage(f, set, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    input.buffer, 0, VK_WHOLE_SIZE);
      write_storage(f, set, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    output.buffer, 0, VK_WHOLE_SIZE);
      write_storage(f, set, arms[i].binding, arms[i].array_element,
                    arms[i].type, input.buffer, arms[i].offset,
                    arms[i].range);
      VkResult recorded = record_dispatch(f, set, DISPATCH_GROUPS);
      if (mutation == MUTATION_UNPOISONED_OVERSIZE_WRITE && i == 1)
         recorded = VK_SUCCESS;
      CHECK(recorded != VK_SUCCESS,
            "%s poisons the set and the dispatch recording refuses",
            arms[i].label);
   }

   /* A copy poisons its destination whatever the source holds. */
   VkDescriptorSet source, destination;
   REQUIRE(allocate_set(f, pool, &source) == VK_SUCCESS, "copy source");
   REQUIRE(allocate_set(f, pool, &destination) == VK_SUCCESS,
           "copy destination");
   write_storage(f, source, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 input.buffer, 0, VK_WHOLE_SIZE);
   write_storage(f, source, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 output.buffer, 0, VK_WHOLE_SIZE);
   write_storage(f, destination, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 input.buffer, 0, VK_WHOLE_SIZE);
   write_storage(f, destination, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 output.buffer, 0, VK_WHOLE_SIZE);
   const VkCopyDescriptorSet copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
      .srcSet = source,
      .srcBinding = 0,
      .dstSet = destination,
      .dstBinding = 0,
      .descriptorCount = 1,
   };
   vkUpdateDescriptorSets(f->device, 0, NULL, 1, &copy);
   CHECK(record_dispatch(f, destination, DISPATCH_GROUPS) != VK_SUCCESS,
         "a descriptor copy poisons its destination set");
   CHECK(record_dispatch(f, source, DISPATCH_GROUPS) == VK_SUCCESS,
         "the copy source stays recordable");

   /* An unbound binding refuses at recording without a poison. */
   VkDescriptorSet half;
   REQUIRE(allocate_set(f, pool, &half) == VK_SUCCESS, "half-bound set");
   write_storage(f, half, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 input.buffer, 0, VK_WHOLE_SIZE);
   CHECK(record_dispatch(f, half, DISPATCH_GROUPS) != VK_SUCCESS,
         "an unbound output binding refuses the dispatch recording");
   write_storage(f, half, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 output.buffer, 0, VK_WHOLE_SIZE);
   CHECK(record_dispatch(f, half, DISPATCH_GROUPS) == VK_SUCCESS,
         "binding the output afterwards records");

   vkDestroyDescriptorPool(f->device, pool, NULL);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc == 2 &&
       strcmp(argv[1], "--inject-unpoisoned-oversize-write") == 0) {
      mutation = MUTATION_UNPOISONED_OVERSIZE_WRITE;
   } else if (argc == 2 &&
              strcmp(argv[1], "--inject-pool-overflow-admits") == 0) {
      mutation = MUTATION_POOL_OVERFLOW_ADMITS;
   } else if (argc != 1) {
      fprintf(stderr,
              "usage: %s [--inject-unpoisoned-oversize-write|"
              "--inject-pool-overflow-admits]\n",
              argv[0]);
      return 2;
   }

   struct fixture f = { 0 };
   if (create_fixture(&f) != 0)
      return 1;
   check_layout_admission(&f);
   check_pool_lifetime(&f);
   if (check_offset_and_range(&f) != 0)
      return 1;
   if (check_update_poison(&f) != 0)
      return 1;

   if (failures != 0) {
      fprintf(stderr, "r3v-native-descriptor: %u failures\n", failures);
      return 1;
   }
   printf("r3v-native-descriptor: all checks passed\n");
   return 0;
}
