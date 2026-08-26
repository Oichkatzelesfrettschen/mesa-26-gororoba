/*
 * SPDX-License-Identifier: MIT
 */

#include "r3v_physical_device.h"
#include "r3v_format.h"
#include "r3v_cpu_sync.h"
#include "r3v_memory_properties_contract.h"

#include "r3v_entrypoints.h"
#include "r3v_instance.h"
#include "r3v_private.h"

#include "r3v_native.h"

#include "amd/r300/common/r300_compute_verb.h"

#include "util/disk_cache.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "vk_alloc.h"
#include "vk_enum_defines.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

/* PIPE_FORMAT_* values come from util/format/u_formats.h through
 * idep_mesautil; the ICD compiles with no Gallium include root. */

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <radeon_drm.h>
#include <xf86drm.h>

/* The compute-queue claim derives from the verb ledger: unconditional
 * once every verb executes on both routes, else the exact opt-in over
 * the delivered CPU route. */
static bool
r3v_compute_queue_claimed(void)
{
   const char *gate = getenv(R300_COMPUTE_QUEUE_CLAIM_GATE);
   return r300_compute_verb_queue_claim(
      gate && strcmp(gate, R300_COMPUTE_QUEUE_CLAIM_GATE_VALUE) == 0);
}


static const char *
r3v_chip_name_from_pci_device_id(uint32_t pci_device_id)
{
   switch (pci_device_id) {
   case R3V_PCI_DEVICE_ID_RS482:
      return "ATI RS480 (RS482)";
   case R3V_PCI_DEVICE_ID_RS485:
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
    * Where the RS482 path has no single native 4096-wide render surface,
    * r3v presents the Vulkan floor through a 2560 hardware-backed span plus
    * a residual span.  Native r300g resources remain the fast path for images
    * that fit in one span. */
static void
r3v_physical_device_init_limits(struct vk_properties *const props,
                                   uint64_t const gart_size_kb)
{
   /* Texture and image dimensions.  The RS482 render path accepts a 2560-wide
    * hardware span; r3v composes the Vulkan 4096 floor from that fast path
    * plus a residual span when an image exceeds the single-span limit. */
   props->maxImageDimension1D = R3V_VK10_MIN_IMAGE_DIMENSION_1D;
   props->maxImageDimension2D = R3V_MAX_IMAGE_DIMENSION_2D;
   props->maxImageDimension3D = 256;
   props->maxImageDimensionCube = R3V_VK10_MIN_IMAGE_DIMENSION_CUBE;
   props->maxImageArrayLayers = R3V_NATIVE_MAX_ARRAY_LAYERS;

   /* Texel buffer size: R3xx has no native texel buffer object.  The
    * Vulkan 1.4 minimum is 65536; r3v_CreateBufferView enforces the same
    * ceiling on the recorded range. */
   props->maxTexelBufferElements = R3V_NATIVE_MAX_TEXEL_BUFFER_ELEMENTS;

   /* PS constant store: R300_PFS_PARAM_0..31 yields 32 vec4 slots, or
    * 512 bytes.  The Vulkan minimum maxUniformBufferRange is 16 KiB,
    * so we round up to that bound; the descriptor binding still maps
    * down to the hardware 32 slots. */
   props->maxUniformBufferRange = R3V_VK10_MIN_UNIFORM_BUFFER_RANGE;

   /* SSBO size advertise.  R3xx has no native SSBO; the compute-as-raster
    * substrate maps stores to RB3D color export backed by the radeon GART.
    * Cap the advertised maxStorageBufferRange to 512 MB only when the
    * kernel-reported GART (DRM_RADEON_GEM_INFO gart_size) is >= 1 GB;
    * otherwise advertise the Vulkan minimum. */
   if (gart_size_kb >= 1024u * 1024u)
      props->maxStorageBufferRange = 512u * 1024u * 1024u; /* 512 MB */
   else
      props->maxStorageBufferRange = R3V_VK10_MIN_STORAGE_BUFFER_RANGE;
   /* maxBufferSize is the VkPhysicalDeviceMaintenance4Properties /
    * Vulkan 1.3 field filled by the properties2 path. Keep it at least
    * maxStorageBufferRange so a future maintenance4/1.3 advertisement
    * does not violate the maxBufferSize >= maxStorageBufferRange rule.
    * r3v still targets apiVersion 1.0 and does not advertise
    * VK_KHR_maintenance4 yet; the field is still set in vk_properties. */
   props->maxBufferSize = props->maxStorageBufferRange;
   props->maxPushConstantsSize = R3V_MAX_PUSH_CONSTANTS_SIZE;

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
   props->maxVertexInputAttributes = R300_VERTEX_JOB_MAX_INPUTS;
   props->maxVertexInputBindings = R3V_NATIVE_MAX_VERTEX_BINDINGS;
   props->maxVertexInputAttributeOffset =
      R3V_NATIVE_MAX_VERTEX_ATTRIBUTE_OFFSET;
   props->maxVertexInputBindingStride = R3V_NATIVE_MAX_VERTEX_BINDING_STRIDE;

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

   /* Fragment shader budget for the RS482/RS485 R3V target.
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
      R3V_VK10_MIN_COMPUTE_SHARED_MEMORY_SIZE;
   props->maxComputeWorkGroupCount[0] = 65535;
   props->maxComputeWorkGroupCount[1] = 65535;
   props->maxComputeWorkGroupCount[2] = 65535;
   props->maxComputeWorkGroupInvocations =
      R3V_VK10_MIN_COMPUTE_WORKGROUP_INVOCATIONS;
   props->maxComputeWorkGroupSize[0] = R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_X;
   props->maxComputeWorkGroupSize[1] = R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Y;
   props->maxComputeWorkGroupSize[2] = R3V_VK10_MIN_COMPUTE_WORKGROUP_SIZE_Z;

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
   props->maxViewportDimensions[0] = R3V_MAX_RENDER_EXTENT;
   props->maxViewportDimensions[1] = R3V_MAX_RENDER_EXTENT;
   props->viewportBoundsRange[0] = -8192.0f;
   props->viewportBoundsRange[1] = 8191.0f;
   props->viewportSubPixelBits = 0;

   props->minMemoryMapAlignment = 64;
   props->minTexelBufferOffsetAlignment =
      R3V_NATIVE_MIN_TEXEL_BUFFER_OFFSET_ALIGNMENT;
   props->minUniformBufferOffsetAlignment = 16;
   props->minStorageBufferOffsetAlignment = 16;

   props->minTexelOffset = -8;
   props->maxTexelOffset = 7;
   props->minTexelGatherOffset = -8;
   props->maxTexelGatherOffset = 7;
   props->minInterpolationOffset = -0.5f;
   props->maxInterpolationOffset = 0.4375f;
   props->subPixelInterpolationOffsetBits = 4;

   /* The render pass admits the render family's extent alone, so the
    * framebuffer limits advertise the render-shape family's ceiling. */
   props->maxFramebufferWidth = R3V_MAX_RENDER_EXTENT;
   props->maxFramebufferHeight = R3V_MAX_RENDER_EXTENT;
   props->maxFramebufferLayers = 1;

   props->framebufferColorSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->framebufferDepthSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->framebufferStencilSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->framebufferNoAttachmentsSampleCounts =
      R3V_SUPPORTED_SAMPLE_COUNTS;

   /* R300 binds up to four simultaneous colour buffers (COLOROFFSET0..3 /
    * US_OUT_FMT_0..3), and the replay now binds every subpass colour attachment
    * at its own cbuf slot, so four is the honest count and matches the gallium
    * max_render_targets cap.  r300 shares one blend state and colour mask across
    * all cbufs (RB3D_CBLEND), so independentBlend is advertised false; Vulkan
    * then requires every attachment's blend state to be identical, which makes
    * binding pAttachments[0] for all of them spec-correct. */
   props->maxColorAttachments = 4;
   props->sampledImageColorSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageDepthSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->sampledImageStencilSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->storageImageSampleCounts = R3V_SUPPORTED_SAMPLE_COUNTS;
   props->maxSampleMaskWords = 1;

   props->timestampComputeAndGraphics = VK_FALSE;
   props->timestampPeriod = 0.0f;

   /* R300 supports 6 user clip planes through R300_VAP_CLIP_CNTL. */
   props->maxClipDistances = 6;
   props->maxCullDistances = 0;
   props->maxCombinedClipAndCullDistances = 6;

   props->discreteQueuePriorities = 2;

   /* The native feature set carries neither largePoints nor wideLines,
    * and a device without those features reports the fixed [1,1] size
    * ranges with zero granularity (Vulkan 1.0, Limit Requirements:
    * granularity applies only when the corresponding feature is
    * supported). */
   props->pointSizeRange[0] = 1.0f;
   props->pointSizeRange[1] = 1.0f;
   props->lineWidthRange[0] = 1.0f;
   props->lineWidthRange[1] = 1.0f;
   props->pointSizeGranularity = 0.0f;
   props->lineWidthGranularity = 0.0f;

   props->strictLines = VK_FALSE;
   props->standardSampleLocations = VK_TRUE;
   props->optimalBufferCopyOffsetAlignment = 128;
   props->optimalBufferCopyRowPitchAlignment = 128;
   props->nonCoherentAtomSize = 64;
}

static void
r3v_physical_device_init_properties(struct vk_properties *const props,
                                       uint32_t const pci_vendor_id,
                                       uint32_t const pci_device_id,
                                       uint64_t const gart_size_kb)
{
   memset(props, 0, sizeof(*props));

   r3v_physical_device_init_limits(props, gart_size_kb);

   props->apiVersion = R3V_API_VERSION;

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

   const char *const chip_name = r3v_chip_name_from_pci_device_id(pci_device_id);
   snprintf(props->deviceName, sizeof(props->deviceName), "%s",
            chip_name);

   /* Pipeline-cache UUID: BLAKE3 of the driver build identity plus the PCI
    * device ID, so a driver rebuild or a chip switch invalidates stale
    * disk_cache and vk_pipeline_cache entries.  disk_cache_get_function_identifier
    * derives the build id from the r3v .so via dladdr.  Mirrors the
    * construction in terakan_physical_device.c. */
   {
      struct mesa_blake3 uuid_ctx;
      _mesa_blake3_init(&uuid_ctx);
      disk_cache_get_function_identifier(r3v_physical_device_init_properties,
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
    * driver and the enum has no value 0.  r3v is a downstream
    * driver that is not submitted upstream, so no VkDriverId will be
    * allocated for it.  Reusing an existing ID such as
    * VK_DRIVER_ID_MESA_RADV is rejected: an application keying off
    * driverID would apply RADV-specific workarounds to r3v and
    * misbehave.  0 (out-of-enum) is the least-harmful honest value;
    * driverName ("r3v") and driverInfo carry the real attribution.
    * dEQP-VK.api.driver_properties may flag a 0 driverID, which is
    * accepted: r3v is not run for conformance submission.
    */
   props->driverID = (VkDriverId)0;
   snprintf(props->driverName, sizeof(props->driverName), "%s", "r3v");
   snprintf(props->driverInfo, sizeof(props->driverInfo), "%s",
            "Mesa r3v (Radeon DRM, private-cell experimental, nonconformant)");
   props->conformanceVersion = (VkConformanceVersion){0, 0, 0, 0};
   /* VK_EXT_custom_border_color: the border colour lives in the sampler CSO,
    * so the count is bounded only by sampler objects; report the 1.0-era
    * sampler allocation floor.  VK_EXT_line_rasterization: r300's line
    * rasteriser walks at 1/16th-pixel steps (4 sub-pixel bits, the GL
    * subpixel precision the chip family advertises). */
   props->maxCustomBorderColorSamplers = 4000;
   props->lineSubPixelPrecisionBits = 4;
   /* VK_KHR_maintenance2: r300 clips points against the cull volume like all
    * user-clip geometry. */
   props->pointClippingBehavior =
      VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
   /* VK_KHR_depth_stencil_resolve: single-sample device, so SAMPLE_ZERO is
    * the whole honest mode set and independent resolve is vacuous. */
   props->supportedDepthResolveModes   = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
   props->supportedStencilResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
   props->independentResolveNone = true;
   props->independentResolve     = true;
   /* R3V exposes one fixed shader floating-point policy and no
    * VK_KHR_shader_float_controls modes. */
   props->denormBehaviorIndependence =
      VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
   props->roundingModeIndependence =
      VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
}


/* The native implementation advertises only surfaces its own entry points
 * execute.  Every 1.0 memory and buffer entry point routes through the
 * vk_common *2-form bridges into the native one-BO implementations.  The
 * three advertised extensions are the memory-requirements contract the
 * image path executes: the *2 query and bind entry points resolve, and
 * VK_KHR_dedicated_allocation carries the required-dedicated signal
 * that states the image's offset-zero binding to an allocator; a
 * further extension returns with the native route that executes it.
 */
static const struct vk_device_extension_table
   r3v_native_device_extensions_supported = {
      .KHR_get_memory_requirements2 = true,
      .KHR_bind_memory2 = true,
      .KHR_dedicated_allocation = true,
   };

static void
r3v_physical_device_init_features(struct vk_features *features)
{
   memset(features, 0, sizeof(*features));
   /* The native implementation executes no optional feature; the
    * feature set is the core-1.0 baseline, and each optional bit
    * returns with the native route that makes it true.
    * robustBufferAccess is core 1.0's one mandatory feature: enabled,
    * an out-of-bounds vertex record reads zeros through the CPU vertex
    * executor (r300_vertex_stream.oob_reads_zero) and storage ranges
    * are validated against their binding and memory bound at
    * admission; disabled, the out-of-bounds record refuses the draw
    * before any write. */
   features->robustBufferAccess = true;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r3v_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(r3v_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->vk.instance, pName);
}

/* sw_device follows R3V_WSI_SW.  A value beginning with '1' sets sw_device
 * true and passes -1 for the fd, the Mesa common WSI software mode (the
 * lavapipe pattern): CPU-reachable swapchain images presented through the
 * xcb-shm path, no dma-buf, modifier, or external-memory support required
 * of the radeon winsys.  Every other value--unset, empty, or any other
 * leading byte--routes wsi_device_init through the render-node fd, the
 * DRM/DRI3 path.  wants_linear keeps the swapchain images row-major in
 * both routes, the layout the present copy reads and the one r3v's
 * single-tile linear images provide. */
static VkResult
r3v_init_wsi(struct r3v_physical_device *device)
{
   /* GPU-resident present by default: the render-node fd lets the common WSI
    * take the DRM/DRI3 path, presenting the dma-buf-exported scanout images
    * the export substrate provides -- the contract the GL oracle measured.
    * R3V_WSI_SW=1 falls back to the xcb-shm CPU copy, the proven
    * bring-up baseline. */
   const char *wsi_sw_env = getenv("R3V_WSI_SW");
   const bool wsi_sw = wsi_sw_env && wsi_sw_env[0] == '1';
   VkResult result =
      wsi_device_init(&device->wsi_device,
                      r3v_physical_device_to_handle(device),
                      r3v_wsi_proc_addr,
                      &device->vk.instance->alloc,
                      wsi_sw ? -1 : device->render_node_fd, NULL,
                      &(struct wsi_device_options){.sw_device = wsi_sw});
   if (result != VK_SUCCESS)
      return result;

   device->wsi_device.wants_linear = true;
   device->vk.wsi_device = &device->wsi_device;
   return VK_SUCCESS;
}

static void
r3v_finish_wsi(struct r3v_physical_device *device)
{
   if (device->vk.wsi_device == NULL)
      return;
   device->vk.wsi_device = NULL;
   wsi_device_finish(&device->wsi_device, &device->vk.instance->alloc);
}

void
r3v_physical_device_destroy(struct vk_physical_device *const device_base)
{
   struct r3v_physical_device *const device =
      container_of(device_base, struct r3v_physical_device, vk);

   r3v_finish_wsi(device);


   if (device->render_node_fd >= 0)
      close(device->render_node_fd);

   vk_physical_device_finish(&device->vk);
   vk_free(&device->vk.instance->alloc, device);
}

/* CCN reflects the multi-step DRM device probing sequence: filter by node
 * type, PCI vendor/device IDs, and capability query, each with an exit path. */

static int
r3v_open_radeon_render_node(struct vk_instance *instance,
                               struct _drmDevice *const drm_device)
{
   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PCI ||
       drm_device->deviceinfo.pci->vendor_id != R3V_VENDOR_ID_ATI ||
       !r3v_pci_device_id_is_supported(drm_device->deviceinfo.pci->device_id)) {
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
r3v_physical_device_try_create_for_drm(struct vk_instance *const instance_base,
                                          struct _drmDevice *const drm_device,
                                          struct vk_physical_device **const device_out)
{   int render_node_fd = r3v_open_radeon_render_node(instance_base, drm_device);
   if (render_node_fd < 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct r3v_instance *const instance =
      container_of(instance_base, struct r3v_instance, vk);

   struct r3v_physical_device *const device =
      vk_alloc(&instance->vk.alloc, sizeof(*device), alignof(struct r3v_physical_device),
               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      close(render_node_fd);
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   device->pci_vendor_id = drm_device->deviceinfo.pci->vendor_id;
   device->pci_device_id = drm_device->deviceinfo.pci->device_id;
   device->render_node_fd = render_node_fd;

   /* DRM node device IDs for VK_EXT_physical_device_drm.  fstat the render fd
    * for its major/minor and stat the primary node path when the DRM device
    * exposes one; Zink matches the EGL render-node fd against these. */
   device->has_primary_node = false;
   device->primary_node_major = 0;
   device->primary_node_minor = 0;
   device->render_node_major = 0;
   device->render_node_minor = 0;
   {
      struct stat node_stat;
      if (fstat(render_node_fd, &node_stat) == 0) {
         device->render_node_major = major(node_stat.st_rdev);
         device->render_node_minor = minor(node_stat.st_rdev);
      }
      if ((drm_device->available_nodes & (1 << DRM_NODE_PRIMARY)) &&
          stat(drm_device->nodes[DRM_NODE_PRIMARY], &node_stat) == 0) {
         device->has_primary_node = true;
         device->primary_node_major = major(node_stat.st_rdev);
         device->primary_node_minor = minor(node_stat.st_rdev);
      }
   }


   /* Slot 0 is the GPU-fence-backed binary cpu_sync; slot 1 is a timeline
    * emulated on it.  radeon.ko does not set DRIVER_SYNCOBJ (amdgpu does), so
    * drm_syncobj_create_ioctl's drm_core_check_feature(dev, DRIVER_SYNCOBJ)
    * gate returns -EOPNOTSUPP and DRM_CAP_SYNCOBJ reads 0 on the render node
    * (measured on RS482: DRM_IOCTL_SYNCOBJ_CREATE returns errno 95).  No
    * DRM syncobj means vk_drm_syncobj_get_type() would report features == 0,
    * so there is no slot for it here.  The consequence reaches past the
    * timeline: VK_KHR_external_semaphore_fd / VK_KHR_external_fence_fd stay
    * unadvertised because both handle types are unreachable -- OPAQUE_FD needs
    * a syncobj handle, and SYNC_FD needs a struct sync_file the radeon CS
    * submit ioctl never emits.  Cross-process interop on radeon is implicit
    * dma_resv fencing on the shared BO (the path VK_KHR_external_memory_fd
    * already rides), not an explicit exported semaphore.
    * vk_sync_timeline_get_type returns the wrapper type by value, so it is
    * stored on the physical device and the table points at its embedded sync. */
   device->timeline_sync_type = vk_sync_timeline_get_type(&r3v_cpu_sync_type);
   device->sync_types[0] = &r3v_cpu_sync_type;
   device->sync_types[1] = &device->timeline_sync_type.sync;
   device->sync_types[2] = NULL;

   struct vk_features features;
   r3v_physical_device_init_features(&features);

   /* vk_physical_device_init copies the completed properties struct by value,
    * so the fields are populated before initialization. */
   /* The storage-buffer ceiling tracks the kernel's GART provisioning:
    * DRM_RADEON_GEM_INFO reports gart_size, the pool every GEM allocation
    * lands in, so the limit derives from the same fact the memory heap does;
    * a failed query leaves the Vulkan minimum advertised. */
   uint64_t gart_size_kb = 0;
   {
      struct drm_radeon_gem_info gem_info = {0};
      if (drmCommandWriteRead(device->render_node_fd, DRM_RADEON_GEM_INFO,
                              &gem_info, sizeof(gem_info)) == 0)
         gart_size_kb = gem_info.gart_size / 1024;
   }
   struct vk_properties properties;
   r3v_physical_device_init_properties(&properties, device->pci_vendor_id,
                                          device->pci_device_id,
                                          gart_size_kb);

   /* VK_EXT_physical_device_drm: fill the DRM node IDs Zink matches the EGL DRM
    * fd against (init_properties zeroed the rest of the struct). */
   properties.drmHasRender    = true;
   properties.drmRenderMajor  = device->render_node_major;
   properties.drmRenderMinor  = device->render_node_minor;
   properties.drmHasPrimary   = device->has_primary_node;
   properties.drmPrimaryMajor = device->primary_node_major;
   properties.drmPrimaryMinor = device->primary_node_minor;

   /* VK_EXT_pci_bus_info: the PCI address of the enumerated DRM device.  The
    * common WSI matches these against the DRI3 fd's PCI address to decide
    * same-GPU presentation (native dma-buf images) versus the cross-GPU
    * prime-blit buffer path.  bustype DRM_BUS_PCI is guaranteed by
    * r3v_open_radeon_render_node, so businfo.pci is always populated. */
   properties.pciDomain   = drm_device->businfo.pci->domain;
   properties.pciBus      = drm_device->businfo.pci->bus;
   properties.pciDevice   = drm_device->businfo.pci->dev;
   properties.pciFunction = drm_device->businfo.pci->func;

   /* Driver entrypoints precede WSI surface queries. The common physical-device
    * runtime fills the remaining entrypoints during initialization. */
   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &r3v_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &wsi_physical_device_entrypoints, false);

   const struct vk_device_extension_table *supported_extensions =
      &r3v_native_device_extensions_supported;
   VkResult result = vk_physical_device_init(&device->vk, &instance->vk,
                                             supported_extensions,
                                             &features, &properties, &dispatch_table);
   if (result != VK_SUCCESS) {
      /* terakan_physical_device_init does not call vk_physical_device_finish
       * on init failure (terakan_physical_device.c fail_isa label); the
       * runtime helper only requires finish after a successful init. */
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   device->vk.supported_sync_types = device->sync_types;
   device->compute_queue_claimed = r3v_compute_queue_claimed();

   result = r3v_init_wsi(device);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&device->vk);
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   if (instance->debug_flags & R3V_DEBUG_STARTUP) {
      fprintf(stderr,
              "r3v: info: Found compatible DRM device '%s' (%04x:%04x).\n",
              drm_device->nodes[DRM_NODE_RENDER], device->pci_vendor_id,
              device->pci_device_id);
   }

   *device_out = &device->vk;
   return VK_SUCCESS;
}

/* Queue family enumeration.  Advertises one graphics queue family with
 * one queue; VK_QUEUE_COMPUTE_BIT follows the verb ledger's queue
 * claim. */
VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                               uint32_t *pCount,
                                               VkQueueFamilyProperties2 *pProperties)
{
   VK_FROM_HANDLE(r3v_physical_device, pdev, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pProperties, pCount);
   /* The public recording surface records the graphics command subset
    * -- render pass, pipeline bind, vertex bind, draw -- on this
    * family, so GRAPHICS is advertised.  COMPUTE returns with the
    * verb ledger's queue claim (r300_compute_verb_queue_claim):
    * unconditional once every verb executes on both routes, else the
    * exact R3V_NATIVE_COMPUTE_QUEUE_EXPERIMENTAL=1 opt-in over the
    * delivered CPU route, because the admitted verb subset stays
    * nonconformant against the full compute contract the bit claims.
    */
   VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT;
   if (pdev->compute_queue_claimed)
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

/* Non-static: r3v_CreateBufferView (r3v_native_object.c) queries this
 * table directly, so the format-feature grant and the buffer-view
 * admission it gates read from one function rather than two mirrored
 * switches.
 */
void
r3v_get_format_properties(const struct r3v_physical_device *const device,
                             VkFormat vk_format,
                             VkFormatProperties3 *const properties)
{
   memset(properties, 0, sizeof(*properties));
   properties->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;

   /* The format capabilities are the public recording surface's accepted
    * subset, advertised exactly so a capability-aware application reaches
    * the qualified route: the two 32-bpp lane orders the render-shape
    * cell places into its target and the F32-family vertex formats the
    * CPU vertex executor gathers.
    */
   switch (vk_format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      /* Both 32-bpp lane orders carry the sampled-image grant: the
       * sampling cell's FORMAT1 per-channel selects route each order's
       * memory bytes to shader R/G/B/A over the TX unit's W8Z8Y8X8
       * word, so neither format converts.
       */
      properties->linearTilingFeatures =
         VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
      properties->optimalTilingFeatures = properties->linearTilingFeatures;
      properties->bufferFeatures =
         VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
      break;
   case VK_FORMAT_B8G8R8A8_UNORM:
      /* The render family's color-attachment grant plus the transfer
       * family's copy grant: the recorded vkCmdCopy* subset executes
       * the transfer features through host mappings at submission. Each
       * format names one US_OUT_FMT_0 lane order the cell emits
       * (r3v_native_render_lane_order), so both carry the attachment
       * bit. Both formats also sit in the transfer-image texel table
       * below, so they carry the same texel-buffer grant. Both tilings
       * execute the one linear span, VK_IMAGE_TILING_OPTIMAL under
       * Vulkan's opaque layout contract, so the two tiling grants are
       * equal.
       */
      properties->linearTilingFeatures =
         VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
      properties->optimalTilingFeatures = properties->linearTilingFeatures;
      /* tests/r3v_conformance_nonpass_ledger.tsv row
       * mandatory_format_feature_absent names the RS480 die's absent
       * storage-image and integer-format routes; the same silicon gap
       * withholds VK_FORMAT_FEATURE_2_STORAGE_TEXEL_BUFFER_BIT, so the
       * grant is the uniform texel-buffer bit alone.
       */
      properties->bufferFeatures = VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
      break;
   case VK_FORMAT_R8G8B8A8_UINT:
   case VK_FORMAT_R16G16B16A16_UINT:
   case VK_FORMAT_R32G32B32A32_UINT:
   case VK_FORMAT_R32_UINT:
      /* The transfer family's texel table: the copies move these
       * texels by size through host mappings and never interpret them,
       * so the grant is the two transfer bits on the linear layout,
       * and identically on VK_IMAGE_TILING_OPTIMAL since
       * r3v_CreateImage executes both tilings as the one linear span
       * (r3v_native_transfer_footprint_bytes) for this family.
       * r3v_CreateBufferView queries this same table for the uniform
       * texel-buffer admission; the storage bit stays withheld for the
       * reason above.
       */
      properties->linearTilingFeatures =
         VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
      properties->optimalTilingFeatures =
         VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
      properties->bufferFeatures = VK_FORMAT_FEATURE_2_UNIFORM_TEXEL_BUFFER_BIT;
      break;
   case VK_FORMAT_R32_SFLOAT:
   case VK_FORMAT_R32G32_SFLOAT:
   case VK_FORMAT_R32G32B32_SFLOAT:
   case VK_FORMAT_R32G32B32A32_SFLOAT:
      properties->bufferFeatures = VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
      break;
   default:
      break;
   }
}

VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice,
                                          VkFormat format,
                                          VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(r3v_physical_device, device, physicalDevice);

   VkFormatProperties3 properties3;
   r3v_get_format_properties(device, format, &properties3);

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
r3v_image_usage_supported(VkImageUsageFlags usage,
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

   /* Input attachments ride the sampled path (subpassLoad lowers to a
    * normalized texture read), so sampled capability is input-attachment
    * capability -- the same grant vkCreateImage makes.  Lazily-allocated
    * transient attachments have no backing model in the synchronous replay,
    * so TRANSIENT stays rejected on both sides. */
   if ((usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) &&
       !(features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT))
      return false;
   if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
      return false;

   return true;
}

static VkResult
r3v_get_image_format_properties(
   const struct r3v_physical_device *const device,
   const VkPhysicalDeviceImageFormatInfo2 *const info,
   VkImageFormatProperties *const image_properties)
{
   VkFormatProperties3 format_properties;
   r3v_get_format_properties(device, info->format, &format_properties);

   /* The native image contract carries two flat families over the 1D and
    * 2D types -- the render family, which takes the color-attachment bit
    * with the sampled and transfer bits beside it at no create flag, and
    * the sampling family, which takes the transfer and sampled bits at no
    * create flag or at VK_IMAGE_CREATE_ALIAS_BIT, whose aliasing window
    * is the linear footprint (r3v_native_image.c) -- so the query reports
    * every other type, flag, or usage unsupported before the shared type
    * switch, the same refusal vkCreateImage applies.
    */
   const VkImageUsageFlags r3v_native_transfer_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   const bool r3v_native_render_query =
      (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
      (info->usage & ~(VkImageUsageFlags)(
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT |
                         r3v_native_transfer_usage)) == 0;
   const bool r3v_native_sampling_query =
      !r3v_native_render_query && info->usage != 0 &&
      (info->usage & ~(VkImageUsageFlags)(VK_IMAGE_USAGE_SAMPLED_BIT |
                                          r3v_native_transfer_usage)) == 0;
   const VkImageCreateFlags r3v_native_admitted_flags =
      r3v_native_sampling_query ? VK_IMAGE_CREATE_ALIAS_BIT : 0;
   if ((info->type != VK_IMAGE_TYPE_2D && info->type != VK_IMAGE_TYPE_1D) ||
       (info->flags & ~r3v_native_admitted_flags) ||
       (!r3v_native_render_query && !r3v_native_sampling_query))
      goto unsupported;

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

   if (!r3v_image_usage_supported(info->usage, image_features))
      goto unsupported;

   VkExtent3D max_extent;
   uint32_t max_mip_levels;
   uint32_t max_array_layers;

   /* Physical-device limits expose Vulkan's floor.  Per-format properties
    * expose the implemented flat-image contract so vkCreateImage and format
    * queries agree: one mip level, the layer count a view selects one
    * layer out of, and the family's own extent ceiling.  The 1D type is
    * the height-one member of that same layout, so it reports the same
    * width bound.
    */
   const uint32_t r3v_native_family_extent =
      r3v_native_render_query ? R3V_NATIVE_RENDER_MAX_EXTENT
                              : R3V_NATIVE_TRANSFER_DIMENSION_MAX;
   max_mip_levels = 1;
   max_array_layers = R3V_NATIVE_MAX_ARRAY_LAYERS;
   switch (info->type) {
   case VK_IMAGE_TYPE_1D:
      max_extent = (VkExtent3D){ r3v_native_family_extent, 1, 1 };
      break;
   case VK_IMAGE_TYPE_2D:
      max_extent = (VkExtent3D){ r3v_native_family_extent,
                                 r3v_native_family_extent, 1 };
      break;
   case VK_IMAGE_TYPE_3D:
      /* Every image is one flat 2D linear layer over its GEM BO
       * (r3v_native_image.c), so a 3D image's depth slices have no storage.
       * Report VK_IMAGE_TYPE_3D unsupported so this query, vkCreateImage,
       * and the transfer path agree, and a 3D image test is NotSupported
       * rather than silently incorrect.  VK 1.0 requires
       * maxImageDimension3D >= 256, which the device limit reports and no
       * format honors until a 3D layout exists. */
      goto unsupported;
   default:
      goto unsupported;
   }

   /* A linear image is one row-major span, so its extent is bounded by the
    * single-span render dimension vkCreateImage's linear accept gate
    * enforces.  Report the same bound here so the two stay one contract. */
   if (info->tiling == VK_IMAGE_TILING_LINEAR) {
      max_extent.width = MIN2(max_extent.width,
                              R3V_R3XX_MAX_RENDER_DIMENSION);
      max_extent.height = MIN2(max_extent.height,
                               R3V_R3XX_MAX_RENDER_DIMENSION);
   }

   /* Every format supports one sample: the render cell rasterizes at
    * one sample per pixel and the transfers move one sample's bytes,
    * matching the single-sample device limits. */
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

VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   /* Buffer export is not advertised; zeroed properties is the spec's
    * "unsupported handle type" answer. */
   pExternalBufferProperties->externalMemoryProperties =
      (VkExternalMemoryProperties){0};
}

VKAPI_ATTR VkResult VKAPI_CALL
r3v_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   VK_FROM_HANDLE(r3v_physical_device, device, physicalDevice);

   /* dma-buf (and the opaque-fd alias of the same PRIME fd) is exportable for
    * 2D images; every other handle type stays rejected.  Import is accepted
    * only as the round-trip of an exported BO (dedicated allocations). */
   const VkPhysicalDeviceExternalImageFormatInfo *external_info =
      vk_find_struct_const(pImageFormatInfo->pNext,
                           PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   /* The native link set carries no export entry point and the native
    * extension table advertises no external-memory family, so an
    * external-handle query reports the format unsupported for that use
    * instead of promising an export route no advertised extension can
    * reach.
    */
   const VkExternalMemoryHandleTypeFlags supported_handles = 0;
   if (external_info && external_info->handleType != 0 &&
       (!(external_info->handleType & supported_handles) ||
        pImageFormatInfo->type != VK_IMAGE_TYPE_2D))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkResult result =
      r3v_get_image_format_properties(device, pImageFormatInfo,
                                         &pImageFormatProperties->imageFormatProperties);
   if (result != VK_SUCCESS)
      return result;

   VkExternalImageFormatProperties *const external_properties =
      vk_find_struct(pImageFormatProperties->pNext,
                     EXTERNAL_IMAGE_FORMAT_PROPERTIES);
   if (external_properties) {
      VkExternalMemoryProperties props = {0};
      if (external_info && (external_info->handleType & supported_handles)) {
         props.externalMemoryFeatures =
            VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
            VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
         props.exportFromImportedHandleTypes = 0;
         props.compatibleHandleTypes = supported_handles;
      }
      external_properties->externalMemoryProperties = props;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceSparseImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount,
   VkSparseImageFormatProperties2 *pProperties)
{
   *pPropertyCount = 0;
}

/* Fallback heap sizes when DRM_RADEON_GEM_INFO reports zero; the query is
 * the kernel's own gart_size / vram_size.  RS482/RS485 is UMA: the GART
 * aperture and the BIOS-carved shared-VRAM partition overlap in physical
 * memory, so a probe needing the exact physical split must treat the two
 * heaps as one shared pool. */
#define R3V_PLACEHOLDER_GTT_HEAP_SIZE     (128ULL * 1024 * 1024)
#define R3V_PLACEHOLDER_VRAM_HEAP_SIZE    ( 64ULL * 1024 * 1024)

VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VkPhysicalDeviceMemoryProperties *const m = &pMemoryProperties->memoryProperties;

   /* DRM_RADEON_GEM_INFO reports gart_size and vram_size, the two kernel
    * pools every native GEM allocation lands in; their sum sizes the one
    * native heap, and r3v_native_memory_properties_fill clamps it to the
    * platform ceiling and lays out the types.
    */
   VK_FROM_HANDLE(r3v_physical_device, pdev, physicalDevice);
   uint64_t heap_bytes =
      R3V_PLACEHOLDER_GTT_HEAP_SIZE + R3V_PLACEHOLDER_VRAM_HEAP_SIZE;
   struct drm_radeon_gem_info gem_info = {0};
   if (drmCommandWriteRead(pdev->render_node_fd, DRM_RADEON_GEM_INFO,
                           &gem_info, sizeof(gem_info)) == 0 &&
       gem_info.gart_size + gem_info.vram_size > 0)
      heap_bytes = gem_info.gart_size + gem_info.vram_size;

   r3v_native_memory_properties_fill(m, heap_bytes);
   return;
}
