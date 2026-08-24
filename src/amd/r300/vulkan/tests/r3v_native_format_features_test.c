/*
 * SPDX-License-Identifier: MIT
 *
 * Native R3V format-feature query fixture.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

static unsigned failures;

enum mutation_mode {
   MUTATION_NONE,
   MUTATION_UNIFORM_TEXEL_BUFFER,
   MUTATION_PROPERTIES2_DISAGREEMENT,
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

static void
check_format_features(
   VkPhysicalDevice physical_device,
   PFN_vkGetPhysicalDeviceFormatProperties legacy_query,
   PFN_vkGetPhysicalDeviceFormatProperties2KHR properties2_query,
   VkFormat format, VkFormatFeatureFlags expected_buffer_features,
   const char *label)
{
   VkFormatProperties legacy = { 0 };
   legacy_query(physical_device, format, &legacy);

   VkFormatProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
   };
   properties2_query(physical_device, format, &properties2);

   if (mutation == MUTATION_UNIFORM_TEXEL_BUFFER &&
       format == VK_FORMAT_R32_SFLOAT) {
      legacy.bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
      properties2.formatProperties.bufferFeatures |=
         VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
   } else if (mutation == MUTATION_PROPERTIES2_DISAGREEMENT &&
              format == VK_FORMAT_R32_SFLOAT) {
      properties2.formatProperties.bufferFeatures |=
         VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
   }

   CHECK(legacy.linearTilingFeatures ==
            properties2.formatProperties.linearTilingFeatures &&
            legacy.optimalTilingFeatures ==
               properties2.formatProperties.optimalTilingFeatures &&
            legacy.bufferFeatures ==
               properties2.formatProperties.bufferFeatures,
         "%s: the core and properties2 legacy views disagree", label);
   CHECK(legacy.bufferFeatures == expected_buffer_features,
         "%s: buffer features 0x%08" PRIx32 ", expected 0x%08" PRIx32,
         label, (uint32_t)legacy.bufferFeatures,
         (uint32_t)expected_buffer_features);
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--inject-uniform-texel-buffer") == 0) {
      mutation = MUTATION_UNIFORM_TEXEL_BUFFER;
   } else if (argc == 2 &&
              strcmp(argv[1], "--inject-properties2-disagreement") == 0) {
      mutation = MUTATION_PROPERTIES2_DISAGREEMENT;
   } else if (argc != 1) {
      fprintf(stderr,
              "usage: %s [--inject-uniform-texel-buffer | "
              "--inject-properties2-disagreement]\n",
              argv[0]);
      return 2;
   }

   const char *const extensions[] = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
   };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo =
            &(VkApplicationInfo){
               .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
               .apiVersion = VK_API_VERSION_1_0,
            },
         .enabledExtensionCount = 2,
         .ppEnabledExtensionNames = extensions,
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS && instance != VK_NULL_HANDLE,
         "native instance creation returned %d", result);
   if (instance == VK_NULL_HANDLE)
      return 1;

   uint32_t physical_device_count = 1;
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &physical_device_count,
                                       &physical_device);
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
            physical_device_count == 1 &&
            physical_device != VK_NULL_HANDLE,
         "native physical-device enumeration returned %d count %u", result,
         physical_device_count);
   if (physical_device == VK_NULL_HANDLE) {
      vkDestroyInstance(instance, NULL);
      return 1;
   }

   /* VK_KHR_external_memory_capabilities executes as the zeroed answer:
    * no handle type is exportable or importable, so every queried
    * handle type reports empty external-memory properties.
    */
   PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR external_query =
      (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR)
         vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
   CHECK(external_query != NULL,
         "the loader resolves vkGetPhysicalDeviceExternalBufferPropertiesKHR");
   if (external_query != NULL) {
      const VkExternalMemoryHandleTypeFlagBits handle_types[] = {
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
         VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      for (unsigned i = 0; i < 2; i++) {
         VkExternalBufferPropertiesKHR props = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES_KHR,
         };
         external_query(
            physical_device,
            &(VkPhysicalDeviceExternalBufferInfoKHR){
               .sType =
                  VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO_KHR,
               .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               .handleType = handle_types[i],
            },
            &props);
         CHECK(props.externalMemoryProperties.externalMemoryFeatures == 0 &&
                  props.externalMemoryProperties.compatibleHandleTypes == 0 &&
                  props.externalMemoryProperties
                        .exportFromImportedHandleTypes == 0,
               "external buffer properties for handle type 0x%x are zeroed",
               handle_types[i]);
      }
   }

   PFN_vkGetPhysicalDeviceFormatProperties legacy_query =
      (PFN_vkGetPhysicalDeviceFormatProperties)vkGetInstanceProcAddr(
         instance, "vkGetPhysicalDeviceFormatProperties");
   PFN_vkGetPhysicalDeviceFormatProperties2KHR properties2_query =
      (PFN_vkGetPhysicalDeviceFormatProperties2KHR)vkGetInstanceProcAddr(
         instance, "vkGetPhysicalDeviceFormatProperties2KHR");
   CHECK(legacy_query != NULL && properties2_query != NULL,
         "the loader resolves both native format-property queries");
   if (legacy_query != NULL && properties2_query != NULL) {
      check_format_features(
         physical_device, legacy_query, properties2_query,
         VK_FORMAT_R32_SFLOAT, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
         "R32_SFLOAT vertex fetch");
      /* The transfer family's texel table grants the two transfer bits
       * on the linear layout through both queries; the required
       * optimal-tiling features of these formats (sampled, color
       * attachment, blit, storage) are a recorded conformance deviation
       * and stay ungranted until their routes execute.
       */
      static const VkFormat transfer_formats[] = {
         VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT,
         VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R32G32B32A32_UINT,
         VK_FORMAT_R32_UINT,
      };
      for (unsigned i = 0;
           i < sizeof(transfer_formats) / sizeof(transfer_formats[0]); i++) {
         VkFormatProperties legacy;
         legacy_query(physical_device, transfer_formats[i], &legacy);
         VkFormatProperties2KHR properties2 = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
         };
         properties2_query(physical_device, transfer_formats[i],
                           &properties2);
         CHECK((legacy.linearTilingFeatures &
                (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) ==
                     (VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT) &&
                  properties2.formatProperties.linearTilingFeatures ==
                     legacy.linearTilingFeatures,
               "transfer-family texel format %u grants the two transfer "
               "bits on the linear layout (linear 0x%08x optimal "
               "0x%08x buffer 0x%08x)",
               transfer_formats[i], legacy.linearTilingFeatures,
               legacy.optimalTilingFeatures, legacy.bufferFeatures);
      }
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R8_UNORM, 0,
                            "R8_UNORM non-fetchable buffer");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_D16_UNORM, 0,
                            "D16_UNORM depth buffer");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R8G8B8A8_SRGB, 0,
                            "R8G8B8A8_SRGB buffer");
   }

   /* Every image and pipeline admission executes single-sample alone,
    * so each advertised sample-count limit is the one truthful bit.
    */
   {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(physical_device, &properties);
      const VkSampleCountFlags counts[] = {
         properties.limits.framebufferColorSampleCounts,
         properties.limits.framebufferDepthSampleCounts,
         properties.limits.framebufferStencilSampleCounts,
         properties.limits.framebufferNoAttachmentsSampleCounts,
         properties.limits.sampledImageColorSampleCounts,
         properties.limits.sampledImageIntegerSampleCounts,
         properties.limits.sampledImageDepthSampleCounts,
         properties.limits.sampledImageStencilSampleCounts,
         properties.limits.storageImageSampleCounts,
      };
      for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
         CHECK(counts[i] == VK_SAMPLE_COUNT_1_BIT,
               "sample-count limit %u advertises the single sample "
               "(0x%x)", i, counts[i]);
   }

   vkDestroyInstance(instance, NULL);
   if (failures != 0) {
      fprintf(stderr, "FAILED: %u check(s)\n", failures);
      return 1;
   }

   puts("OK");
   return 0;
}
