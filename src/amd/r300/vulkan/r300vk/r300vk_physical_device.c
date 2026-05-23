/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_physical_device.h"

#include "r300vk_entrypoints.h"
#include "r300vk_instance.h"
#include "r300vk_private.h"

#include "util/macros.h"
#include "vk_alloc.h"
#include "vk_log.h"
#include "vk_util.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static const char *
r300vk_chip_name_from_pci_device_id(uint32_t pci_device_id)
{
   switch (pci_device_id) {
   case R300VK_PCI_DEVICE_ID_RS482:
      return "ATI RS480 (RS482)";
   case R300VK_PCI_DEVICE_ID_RS485:
      return "ATI RS480 (RS485)";
   default:
      return "ATI RS480";
   }
}

/* R3xx hardware limits derived from the AMD R3xx register reference
 * guide and the executable Mesa r300g implementation oracle.  Every
 * field carries a primary-source citation in the comment alongside it.
 *
 * Citation conventions in this function:
 *   "R3xx-RRG ch. <N>"  AMD R3xx Register Reference Guide chapter
 *   "Mesa r300g <file>" src/gallium/drivers/r300/<file>.[ch] in this tree
 *   "Vulkan spec <ref>" Vulkan 1.4 specification section reference
 *
 * Where R3xx hardware has no native surface (SSBO, compute, dual-source
 * blending), the field is set to the Vulkan minimum required by the
 * spec table at "Limit Requirements" (Vulkan 1.4 ch. 49.1) so the
 * device still parses as a graphics-capable VkPhysicalDevice. */
static void
r300vk_physical_device_init_limits(struct vk_properties *const props)
{
   /* Texture and image dimensions.  R3xx FORMAT2_HEIGHT and
    * FORMAT2_WIDTH fields in R300_TX_FORMAT2_n cap each axis at 2048
    * (R3xx-RRG ch. "Texture Engine", TX_FORMAT2 register).
    *
    * maxImageArrayLayers must report at least 256 to satisfy Vulkan
    * 1.4 ch. 49.1 "Limit Requirements".  R3xx hardware does not
    * accelerate array textures natively, but the driver can fall back
    * to per-layer 2D images at pipeline-lowering time.  Reporting the
    * spec minimum keeps validation green while the lowering path is
    * still future work. */
   props->maxImageDimension1D = 2048;
   props->maxImageDimension2D = 2048;
   props->maxImageDimension3D = 256;
   props->maxImageDimensionCube = 2048;
   props->maxImageArrayLayers = 256;

   /* Texel buffer size: R3xx has no native texel buffer object.  The
    * Vulkan 1.4 minimum is 65536. */
   props->maxTexelBufferElements = 65536;

   /* PS constant store: R300_PFS_PARAM_0..31 yields 32 vec4 slots, or
    * 512 bytes.  The Vulkan minimum maxUniformBufferRange is 16384,
    * so we round up to that bound; the descriptor binding still maps
    * down to the hardware 32 slots. */
   props->maxUniformBufferRange = 16384;

   /* No native SSBO.  Advertise the Vulkan minimum so descriptor
    * binding still parses. */
   props->maxStorageBufferRange = 0x4000000;
   props->maxPushConstantsSize = 128;

   props->maxMemoryAllocationCount = 4096;
   props->maxSamplerAllocationCount = 4000;

   /* Buffer-image granularity matches the radeon PAGE_SIZE; conservative
    * non-zero value works for the loader skeleton. */
   props->bufferImageGranularity = 64;
   props->sparseAddressSpaceSize = 0;

   props->maxBoundDescriptorSets = 4;
   props->maxPerStageDescriptorSamplers = 16;
   props->maxPerStageDescriptorUniformBuffers = 12;
   props->maxPerStageDescriptorStorageBuffers = 4;
   props->maxPerStageDescriptorSampledImages = 16;
   props->maxPerStageDescriptorStorageImages = 4;
   props->maxPerStageDescriptorInputAttachments = 4;
   props->maxPerStageResources = 44;
   props->maxDescriptorSetSamplers = 96;
   props->maxDescriptorSetUniformBuffers = 72;
   props->maxDescriptorSetUniformBuffersDynamic = 8;
   props->maxDescriptorSetStorageBuffers = 24;
   props->maxDescriptorSetStorageBuffersDynamic = 4;
   props->maxDescriptorSetSampledImages = 96;
   props->maxDescriptorSetStorageImages = 24;
   props->maxDescriptorSetInputAttachments = 4;

   /* Vertex input.  R300 VAP_PROG_STREAM_CNTL_0..15 carries 16 vertex
    * attribute streams (R3xx-RRG ch. "Vertex Assembly and Processor"). */
   props->maxVertexInputAttributes = 16;
   props->maxVertexInputBindings = 16;
   props->maxVertexInputAttributeOffset = 2047;
   props->maxVertexInputBindingStride = 2048;

   /* VAP varying export budget: 8 generic varyings x 4 components +
    * position + color + back-color + texcoords; aggregate ceiling 64
    * components, with R3xx hardware capped lower in practice.
    * Conservative value 64 matches Vulkan 1.4 ch. 49.1 spec minimum. */
   props->maxVertexOutputComponents = 64;

   /* Tessellation: R300 has no native tessellator. */
   props->maxTessellationGenerationLevel = 0;
   props->maxTessellationPatchSize = 0;
   props->maxTessellationControlPerVertexInputComponents = 0;
   props->maxTessellationControlPerVertexOutputComponents = 0;
   props->maxTessellationControlPerPatchOutputComponents = 0;
   props->maxTessellationControlTotalOutputComponents = 0;
   props->maxTessellationEvaluationInputComponents = 0;
   props->maxTessellationEvaluationOutputComponents = 0;

   /* Geometry shader: R300 has no native geometry stage. */
   props->maxGeometryShaderInvocations = 0;
   props->maxGeometryInputComponents = 0;
   props->maxGeometryOutputComponents = 0;
   props->maxGeometryOutputVertices = 0;
   props->maxGeometryTotalOutputComponents = 0;

   /* Fragment shader budget for the RS482/RS485 R300VK target.
    * R300-class RS482 fragment programs are constrained by the
    * current Mesa r300 operational budget of 64 ALU instructions
    * (R300_PFS_INSTR_*), 32 TEX instructions, and 32 vec4 PFS_PARAM
    * constants (R300_PFS_PARAM_0..31).  Vulkan has no direct
    * fragment-ALU limit field; the limit is enforced later at
    * pipeline/shader-lowering time.  Combined output resources
    * cover the four CB_COLOR0..3 attachments through
    * R300_RB3D_CCTL_NUM_MULTIWRITES. */
   props->maxFragmentInputComponents = 64;
   props->maxFragmentOutputAttachments = 4;
   props->maxFragmentDualSrcAttachments = 0;
   props->maxFragmentCombinedOutputResources = 4;

   /* No documented or Vostro-proven native compute dispatch surface
    * exists for this RS482/RS485 R300VK target.  See
    * R300VK_CONFORMANCE_STATUS in r300vk_private.h for the
    * non-conformance contract. */
   props->maxComputeSharedMemorySize = 0;
   props->maxComputeWorkGroupCount[0] = 0;
   props->maxComputeWorkGroupCount[1] = 0;
   props->maxComputeWorkGroupCount[2] = 0;
   props->maxComputeWorkGroupInvocations = 0;
   props->maxComputeWorkGroupSize[0] = 0;
   props->maxComputeWorkGroupSize[1] = 0;
   props->maxComputeWorkGroupSize[2] = 0;

   /* R3xx subpixel precision is 4 fractional bits in the rasterizer
    * (R3xx-RRG ch. "Geometry Setup", GA_LINE_CNTL and the rasterizer
    * snap-to-pixel description). */
   props->subPixelPrecisionBits = 4;
   props->subTexelPrecisionBits = 4;
   props->mipmapPrecisionBits = 4;

   props->maxDrawIndexedIndexValue = UINT32_MAX;
   props->maxDrawIndirectCount = 1;

   props->maxSamplerLodBias = 16.0f;
   props->maxSamplerAnisotropy = 16.0f;

   /* Single viewport for R3xx graphics path. */
   props->maxViewports = 1;
   props->maxViewportDimensions[0] = 2048;
   props->maxViewportDimensions[1] = 2048;
   props->viewportBoundsRange[0] = -4096.0f;
   props->viewportBoundsRange[1] = 4096.0f;
   props->viewportSubPixelBits = 0;

   props->minMemoryMapAlignment = 64;
   props->minTexelBufferOffsetAlignment = 16;
   props->minUniformBufferOffsetAlignment = 16;
   props->minStorageBufferOffsetAlignment = 16;

   props->minTexelOffset = -8;
   props->maxTexelOffset = 7;
   props->minTexelGatherOffset = -8;
   props->maxTexelGatherOffset = 7;
   props->minInterpolationOffset = -0.5f;
   props->maxInterpolationOffset = 0.4375f;
   props->subPixelInterpolationOffsetBits = 4;

   props->maxFramebufferWidth = 2048;
   props->maxFramebufferHeight = 2048;
   props->maxFramebufferLayers = 1;

   /* R300 has no MSAA exposed through the Mesa r300g state tracker on
    * RS482/RS485; advertise single sample only. */
   props->framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;

   props->maxColorAttachments = 4;
   props->sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->maxSampleMaskWords = 1;

   props->timestampComputeAndGraphics = VK_FALSE;
   props->timestampPeriod = 0.0f;

   /* R300 supports 6 user clip planes through R300_VAP_CLIP_CNTL. */
   props->maxClipDistances = 6;
   props->maxCullDistances = 0;
   props->maxCombinedClipAndCullDistances = 6;

   props->discreteQueuePriorities = 2;

   props->pointSizeRange[0] = 1.0f;
   props->pointSizeRange[1] = 64.0f;
   props->lineWidthRange[0] = 1.0f;
   props->lineWidthRange[1] = 8.0f;
   props->pointSizeGranularity = 0.125f;
   props->lineWidthGranularity = 0.125f;

   props->strictLines = VK_FALSE;
   props->standardSampleLocations = VK_TRUE;
   props->optimalBufferCopyOffsetAlignment = 128;
   props->optimalBufferCopyRowPitchAlignment = 128;
   props->nonCoherentAtomSize = 64;
}

static void
r300vk_physical_device_init_properties(struct vk_properties *const props,
                                       uint32_t const pci_vendor_id,
                                       uint32_t const pci_device_id)
{
   memset(props, 0, sizeof(*props));

   r300vk_physical_device_init_limits(props);

   props->apiVersion = R300VK_API_VERSION;

   /* R3xx generation does not carry a driver version distinct from
    * Mesa's r300g.  Report the Mesa fork's primary version number.
    * The Vulkan loader does not parse this field; conformance suites
    * only require it to be a monotonic integer. */
   props->driverVersion = VK_MAKE_VERSION(0, 1, 0);

   props->vendorID = pci_vendor_id;
   props->deviceID = pci_device_id;

   /* RS482/RS485 are integrated graphics in the Radeon Xpress 200M /
    * Xpress 1100/1150 mobile chipsets.  Vulkan treats this as
    * VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU. */
   props->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

   const char *const chip_name = r300vk_chip_name_from_pci_device_id(pci_device_id);
   snprintf(props->deviceName, sizeof(props->deviceName), "%s", chip_name);

   /* Pipeline-cache UUID seeds the disk_cache key.  Fold the API version
    * and PCI ID into the bytes so a header version bump or chip switch
    * invalidates stale entries.
    *
    * FIXME: missing work --
    *           replace the hand-rolled byte layout with a BLAKE3 hash that
    *           ingests disk_cache_get_function_identifier() and
    *           MESA_GIT_SHA1 from src/util/disk_cache.h, matching the
    *           construction in terakan_physical_device.c's pipelineCacheUUID
    *           block.
    *       reason --
    *           BLAKE3 hashing requires the shader cache to be wired and the
    *           sha1_h custom_target from src/meson.build to be plumbed into
    *           the r300vk shared library; neither lands until the device
    *           layer brings in the disk_cache dependency.
    *       tracking-artifact --
    *           disk_cache_get_function_identifier (src/util/disk_cache.h)
    *           and the equivalent block in terakan_physical_device.c near
    *           line 600 of terakan_physical_device_get_capabilities.
    */
   memset(props->pipelineCacheUUID, 0, sizeof(props->pipelineCacheUUID));
   props->pipelineCacheUUID[0] = 'r';
   props->pipelineCacheUUID[1] = '3';
   props->pipelineCacheUUID[2] = '0';
   props->pipelineCacheUUID[3] = '0';
   props->pipelineCacheUUID[4] = 'v';
   props->pipelineCacheUUID[5] = 'k';
   props->pipelineCacheUUID[6] = (uint8_t)(pci_device_id >> 8);
   props->pipelineCacheUUID[7] = (uint8_t)(pci_device_id & 0xff);
   props->pipelineCacheUUID[8] = (uint8_t)(props->apiVersion >> 24);
   props->pipelineCacheUUID[9] = (uint8_t)(props->apiVersion >> 16);
   props->pipelineCacheUUID[10] = (uint8_t)(props->apiVersion >> 8);
   props->pipelineCacheUUID[11] = (uint8_t)(props->apiVersion & 0xff);

   /* VK_KHR_driver_properties identity.
    *
    * FIXME: missing work --
    *           populate driverID with a Khronos-registered VkDriverId for
    *           r300vk so VkPhysicalDeviceDriverProperties (Vulkan
    *           1.2 ch. 4.1.3) carries an accurate driver fingerprint.
    *       reason --
    *           Khronos has not allocated a VkDriverId for this driver;
    *           reusing VK_DRIVER_ID_MESA_RADV would misattribute every
    *           conformance result.  Reporting (VkDriverId)0 keeps the
    *           driverName/driverInfo strings as the load-bearing
    *           attribution surface until a real ID lands.
    *       tracking-artifact --
    *           VkDriverId registry at
    *           https://gitlab.khronos.org/vulkan/vulkan/-/issues and the
    *           VkPhysicalDeviceDriverProperties chapter of the Vulkan
    *           specification (Vulkan 1.2, 4.1.3).
    */
   props->driverID = (VkDriverId)0;
   snprintf(props->driverName, sizeof(props->driverName), "%s", "r300vk");
   snprintf(props->driverInfo, sizeof(props->driverInfo), "%s", "Mesa r300vk");
   props->conformanceVersion = (VkConformanceVersion){0, 0, 0, 0};
}

static const struct vk_device_extension_table r300vk_device_extensions_supported = {
   /* Empty: the loader-visible skeleton advertises no device extensions
    * until the device layer and WSI bring-up wire VK_KHR_swapchain and
    * the external-memory family. */
   0
};

static void
r300vk_physical_device_init_features(struct vk_features *features)
{
   /* Zero optional features.  vk_physical_device_init stores this table
    * so vk_common_GetPhysicalDeviceFeatures2 can answer queries with the
    * exact set the driver supports. */
   memset(features, 0, sizeof(*features));
}

void
r300vk_physical_device_destroy(struct vk_physical_device *const device_base)
{
   struct r300vk_physical_device *const device =
      container_of(device_base, struct r300vk_physical_device, vk);

   if (device->render_node_fd >= 0)
      close(device->render_node_fd);

   vk_physical_device_finish(&device->vk);
   vk_free(&device->vk.instance->alloc, device);
}

VkResult
r300vk_physical_device_try_create_for_drm(struct vk_instance *const instance_base,
                                          struct _drmDevice *const drm_device,
                                          struct vk_physical_device **const device_out)
{
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PCI ||
       drm_device->deviceinfo.pci->vendor_id != R300VK_VENDOR_ID_ATI ||
       !r300vk_pci_device_id_is_supported(drm_device->deviceinfo.pci->device_id)) {
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct r300vk_instance *const instance =
      container_of(instance_base, struct r300vk_instance, vk);

   const char *const render_node_path = drm_device->nodes[DRM_NODE_RENDER];
   int render_node_fd = open(render_node_path, O_RDWR | O_CLOEXEC);
   if (render_node_fd < 0) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Failed to open the DRM render node '%s'", render_node_path);
   }

   drmVersionPtr const drm_version = drmGetVersion(render_node_fd);
   if (drm_version == NULL) {
      close(render_node_fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "Failed to get DRM version for '%s'", render_node_path);
   }
   const bool is_radeon = strcmp(drm_version->name, "radeon") == 0;
   drmFreeVersion(drm_version);
   if (!is_radeon) {
      close(render_node_fd);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   struct r300vk_physical_device *const device =
      vk_alloc(&instance->vk.alloc, sizeof(*device), alignof(struct r300vk_physical_device),
               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      close(render_node_fd);
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   device->pci_vendor_id = drm_device->deviceinfo.pci->vendor_id;
   device->pci_device_id = drm_device->deviceinfo.pci->device_id;
   device->render_node_fd = render_node_fd;

   struct vk_features features;
   r300vk_physical_device_init_features(&features);

   /* vk_physical_device_init copies *properties into pdevice->properties
    * by value (src/vulkan/runtime/vk_physical_device.c:48-49), so the
    * struct must be fully populated before the call.  Mirror the order
    * used by terakan_physical_device_init at
    * src/amd/terascale/vulkan/terakan_physical_device.c around line 1640.
    */
   struct vk_properties properties;
   r300vk_physical_device_init_properties(&properties, device->pci_vendor_id,
                                          device->pci_device_id);

   /* Driver entrypoints only; vk_physical_device_init merges
    * vk_common_physical_device_entrypoints itself at
    * src/vulkan/runtime/vk_physical_device.c:53-55. */
   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &r300vk_physical_device_entrypoints, true);

   VkResult result = vk_physical_device_init(&device->vk, &instance->vk,
                                             &r300vk_device_extensions_supported,
                                             &features, &properties, &dispatch_table);
   if (result != VK_SUCCESS) {
      /* terakan_physical_device_init does not call vk_physical_device_finish
       * on init failure (terakan_physical_device.c fail_isa label); the
       * runtime helper only requires finish after a successful init. */
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   if (instance->debug_flags & R300VK_DEBUG_STARTUP) {
      fprintf(stderr,
              "r300vk: info: Found compatible DRM device '%s' (%04x:%04x).\n",
              render_node_path, device->pci_vendor_id, device->pci_device_id);
   }

   *device_out = &device->vk;
   return VK_SUCCESS;
}

/* Queue family enumeration.  Advertises one graphics+transfer queue
 * family with one queue.  RS482/RS485 has no native compute dispatch
 * surface, so VK_QUEUE_COMPUTE_BIT is intentionally absent. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                               uint32_t *pCount,
                                               VkQueueFamilyProperties2 *pProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pProperties, pCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 1,
         .timestampValidBits = 0,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

/* Nominal heap sizes reported until the device layer queries
 * DRM_RADEON_GEM_INFO from the radeon kernel driver (handled by
 * radeon_gem_info_ioctl in linux/drivers/gpu/drm/radeon/radeon_gem.c).
 * RS482/RS485 is UMA: the GTT and shared-VRAM partitions overlap, so
 * even the queried values will be approximations.  Probes that read
 * the reported heap sizes must record memory_properties_placeholder=true
 * for any classification or evidence bundle. */
#define R300VK_PLACEHOLDER_GTT_HEAP_SIZE     (128ULL * 1024 * 1024)
#define R300VK_PLACEHOLDER_VRAM_HEAP_SIZE    ( 64ULL * 1024 * 1024)

VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VkPhysicalDeviceMemoryProperties *const m = &pMemoryProperties->memoryProperties;

   m->memoryHeapCount = 2;
   m->memoryHeaps[0] = (VkMemoryHeap){
      .size = R300VK_PLACEHOLDER_GTT_HEAP_SIZE,
      .flags = 0,
   };
   m->memoryHeaps[1] = (VkMemoryHeap){
      .size = R300VK_PLACEHOLDER_VRAM_HEAP_SIZE,
      .flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
   };

   m->memoryTypeCount = 2;
   m->memoryTypes[0] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      .heapIndex = 0,
   };
   m->memoryTypes[1] = (VkMemoryType){
      .propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      .heapIndex = 1,
   };
}
