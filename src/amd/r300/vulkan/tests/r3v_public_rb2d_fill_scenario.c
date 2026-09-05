/*
 * SPDX-License-Identifier: MIT
 *
 * Public-API scenario for the RB2D constant-fill cell.  Every call below
 * enters the installed Vulkan loader; the file defines no driver symbol
 * and links libvulkan and libc alone.
 */

#include "r3v_public_rb2d_fill_scenario.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static bool
fail(struct r3v_public_rb2d_fill_scenario *s, const char *what, VkResult r)
{
   if (s->failure[0] == '\0') {
      if (r == VK_SUCCESS)
         snprintf(s->failure, sizeof(s->failure), "%s", what);
      else
         snprintf(s->failure, sizeof(s->failure), "%s: %s", what,
                  r3v_public_rb2d_fill_result_name(r));
   }
   return false;
}

const char *
r3v_public_rb2d_fill_result_name(VkResult r)
{
   switch (r) {
   case VK_SUCCESS:
      return "VK_SUCCESS";
   case VK_NOT_READY:
      return "VK_NOT_READY";
   case VK_TIMEOUT:
      return "VK_TIMEOUT";
   case VK_INCOMPLETE:
      return "VK_INCOMPLETE";
   case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_MEMORY_MAP_FAILED:
      return "VK_ERROR_MEMORY_MAP_FAILED";
   case VK_ERROR_INCOMPATIBLE_DRIVER:
      return "VK_ERROR_INCOMPATIBLE_DRIVER";
   case VK_ERROR_UNKNOWN:
      return "VK_ERROR_UNKNOWN";
   default:
      return "(other)";
   }
}

bool
r3v_public_rb2d_fill_dso_mapped(const char *path)
{
   FILE *maps = fopen("/proc/self/maps", "r");
   if (maps == NULL)
      return false;
   char line[4096];
   bool found = false;
   while (fgets(line, sizeof(line), maps) != NULL) {
      if (strstr(line, path) != NULL) {
         found = true;
         break;
      }
   }
   fclose(maps);
   return found;
}

/* The lowest host-visible memory type the buffer admits; the sealed
 * cell binds type 0 (host-visible, coherent GTT) and the wrapper holds
 * the chosen index against its declaration. */
static bool
select_memory_type(struct r3v_public_rb2d_fill_scenario *s, uint32_t admitted)
{
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(s->physical_device, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
      const VkMemoryPropertyFlags flags = mp.memoryTypes[i].propertyFlags;
      if ((admitted & (1u << i)) == 0 ||
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
         continue;
      s->memory_type_index = i;
      s->host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      return true;
   }
   return fail(s, "no host-visible memory type admits the buffer", VK_SUCCESS);
}

bool
r3v_public_rb2d_fill_scenario_open(
   struct r3v_public_rb2d_fill_scenario *s,
   const struct r3v_public_rb2d_fill_scenario_config *config)
{
   memset(s, 0, sizeof(*s));
   s->config = *config;
   s->submit_result = VK_NOT_READY;
   s->wait_result = VK_NOT_READY;
   const struct r3v_public_rb2d_fill_cell *cell = config->cell;
   if (!r3v_public_rb2d_fill_cell_valid(cell))
      return fail(s, "the cell is malformed", VK_SUCCESS);

   VkResult r = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &s->instance);
   if (r != VK_SUCCESS)
      return fail(s, "vkCreateInstance", r);

   uint32_t count = 1;
   r = vkEnumeratePhysicalDevices(s->instance, &count, &s->physical_device);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count != 1 ||
       s->physical_device == VK_NULL_HANDLE)
      return fail(s, "vkEnumeratePhysicalDevices found no device", r);
   vkGetPhysicalDeviceProperties(s->physical_device, &s->properties);
   if ((config->required_vendor_id != 0 &&
        s->properties.vendorID != config->required_vendor_id) ||
       (config->required_device_id != 0 &&
        s->properties.deviceID != config->required_device_id))
      return fail(s, "the enumerated device is not the required one",
                  VK_SUCCESS);

   const float priority = 1.0f;
   r = vkCreateDevice(
      s->physical_device,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &priority,
            },
      },
      NULL, &s->device);
   if (r != VK_SUCCESS)
      return fail(s, "vkCreateDevice", r);
   vkGetDeviceQueue(s->device, 0, 0, &s->queue);
   if (s->queue == VK_NULL_HANDLE)
      return fail(s, "vkGetDeviceQueue returned no queue", VK_SUCCESS);

   r = vkCreateBuffer(s->device,
                      &(VkBufferCreateInfo){
                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                         .size = cell->allocation_bytes,
                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                      },
                      NULL, &s->buffer);
   if (r != VK_SUCCESS)
      return fail(s, "vkCreateBuffer", r);
   VkMemoryRequirements reqs;
   vkGetBufferMemoryRequirements(s->device, s->buffer, &reqs);
   if (reqs.size != cell->allocation_bytes)
      return fail(s, "the buffer's memory requirement differs from its size",
                  VK_SUCCESS);
   if (!select_memory_type(s, reqs.memoryTypeBits))
      return false;
   r = vkAllocateMemory(s->device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = cell->allocation_bytes,
                           .memoryTypeIndex = s->memory_type_index,
                        },
                        NULL, &s->memory);
   if (r != VK_SUCCESS)
      return fail(s, "vkAllocateMemory", r);
   r = vkBindBufferMemory(s->device, s->buffer, s->memory, 0);
   if (r != VK_SUCCESS)
      return fail(s, "vkBindBufferMemory", r);

   r = vkMapMemory(s->device, s->memory, 0, VK_WHOLE_SIZE, 0, (void **)&s->map);
   if (r != VK_SUCCESS || s->map == NULL)
      return fail(s, "vkMapMemory", r);
   r3v_public_rb2d_fill_initialize(cell, s->map);
   if (!s->host_coherent) {
      r = vkFlushMappedMemoryRanges(
         s->device, 1,
         &(VkMappedMemoryRange){
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = s->memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
         });
      if (r != VK_SUCCESS)
         return fail(s, "vkFlushMappedMemoryRanges", r);
   }
   if (config->protect_destination) {
      const long page = sysconf(_SC_PAGESIZE);
      if (page <= 0 || ((uintptr_t)s->map % (uintptr_t)page) != 0)
         return fail(s, "the mapping is not page-aligned; protection would "
                        "cover the wrong bytes",
                     VK_SUCCESS);
      if (mprotect(s->map, cell->allocation_bytes, PROT_READ) != 0) {
         snprintf(s->failure, sizeof(s->failure), "mprotect: %s",
                  strerror(errno));
         return false;
      }
      s->map_protected = true;
   }
   return true;
}

bool
r3v_public_rb2d_fill_scenario_record(struct r3v_public_rb2d_fill_scenario *s)
{
   const struct r3v_public_rb2d_fill_cell *cell = s->config.cell;
   VkResult r = vkCreateCommandPool(
      s->device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &s->pool);
   if (r != VK_SUCCESS)
      return fail(s, "vkCreateCommandPool", r);
   r = vkAllocateCommandBuffers(
      s->device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = s->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &s->cmd);
   if (r != VK_SUCCESS)
      return fail(s, "vkAllocateCommandBuffers", r);
   r = vkBeginCommandBuffer(
      s->cmd, &(VkCommandBufferBeginInfo){
                 .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                 .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
              });
   if (r != VK_SUCCESS)
      return fail(s, "vkBeginCommandBuffer", r);
   vkCmdFillBuffer(s->cmd, s->buffer, cell->fill_offset, cell->fill_bytes,
                   cell->fill_value);
   r = vkEndCommandBuffer(s->cmd);
   if (r != VK_SUCCESS)
      return fail(s, "vkEndCommandBuffer", r);
   r = vkCreateFence(s->device,
                     &(VkFenceCreateInfo){
                        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                     },
                     NULL, &s->fence);
   if (r != VK_SUCCESS)
      return fail(s, "vkCreateFence", r);
   return true;
}

bool
r3v_public_rb2d_fill_scenario_submit(struct r3v_public_rb2d_fill_scenario *s)
{
   s->submit_result = vkQueueSubmit(s->queue, 1,
                                    &(VkSubmitInfo){
                                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                       .commandBufferCount = 1,
                                       .pCommandBuffers = &s->cmd,
                                    },
                                    s->fence);
   if (s->submit_result != VK_SUCCESS)
      return fail(s, "vkQueueSubmit", s->submit_result);
   return true;
}

bool
r3v_public_rb2d_fill_scenario_wait(struct r3v_public_rb2d_fill_scenario *s)
{
   s->wait_result = vkWaitForFences(s->device, 1, &s->fence, VK_TRUE,
                                    s->config.wait_bound_ns);
   if (s->wait_result != VK_SUCCESS)
      return fail(s, "vkWaitForFences", s->wait_result);
   if (!s->host_coherent) {
      const VkResult r = vkInvalidateMappedMemoryRanges(
         s->device, 1,
         &(VkMappedMemoryRange){
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = s->memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
         });
      if (r != VK_SUCCESS)
         return fail(s, "vkInvalidateMappedMemoryRanges", r);
   }
   return true;
}

const uint8_t *
r3v_public_rb2d_fill_scenario_image(const struct r3v_public_rb2d_fill_scenario *s)
{
   return s->map;
}

void
r3v_public_rb2d_fill_scenario_close(struct r3v_public_rb2d_fill_scenario *s)
{
   if (s->map != NULL) {
      if (s->map_protected)
         mprotect(s->map, s->config.cell->allocation_bytes,
                  PROT_READ | PROT_WRITE);
      vkUnmapMemory(s->device, s->memory);
   }
   if (s->fence != VK_NULL_HANDLE)
      vkDestroyFence(s->device, s->fence, NULL);
   if (s->pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(s->device, s->pool, NULL);
   if (s->buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(s->device, s->buffer, NULL);
   if (s->memory != VK_NULL_HANDLE)
      vkFreeMemory(s->device, s->memory, NULL);
   if (s->device != VK_NULL_HANDLE)
      vkDestroyDevice(s->device, NULL);
   if (s->instance != VK_NULL_HANDLE)
      vkDestroyInstance(s->instance, NULL);
   memset(s, 0, sizeof(*s));
}
