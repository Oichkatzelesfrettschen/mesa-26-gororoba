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
#include "r3v_native_shim_arming.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include <vulkan/vulkan.h>

#include "util/mesa-blake3.h"

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
   const bool ownership_success = strcmp(argv[1], "success") == 0;
   const bool withhold = strcmp(argv[1], "withhold") == 0;
   const bool refuse = strcmp(argv[1], "refuse") == 0;
   const bool initialize_success =
      strcmp(argv[1], "initialize-success") == 0;
   const bool initialize_cs_refuse =
      strcmp(argv[1], "initialize-cs-refuse") == 0;
   const bool initialize_completion_fail =
      strcmp(argv[1], "initialize-completion-fail") == 0;
   const bool fast_clear_success =
      strcmp(argv[1], "fast-clear-success") == 0;
   const bool fast_clear_cs_refuse =
      strcmp(argv[1], "fast-clear-cs-refuse") == 0;
   const bool fast_clear_completion_fail =
      strcmp(argv[1], "fast-clear-completion-fail") == 0;
   const bool materialize_success =
      strcmp(argv[1], "materialize-success") == 0;
   const bool materialize_cs_refuse =
      strcmp(argv[1], "materialize-cs-refuse") == 0;
   const bool materialize_completion_fail =
      strcmp(argv[1], "materialize-completion-fail") == 0;
   const bool initialize = initialize_success || initialize_cs_refuse ||
                           initialize_completion_fail;
   const bool fast_clear = fast_clear_success || fast_clear_cs_refuse ||
                           fast_clear_completion_fail;
   const bool materialize = materialize_success || materialize_cs_refuse ||
                            materialize_completion_fail;
   const bool records_fast_clear = fast_clear || materialize;
   const bool metadata_operation = initialize || records_fast_clear;
   const bool cs_refuse = initialize_cs_refuse || fast_clear_cs_refuse ||
                          materialize_cs_refuse;
   const bool completion_fail = initialize_completion_fail ||
                                fast_clear_completion_fail ||
                                materialize_completion_fail;
   const bool success = ownership_success || initialize_success ||
                        fast_clear_success || materialize_success;
   assert(ownership_success || withhold || refuse || metadata_operation);
   if (withhold)
      assert(setenv("R3V_NATIVE_SHIM_HYPERZ_WITHHOLD", "1", 1) == 0);
   if (refuse)
      assert(setenv("R3V_NATIVE_SHIM_HYPERZ_REFUSE", "1", 1) == 0);
   if (cs_refuse)
      assert(setenv("R3V_NATIVE_SHIM_CS_REFUSE", "1", 1) == 0);
   if (completion_fail)
      assert(setenv("R3V_NATIVE_SHIM_COMPLETION_FAIL", "1", 1) == 0);

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
   if (initialize)
      assert(setenv("R3V_NATIVE_ZMASK_INITIALIZE_EXPERIMENTAL", "1", 1) ==
             0);
   if (records_fast_clear)
      assert(setenv("R3V_NATIVE_ZMASK_FAST_CLEAR_EXPERIMENTAL", "1", 1) ==
             0);

   char manifest_directory[PATH_MAX] = {0};
   if (metadata_operation) {
      const char *temporary_root = getenv("TMPDIR");
      if (temporary_root == NULL || temporary_root[0] == '\0')
         temporary_root = "/tmp";
      assert(snprintf(manifest_directory, sizeof(manifest_directory),
                      "%s/r3v-zmask-queue-XXXXXX", temporary_root) > 0);
      assert(mkdtemp(manifest_directory) != NULL);
      assert(setenv("R3V_NATIVE_MANIFEST_DIR", manifest_directory, 1) == 0);
      assert(setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1) == 0);
      struct utsname host;
      assert(uname(&host) == 0);
      assert(setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release,
                    1) == 0);
      assert(setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
                    R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1) == 0);
   }

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
   if (metadata_operation)
      r3v_native_install_shim_arming(native_device);
   assert(native_device->zmask_owner.image == NULL);
   assert(native_device->zmask_owner.metadata.status ==
          R3V_NATIVE_ZMASK_METADATA_RETIRED);
   assert(native_device->transport_cs_ioctl_count == 0u);
   assert(native_device->queue_status == R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkCreateImage);
   LOAD_DEVICE(vkDestroyImage);
   LOAD_DEVICE(vkGetImageMemoryRequirements);
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkBindImageMemory);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkCmdPipelineBarrier);
   LOAD_DEVICE(vkCmdClearDepthStencilImage);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);
   assert(vkGetDeviceQueue != NULL);
   assert(vkCreateCommandPool != NULL);
   assert(vkDestroyCommandPool != NULL);
   assert(vkAllocateCommandBuffers != NULL);
   assert(vkCreateImage != NULL);
   assert(vkDestroyImage != NULL);
   assert(vkGetImageMemoryRequirements != NULL);
   assert(vkAllocateMemory != NULL);
   assert(vkFreeMemory != NULL);
   assert(vkBindImageMemory != NULL);
   assert(vkBeginCommandBuffer != NULL);
   assert(vkEndCommandBuffer != NULL);
   assert(vkCmdPipelineBarrier != NULL);
   assert(vkCmdClearDepthStencilImage != NULL);
   assert(vkQueueSubmit != NULL);
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
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE;
   if (metadata_operation) {
      assert(vkCreateImage(
                device,
                &(VkImageCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                   .imageType = VK_IMAGE_TYPE_2D,
                   .format = VK_FORMAT_D24_UNORM_S8_UINT,
                   .extent = {64u, 64u, 1u},
                   .mipLevels = 1u,
                   .arrayLayers = 1u,
                   .samples = VK_SAMPLE_COUNT_1_BIT,
                   .tiling = VK_IMAGE_TILING_OPTIMAL,
                   .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                },
                NULL, &image) == VK_SUCCESS);
      VkMemoryRequirements requirements;
      vkGetImageMemoryRequirements(device, image, &requirements);
      assert(vkAllocateMemory(
                device,
                &(VkMemoryAllocateInfo){
                   .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                   .allocationSize = requirements.size + requirements.alignment,
                   .memoryTypeIndex = 0u,
                },
                NULL, &image_memory) == VK_SUCCESS);
      assert(vkBindImageMemory(device, image, image_memory,
                               requirements.alignment) == VK_SUCCESS);
      struct r3v_native_image *native_image =
         r3v_native_image_from_handle(image);
      assert(native_image->zmask_layout_admitted);
      assert(native_image->committed_submission.representation ==
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
      assert(native_image->committed_submission.zmask_metadata.status ==
             R3V_NATIVE_ZMASK_METADATA_RETIRED);
      native_device->submit_hazard_accepted = true;
      if (initialize) {
         assert(r3v_native_record_zmask_initialize(command_buffer, image) ==
                VK_SUCCESS);
      } else {
         const VkImageMemoryBarrier layout_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = {
               .aspectMask =
                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
               .levelCount = 1u,
               .layerCount = 1u,
            },
         };
         vkCmdPipelineBarrier(command_buffer,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, NULL,
                              0u, NULL, 1u, &layout_barrier);
         const VkClearDepthStencilValue clear = {
            .depth = 0.25f,
            .stencil = 0xa5u,
         };
         const VkImageSubresourceRange range = {
            .aspectMask =
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
            .levelCount = 1u,
            .layerCount = 1u,
         };
         vkCmdClearDepthStencilImage(command_buffer, image,
                                     VK_IMAGE_LAYOUT_GENERAL, &clear, 1u,
                                     &range);
         if (materialize) {
            struct r3v_native_cmd_buffer *recording_command =
               r3v_native_cmd_buffer_from_handle(command_buffer);
            assert(recording_command->image_state_count == 1u);
            const struct r3v_native_cmd_image_state *recording_state =
               &recording_command->image_states[0];
            assert(recording_state->image == native_image);
            assert(recording_state->current_representation_set);
            assert(recording_state->current_representation ==
                   R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR);
            assert(recording_state->current_zmask_metadata_set);
            assert(recording_state->current_zmask_metadata.status ==
                   R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR);
            assert(r3v_native_record_zmask_materialize(
                      command_buffer, image,
                      R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR,
                      &recording_state->current_zmask_metadata) == VK_SUCCESS);
         }
      }
   } else {
      assert(r3v_native_record_zmask_ownership_only(command_buffer) ==
             VK_SUCCESS);
   }
   assert(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);

   struct r3v_native_cmd_buffer *native_command =
      r3v_native_cmd_buffer_from_handle(command_buffer);
   assert(metadata_operation ? native_command->ib_size_dwords != 0u
                             : native_command->ib_size_dwords == 0u);
   assert(native_command->reference_count ==
          (materialize ? 3u : metadata_operation ? 1u : 0u));
   assert(native_command->image_state_count ==
          (metadata_operation ? 1u : 0u));
   if (materialize) {
      struct r3v_native_image *recorded_image =
         r3v_native_image_from_handle(image);
      assert(recorded_image->committed_submission.representation ==
             R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED);
      assert(recorded_image->committed_submission.zmask_metadata.status ==
             R3V_NATIVE_ZMASK_METADATA_RETIRED);
      assert(native_device->zmask_owner.image == NULL);
      assert(native_device->zmask_owner.metadata.status ==
             R3V_NATIVE_ZMASK_METADATA_RETIRED);
   }
   const uint32_t metadata_operation_index = records_fast_clear ? 1u : 0u;
   assert(native_command->ordered_operation_count ==
          (materialize ? 3u : records_fast_clear ? 2u : 1u));
   if (records_fast_clear)
      assert(native_command->ordered_operations[0].kind ==
             R3V_NATIVE_ORDERED_OPERATION_IMAGE_BARRIER);
   assert(native_command->ordered_operations[metadata_operation_index].kind ==
          (initialize
              ? R3V_NATIVE_ORDERED_OPERATION_IMAGE_ZMASK_INITIALIZE
              : records_fast_clear
                   ? R3V_NATIVE_ORDERED_OPERATION_IMAGE_FAST_CLEAR
                   : R3V_NATIVE_ORDERED_OPERATION_HYPERZ_ACQUIRE));
   if (materialize) {
      assert(native_command->ordered_operations[2].kind ==
             R3V_NATIVE_ORDERED_OPERATION_IMAGE_MATERIALIZE);
      assert(native_command->ordered_operations[1].ib_position_dwords <
             native_command->ordered_operations[2].ib_position_dwords);
      assert(native_command->ordered_operations[2].ib_position_dwords ==
             native_command->ib_size_dwords);
   }
   if (metadata_operation) {
      char digest[2u * BLAKE3_OUT_LEN + 1u];
      r300_triangle_ib_digest_hex(native_command->ib,
                                  native_command->ib_size_dwords, digest);
      assert(setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", digest, 1) == 0);
   }

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
   assert(acquire_count() == 1u);
   assert(release_count() == (cs_refuse ? 1u : 0u));
   assert(cs_count() == (metadata_operation ? 1u : 0u));
   const bool retains_ownership =
      ownership_success || initialize_success || fast_clear_success ||
      materialize_success || completion_fail;
   assert(ownership_held() == retains_ownership);
   assert(native_command->image_state_count ==
          (metadata_operation ? 1u : 0u));
   assert(native_device->hyperz_ownership ==
          (retains_ownership ? R300_ZB_HYPERZ_OWNED
                             : R300_ZB_HYPERZ_UNOWNED));
   struct r3v_native_image *native_image =
      image != VK_NULL_HANDLE ? r3v_native_image_from_handle(image) : NULL;
   assert(native_device->zmask_owner.image ==
          ((initialize_success || fast_clear_success) ? native_image : NULL));
   assert(native_device->zmask_owner.metadata.status ==
          (initialize_success
              ? R3V_NATIVE_ZMASK_METADATA_INITIALIZED
              : fast_clear_success ? R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR
                                   : R3V_NATIVE_ZMASK_METADATA_RETIRED));
   assert(native_device->transport_cs_ioctl_count ==
          (metadata_operation ? 1u : 0u));
   assert(native_device->queue_status ==
          (ownership_success
              ? R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION
              : initialize_success || fast_clear_success || materialize_success
                   ? R3V_NATIVE_QUEUE_STATUS_COMPLETED
                   : completion_fail
                        ? R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE
                        : R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED));
   if (native_image != NULL) {
      assert(native_image->committed_submission.representation ==
             (fast_clear_success
                 ? R3V_NATIVE_IMAGE_REPRESENTATION_ZMASK_FAST_CLEAR
                 : R3V_NATIVE_IMAGE_REPRESENTATION_UNCOMPRESSED_TILED));
      assert(native_image->committed_submission.zmask_metadata.status ==
             (initialize_success
                 ? R3V_NATIVE_ZMASK_METADATA_INITIALIZED
                 : fast_clear_success ? R3V_NATIVE_ZMASK_METADATA_FAST_CLEAR
                                      : R3V_NATIVE_ZMASK_METADATA_RETIRED));
      if (fast_clear_success) {
         assert(native_image->committed_submission.zmask_metadata
                   .clear_depth_code == 0x400000u);
         assert(native_image->committed_submission.zmask_metadata
                   .clear_stencil == 0xa5u);
      }
   }

   vkDestroyCommandPool(device, command_pool, NULL);
   if (image != VK_NULL_HANDLE)
      vkDestroyImage(device, image, NULL);
   if (image_memory != VK_NULL_HANDLE)
      vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL);
   assert(acquire_count() == 1u);
   assert(release_count() ==
          ((metadata_operation || ownership_success) ? 1u : 0u));
   assert(cs_count() == (metadata_operation ? 1u : 0u));
   assert(!ownership_held());
   vkDestroyInstance(instance, NULL);
   return 0;
}
