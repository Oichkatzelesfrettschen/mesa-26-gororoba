/*
 * SPDX-License-Identifier: MIT
 */

#include "r300vk_physical_device.h"
#include "r300vk_format.h"
#include "r300vk_cpu_sync.h"

#include "r300vk_entrypoints.h"
#include "r300vk_instance.h"
#include "r300vk_private.h"

#include "util/disk_cache.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "vk_alloc.h"
#include "vk_enum_defines.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

#ifdef R300VK_GALLIUM_BACKEND
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "r300/r300_context.h"
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
#include <sys/stat.h>
#include <sys/sysmacros.h>
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
    * Where the RS482 path has no single native 4096-wide render surface,
    * r300vk presents the Vulkan floor through a 2560 hardware-backed span plus
    * a residual span.  Native r300g resources remain the fast path for images
    * that fit in one span. */
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

   /* The Vulkan 1.0 floor.  r300_set_framebuffer_state refuses anything
    * wider or taller than 2560, but reporting 2560 here breaks zink's
    * surfaceless default-framebuffer completeness (measured: every GL FBO
    * context fails "Framebuffer is not complete" with 2560 advertised), so
    * the floor stays and the replay clamps recorded render areas to the
    * silicon cap instead -- the stale-zsbuf clear crash is prevented by the
    * clamp, and an oversize bind degrades to the r300g refusal warning
    * rather than corrupting state. */
   props->maxFramebufferWidth = R300VK_VK10_MIN_FRAMEBUFFER_DIMENSION;
   props->maxFramebufferHeight = R300VK_VK10_MIN_FRAMEBUFFER_DIMENSION;
   props->maxFramebufferLayers = 1;

   props->framebufferColorSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferDepthSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferStencilSampleCounts = R300VK_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferNoAttachmentsSampleCounts =
      R300VK_VK10_REQUIRED_SAMPLE_COUNTS;

   /* The replay's framebuffer state binds colour attachment 0 only, so one is
    * the honest count (four over-promised what the replay never delivered).
    * One attachment also makes independentBlend vacuously true: r300 shares a
    * single blend state across its MRT cbufs, so raising this past one again
    * requires either real per-attachment blend handling in the replay or
    * dropping independentBlend. */
   props->maxColorAttachments = 1;
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
}

static const struct vk_device_extension_table r300vk_device_extensions_supported = {
   /* Host-side query reset is a CPU clear of the per-slot query storage
    * (r300vk_ResetQueryPool); it needs no GPU-side encoding, so the
    * always-available host queue model supports it directly.  Advertising it
    * also exposes the core vkResetQueryPool entrypoint on this Vulkan 1.0
    * device.  WSI (VK_KHR_swapchain) and the external-memory family stay
    * withheld until the device layer brings them up. */
   .EXT_host_query_reset = true,
   /* VK_EXT_physical_device_drm: exposes the render/primary node major/minor in
    * VkPhysicalDeviceDrmPropertiesEXT.  Zink's display-device selection matches
    * the EGL DRM fd against these to pick this pdev, so without it
    * zink-on-r300vk fails at "choose pdev" before any feature check. */
   .EXT_physical_device_drm = true,
   /* VK_EXT_pci_bus_info: the common WSI's same-GPU check
    * (wsi_device_matches_drm_fd) compares VkPhysicalDevicePCIBusInfoPropertiesEXT
    * against the PCI address behind the DRI3 fd the X server returns.  A zeroed
    * struct mismatches the real device address, wsi_drm_image_needs_buffer_blit
    * then reports a cross-GPU configuration, and every swapchain is routed
    * through the prime-blit buffer path instead of native dma-buf images. */
   .EXT_pci_bus_info = true,
   /* VK_KHR_maintenance2: capability bundle with no new entry points r300vk
    * must implement (vk_common provides the input-attachment-aspect and
    * tessellation-domain plumbing; point clipping behaviour is reported in
    * the properties below).  zink lists it in the GL3-era dependency closure
    * alongside image_format_list and depth_stencil_resolve. */
   .KHR_maintenance2 = true,
   /* VK_KHR_image_format_list: validation-time metadata only -- the
    * VkImageFormatListCreateInfo chained on VkImageCreateInfo narrows the
    * MUTABLE view-format set.  r300vk creates single-format images and
    * ignores the list, which is a conforming implementation (the list is a
    * promise from the app, not an obligation on the driver). */
   .KHR_image_format_list = true,
   /* VK_KHR_depth_stencil_resolve: with no multisample path every image is
    * single-sample, so the only honest resolve mode set is SAMPLE_ZERO
    * (reported in the properties); requires create_renderpass2, already
    * advertised. */
   .KHR_depth_stencil_resolve = true,
   /* VK_KHR_dynamic_rendering: r300vk_CmdBeginRendering / r300vk_CmdEndRendering
    * translate VkRenderingInfo into the same colour-attachment framebuffer the
    * render-pass replay drives, so a render target needs no VkRenderPass or
    * VkFramebuffer object. */
   .KHR_dynamic_rendering = true,
   /* VK_KHR_create_renderpass2: r300vk_CreateRenderPass2 and the
    * r300vk_CmdBeginRenderPass2 / r300vk_CmdEndRenderPass2 entry points mirror
    * the 1.0 render-pass path onto r300vk's own render-pass object, so the 2.0
    * surface needs no common-runtime emulation. */
   .KHR_create_renderpass2 = true,
   /* VK_KHR_descriptor_update_template: vk_common builds the template object;
    * r300vk_UpdateDescriptorSetWithTemplate applies each entry through
    * r300vk_UpdateDescriptorSets, reusing the descriptor-write bounds checks. */
   .KHR_descriptor_update_template = true,
   /* VK_KHR_maintenance1: a capabilities extension with no feature struct and a
    * single new entry point, vkTrimCommandPool, which vk_common provides.  Its
    * load-bearing capability for a GL-on-Vulkan client is a negative viewport
    * height (the y-flip): viewport_vk_to_gallium already derives scale[1] and
    * translate[1] from the signed VkViewport::height, so a flipped viewport
    * lands correctly.  The 2D-array-from-3D and 2D/3D copy capabilities are
    * vacuous here because r300vk does not expose 3D images. */
   .KHR_maintenance1 = true,
   /* VK_KHR_imageless_framebuffer: r300vk_CreateFramebuffer accepts the
    * IMAGELESS flag and stores no views, and r300vk_record_begin_render_pass
    * sources the colour view from the VkRenderPassAttachmentBeginInfo at begin
    * time.  Gated by the imagelessFramebuffer feature below. */
   .KHR_imageless_framebuffer = true,
   /* VK_KHR_maintenance5: r300vk implements CmdBindIndexBuffer2,
    * GetImageSubresourceLayout2, and GetDeviceImageSubresourceLayout, and
    * vk_common provides GetRenderingAreaGranularity.  The maintenance5 property
    * query reports the conservative (all-false) rasterization capabilities. */
   .KHR_maintenance5 = true,
   /* VK_KHR_timeline_semaphore: a timeline emulated on the GPU-fence-backed
    * binary cpu_sync (sync_types[1]); the GetSemaphoreCounterValue /
    * WaitSemaphores / SignalSemaphore entry points come from vk_common. */
   .KHR_timeline_semaphore = true,
   /* VK_EXT_robustness2 for nullDescriptor: the descriptor replay skips a
    * VK_NULL_HANDLE view/buffer, leaving the unit unbound (which reads zero on
    * the r300 PFS), so a null descriptor is safe to access.  robustBufferAccess2
    * and robustImageAccess2 stay false. */
   .EXT_robustness2 = true,
   /* VK_KHR_swapchain through Mesa's common WSI in software mode
    * (r300vk_init_wsi): swapchain entry points come from
    * wsi_device_entrypoints, already layered into the device dispatch.
    * Presentation copies the CPU-reachable swapchain image out via xcb-shm. */
   .KHR_swapchain = true,
   /* VK_EXT_extended_dynamic_state: zink's draw path loads
    * vkCmdBindVertexBuffers2 unconditionally and, with the extension present,
    * drives cull/front-face/topology/depth/stencil through vkCmdSet*; all of
    * those record R300VK_CMD_SET_DYNAMIC_STATE entries the replay merges. */
   .EXT_extended_dynamic_state = true,
   /* VK_EXT_scalar_block_layout: the descriptor-UBO and push-constant paths
    * load CONST[0] by byte offset through the keystone lowering, so packing
    * tighter than std140 changes only the offsets the compiler already
    * computes.  zink lists the feature among its base requirements. */
   .EXT_scalar_block_layout = true,
   /* VK_EXT_depth_clip_enable: r300's clip block is programmed through the
    * rasterizer CSO's depth_clip_near/far, captured in the pipeline rs
    * template; the create-time parse maps the extension's explicit
    * depthClipEnable onto them.  zink emits a per-draw warning without it. */
   .EXT_depth_clip_enable = true,
   /* VK_EXT_custom_border_color: r300 keeps a full per-sampler border colour
    * (TX_BORDER_COLOR); the sampler CSO carries it through
    * pipe_sampler_state.border_color, format-independent. */
   .EXT_custom_border_color = true,
   /* VK_EXT_line_rasterization: Bresenham (plus GL line-stipple hardware for
    * the stippled variant) is what the silicon draws; rectangular and smooth
    * modes stay false. */
   .EXT_line_rasterization = true,
   /* dma-buf export, the wsi-drm substrate: external images are single-tile
    * SHARED|SCANOUT linear BOs and vkGetMemoryFdKHR exports them through the
    * winsys PRIME path -- the contract the r300g/GL DRI3 oracle measured
    * (export once per swapchain image, then only CS per frame). */
   .KHR_external_memory = true,
   .KHR_external_memory_fd = true,
   .EXT_external_memory_dma_buf = true,
};

static void
r300vk_physical_device_init_features(struct vk_features *features)
{
   memset(features, 0, sizeof(*features));
   features->robustBufferAccess = true;
   features->scalarBlockLayout = true;
   /* Logic ops run in r300's ROP unit (RB3D_ROPCNTL); the blend CSO carries
    * logicop_enable/logicop_func and r300g programs the ROP3 code. */
   features->logicOp = true;
   features->customBorderColors = true;
   features->customBorderColorWithoutFormat = true;
   features->bresenhamLines = true;
   features->stippledBresenhamLines = true;
   features->depthClipEnable = true;
   features->largePoints = true;
   features->wideLines = true;
   features->samplerAnisotropy = true;
   /* r300's ZPASS occlusion counter returns an exact sample count, so a precise
    * occlusion query is what the replay records (basic occlusion queries need no
    * feature bit).  Pipeline-statistics queries stay unsupported. */
   features->occlusionQueryPrecise = true;
   /* Host query reset clears the per-slot CPU query storage; the serialized
    * replay queue retires all prior work before the host call returns, so the
    * clear never races a GPU-side query write. */
   features->hostQueryReset = true;
   /* The dynamic-rendering command path (r300vk_CmdBeginRendering) records the
    * colour-attachment framebuffer from VkRenderingInfo, so advertise the
    * feature bit that gates VK_KHR_dynamic_rendering. */
   features->dynamicRendering = true;
   /* Imageless framebuffers carry no views; r300vk_CreateFramebuffer records the
    * IMAGELESS flag and the begin path reads the views from the
    * VkRenderPassAttachmentBeginInfo, so advertise the gating feature. */
   features->imagelessFramebuffer = true;
   /* The maintenance5 entry points (CmdBindIndexBuffer2, the two subresource
    * layout getters) are implemented, so advertise the gating feature. */
   features->maintenance5 = true;
   /* Timeline semaphores are emulated on the binary cpu_sync (sync_types[1]);
    * zink_screen requires this feature or feats12.timelineSemaphore. */
   features->timelineSemaphore = true;
   /* nullDescriptor: the descriptor replay tolerates a VK_NULL_HANDLE binding
    * (skipped, unit left unbound -> reads zero on r300).  zink_screen rejects a
    * device without it. */
   features->nullDescriptor = true;
   /* With maxColorAttachments == 1 there is no second attachment to blend
    * differently, so per-attachment-independent blend state is vacuously
    * satisfied.  zink keys GL's EXT_blend_equation_separate (a GL 2.0
    * requirement) on this feature, and separate RGB/alpha blend equations on
    * the single attachment are core Vulkan pipeline state the r300 RB3D
    * blend hardware implements. */
   features->independentBlend = true;
   /* Gates VK_EXT_extended_dynamic_state: zink selects its dynamic-state draw
    * template only when the feature reports true, and the vkCmdSet* family
    * records R300VK_CMD_SET_DYNAMIC_STATE replay entries. */
   features->extendedDynamicState = true;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r300vk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(r300vk_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->vk.instance, pName);
}

/* Mesa common WSI in software mode, the lavapipe pattern: sw_device makes the
 * swapchain allocate CPU-reachable images and present through the xcb-shm
 * path, so the radeon winsys needs no dma-buf, modifier, or external-memory
 * support.  wants_linear keeps the swapchain images row-major, the layout the
 * present copy reads and the one r300vk's single-tile linear images provide. */
static VkResult
r300vk_init_wsi(struct r300vk_physical_device *device)
{
   /* GPU-resident present by default: the render-node fd lets the common WSI
    * take the DRM/DRI3 path, presenting the dma-buf-exported scanout images
    * the export substrate provides -- the contract the GL oracle measured.
    * R300VK_WSI_SW=1 falls back to the xcb-shm CPU copy, the proven
    * bring-up baseline. */
   const char *wsi_sw_env = getenv("R300VK_WSI_SW");
   const bool wsi_sw = wsi_sw_env && wsi_sw_env[0] == '1';
   VkResult result =
      wsi_device_init(&device->wsi_device,
                      r300vk_physical_device_to_handle(device),
                      r300vk_wsi_proc_addr,
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
r300vk_finish_wsi(struct r300vk_physical_device *device)
{
   if (device->vk.wsi_device == NULL)
      return;
   device->vk.wsi_device = NULL;
   wsi_device_finish(&device->wsi_device, &device->vk.instance->alloc);
}

void
r300vk_physical_device_destroy(struct vk_physical_device *const device_base)
{
   struct r300vk_physical_device *const device =
      container_of(device_base, struct r300vk_physical_device, vk);

   r300vk_finish_wsi(device);

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

   /* Slot 0 is the GPU-fence-backed binary cpu_sync; slot 1 is a timeline
    * emulated on it (radeon has no DRM_CAP_SYNCOBJ, so no hardware timeline).
    * vk_sync_timeline_get_type returns the wrapper type by value, so it is
    * stored on the physical device and the table points at its embedded sync. */
   device->timeline_sync_type = vk_sync_timeline_get_type(&r300vk_cpu_sync_type);
   device->sync_types[0] = &r300vk_cpu_sync_type;
   device->sync_types[1] = &device->timeline_sync_type.sync;
   device->sync_types[2] = NULL;

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
    * r300vk_open_radeon_render_node, so businfo.pci is always populated. */
   properties.pciDomain   = drm_device->businfo.pci->domain;
   properties.pciBus      = drm_device->businfo.pci->bus;
   properties.pciDevice   = drm_device->businfo.pci->dev;
   properties.pciFunction = drm_device->businfo.pci->func;

   /* Driver entrypoints first, then the WSI surface queries
    * (vkGetPhysicalDeviceSurfaceSupportKHR and friends live at
    * physical-device level in wsi_physical_device_entrypoints; without this
    * overlay zink's kopper_CreateSurface calls a NULL pointer right after
    * surface creation).  vk_physical_device_init merges
    * vk_common_physical_device_entrypoints itself at
    * src/vulkan/runtime/vk_physical_device.c:53-55. */
   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &r300vk_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                      &wsi_physical_device_entrypoints, false);

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

   result = r300vk_init_wsi(device);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&device->vk);
#ifdef R300VK_GALLIUM_BACKEND
      if (device->screen)
         device->screen->destroy(device->screen);
#endif
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

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

/* True when a Gallium pipe_screen is attached.  Loader-only builds have no
 * screen, so capabilities that depend on the r300g screen and its SW-TCL draw
 * module must gate on this rather than advertising support no backend provides. */
static bool
r300vk_has_gallium_screen(const struct r300vk_physical_device *const device)
{
#ifdef R300VK_GALLIUM_BACKEND
   return device->screen != NULL;
#else
   (void)device;
   return false;
#endif
}

/* True when r300's blitter (util_blitter, reached through pipe->blit) accepts
 * the format's resource layout.  Wraps the r300g predicate so the loader-only
 * build, which has no Gallium blitter, advertises no BLIT feature. */
static bool
r300vk_format_blit_supported(enum pipe_format pipe_format)
{
#ifdef R300VK_GALLIUM_BACKEND
   return r300_is_blit_supported(pipe_format);
#else
   (void)pipe_format;
   return false;
#endif
}

static void
r300vk_get_format_properties(const struct r300vk_physical_device *const device,
                             VkFormat vk_format,
                             VkFormatProperties3 *const properties)
{
   memset(properties, 0, sizeof(*properties));
   properties->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;

   const enum pipe_format pipe_format = r300vk_vk_format_to_pipe_format(vk_format);
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
    * lossless byte layout. */
   if (supports_depth_stencil || supports_sampler_view || supports_render_target) {
      image_features |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;

      if (r300vk_format_supports_transfer_dst(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   }

   /* BLIT_SRC/DST advertise the GPU blit (r300vk_replay_blit -> pipe->blit ->
    * util_blitter), which scales, filters, and format-casts a region.
    * r300_is_blit_supported gates the resource layouts r300's blitter accepts
    * (plain, S3TC, RGTC).  A blit source is sampled, so BLIT_SRC needs
    * PIPE_BIND_SAMPLER_VIEW; a blit destination is rendered, so BLIT_DST needs
    * PIPE_BIND_RENDER_TARGET.  Pairing each bit with its bind keeps a
    * sample-only format (a compressed layout, which r300 cannot render to)
    * BLIT_SRC without falsely advertising BLIT_DST.  The replay samples the
    * source as a texture, and r300vk tiles every optimal image at the sampler
    * cap, so r300vk_replay_blit blits a source past the cap one in-cap tile at a
    * time and honors the advertised maxImageDimension2D up to the 4096 floor. */
   if (supports_sampler_view && r300vk_format_blit_supported(pipe_format))
      image_features |= VK_FORMAT_FEATURE_2_BLIT_SRC_BIT;
   if (supports_render_target && r300vk_format_blit_supported(pipe_format))
      image_features |= VK_FORMAT_FEATURE_2_BLIT_DST_BIT;

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
    * legacy GL path never exposed integer attributes), so admit a pure-integer
    * format the draw module can fetch.  Two bounds keep the override honest: it
    * needs a live Gallium screen (a loader-only build has no draw module, so no
    * software fetch), and the draw translator rewrites integer inputs to
    * R32G32B32A32_*, so a component wider than 32 bits (R64_*) has no fetch path
    * and must not be advertised. */
   const bool vertex_fetchable =
      r300vk_screen_supports_format(device, pipe_format, PIPE_BUFFER,
                                    PIPE_BIND_VERTEX_BUFFER) ||
      (r300vk_has_gallium_screen(device) &&
       util_format_is_pure_integer(pipe_format) &&
       util_format_get_component_bits(pipe_format,
                                      UTIL_FORMAT_COLORSPACE_RGB, 0) <= 32);
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

   /* VK_IMAGE_TILING_LINEAR is backed by a single PIPE_BIND_LINEAR row-major
    * r300g tile, but only as transfer staging: deqp's draw readback copies an
    * optimal render target into a linear TRANSFER_DST image and maps it.
    * Advertise linear transfer features exactly where r300vk_CreateImage's
    * linear accept gate allows the image -- a real image format with a lossless
    * transfer-destination byte layout.  Sampled, color, and depth/stencil
    * features stay optimal-only because the linear tile carries no swizzle. */
   VkFormatFeatureFlags2 linear_features = 0;
   if ((supports_sampler_view || supports_render_target) &&
       r300vk_format_supports_transfer_dst(pipe_format)) {
      linear_features = VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   }

   properties->linearTilingFeatures = linear_features;
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
      /* The flat multi-tile reach.  Usage-keyed extents were measured to
       * break zink's surfaceless framebuffer (its internal sampled-usage
       * probe at the device dimension must succeed), so the dimension stays
       * uniform and the mip ceiling reflects the real vkCreateImage
       * acceptance: chains live on one tile, so 12 levels (2048-base). */
      max_extent = (VkExtent3D){
         device->vk.properties.maxImageDimension2D,
         device->vk.properties.maxImageDimension2D,
         1,
      };
      max_mip_levels = util_logbase2(R300VK_R3XX_MAX_TEXTURE_DIMENSION) + 1;
      max_array_layers = 1;
      break;
   case VK_IMAGE_TYPE_3D:
      /* r300vk backs every image with a single PIPE_TEXTURE_2D resource of
       * depth0 == 1 (r300vk_image_create_tile_resources), so a 3D image's
       * depth slices have no storage and any depth > 1 access reads or writes
       * the wrong slice.  Report VK_IMAGE_TYPE_3D unsupported so this query,
       * vkCreateImage, and the transfer/blit replay agree, and a 3D image test
       * is NotSupported rather than silently incorrect.  This is a deliberate,
       * deferred conformance gap: VK 1.0 requires maxImageDimension3D >= 256,
       * which r300vk reports as a device limit but cannot honor per format
       * until a real 3D resource path exists. */
      goto unsupported;
   default:
      goto unsupported;
   }

   /* A linear image is one row-major r300g tile, so its extent is bounded by
    * the single-tile limit vkCreateImage's linear accept gate enforces.
    * Report the same bound here so the two stay one contract. */
   if (info->tiling == VK_IMAGE_TILING_LINEAR) {
      max_extent.width = MIN2(max_extent.width,
                              R300VK_R3XX_MAX_RENDER_DIMENSION);
      max_extent.height = MIN2(max_extent.height,
                               R300VK_R3XX_MAX_RENDER_DIMENSION);
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

VKAPI_ATTR void VKAPI_CALL
r300vk_GetPhysicalDeviceExternalBufferProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
   VkExternalBufferProperties *pExternalBufferProperties)
{
   /* Buffer export is not wired; images carry the dma-buf path (their BOs are
    * real winsys resources, while buffers ride the CPU-replay storage model).
    * Zeroed properties is the spec's "unsupported handle type" answer. */
   pExternalBufferProperties->externalMemoryProperties =
      (VkExternalMemoryProperties){0};
}

VKAPI_ATTR VkResult VKAPI_CALL
r300vk_GetPhysicalDeviceImageFormatProperties2(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
   VkImageFormatProperties2 *pImageFormatProperties)
{
   VK_FROM_HANDLE(r300vk_physical_device, device, physicalDevice);

   /* dma-buf (and the opaque-fd alias of the same PRIME fd) is exportable for
    * 2D images; every other handle type stays rejected.  Import is accepted
    * only as the round-trip of an exported BO (dedicated allocations). */
   const VkPhysicalDeviceExternalImageFormatInfo *external_info =
      vk_find_struct_const(pImageFormatInfo->pNext,
                           PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   const VkExternalMemoryHandleTypeFlags supported_handles =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   if (external_info && external_info->handleType != 0 &&
       (!(external_info->handleType & supported_handles) ||
        pImageFormatInfo->type != VK_IMAGE_TYPE_2D))
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
   /* This is a UMA integrated part with no GPU/CPU cache coherency in
    * hardware.  The radeon kernel disables GART snooping globally
    * (rs400_gart_enable writes AGP_MODE_CNTL REQ_TYPE_SNOOP_DIS) and marks
    * every GART page unsnooped, the K8 host core reports no self-snoop, and
    * the K8 NPT Family 0Fh host does not support atomic read-modify-write
    * HyperTransport commands.  HOST_COHERENT is honest only because the
    * driver never allows concurrent CPU and GPU access to the same memory:
    * all GPU work for a submit runs inside one synchronous
    * r300vk_queue_driver_submit, which copies each host map into its bound
    * resource at the submit entry and back at the fence.  The coherency
    * promise is kept by serialization and explicit copy, not by snoop. */
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
