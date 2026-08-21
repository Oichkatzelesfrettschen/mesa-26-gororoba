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

#ifdef R3V_NATIVE_BACKEND
#include "r3v_native.h"
#endif

#include "util/disk_cache.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "vk_alloc.h"
#include "vk_enum_defines.h"
#include "vk_format.h"
#include "vk_log.h"
#include "vk_util.h"

/* Header-only pipe enums (PIPE_TEXTURE_*, PIPE_BIND_*) live in
 * pipe/p_defines.h and serve the Gallium format queries alone;
 * PIPE_FORMAT_* values come from util/format/u_formats.h through
 * idep_mesautil, so the native lane compiles with no Gallium include
 * root.  Only the Gallium screen machinery below stays gated. */
#ifndef R3V_NATIVE_BACKEND
#include "pipe/p_defines.h"
#endif

#ifdef R3V_GALLIUM_BACKEND
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
#include <radeon_drm.h>
#include <xf86drm.h>

static bool
r3v_hybrid_compute_enabled(void)
{
   const char *gate = r3v_getenv_compat(R3V_HYBRID_COMPUTE_ENV, "R300VK_HYBRID_COMPUTE_EXPERIMENTAL");
   return gate && strcmp(gate, R3V_HYBRID_COMPUTE_ENV_VALUE) == 0;
}

#ifndef R3V_NATIVE_BACKEND
/* The registry dependency graph makes four advertised extensions invalid
 * at apiVersion 1.0 on this device: VK_KHR_create_renderpass2 requires
 * VK_KHR_multiview, whose mandatory multiview feature needs layered
 * rendering (maxFramebufferLayers is 1 here, so no honest multiview
 * exists); VK_KHR_maintenance5 requires core 1.1;
 * VK_KHR_depth_stencil_resolve sits on create_renderpass2 and
 * VK_KHR_dynamic_rendering on depth_stencil_resolve.  The default
 * surface withholds all four, so every advertised extension satisfies
 * its registry dependencies (dEQP-VK.info.device_extension_dependencies).
 * zink_device_info.py marks create_renderpass2, dynamic_rendering, and
 * maintenance5 required=True, so the zink lane opens the full baseline
 * surface with this exact opt-in and carries the dependency violation as
 * its recorded conformance cost. */
static bool
r3v_zink_baseline_surface_enabled(void)
{
   const char *gate = getenv("R3V_ZINK_BASELINE_SURFACE");
   return gate && strcmp(gate, "1") == 0;
}
#endif

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
   props->maxImageDimension2D = R3V_VK10_MIN_IMAGE_DIMENSION_2D;
   props->maxImageDimension3D = 256;
   props->maxImageDimensionCube = R3V_VK10_MIN_IMAGE_DIMENSION_CUBE;
   props->maxImageArrayLayers = 256;

   /* Texel buffer size: R3xx has no native texel buffer object.  The
    * Vulkan 1.4 minimum is 65536. */
   props->maxTexelBufferElements = 65536;

   /* PS constant store: R300_PFS_PARAM_0..31 yields 32 vec4 slots, or
    * 512 bytes.  The Vulkan minimum maxUniformBufferRange is 16 KiB,
    * so we round up to that bound; the descriptor binding still maps
    * down to the hardware 32 slots. */
   props->maxUniformBufferRange = R3V_VK10_MIN_UNIFORM_BUFFER_RANGE;

   /* SSBO size advertise.  R3xx has no native SSBO; the compute-as-raster
    * substrate maps stores to RB3D color export backed by the radeon GART.
    * Cap the advertised maxStorageBufferRange to 512 MB only when the
    * kernel-reported GART (info.gart_size_kb) is >= 1 GB; otherwise
    * advertise the Vulkan minimum.  Mirrors r300_screen.c's
    * max_shader_buffer_size gate. */
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
   props->maxViewportDimensions[0] = R3V_VK10_MIN_VIEWPORT_DIMENSION;
   props->maxViewportDimensions[1] = R3V_VK10_MIN_VIEWPORT_DIMENSION;
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
   props->maxFramebufferWidth = R3V_VK10_MIN_FRAMEBUFFER_DIMENSION;
   props->maxFramebufferHeight = R3V_VK10_MIN_FRAMEBUFFER_DIMENSION;
   props->maxFramebufferLayers = 1;

   props->framebufferColorSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferDepthSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferStencilSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->framebufferNoAttachmentsSampleCounts =
      R3V_VK10_REQUIRED_SAMPLE_COUNTS;

   /* R300 binds up to four simultaneous colour buffers (COLOROFFSET0..3 /
    * US_OUT_FMT_0..3), and the replay now binds every subpass colour attachment
    * at its own cbuf slot, so four is the honest count and matches the gallium
    * max_render_targets cap.  r300 shares one blend state and colour mask across
    * all cbufs (RB3D_CBLEND), so independentBlend is advertised false; Vulkan
    * then requires every attachment's blend state to be identical, which makes
    * binding pAttachments[0] for all of them spec-correct. */
   props->maxColorAttachments = 4;
   props->sampledImageColorSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
   props->sampledImageDepthSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->sampledImageStencilSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->storageImageSampleCounts = R3V_VK10_REQUIRED_SAMPLE_COUNTS;
   props->maxSampleMaskWords = 1;

   props->timestampComputeAndGraphics = VK_FALSE;
   props->timestampPeriod = 0.0f;

   /* R300 supports 6 user clip planes through R300_VAP_CLIP_CNTL. */
   props->maxClipDistances = 6;
   props->maxCullDistances = 0;
   props->maxCombinedClipAndCullDistances = 6;

   props->discreteQueuePriorities = 2;

#ifdef R3V_NATIVE_BACKEND
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
#else
   props->pointSizeRange[0] = 1.0f;
   props->pointSizeRange[1] = 64.0f;
   props->lineWidthRange[0] = 1.0f;
   props->lineWidthRange[1] = 8.0f;
   props->pointSizeGranularity = 0.125f;
   props->lineWidthGranularity = 0.125f;
#endif

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
#ifdef R3V_NATIVE_BACKEND
   snprintf(props->deviceName, sizeof(props->deviceName), "%s (r3v-native)",
            chip_name);
#else
   snprintf(props->deviceName, sizeof(props->deviceName), "%s", chip_name);
#endif

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
#ifdef R3V_NATIVE_BACKEND
   /* The native ICD coexists with the Gallium-backed ICD under one loader,
    * so each carries a distinct driverName and the deviceName states which
    * implementation the application selected. */
   snprintf(props->driverName, sizeof(props->driverName), "%s", "r3v-native");
   snprintf(props->driverInfo, sizeof(props->driverInfo), "%s",
            "Mesa r3v native (Radeon DRM, private-cell experimental, nonconformant)");
#else
   snprintf(props->driverName, sizeof(props->driverName), "%s", "r3v");
   snprintf(props->driverInfo, sizeof(props->driverInfo), "%s", "Mesa r3v");
#endif
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

#ifndef R3V_NATIVE_BACKEND
static const struct vk_device_extension_table r3v_device_extensions_supported = {
   /* Host-side query reset is a CPU clear of the per-slot query storage
    * (r3v_ResetQueryPool); it needs no GPU-side encoding, so the
    * always-available host queue model supports it directly.
    * r3v_GetDeviceProcAddr maps the promoted core spelling to the same
    * implementation when the extension is enabled on this Vulkan 1.0 device.
    * WSI (VK_KHR_swapchain) and the external-memory family stay withheld until
    * the device layer brings them up. */
   .EXT_host_query_reset = true,
   /* VK_EXT_physical_device_drm: exposes the render/primary node major/minor in
    * VkPhysicalDeviceDrmPropertiesEXT.  Zink's display-device selection matches
    * the EGL DRM fd against these to pick this pdev, so without it
    * zink-on-r3v fails at "choose pdev" before any feature check. */
   .EXT_physical_device_drm = true,
   /* VK_EXT_pci_bus_info: the common WSI's same-GPU check
    * (wsi_device_matches_drm_fd) compares VkPhysicalDevicePCIBusInfoPropertiesEXT
    * against the PCI address behind the DRI3 fd the X server returns.  A zeroed
    * struct mismatches the real device address, wsi_drm_image_needs_buffer_blit
    * then reports a cross-GPU configuration, and every swapchain is routed
    * through the prime-blit buffer path instead of native dma-buf images. */
   .EXT_pci_bus_info = true,
   /* VK_KHR_maintenance2: capability bundle with no new entry points r3v
    * must implement (vk_common provides the input-attachment-aspect and
    * tessellation-domain plumbing; point clipping behaviour is reported in
    * the properties below).  zink lists it in the GL3-era dependency closure
    * alongside image_format_list and depth_stencil_resolve. */
   .KHR_maintenance2 = true,
   /* VK_KHR_image_format_list: validation-time metadata only -- the
    * VkImageFormatListCreateInfo chained on VkImageCreateInfo narrows the
    * MUTABLE view-format set.  r3v creates single-format images and
    * ignores the list, which is a conforming implementation (the list is a
    * promise from the app, not an obligation on the driver). */
   .KHR_image_format_list = true,
   /* VK_KHR_depth_stencil_resolve: with no multisample path every image is
    * single-sample, so the only honest resolve mode set is SAMPLE_ZERO
    * (reported in the properties); requires create_renderpass2, already
    * advertised. */
   .KHR_depth_stencil_resolve = true,
   /* VK_KHR_dynamic_rendering: r3v_CmdBeginRendering / r3v_CmdEndRendering
    * translate VkRenderingInfo into the same colour-attachment framebuffer the
    * render-pass replay drives, so a render target needs no VkRenderPass or
    * VkFramebuffer object. */
   .KHR_dynamic_rendering = true,
   /* VK_KHR_create_renderpass2: r3v_CreateRenderPass2 and the
    * r3v_CmdBeginRenderPass2 / r3v_CmdEndRenderPass2 entry points mirror
    * the 1.0 render-pass path onto r3v's own render-pass object, so the 2.0
    * surface needs no common-runtime emulation. */
   .KHR_create_renderpass2 = true,
   /* VK_KHR_descriptor_update_template: vk_common builds the template object;
    * r3v_UpdateDescriptorSetWithTemplate applies each entry through
    * r3v_UpdateDescriptorSets, reusing the descriptor-write bounds checks. */
   .KHR_descriptor_update_template = true,
   /* VK_KHR_maintenance1: a capabilities extension with no feature struct and a
    * single new entry point, vkTrimCommandPool, which vk_common provides.  Its
    * load-bearing capability for a GL-on-Vulkan client is a negative viewport
    * height (the y-flip): viewport_vk_to_gallium already derives scale[1] and
    * translate[1] from the signed VkViewport::height, so a flipped viewport
    * lands correctly.  The 2D-array-from-3D and 2D/3D copy capabilities are
    * vacuous here because r3v does not expose 3D images. */
   .KHR_maintenance1 = true,
   /* VK_KHR_imageless_framebuffer: r3v_CreateFramebuffer accepts the
    * IMAGELESS flag and stores no views, and r3v_record_begin_render_pass
    * sources the colour view from the VkRenderPassAttachmentBeginInfo at begin
    * time.  Gated by the imagelessFramebuffer feature below. */
   .KHR_imageless_framebuffer = true,
   /* VK_KHR_maintenance5: r3v implements CmdBindIndexBuffer2,
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
    * (r3v_init_wsi): swapchain entry points come from
    * wsi_device_entrypoints, already layered into the device dispatch.
    * Presentation copies the CPU-reachable swapchain image out via xcb-shm. */
   .KHR_swapchain = true,
   /* VK_EXT_extended_dynamic_state: zink's draw path loads
    * vkCmdBindVertexBuffers2 unconditionally and, with the extension present,
    * drives cull/front-face/topology/depth/stencil through vkCmdSet*; all of
    * those record R3V_CMD_SET_DYNAMIC_STATE entries the replay merges. */
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
   /* VK_KHR_bind_memory2: r3v_BindBufferMemory2 / r3v_BindImageMemory2
    * already back the batched bind as the same per-resource pipe_resource bind
    * the 1.0 single-bind entry points perform, in a loop. */
   .KHR_bind_memory2 = true,
   /* VK_KHR_get_memory_requirements2: r3v_GetBufferMemoryRequirements2 and
    * r3v_GetImageMemoryRequirements2 return the size/alignment the 1.0
    * getters compute, and r3v_GetImageSparseMemoryRequirements2 reports a
    * zero count because the device exposes no sparse residency. */
   .KHR_get_memory_requirements2 = true,
   /* VK_KHR_dedicated_allocation: VkMemoryDedicatedAllocateInfo is already
    * honoured at allocation time (r3v_AllocateMemory records dedicated_image
    * for the PRIME export path); the requirements getters now report dedication
    * as required only for an external image whose single SHARED|SCANOUT BO is
    * exported, and never for a suballocatable resource. */
   .KHR_dedicated_allocation = true,
   /* VK_KHR_driver_properties: VkPhysicalDeviceDriverProperties (driverName
    * "r3v", driverInfo, conformanceVersion) is already populated in
    * r3v_physical_device_init_properties, so the query reaches real data. */
   .KHR_driver_properties = true,
   /* VK_KHR_format_feature_flags2: r3v_GetPhysicalDeviceFormatProperties2
    * already fills the chained VkFormatProperties3, the 64-bit-flag form of the
    * same capabilities, so the extension exposes data the driver computes. */
   .KHR_format_feature_flags2 = true,
   /* VK_KHR_uniform_buffer_standard_layout: a std430-style UBO layout is a
    * subset of what the already-advertised EXT_scalar_block_layout accepts; the
    * keystone CONST[0] byte-offset lowering computes the same offsets. */
   .KHR_uniform_buffer_standard_layout = true,
   /* VK_KHR_relaxed_block_layout: a relaxation of std140 alignment that
    * scalarBlockLayout subsumes, so the compiler's offsets are unchanged -- a
    * no-op capability advertisement. */
   .KHR_relaxed_block_layout = true,
   /* VK_KHR_storage_buffer_storage_class: the SPIR-V StorageBuffer storage
    * class the descriptor path already lowers (vulkan_resource_index into the
    * RC_FILE_CONSTANT loads); advertising it adds no new lowering. */
   .KHR_storage_buffer_storage_class = true,
   /* VK_KHR_sampler_mirror_clamp_to_edge: vk_address_mode_to_pipe maps
    * VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE to
    * PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE, which r300_state programs into TX_WRAP;
    * gated by the samplerMirrorClampToEdge feature below. */
   .KHR_sampler_mirror_clamp_to_edge = true,
};
#endif /* R3V_NATIVE_BACKEND */

#ifdef R3V_NATIVE_BACKEND
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
#endif

static void
r3v_physical_device_init_features(struct vk_features *features)
{
   memset(features, 0, sizeof(*features));
#ifdef R3V_NATIVE_BACKEND
   /* The native implementation executes no optional feature; the
    * feature set is the core-1.0 baseline, and each optional bit
    * returns with the native route that makes it true.
    * robustBufferAccess is core 1.0's one mandatory feature, and the
    * native lane keeps its semantics by admission: every buffer range
    * a recorded command names is validated against its binding and
    * memory bound before execution, so no admitted access reaches out
    * of bounds. */
   features->robustBufferAccess = true;
   return;
#endif
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
   /* The dynamic-rendering command path (r3v_CmdBeginRendering) records the
    * colour-attachment framebuffer from VkRenderingInfo, so advertise the
    * feature bit that gates VK_KHR_dynamic_rendering. */
   features->dynamicRendering = true;
   /* Imageless framebuffers carry no views; r3v_CreateFramebuffer records the
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
   /* r300 programs one RB3D_CBLEND blend state and one colour mask shared
    * across all bound cbufs (the gallium r300 blend atom emits state->rt[0]
    * only), so it cannot blend separate attachments differently.  With MRT now
    * reachable (maxColorAttachments == 4) this must be false: Vulkan then
    * requires every colour-blend attachment to be identical, matching what the
    * hardware does.  zink maps this to PIPE_CAP_INDEPENDENT_BLEND_FUNC only
    * (per-draw-buffer blend, GL 3.0+/ARB_draw_buffers_blend), which r300 lacks;
    * EXT_blend_equation_separate is the separate-RGB/alpha cap and is
    * independent of this bit, so dropping it loses no honest GL surface. */
   features->independentBlend = false;
   /* Gates VK_EXT_extended_dynamic_state: zink selects its dynamic-state draw
    * template only when the feature reports true, and the vkCmdSet* family
    * records R3V_CMD_SET_DYNAMIC_STATE replay entries. */
   features->extendedDynamicState = true;
   /* VK_KHR_uniform_buffer_standard_layout: a UBO laid out std430-style is a
    * subset of what the already-true scalarBlockLayout accepts. */
   features->uniformBufferStandardLayout = true;
   /* VK_KHR_sampler_mirror_clamp_to_edge: vk_address_mode_to_pipe maps the wrap
    * mode to PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE, which r300 programs in TX_WRAP. */
   features->samplerMirrorClampToEdge = true;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
r3v_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(r3v_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(pdevice->vk.instance, pName);
}

/* Mesa common WSI in software mode, the lavapipe pattern: sw_device makes the
 * swapchain allocate CPU-reachable images and present through the xcb-shm
 * path, so the radeon winsys needs no dma-buf, modifier, or external-memory
 * support.  wants_linear keeps the swapchain images row-major, the layout the
 * present copy reads and the one r3v's single-tile linear images provide. */
static VkResult
r3v_init_wsi(struct r3v_physical_device *device)
{
   /* GPU-resident present by default: the render-node fd lets the common WSI
    * take the DRM/DRI3 path, presenting the dma-buf-exported scanout images
    * the export substrate provides -- the contract the GL oracle measured.
    * R3V_WSI_SW=1 falls back to the xcb-shm CPU copy, the proven
    * bring-up baseline. */
   const char *wsi_sw_env = r3v_getenv_compat("R3V_WSI_SW", "R300VK_WSI_SW");
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

#ifdef R3V_GALLIUM_BACKEND
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

#ifdef R3V_GALLIUM_BACKEND
   struct pipe_screen_config screen_config = {0};
   device->rws = radeon_drm_winsys_create(render_node_fd, &screen_config,
                                          r300_screen_create);
   if (!device->rws || !device->rws->screen) {
      if (device->rws && device->rws->screen)
         device->rws->screen->destroy(device->rws->screen);
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "r3v: failed to create r300g pipe_screen for '%s'",
                       drm_device->nodes[DRM_NODE_RENDER]);
   }
   device->screen = device->rws->screen;
#endif

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
   /* Source gart_size_kb from the r300 screen's radeon_info so the
    * advertised SSBO ceiling tracks the kernel's actual GART provisioning.
    * Without the gallium backend (loader-only R0 build) the screen is
    * absent; pass 0 so init_limits falls back to the default 128 MB
    * advertise. */
   uint64_t gart_size_kb = 0;
#ifdef R3V_GALLIUM_BACKEND
   if (device->screen)
      gart_size_kb = r300_screen(device->screen)->info.gart_size_kb;
#endif
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

#ifdef R3V_NATIVE_BACKEND
   const struct vk_device_extension_table *supported_extensions =
      &r3v_native_device_extensions_supported;
#else
   /* The registry-invalid four and their feature bits leave the surface
    * together; the gate rationale sits at
    * r3v_zink_baseline_surface_enabled. */
   struct vk_device_extension_table gallium_extensions =
      r3v_device_extensions_supported;
   if (!r3v_zink_baseline_surface_enabled()) {
      gallium_extensions.KHR_create_renderpass2 = false;
      gallium_extensions.KHR_depth_stencil_resolve = false;
      gallium_extensions.KHR_dynamic_rendering = false;
      gallium_extensions.KHR_maintenance5 = false;
      features.dynamicRendering = false;
      features.maintenance5 = false;
   }
   const struct vk_device_extension_table *supported_extensions =
      &gallium_extensions;
#endif
   VkResult result = vk_physical_device_init(&device->vk, &instance->vk,
                                             supported_extensions,
                                             &features, &properties, &dispatch_table);
   if (result != VK_SUCCESS) {
      /* terakan_physical_device_init does not call vk_physical_device_finish
       * on init failure (terakan_physical_device.c fail_isa label); the
       * runtime helper only requires finish after a successful init. */
#ifdef R3V_GALLIUM_BACKEND
      if (device->screen)
         device->screen->destroy(device->screen);
#endif
      close(render_node_fd);
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   device->vk.supported_sync_types = device->sync_types;
   device->hybrid_compute_enabled = r3v_hybrid_compute_enabled();

   result = r3v_init_wsi(device);
   if (result != VK_SUCCESS) {
      vk_physical_device_finish(&device->vk);
#ifdef R3V_GALLIUM_BACKEND
      if (device->screen)
         device->screen->destroy(device->screen);
#endif
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

/* Queue family enumeration.  Advertises one graphics+transfer queue
 * family with one queue.  RS482/RS485 has no native compute dispatch
 * surface, so VK_QUEUE_COMPUTE_BIT is absent unless the hybrid compute
 * experiment is explicitly enabled for CTS/RCA work. */
VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                               uint32_t *pCount,
                                               VkQueueFamilyProperties2 *pProperties)
{
   VK_FROM_HANDLE(r3v_physical_device, pdev, physicalDevice);
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pProperties, pCount);
#ifdef R3V_NATIVE_BACKEND
   /* The public recording surface records the graphics command subset
    * -- render pass, pipeline bind, vertex bind, draw -- on this
    * family, so GRAPHICS is advertised.  Each further bit returns with
    * the recording surface that executes it: COMPUTE returns with the
    * dispatch recording and the CPU compute route, behind the same
    * exact R3V_HYBRID_COMPUTE_EXPERIMENTAL=1 opt-in as the
    * Gallium-backed lane, because the admitted kernel subset stays
    * nonconformant against the full compute contract the bit claims.
    */
   VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT;
   if (pdev->hybrid_compute_enabled)
      queue_flags |= VK_QUEUE_COMPUTE_BIT;
#else
   VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;

   if (pdev->hybrid_compute_enabled)
      queue_flags |= VK_QUEUE_COMPUTE_BIT;
#endif

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p) {
      p->queueFamilyProperties = (VkQueueFamilyProperties){
         .queueFlags = queue_flags,
         .queueCount = 1,
         .timestampValidBits = 0,
         .minImageTransferGranularity = {1, 1, 1},
      };
   }
}

#ifndef R3V_NATIVE_BACKEND
static bool
r3v_screen_supports_format(const struct r3v_physical_device *const device,
                              enum pipe_format format,
                              enum pipe_texture_target target,
                              unsigned bindings)
{
#ifdef R3V_GALLIUM_BACKEND
   return device->screen &&
          device->screen->is_format_supported(device->screen, format, target,
                                              0, 0, bindings);
#else
   return false;
#endif
}
#endif /* R3V_NATIVE_BACKEND */

/* True when a Gallium pipe_screen is attached.  Loader-only builds have no
 * screen, so capabilities that depend on the r300g screen and its SW-TCL draw
 * module must gate on this rather than advertising support no backend provides. */
static bool
r3v_has_gallium_screen(const struct r3v_physical_device *const device)
{
#ifdef R3V_GALLIUM_BACKEND
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
r3v_format_blit_supported(enum pipe_format pipe_format)
{
#ifdef R3V_GALLIUM_BACKEND
   return r300_is_blit_supported(pipe_format);
#else
   (void)pipe_format;
   return false;
#endif
}

static void
r3v_get_format_properties(const struct r3v_physical_device *const device,
                             VkFormat vk_format,
                             VkFormatProperties3 *const properties)
{
   memset(properties, 0, sizeof(*properties));
   properties->sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;

#ifdef R3V_NATIVE_BACKEND
   /* The native backend has no Gallium screen; its capabilities are the
    * public recording surface's accepted subset, advertised exactly so
    * a capability-aware application reaches the qualified route: the
    * linear B8G8R8A8 color target and the F32-family vertex formats the
    * CPU vertex executor gathers.
    */
   switch (vk_format) {
   case VK_FORMAT_B8G8R8A8_UNORM:
      /* The render family's color-attachment grant plus the transfer
       * family's copy grant: the recorded vkCmdCopy* subset executes
       * the transfer features through host mappings at submission.
       */
      properties->linearTilingFeatures =
         VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
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
#else /* R3V_NATIVE_BACKEND */

   const enum pipe_format pipe_format = r3v_vk_format_to_pipe_format(vk_format);
   if (pipe_format == PIPE_FORMAT_NONE)
      return;

   VkFormatFeatureFlags2 image_features = 0;
   VkFormatFeatureFlags2 buffer_features = 0;
   const bool supports_depth_stencil =
      r3v_screen_supports_format(device, pipe_format, PIPE_TEXTURE_2D,
                                    PIPE_BIND_DEPTH_STENCIL);
   const bool supports_sampler_view =
      r3v_screen_supports_format(device, pipe_format, PIPE_TEXTURE_2D,
                                    PIPE_BIND_SAMPLER_VIEW);
   const bool supports_render_target =
      r3v_screen_supports_format(device, pipe_format, PIPE_TEXTURE_2D,
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

      /* r300 exposes depth comparison (shadow) samplers for every depth/stencil
       * format that the sampler can read.  The TX unit's SHADOWCOMP bit enables
       * the per-texel compare against a reference value.  Advertise the
       * VkFormatFeatureFlags2 flag so drivers and CTS agree on which formats
       * support VkSamplerCreateInfo::compareEnable. */
      if (util_format_is_depth_or_stencil(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
   }

   if (supports_render_target) {
      image_features |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;

      if (!util_format_is_pure_integer(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BLEND_BIT;
   }

   /* r3v implements image transfer in both directions on the same r300g tile
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

      if (r3v_format_supports_transfer_dst(pipe_format))
         image_features |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   }

   /* BLIT_SRC/DST advertise the GPU blit (r3v_replay_blit -> pipe->blit ->
    * util_blitter), which scales, filters, and format-casts a region.
    * r300_is_blit_supported gates the resource layouts r300's blitter accepts
    * (plain, S3TC, RGTC).  A blit source is sampled, so BLIT_SRC needs
    * PIPE_BIND_SAMPLER_VIEW; a blit destination is rendered and must also be
    * creatable with VK_IMAGE_USAGE_TRANSFER_DST_BIT, because vkCmdBlitImage2
    * requires that usage on the destination image.  Pairing each bit with the
    * usage path keeps sample-only or CPU-transfer-limited formats out of
    * unreachable BLIT_DST advertisements.  The replay samples the source as a
    * texture, and r3v tiles every optimal image at the sampler cap, so
    * r3v_replay_blit blits a source past the cap one in-cap tile at a time
    * and honors the advertised maxImageDimension2D up to the 4096 floor. */
   if (supports_sampler_view && r3v_format_blit_supported(pipe_format))
      image_features |= VK_FORMAT_FEATURE_2_BLIT_SRC_BIT;
   if (supports_render_target && r3v_format_blit_supported(pipe_format) &&
       r3v_format_supports_transfer_dst(pipe_format))
      image_features |= VK_FORMAT_FEATURE_2_BLIT_DST_BIT;

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
      r3v_screen_supports_format(device, pipe_format, PIPE_BUFFER,
                                    PIPE_BIND_VERTEX_BUFFER) ||
      (r3v_has_gallium_screen(device) &&
       util_format_is_pure_integer(pipe_format) &&
       util_format_get_component_bits(pipe_format,
                                      UTIL_FORMAT_COLORSPACE_RGB, 0) <= 32);
   /* Depth/stencil and sRGB formats carry no buffer features even if r300g can
    * fetch their underlying bits.  The shared helper also keeps texel-buffer
    * features absent until descriptor storage and replay implement them. */
   buffer_features = r3v_format_buffer_features(pipe_format, vertex_fetchable);

   /* VK_IMAGE_TILING_LINEAR is backed by a single PIPE_BIND_LINEAR row-major
    * r300g tile, but only as transfer staging: deqp's draw readback copies an
    * optimal render target into a linear TRANSFER_DST image and maps it.
    * Advertise linear transfer features exactly where r3v_CreateImage's
    * linear accept gate allows the image -- a real image format with a lossless
    * transfer-destination byte layout.  Sampled, color, and depth/stencil
    * features stay optimal-only because the linear tile carries no swizzle. */
   VkFormatFeatureFlags2 linear_features = 0;
   if ((supports_sampler_view || supports_render_target) &&
       r3v_format_supports_transfer_dst(pipe_format)) {
      linear_features = VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                        VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
   }

   properties->linearTilingFeatures = linear_features;
   properties->optimalTilingFeatures = image_features;
   properties->bufferFeatures = buffer_features;
#endif /* R3V_NATIVE_BACKEND */
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

#ifdef R3V_NATIVE_BACKEND
   /* The native image contract carries two 2D flat families with no
    * create flags -- color-attachment usage alone, and transfer usage
    * alone -- so the query reports every other type, flagged, or
    * differently-used request unsupported before the shared type
    * switch, the same refusal vkCreateImage applies.
    */
   const VkImageUsageFlags r3v_native_transfer_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   const bool r3v_native_transfer_query =
      info->usage != 0 &&
      (info->usage & ~r3v_native_transfer_usage) == 0;
   if (info->type != VK_IMAGE_TYPE_2D || info->flags != 0 ||
       (info->usage != VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT &&
        !r3v_native_transfer_query))
      goto unsupported;
#endif

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
#ifdef R3V_NATIVE_BACKEND
      /* The reported ceiling follows the family the usage names --
       * the qualified cell's fixed target for attachment usage, the
       * linear transfer bound for transfer usage -- so vkCreateImage
       * accepts exactly what this query admits for each.
       */
      max_extent =
         r3v_native_transfer_query
            ? (VkExtent3D){ R3V_NATIVE_TRANSFER_DIMENSION_MAX,
                            R3V_NATIVE_TRANSFER_DIMENSION_MAX, 1 }
            : (VkExtent3D){ R3V_NATIVE_TARGET_WIDTH,
                            R3V_NATIVE_TARGET_HEIGHT, 1 };
      max_mip_levels = 1;
      max_array_layers = 1;
      break;
#else
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
      max_mip_levels = util_logbase2(R3V_R3XX_MAX_TEXTURE_DIMENSION) + 1;
      max_array_layers = 1;
      break;
#endif
   case VK_IMAGE_TYPE_3D:
      /* r3v backs every image with a single PIPE_TEXTURE_2D resource of
       * depth0 == 1 (r3v_image_create_tile_resources), so a 3D image's
       * depth slices have no storage and any depth > 1 access reads or writes
       * the wrong slice.  Report VK_IMAGE_TYPE_3D unsupported so this query,
       * vkCreateImage, and the transfer/blit replay agree, and a 3D image test
       * is NotSupported rather than silently incorrect.  This is a deliberate,
       * deferred conformance gap: VK 1.0 requires maxImageDimension3D >= 256,
       * which r3v reports as a device limit but cannot honor per format
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
                              R3V_R3XX_MAX_RENDER_DIMENSION);
      max_extent.height = MIN2(max_extent.height,
                               R3V_R3XX_MAX_RENDER_DIMENSION);
   }

   /* r3v has no multisample path: the image model is single-sample r300g
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
r3v_GetPhysicalDeviceExternalBufferProperties(
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
#ifdef R3V_NATIVE_BACKEND
   /* The native link set carries no export entry point and the native
    * extension table advertises no external-memory family, so an
    * external-handle query reports the format unsupported for that use
    * instead of promising an export route no advertised extension can
    * reach.
    */
   const VkExternalMemoryHandleTypeFlags supported_handles = 0;
#else
   const VkExternalMemoryHandleTypeFlags supported_handles =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
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

/* Fallback heap sizes for the loader-only build (no Gallium oracle) or when the
 * winsys query reports zero.  The Gallium-backed build reports the real sizes
 * from the radeon winsys instead (see r3v_GetPhysicalDeviceMemoryProperties2),
 * which the winsys read via DRM_RADEON_GEM_INFO at creation -- radeon_drm_winsys.c
 * populates info.gart_size_kb / vram_size_kb.  RS482/RS485 is UMA: the GART
 * aperture and the BIOS-carved shared-VRAM partition overlap in physical memory,
 * so a probe needing the exact physical split must treat the two heaps as one
 * shared pool. */
#define R3V_PLACEHOLDER_GTT_HEAP_SIZE     (128ULL * 1024 * 1024)
#define R3V_PLACEHOLDER_VRAM_HEAP_SIZE    ( 64ULL * 1024 * 1024)

VKAPI_ATTR void VKAPI_CALL
r3v_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                          VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   VkPhysicalDeviceMemoryProperties *const m = &pMemoryProperties->memoryProperties;

#ifdef R3V_NATIVE_BACKEND
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
#else
   /* Report the real GART and shared-VRAM sizes when a Gallium r300g oracle is
    * attached.  query_info hands back the radeon_info the winsys cached from
    * DRM_RADEON_GEM_INFO at creation; gart_size_kb / vram_size_kb are the total
    * aperture sizes.  The loader-only build (no rws) keeps the nominal fallbacks. */
   uint64_t gtt_bytes  = R3V_PLACEHOLDER_GTT_HEAP_SIZE;
   uint64_t vram_bytes = R3V_PLACEHOLDER_VRAM_HEAP_SIZE;
#ifdef R3V_GALLIUM_BACKEND
   VK_FROM_HANDLE(r3v_physical_device, pdev, physicalDevice);
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
    * r3v_queue_driver_submit, which copies each host map into its bound
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
#endif /* R3V_NATIVE_BACKEND */
}
