/*
 * SPDX-License-Identifier: MIT
 *
 * Executes the zero-PM4 HyperZ ownership operation through the native queue
 * on the Radeon noop drm-shim.  The shim's INFO and CS counters distinguish
 * one ownership acquisition from a command submission, while the command
 * buffer state proves the operation records no image transition or BO access.
 */

#undef NDEBUG

#define VK_NO_PROTOTYPES
#include "r3v_native.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *name);

typedef uint64_t (*counter_fn)(void);
typedef bool (*ownership_fn)(void);

static void *
required_shim_symbol(const char *name)
{
   dlerror();
   void *symbol = dlsym(RTLD_DEFAULT, name);
   assert(symbol != NULL);
   assert(dlerror() == NULL);
   return symbol;
}

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)get_instance_proc_addr(instance, #name)
#define LOAD_DEVICE(name) \
   PFN_##name name = (PFN_##name)get_device_proc_addr(device, #name)

int
main(int argc, char **argv)
{
   assert(argc == 2);
   const bool success = strcmp(argv[1], "success") == 0;
   const bool withhold = strcmp(argv[1], "withhold") == 0;
   const bool refuse = strcmp(argv[1], "refuse") == 0;
   assert(success || withhold || refuse);
   if (withhold)
      assert(setenv("R3V_NATIVE_SHIM_HYPERZ_WITHHOLD", "1", 1) == 0);
   if (refuse)
      assert(setenv("R3V_NATIVE_SHIM_HYPERZ_REFUSE", "1", 1) == 0);

   counter_fn cs_count =
      (counter_fn)required_shim_symbol("drm_shim_test_radeon_cs_ioctls");
   counter_fn acquire_count = (counter_fn)required_shim_symbol(
      "drm_shim_test_radeon_hyperz_acquire_ioctls");
   counter_fn release_count = (counter_fn)required_shim_symbol(
      "drm_shim_test_radeon_hyperz_release_ioctls");
   ownership_fn ownership_held = (ownership_fn)required_shim_symbol(
      "drm_shim_test_radeon_hyperz_owned");
   assert(cs_count() == 0u);
   assert(acquire_count() == 0u);
   assert(release_count() == 0u);
   assert(!ownership_held());

   assert(setenv("R3V_NATIVE_ZMASK_OWNERSHIP_EXPERIMENTAL", "1", 1) == 0);

   PFN_vkGetInstanceProcAddr get_instance_proc_addr =
      (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)get_instance_proc_addr(NULL, "vkCreateInstance");
   assert(create_instance != NULL);

   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(
             &(VkInstanceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
             },
             NULL, &instance) == VK_SUCCESS);
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);
   assert(vkEnumeratePhysicalDevices != NULL);
   assert(vkCreateDevice != NULL);
   assert(vkGetDeviceProcAddr != NULL);
   assert(vkDestroyInstance != NULL);
   PFN_vkGetDeviceProcAddr get_device_proc_addr = vkGetDeviceProcAddr;

   uint32_t physical_device_count = 1u;
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   VkResult result = vkEnumeratePhysicalDevices(
      instance, &physical_device_count, &physical_device);
   assert((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
          physical_device_count == 1u && physical_device != VK_NULL_HANDLE);

   const float queue_priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   assert(vkCreateDevice(
             physical_device,
             &(VkDeviceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount = 1u,
                .pQueueCreateInfos =
                   &(VkDeviceQueueCreateInfo){
                      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                      .queueFamilyIndex = 0u,
                      .queueCount = 1u,
                      .pQueuePriorities = &queue_priority,
                   },
             },
             NULL, &device) == VK_SUCCESS);
   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   assert(native_device->zmask_owner.image == NULL);
   assert(native_device->zmask_owner.metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   assert(native_device->transport_cs_ioctl_count == 0u);
   assert(native_device->queue_status == R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkQueueWaitIdle);
   LOAD_DEVICE(vkDestroyDevice);
   assert(vkGetDeviceQueue != NULL);
   assert(vkCreateCommandPool != NULL);
   assert(vkDestroyCommandPool != NULL);
   assert(vkAllocateCommandBuffers != NULL);
   assert(vkBeginCommandBuffer != NULL);
   assert(vkEndCommandBuffer != NULL);
   assert(vkQueueSubmit != NULL);
   assert(vkQueueWaitIdle != NULL);
   assert(vkDestroyDevice != NULL);

   VkCommandPool command_pool = VK_NULL_HANDLE;
   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0u,
             },
             NULL, &command_pool) == VK_SUCCESS);
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = command_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1u,
             },
             &command_buffer) == VK_SUCCESS);
   assert(vkBeginCommandBuffer(
             command_buffer,
             &(VkCommandBufferBeginInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
             }) == VK_SUCCESS);
   assert(r3v_native_record_zmask_ownership_only(command_buffer) ==
          VK_SUCCESS);
   assert(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);

   struct r3v_native_cmd_buffer *native_command =
      r3v_native_cmd_buffer_from_handle(command_buffer);
   assert(native_command->ib_size_dwords == 0u);
   assert(native_command->reference_count == 0u);
   assert(native_command->image_state_count == 0u);
   assert(native_command->ordered_operation_count == 1u);
   assert(native_command->ordered_operations[0].kind ==
          R3V_NATIVE_ORDERED_OPERATION_HYPERZ_ACQUIRE);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0u, 0u, &queue);
   assert(queue != VK_NULL_HANDLE);
   result = vkQueueSubmit(
      queue, 1u,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1u,
         .pCommandBuffers = &command_buffer,
      },
      VK_NULL_HANDLE);
   assert(result == (success ? VK_SUCCESS : VK_ERROR_DEVICE_LOST));
   if (success)
      assert(vkQueueWaitIdle(queue) == VK_SUCCESS);
   assert(acquire_count() == 1u);
   assert(release_count() == 0u);
   assert(cs_count() == 0u);
   assert(ownership_held() == success);
   assert(native_command->image_state_count == 0u);
   assert(native_device->hyperz_ownership ==
          (success ? R300_ZB_HYPERZ_OWNED : R300_ZB_HYPERZ_UNOWNED));
   assert(native_device->zmask_owner.image == NULL);
   assert(native_device->zmask_owner.metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   assert(native_device->transport_cs_ioctl_count == 0u);
   assert(native_device->queue_status ==
          (success ? R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION
                   : R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED));

   vkDestroyCommandPool(device, command_pool, NULL);
   vkDestroyDevice(device, NULL);
   assert(acquire_count() == 1u);
   assert(release_count() == (success ? 1u : 0u));
   assert(cs_count() == 0u);
   assert(!ownership_held());
   vkDestroyInstance(instance, NULL);
   return 0;
}
