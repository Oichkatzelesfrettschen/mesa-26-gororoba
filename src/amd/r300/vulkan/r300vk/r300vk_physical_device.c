/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_physical_device.h"
#include "r300vk_cpu_sync.h"

#include "r300vk_entrypoints.h"
#include "r300vk_instance.h"
#include "r300vk_private.h"

#include "pipe/p_defines.h"
#include "util/disk_cache.h"
#include "util/format/u_format.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "vk_alloc.h"
#include "vk_enum_defines.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

#ifdef R300VK_GALLIUM_BACKEND
#include "pipe/p_screen.h"
#include "r300/r300_public.h"
#include "r300/r300_screen.h"
#include "winsys/radeon_winsys.h"
#endif

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

static bool
r300vk_hybrid_compute_enabled(void)
{
   const char *gate = getenv(R300VK_HYBRID_COMPUTE_ENV);
   return gate && strcmp(gate, R300VK_HYBRID_COMPUTE_ENV_VALUE) == 0;
}

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

/* R3xx hardware limits are split between the Vulkan 1.0 physical-device
 * minimums the ICD must advertise and the smaller RS482 execution caps that
 * the resource paths tile or reject at creation time.  The executable Mesa
 * r300g oracle is the execution cap; the Vulkan limit table is the API floor.
 *
 * Citation conventions in this function:
 *   "R3xx-RRG ch. <N>"  AMD R3xx Register Reference Guide chapter
 *   "Mesa r300g <file>" src/gallium/drivers/r300/<file>.[ch] in this tree
 *   "Vulkan spec <ref>" Vulkan 1.4 specification section reference
 *
 * Where the RS482 path has no single native 4096-wide render surface, r300vk
 * presents the Vulkan floor through a 2560 hardware-backed span plus a residual
 * span.  Native r300g resources remain the fast path for images that fit in
 * one span. */
static void
r300vk_physical_device_init_limits(struct vk_properties *const props,
                                   uint64_t const gart_size_kb)
{
   /* Texture and image dimensions.  The RS482 render path accepts a 2560-wide
    * hardware span; r300vk composes the Vulkan 4096 floor from that fast path
    * plus a residual span when an image exceeds the single-span limit. */
   props->maxImageDimension1D = R300VK_VK10_MIN_IMAGE_DIMENSION_1D;
   props->maxImageDimension2D = R300VK_VK10_MIN_IMAGE_DIMENSION_2D;
   props->maxImageDimension3D = 256;
   props->maxImageDimensionCube = R300VK_VK10_MIN_IMAGE_DIMENSION_CUBE;
   props->maxImageArrayLayers = 256;

   /* Texel buffer size: R3xx has no native texel buffer object.  The
    * Vulkan 1.4 minimum is 65536. */
   props->maxTexelBufferElements = 65536;

   /* PS constant store: R300_PFS_PARAM_0..31 yields 32 vec4 slots, or
    * 512 bytes.  The Vulkan minimum maxUniformBufferRange is 16384,
    * so we round up to that bound; the descriptor binding still maps
    * down to the hardware 32 slots. */
   props->maxUniformBufferRange = 16384;

   /* SSBO size advertise.  R3xx has no native SSBO; the compute-as-raster
    * substrate maps stores to RB3D color export backed by the radeon GART.
    * Cap the advertised maxStorageBufferRange to 512 MB only when the
    * kernel-reported GART (info.gart_size_kb) is >= 1 GB; otherwise
    * advertise the Vulkan minimum.  Mirrors r300_screen.c's
    * max_shader_buffer_size gate. */
   if (gart_size_kb >= 1024u * 1024u)
      props->maxStorageBufferRange = 512u * 1024u * 1024u; /* 512 MB */
   else
      props->maxStorageBufferRange = R300VK_VK10_MIN_STORAGE_BUFFER_RANGE;
   /* TODO: elevate VkPhysicalDeviceVulkan13Properties.maxBufferSize and
    *       VkPhysicalDeviceMaintenance4Properties.maxBufferSize in
    *       lock-step with maxStorageBufferRange.  Vulkan spec 47.78
    *       requires maxBufferSize >= maxStorageBufferRange whenever the
    *       Vulkan 1.3 properties chain or VK_KHR_maintenance4 is
    *       advertised.
    *       reason -- r300vk currently advertises API version 1.0
    *       (R300VK_API_VERSION = VK_MAKE_API_VERSION(0, 1, 0, ...) in
    *       r300vk_private.h) and does not yet expose maintenance4; the
    *       maxBufferSize field lives in a properties chain the loader
    *       does not query at the 1.0 advertise level, so populating it
    *       would write a value the application cannot reach.
    *       tracking -- VkPhysicalDeviceMaintenance4Properties .maxBufferSize
    *       (header vulkan_core.h field, lock-step elevation lands with the
    *       r300vk maintenance4 advertise). */
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

   props->maxComputeSharedMemorySize =
      R300VK_VK10_MIN_COMPUTE_SHARED_MEMORY_SIZE;
   props->maxComputeWorkGroupCount[0] = 65535;
   props->maxComputeWorkGroupCount[1] = 65535;
   props->maxComputeWorkGroupCount[2] = 65535;
   props->maxComputeWorkGroupInvocations =
      R300VK_VK10_MIN_COMPUTE_WORKGROUP_INVOCATIONS;
   props->maxComputeWorkGroupSize[0] = R300VK_VK10_MIN_COMPUTE_WORKGROUP_SIZE_X;
   props->maxComputeWorkGroupSize[1] = R300VK_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Y;
   props->maxComputeWorkGroupSize[2] = R300VK_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Z;

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
   props->maxViewportDimensions[0] = R300VK_VK10_MIN_VIEWPORT_DIMENSION;
   props->maxViewportDimensions[1] = R300VK_VK10_MIN_VIEWPORT_DIMENSION;
   props->viewportBoundsRange[0] = -8192.0f;
   props->viewportBoundsRange[1] = 8191.0f;
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

   props->maxFramebufferWidth = R300VK_VK10_MIN_FRAMEBUFFER_DIMENSION;
   props->maxFramebufferHeight = R300VK_VK10_MIN_FRAMEBUFFER_DIMENSION;
   props->maxFramebufferLayers = 1;

   props->framebufferColorSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferDepthSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferStencilSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferNoAttachmentsSampleCounts =
      R300VK_VK10_REQUIRED_SAMPLE_COUNTS;

   props->maxColorAttachments = 4;
   props->sampledImageColorSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageDepthSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->sampledImageStencilSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->storageImageSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
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
                                       uint32_t const pci_device_id,
                                       uint64_t const gart_size_kb)
{
   memset(props, 0, sizeof(*props));

   r300vk_physical_device_init_limits(props, gart_size_kb);

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

   /* Pipeline-cache UUID: BLAKE3 of the driver build identity plus the PCI
    * device ID, so a driver rebuild or a chip switch invalidates stale
    * disk_cache and vk_pipeline_cache entries.  disk_cache_get_function_identifier
    * derives the build id from the r300vk .so via dladdr.  Mirrors the
    * construction in terakan_physical_device.c. */
   {
      struct mesa_blake3 uuid_ctx;
      _mesa_blake3_init(&uuid_ctx);
      disk_cache_get_function_identifier(r300vk_physical_device_init_properties,
                                         &uuid_ctx);
      _mesa_blake3_update(&uuid_ctx, &pci_device_id, sizeof(pci_device_id));
      uint8_t uuid_hash[BLAKE3_OUT_LEN];
      _mesa_blake3_final(&uuid_ctx, uuid_hash);
      static_assert(sizeof(props->pipelineCacheUUID) <= BLAKE3_OUT_LEN,
                    "pipelineCacheUUID must fit in BLAKE3 output");
      memcpy(props->pipelineCacheUUID, uuid_hash,
             sizeof(props->pipelineCacheUUID));
   }

   /* VK_KHR_driver_properties identity.
    *
    * driverID is deliberately (VkDriverId)0.  The VkDriverId enum
    * (VkPhysicalDeviceDriverProperties, Vulkan 1.2 ch. 4.1.3) is a
    * Khronos-allocated registry; every value names a specific shipping
    * driver and the enum has no value 0.  r300vk is a downstream
    * driver that is not submitted upstream, so no VkDriverId will be
    * allocated for it.  Reusing an existing ID such as
    * VK_DRIVER_ID_MESA_RADV is rejected: an application keying off
    * driverID would apply RADV-specific workarounds to r300vk and
    * misbehave.  0 (out-of-enum) is the least-harmful honest value;
    * driverName ("r300vk") and driverInfo carry the real attribution.
    * dEQP-VK.api.driver_properties may flag a 0 driverID, which is
    * accepted: r300vk is not run for conformance submission.
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
   memset(features, 0, sizeof(*features));
   features->robustBufferAccess = true;
   features->largePoints = true;
   features->wideLines = true;
   features->samplerAnisotropy = true;
}

void
r300vk_physical_device_destroy(struct vk_physical_device *const device_base)
{
   struct r300vk_physical_device *const device =
      container_of(device_base, struct r300vk_physical_device, vk);

#ifdef R300VK_GALLIUM_BACKEND
   if (device->screen)
      device->screen->destroy(device->screen);
#endif

   if (device->render_node_fd >= 0)
      close(device->render_node_fd);

   vk_physical_device_finish(&device->vk);
   vk_free(&device->vk.instance->alloc, device);
}

/* CCN reflects the multi-step DRM device probing sequence: filter by node
 * type, PCI vendor/device IDs, and capability query, each with an exit path. */

static int
r300vk_open_radeon_render_node(struct vk_instance *instance,
                               struct _drmDevice *const drm_device)
{
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PCI ||
       drm_device->deviceinfo.pci->vendor_id != R300VK_VENDOR_ID_ATI ||
       !r300vk_pci_device_id_is_supported(drm_device->deviceinfo.pci->device_id)) {
      return -1;
   }

   const char *const render_node_path = drm_device->nodes[DRM_NODE_RENDER];
   int render_node_fd = open(render_node_path, O_RDWR | O_CLOEXEC);
   if (render_node_fd < 0)
      return -1;

   drmVersionPtr const drm_version = drmGetVersion(render_node_fd);
   if (drm_version == NULL) {
      close(render_node_fd);
      return -1;
   }
   const bool is_radeon = strcmp(drm_version->name, "radeon") == 0;
   drmFreeVersion(drm_version);
   if (!is_radeon) {
      close(render_node_fd);
      return -1;
   }

   return render_node_fd;
}

VkResult
r300vk_physical_device_try_create_for_drm(struct vk_instance *const instance_base,
                                          struct _drmDevice *const drm_device,
                                          struct vk_physical_device **const device_out)
{   int render_node_fd = r300vk_open_radeon_render_node(instance_base, drm_device);
   if (render_node_fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct r300vk_instance *const instance =
      container_of(instance_base, struct r300vk_instance, vk);

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

#ifdef R300VK_GALLIUM_BACKEND
   struct pipe_screen_config screen_config = {0};
   device->rws = radeon_drm_winsys_create(render_node_fd, &screen_config,
                                          r300_screen_create);
   if (!device->rws || !device->rws->screen) {
      if (device->rws && device->rws->screen)
         device->rws->screen->destroy(device->rws->screen);
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "r300vk: failed to create r300g pipe_screen for '%s'",
                       drm_device->nodes[DRM_NODE_RENDER]);
   }
   device->screen = device->rws->screen;
#endif

   device->sync_types[0] = &r300vk_cpu_sync_type;
   device->sync_types[1] = NULL;

   struct vk_features features;
   r300vk_physical_device_init_features(&features);

   /* vk_physical_device_init copies *properties into pdevice->properties
    * by value (src/vulkan/runtime/vk_physical_device.c:48-49), so the
    * struct must be fully populated before the call.  Mirror the order
    * used by terakan_physical_device_init at
    * src/amd/terascale/vulkan/terakan_physical_device.c around line 1640.
    */
   /* Source gart_size_kb from the r300 screen's radeon_info so the
    * advertised SSBO ceiling tracks the kernel's actual GART provisioning.
    * Without the gallium backend (loader-only R0 build) the screen is
    * absent; pass 0 so init_limits falls back to the default 128 MB
    * advertise. */
   uint64_t gart_size_kb = 0;
#ifdef R300VK_GALLIUM_BACKEND
   if (device->screen)
      gart_size_kb = r300_screen(device->screen)->info.gart_size_kb;
#endif
   struct vk_properties properties;
   r300vk_physical_device_init_properties(&properties, device->pci_vendor_id,
                                          device->pci_device_id,
                                          gart_size_kb);

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
#ifdef R300VK_GALLIUM_BACKEND
      if (device->screen)
         device->screen->destroy(device->screen);
#endif
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   device->vk.supported_sync_types = device->sync_types;
   device->hybrid_compute_enabled = r300vk_hybrid_compute_enabled();

   if (instance->debug_flags & R300VK_DEBUG_STARTUP) {
      fprintf(stderr,
              "r300vk: info: Found compatible DRM device '%s' (%04x:%04x).\n",
              drm_device->nodes[DRM_NODE_RENDER], device->pci_vendor_id,
              device->pci_device_id);
   }

   *device_out = &device->vk;
   return VK_SUCCESS;
}

/* Queue family enumeration.  Advertises one graphics+transfer queue
 * family with one queue.  RS482/RS485 has no native compute dispatch
 * surface, so VK_QUEUE_COMPUTE_BIT is absent unless the hybrid compute
 * experiment is explicitly enabled for CTS/RCA work. */
VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                               uint32_t *pCount,
                                               VkQueueFamilyProperties2 *pProperties)
{
   VK_FROM_HANDLE(r300vk_physical_device, pdev, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pProperties, pCount);
   VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;

   if (pdev->hybrid_compute_enabled)
      queue_flags |= VK_QUEUE_COMPUTE_BIT;

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = queue_flags,
         .queueCount = 1,
         .timestampValidBits = 0,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

static bool
r300vk_screen_supports_format(const struct r300vk_physical_device *const device,
                              enum pipe_format format,
                              enum pipe_texture_target target,
                              unsigned bindings)
{
#ifdef R300VK_GALLIUM_BACKEND
   return device->screen &&
          device->screen->is_format_supported(device->screen, format, target,
                                              0, 0, bindings);
#else
   return false;
#endif
}

static bool
r300vk_format_supports_transfer_dst(enum pipe_format pipe_format)
{
   if (util_format_is_compressed(pipe_format) ||
       util_format_is_depth_or_stencil(pipe_format) ||
       util_format_is_snorm(pipe_format))
      return false;

   const struct util_format_description *desc =
      util_format_description(pipe_format);
   return desc && desc->nr_channels > 0 &&
          (desc->channel[0].type != UTIL_FORMAT_TYPE_FLOAT ||
           desc->channel[0].size < 32);
}

static void
r300vk_get_format_properties(const struct r300vk_physical_device *const device,
                             VkFormat vk_format,
                             VkFormatProperties3 *const properties)
{
   memset(properties, 0, sizeof(*properties));
   properties->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;

   const enum pipe_format pipe_format = vk_format_to_pipe_format(vk_format);
   if (pipe_format == PIPE_FORMAT_NONE)
      return;

   VkFormatFeatureFlags2 image_features = 0;
   VkFormatFeatureFlags2 buffer_features = 0;
   const bool supports_depth_stencil =
      r300vk_screen_supports_format(device, pipe_format, PIPE_TEXTURE_2D,
                                    PIPE_BIND_DEPTH_STENCIL);
   const bool supports_sampler_view =
      r300vk_screen_supports_format(device, pipe_format, PIPE_TEXTURE_2D,
                                    PIPE_BIND_SAMPLER_VIEW);
   const bool supports_render_target =
      r300vk_screen_supports_format(device, pipe_format, PIPE_TEXTURE_2D,
                                    PIPE_BIND_RENDER_TARGET);

   if (supports_depth_stencil) {
      /* PIPE_BIND_DEPTH_STENCIL guarantees only depth/stencil attachment use.
       * PIPE_BIND_SAMPLER_VIEW is the separate authority for sampled-image
       * capability. */
      image_features |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
   }

   if (supports_sampler_view) {
      image_features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;

      if (!util_format_is_pure_integer(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
   }

   if (supports_render_target) {
      image_features |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;

      if (!util_format_is_pure_integer(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;
   }

   /* r300vk implements image transfer in both directions on the same r300g tile
    * transfer-map path: CmdCopyImageToBuffer2 for CPU readback and
    * CmdCopyBufferToImage2 / clear commands for CPU-written destinations.  Submit
    * flushes and waits for the GPU, then the CPU transfer pass maps each tile
    * with pipe->texture_map.  Every format allocatable for an attachment or
    * sampled-image path is a transfer source, including depth/stencil formats
    * that only advertise PIPE_BIND_DEPTH_STENCIL.  TRANSFER_DST is narrower:
    * advertise it only where the CPU upload and staging image-copy paths have a
    * lossless byte layout.  BLIT_SRC/DST stay withheld until those replay paths
    * exist. */
   if (supports_depth_stencil || supports_sampler_view || supports_render_target) {
      image_features |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;

      if (r300vk_format_supports_transfer_dst(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   }

   /* Depth/stencil formats carry no buffer features: a VkBuffer cannot hold a
    * depth/stencil format and the spec requires bufferFeatures == 0 for them
    * (dEQP-VK.api.buffer.invalid_buffer_features asserts exactly this).  r300's
    * is_format_supported can accept a depth format's underlying bits as a
    * vertex/texel fetch (e.g. Z16_UNORM read as a 16-bit unorm), so gate the
    * buffer bits on the format not being depth/stencil. */
   const bool is_depth_or_stencil = util_format_is_depth_or_stencil(pipe_format);

   /* RS482 routes all vertex fetch through the SW-TCL Gallium draw module, which
    * fetches in software and handles pure-integer vertex formats too.  r300g's
    * is_format_supported gates pure-integer out of its SW-TCL vertex branch (the
    * legacy GL path never exposed integer attributes), so admit a non-srgb,
    * non-depth/stencil pure-integer format here: it is a valid vertex format the
    * draw module fetches, which is what VERTEX_BUFFER advertises. */
   const bool vertex_fetchable =
      r300vk_screen_supports_format(device, pipe_format, PIPE_BUFFER,
                                    PIPE_BIND_VERTEX_BUFFER) ||
      util_format_is_pure_integer(pipe_format);
   if (!is_depth_or_stencil && !util_format_is_srgb(pipe_format) &&
       vertex_fetchable) {
      buffer_features |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
   }

   /* A Vulkan uniform texel buffer is a typed buffer fetched through a
    * sampler-view-class binding, not an untyped Gallium constant buffer.  Gate
    * UNIFORM_TEXEL_BUFFER on PIPE_BIND_SAMPLER_VIEW for PIPE_BUFFER so the
    * advertised set matches the formats r300g can actually fetch as texel data
    * rather than every format a constant buffer would nominally accept. */
   if (!is_depth_or_stencil &&
       r300vk_screen_supports_format(device, pipe_format, PIPE_BUFFER,
                                     PIPE_BIND_SAMPLER_VIEW)) {
      buffer_features |= VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
   }

   /* r300vk creates r300g-tiled textures regardless of VkImageTiling
    * (r300vk_image.c has no linear-layout path), so advertising linear-tiling
    * image features would promise a row-major layout the driver never
    * produces.  Report linear-tiling images as unsupported; only optimal
    * tiling carries the image feature set. */
   properties->linearTilingFeatures = 0;
   properties->optimalTilingFeatures = image_features;
   properties->bufferFeatures = buffer_features;
}

VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                          VkFormat format,
                                          VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(r300vk_physical_device, device, physicalDevice);

   VkFormatProperties3 properties3;
   r300vk_get_format_properties(device, format, &properties3);

   pFormatProperties->formatProperties = (VkFormatProperties){
      .linearTilingFeatures =
         vk_format_features2_to_features(properties3.linearTilingFeatures),
      .optimalTilingFeatures =
         vk_format_features2_to_features(properties3.optimalTilingFeatures),
      .bufferFeatures =
         vk_format_features2_to_features(properties3.bufferFeatures),
   };

   VkFormatProperties3 *const out_properties3 =
      vk_find_struct(pFormatProperties->pNext, FORMAT_PROPERTIES_3);
   if (out_properties3) {
      out_properties3->linearTilingFeatures = properties3.linearTilingFeatures;
      out_properties3->optimalTilingFeatures = properties3.optimalTilingFeatures;
      out_properties3->bufferFeatures = properties3.bufferFeatures;
   }
}


static bool
r300vk_image_usage_supported(VkImageUsageFlags usage,
                             VkFormatFeatureFlags2 features)
{
   if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT))
      return false;
   if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT))
      return false;
   if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT))
      return false;
   if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT))
      return false;
   if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT))
      return false;
   if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT))
      return false;

   if (usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT))
      return false;

   return true;
}

static VkResult
r300vk_get_image_format_properties(
   const struct r300vk_physical_device *const device,
   const VkPhysicalDeviceImageFormatInfo2 *const info,
   VkImageFormatProperties *const image_properties)
{
   VkFormatProperties3 format_properties;
   r300vk_get_format_properties(device, info->format, &format_properties);

   VkFormatFeatureFlags2 image_features = 0;
   switch (info->tiling) {
   case VK_IMAGE_TILING_LINEAR:
      image_features = format_properties.linearTilingFeatures;
      break;
   case VK_IMAGE_TILING_OPTIMAL:
      image_features = format_properties.optimalTilingFeatures;
      break;
   default:
      goto unsupported;
   }

   if (image_features == 0)
      goto unsupported;

   if (!r300vk_image_usage_supported(info->usage, image_features))
      goto unsupported;

   VkExtent3D max_extent;
   uint32_t max_mip_levels;
   uint32_t max_array_layers;

   /* Physical-device limits expose Vulkan's floor.  Per-format properties
    * expose the implemented flat-image contract so vkCreateImage and format
    * queries agree: one mip level and one array layer. */
   switch (info->type) {
   case VK_IMAGE_TYPE_1D:
      max_extent = (VkExtent3D){
         device->vk.properties.maxImageDimension1D, 1, 1,
      };
      max_mip_levels = 1;
      max_array_layers = 1;
      break;
   case VK_IMAGE_TYPE_2D:
      max_extent = (VkExtent3D){
         device->vk.properties.maxImageDimension2D,
         device->vk.properties.maxImageDimension2D,
         1,
      };
      max_mip_levels = 1;
      max_array_layers = 1;
      break;
   case VK_IMAGE_TYPE_3D:
      max_extent = (VkExtent3D){
         device->vk.properties.maxImageDimension3D,
         device->vk.properties.maxImageDimension3D,
         device->vk.properties.maxImageDimension3D,
      };
      max_mip_levels = 1;
      max_array_layers = 1;
      break;
   default:
      goto unsupported;
   }

   /* r300vk has no multisample path: the image model is single-sample r300g
    * tiles with a CPU transfer/clear replay and no MSAA resolve, so every image
    * format supports exactly one sample.  Reporting more lets a test build a
    * multisample image and then resolve it (CmdResolveImage), which the runtime
    * lowers through an unimplemented destination path and crashes.  The VK 1.0
    * framebuffer*SampleCounts device limits keep the required 4x minimum; this is
    * the per-format image capability, which is honestly single-sample.  The two
    * differ until a real MSAA path exists -- a known, deferred conformance gap. */
   *image_properties = (VkImageFormatProperties){
      .maxExtent = max_extent,
      .maxMipLevels = max_mip_levels,
      .maxArrayLayers = max_array_layers,
      .sampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxResourceSize = UINT32_MAX,
   };
   return VK_SUCCESS;

unsupported:
   *image_properties = (VkImageFormatProperties){0};
   return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

VKAPI_ATTR VkResult VKAPI_CALL
r300vk_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   VK_FROM_HANDLE(r300vk_physical_device, device, physicalDevice);

   const VkPhysicalDeviceExternalImageFormatInfo *external_info =
      vk_find_struct_const(pImageFormatInfo->pNext,
                           PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   if (external_info && external_info->handleType != 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkResult result =
      r300vk_get_image_format_properties(device, pImageFormatInfo,
                                         &pImageFormatProperties->imageFormatProperties);
   if (result != VK_SUCCESS)
      return result;

   VkExternalImageFormatProperties *const external_properties =
      vk_find_struct(pImageFormatProperties->pNext,
                     EXTERNAL_IMAGE_FORMAT_PROPERTIES);
   if (external_properties) {
      external_properties->externalMemoryProperties =
         (VkExternalMemoryProperties){0};
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties2 *pProperties)
{
   *pPropertyCount = 0;
}

/* Fallback heap sizes for the loader-only build (no Gallium oracle) or when the
 * winsys query reports zero.  The Gallium-backed build reports the real sizes
 * from the radeon winsys instead (see r300vk_GetPhysicalDeviceMemoryProperties2),
 * which the winsys read via DRM_RADEON_GEM_INFO at creation -- radeon_drm_winsys.c
 * populates info.gart_size_kb / vram_size_kb.  RS482/RS485 is UMA: the GART
 * aperture and the BIOS-carved shared-VRAM partition overlap in physical memory,
 * so a probe needing the exact physical split must treat the two heaps as one
 * shared pool. */
#define R300VK_PLACEHOLDER_GTT_HEAP_SIZE     (128ULL * 1024 * 1024)
#define R300VK_PLACEHOLDER_VRAM_HEAP_SIZE    ( 64ULL * 1024 * 1024)

VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VkPhysicalDeviceMemoryProperties *const m = &pMemoryProperties->memoryProperties;

   /* Report the real GART and shared-VRAM sizes when a Gallium r300g oracle is
    * attached.  query_info hands back the radeon_info the winsys cached from
    * DRM_RADEON_GEM_INFO at creation; gart_size_kb / vram_size_kb are the total
    * aperture sizes.  The loader-only build (no rws) keeps the nominal fallbacks. */
   uint64_t gtt_bytes  = R300VK_PLACEHOLDER_GTT_HEAP_SIZE;
   uint64_t vram_bytes = R300VK_PLACEHOLDER_VRAM_HEAP_SIZE;
#ifdef R300VK_GALLIUM_BACKEND
   VK_FROM_HANDLE(r300vk_physical_device, pdev, physicalDevice);
   if (pdev->rws && pdev->rws->query_info) {
      struct radeon_info rinfo;
      pdev->rws->query_info(pdev->rws, &rinfo);
      if (rinfo.gart_size_kb)
         gtt_bytes = (uint64_t)rinfo.gart_size_kb * 1024;
      if (rinfo.vram_size_kb)
         vram_bytes = (uint64_t)rinfo.vram_size_kb * 1024;
   }
#endif

   m->memoryHeapCount = 2;
   m->memoryHeaps[0] = (VkMemoryHeap){
      .size = gtt_bytes,
      .flags = 0,
   };
   m->memoryHeaps[1] = (VkMemoryHeap){
      .size = vram_bytes,
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
