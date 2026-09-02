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
   MUTATION_STORAGE_TEXEL_BUFFER,
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

   /* The known-bad targets VK_FORMAT_R32_UINT, an admitted texel
    * format, rather than an unadmitted one: injecting the withheld
    * VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT here is the exact
    * over-grant r3v_get_format_properties must never make (the RS480
    * die lacks the storage-texel-buffer route, tests/
    * r3v_conformance_nonpass_ledger.tsv row
    * mandatory_format_feature_absent), so this calibration observes
    * the driver's real admitted-table grant rather than a format the
    * grant never touches.
    */
   if (mutation == MUTATION_STORAGE_TEXEL_BUFFER &&
       format == VK_FORMAT_R32_UINT) {
      legacy.bufferFeatures |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
      properties2.formatProperties.bufferFeatures |=
         VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
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
   if (argc == 2 && strcmp(argv[1], "--inject-storage-texel-buffer") == 0) {
      mutation = MUTATION_STORAGE_TEXEL_BUFFER;
   } else if (argc == 2 &&
              strcmp(argv[1], "--inject-properties2-disagreement") == 0) {
      mutation = MUTATION_PROPERTIES2_DISAGREEMENT;
   } else if (argc != 1) {
      fprintf(stderr,
              "usage: %s [--inject-storage-texel-buffer | "
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
      /* The transfer family's texel table grants the two copy bits on both
       * the linear and the optimal layout through both
       * queries: r3v_CreateImage executes VK_IMAGE_TILING_OPTIMAL over
       * the transfer family as the identical linear span
       * (r3v_native_transfer_footprint_bytes), so the format-property
       * grant matches what vkCreateImage actually admits. The required
       * optimal-tiling features these formats lack (sampled, color
       * attachment, blit, storage) are a recorded conformance deviation
       * and stay ungranted until their routes execute.  The
       * two UNORM8 formats the render-shape cell places into a target
       * leave this list: each names one US_OUT_FMT_0 lane order, so
       * both carry the color-attachment bit on both layouts.
       */
      static const VkFormat transfer_formats[] = {
         VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R16G16B16A16_UINT,
         VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32_UINT,
      };
      const VkFormatFeatureFlags transfer_bits =
         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
      for (unsigned i = 0;
           i < sizeof(transfer_formats) / sizeof(transfer_formats[0]); i++) {
         VkFormatProperties legacy;
         legacy_query(physical_device, transfer_formats[i], &legacy);
         VkFormatProperties2KHR properties2 = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2_KHR,
         };
         properties2_query(physical_device, transfer_formats[i],
                           &properties2);
         CHECK(legacy.linearTilingFeatures == transfer_bits &&
                  legacy.optimalTilingFeatures == transfer_bits &&
                  properties2.formatProperties.linearTilingFeatures ==
                     legacy.linearTilingFeatures &&
                  properties2.formatProperties.optimalTilingFeatures ==
                     legacy.optimalTilingFeatures,
               "transfer-family texel format %u grants exactly the two copy "
               "bits on the linear and optimal layout, nothing "
               "more (linear 0x%08x optimal 0x%08x buffer 0x%08x)",
               transfer_formats[i], legacy.linearTilingFeatures,
               legacy.optimalTilingFeatures, legacy.bufferFeatures);
      }
      /* r3v_CreateBufferView (r3v_native_object.c) queries
       * r3v_get_format_properties directly for its admission gate, so
       * an exact bufferFeatures match here on every admitted texel
       * format observes that gate rather than the transfer-bit
       * agreement above alone: each of these six formats grants
       * VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, and the two render
       * lane orders grant the vertex-buffer bit beside it, since
       * attribute_format_id maps each to an r300_vertex_format_id the
       * host gather decodes.  The storage-texel-buffer bit stays
       * withheld on every format (tests/r3v_conformance_nonpass_ledger.tsv
       * row mandatory_format_feature_absent).
       */
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_B8G8R8A8_UNORM,
                            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                               VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
                            "B8G8R8A8_UNORM texel buffer and vertex fetch");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R8G8B8A8_UNORM,
                            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT |
                               VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
                            "R8G8B8A8_UNORM texel buffer and vertex fetch");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R8G8B8A8_UINT,
                            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
                            "R8G8B8A8_UINT texel buffer");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R16G16B16A16_UINT,
                            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
                            "R16G16B16A16_UINT texel buffer");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R32G32B32A32_UINT,
                            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
                            "R32G32B32A32_UINT texel buffer");
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R32_UINT,
                            VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT,
                            "R32_UINT texel buffer");
      /* The two render-family lane orders carry the color-attachment
       * grant plus the transfer grants, identically on
       * both layouts: r3v_CreateImage executes VK_IMAGE_TILING_OPTIMAL
       * as the one linear span for both usages, so the two tiling
       * grants are equal.
       */
      static const VkFormat render_formats[] = {
         VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
      };
      for (unsigned i = 0;
           i < sizeof(render_formats) / sizeof(render_formats[0]); i++) {
         const VkFormatFeatureFlags render_bits =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | transfer_bits;
         VkFormatProperties legacy;
         legacy_query(physical_device, render_formats[i], &legacy);
         CHECK((legacy.linearTilingFeatures & render_bits) == render_bits &&
                  legacy.optimalTilingFeatures == legacy.linearTilingFeatures &&
                  (legacy.linearTilingFeatures &
                   (VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                    VK_FORMAT_FEATURE_BLIT_DST_BIT)) == 0,
               "render-family format %u grants color attachment and the "
               "transfer bits without blit bits identically on both layouts "
               "(linear "
               "0x%08x optimal 0x%08x)",
               (unsigned)render_formats[i], legacy.linearTilingFeatures,
               legacy.optimalTilingFeatures);
      }
      /* R8_UNORM joins the vertex-buffer grant: it is a mandatory
       * vertex format whose UNORM8 gather decodes one component per
       * record.  It carries no texel-buffer bit, so the two grants
       * stay separable.
       */
      check_format_features(physical_device, legacy_query, properties2_query,
                            VK_FORMAT_R8_UNORM,
                            VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT,
                            "R8_UNORM vertex fetch");
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
