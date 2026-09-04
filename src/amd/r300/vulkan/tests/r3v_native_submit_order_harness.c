/*
 * SPDX-License-Identifier: MIT
 *
 * Submit-order matrix on the drm-shim fixture: one public
 * render-pass/pipeline/draw recording with a load-op clear over a seeded
 * target, submitted once per arm on a fresh device.  The armed arm is the
 * positive control (the shim absorbs the CS ioctl; the draw executes and
 * the one-shot token is spent).  Every pre-commit failure -- closed gate,
 * injected retention failure, refused authorization, failed vertex mapping
 * -- leaves the target and the carrier byte-identical to their pre-submit
 * content and spends no authorization; the two transport failures --
 * refused CS ioctl, failed completion wait -- come after the draw, so the
 * draw has executed exactly once and the token is spent.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#define VK_NO_PROTOTYPES
#include "r3v_native.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_shim_arming.h"
#include "amd/r300/common/r300_r2vb_fetched_producer.h"
#include "amd/r300/common/r300_r2vb_producer_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"
#include "amd/r300/common/tests/r300_fetched_route_digests.h"
#include "amd/r300/common/tests/r300_retained_route_digests.h"
#include "amd/r300/common/tests/r300_varying_cell_digests.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_ioctl.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

#define CLEAR_SENTINEL ((float)0xa5 / 255.0f)
#define COLOR_SEED 0x5c5c5c5cu

/* The reference triangle in NDC: over the 64x64 target the viewport
 * transform maps it byte-exactly onto the window-space reference
 * payload. */
static const float ndc_triangle[12] = {
   -0.75f, -0.75f, 0.0f, 1.0f,
    0.75f, -0.75f, 0.0f, 1.0f,
    0.00f,  0.75f, 0.0f, 1.0f,
};

enum arm {
   ARM_ARMED,
   ARM_GATE_CLOSED,
   ARM_RETENTION_FAILURE,
   ARM_AUTHORIZATION_REFUSED,
   ARM_MAP_FAILURE,
   ARM_IOCTL_REFUSED,
   ARM_COMPLETION_FAILURE,
   /* Known-bad: the deferred draw executed ahead of a refused submit --
    * the ordering this harness refuses -- so the untouched verdicts of
    * the gate-closed arm must fail. */
   ARM_KNOWN_BAD_PREMATURE_DRAW,
   /* Robust buffer access over a two-record vertex buffer under the
    * three-vertex draw: enabled, the F32_3 attribute's third record
    * reads (0, 0, 0, 1) and the armed submit delivers; enabled with an
    * F32_4 attribute, the third record reads the homogeneous zero
    * position and clipping emits only degenerate records; disabled, the
    * record-time bound proof refuses the recording. */
   ARM_ROBUST_OOB_ENABLED,
   ARM_ROBUST_OOB_W0_DEGENERATE_ARMED,
   ARM_ROBUST_OOB_DISABLED,
   /* The fetched GPU-producer route under the three exact gates, over
    * the reference window-space records: composed, the submit-time IB
    * equals the offline no-submit composition byte for byte (the arming
    * gate compares the two digests), the reference list binds four BOs,
    * the slot BO holds the slot positions, and the shim -- which executes
    * no producer -- leaves the carrier poisoned, so the read-back verdict
    * reports device loss and quarantines the capability; with the
    * composition refused by injection the recording, references,
    * carrier, and slot allocation stay exactly as recorded; with the
    * immediate route's retained digest declared, the gate refuses the
    * composed stream as a bundle mismatch, the fetched stream being a
    * distinct cell. */
   ARM_GPU_FETCHED_COMPOSED,
   /* The same composed arm over the narrower source widths: the records
    * are the reference triangle's leading components at the width's
    * record size as stride and attribute format, the fetch swizzle
    * restores z = 0 and w = 1, and the submit-time IB equals that width's
    * offline composition identity, distinct from the other widths'. */
   ARM_GPU_FETCHED_COMPOSED_F32_3,
   ARM_GPU_FETCHED_COMPOSED_F32_2,
   ARM_GPU_FETCHED_COMPOSE_FAILURE,
   ARM_GPU_FETCHED_WRONG_DIGEST,
   /* The fetched admission's named refusals, each before any write: a
    * bind offset the VBPNTR pointer cannot carry (2), a binding stride
    * the stride field cannot carry (18), a record outside the FP24
    * fixed-point domain (the NDC triangle's negative components), a
    * two-record buffer under the three-vertex draw with robustBufferAccess
    * on (the fetched route has no zero-substitution form), and a source
    * bound into the color target's memory (one relocation entry would
    * fold two roles). */
   ARM_GPU_FETCHED_OFFSET_MISALIGNED,
   ARM_GPU_FETCHED_STRIDE_MISALIGNED,
   ARM_GPU_FETCHED_OUT_OF_DOMAIN,
   ARM_GPU_FETCHED_OUT_OF_BOUNDS,
   ARM_GPU_FETCHED_ALIASED_SOURCE,
   /* The computed-varying pipeline on the CPU route: the varying vertex
    * and pass-through fragment modules record the varying cell (its
    * digest the declared authorization), the deferred draw writes
    * eight-dword records -- the transformed reference positions with
    * the computed tint -- and the armed submit delivers; a varying
    * vertex module bound with the constant fragment module, and the
    * position-only vertex module bound with the pass-through fragment
    * module, each refuse at pipeline creation. */
   ARM_VARYING_ARMED,
   ARM_VARYING_FRAGMENT_MISMATCH,
   ARM_VARYING_MISSING,
   /* The sampled pipeline on the CPU route: the varying vertex module
    * with the sampled fragment module over a set-0 combined image
    * sampler, recording the sampled cell with the texture BO on its
    * third relocation. */
   ARM_SAMPLED_ARMED,
   /* The same pipeline over a three-layer texture whose view selects
    * the last layer: the recorded cell's TX_OFFSET_0 carries that
    * layer's stride, so the stream differs from the layer-zero arm by
    * exactly the addressed layer. */
   ARM_SAMPLED_LAYER_ARMED,
   /* The same pipeline over a VK_IMAGE_VIEW_TYPE_2D_ARRAY view of the
    * same texture: the view creates, since an object_management case
    * that names an array view never samples through it, and the draw
    * refuses, since the TX program addresses one slice through the
    * TX_OFFSET_0 stride the view resolved at creation and no route
    * indexes a slice from the shader coordinate. */
   ARM_SAMPLED_ARRAY_VIEW_REFUSED,
   /* The two-attribute module (location 0 position, location 1 color
    * passed to the varying) on the CPU route: over two bindings (F32_4
    * positions at stride 16, F32_3 colors at stride 12) and over one
    * interleaved binding (32-byte records, the color at offset 16),
    * each records the varying cell and delivers the varying reference
    * carrier; a color attribute whose record crosses its binding's
    * stride refuses at pipeline creation; a color binding left unbound
    * and a color buffer bound into the pass target's footprint each
    * refuse at draw recording; and the fetched producer gates over the
    * two-binding pipeline refuse at admission, the route binding one
    * source relocation role. */
   ARM_MULTI_ATTRIBUTE_ARMED,
   ARM_MULTI_ATTRIBUTE_INTERLEAVED_ARMED,
   ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED,
   ARM_MULTI_ATTRIBUTE_UNBOUND_REFUSED,
   ARM_MULTI_ATTRIBUTE_ALIAS_TARGET_REFUSED,
   ARM_MULTI_ATTRIBUTE_FETCHED_REFUSED,
   /* Indexed draws on the CPU route: three UINT16 indices 0, 1, 2
    * deliver the reference carrier; UINT32 indices read from the second
    * entry of the index buffer under base vertex -3 over a permuted
    * vertex buffer dereference, sum, and deliver the reference carrier
    * in the index order; with robustBufferAccess on and restart disabled
    * on the pipeline, the UINT16 restart value 0xffff is an ordinary
    * index past the bound, so the F32_3 record reads (0, 0, 0, 1) and
    * the draw delivers the robust carrier; restart enabled on the
    * pipeline refuses at creation; an index past the bound with the
    * feature off can still address zero-initialized bytes inside the
    * bound vertex buffer, producing the homogeneous zero position and
    * only degenerate records; an index range past the index buffer, an
    * unbound index buffer, a UINT8 index type, and an index buffer bound
    * into the pass target's footprint each refuse at recording; and the
    * fetched gates over an indexed draw refuse at admission, the producer
    * routes fetching one linear source range. */
   ARM_INDEXED_ARMED,
   ARM_INDEXED_PERMUTED_ARMED,
   ARM_INDEXED_ROBUST_RESTART_ARMED,
   ARM_INDEXED_RESTART_ENABLED_REFUSED,
   ARM_INDEXED_ZERO_RECORD_DEGENERATE_ARMED,
   ARM_INDEXED_RANGE_REFUSED,
   ARM_INDEXED_UNBOUND_REFUSED,
   ARM_INDEXED_UINT8_REFUSED,
   ARM_INDEXED_ALIAS_TARGET_REFUSED,
   ARM_INDEXED_FETCHED_REFUSED,
   /* Instanced draws on the CPU route.  The instance-offset module
    * (location-1 vec4 over an instance-rate binding) under two
    * instances records the two-triangle cell family member and delivers
    * six records, each instance's triangle translated by its own
    * offset record; under one instance from firstInstance 1 it records
    * the reference cell and delivers the triangle translated by record
    * 1 (the Vulkan fetch reads first_instance); the instance-index
    * module under two instances from firstInstance 3 observes
    * InstanceIndex 3 and 4 (the Vulkan value carries firstInstance);
    * the vertex-index module from firstVertex 2 observes VertexIndex 2,
    * 3, 4; with robustBufferAccess on, an instance record past the
    * offset buffer's bound reads zeros and the reference carrier
    * delivers; a zero instance count refuses at recording; an instance
    * record past the bound with the feature off refuses at recording;
    * and the fetched gates over two instances refuse at admission, the
    * producer routes fetching one instance's linear range. */
   ARM_INSTANCED_ARMED,
   ARM_INSTANCED_FIRST_INSTANCE_ARMED,
   ARM_INSTANCED_INDEX_ARMED,
   ARM_VERTEX_INDEX_ARMED,
   ARM_INSTANCED_ROBUST_ARMED,
   ARM_INSTANCED_ZERO_REFUSED,
   /* A six-vertex one-instance list: the recording installs the
    * two-triangle family member and the CPU route gathers all six
    * records; a count that is not a whole number of triangles refuses
    * at recording. */
   ARM_MULTI_TRIANGLE_ARMED,
   ARM_NON_TRIANGLE_COUNT_REFUSED,
   /* Host winding cull: the reference triangle is counter-clockwise in
    * window coordinates, so back culling under CCW front face keeps it
    * (the carrier is the reference) and under CW front face collapses
    * it to degenerate records of its first vertex. */
   ARM_CULL_BACK_KEPT_ARMED,
   ARM_CULL_BACK_DROPPED_ARMED,
   /* A sample mask clearing bit 0 leaves the one sample uncovered, so
    * the host collapses every triangle and the target keeps the clear
    * alone. */
   ARM_SAMPLE_MASK_ZERO_ARMED,
   /* A zero color write mask writes no channel, the same collapsed
    * draw. */
   ARM_WRITE_MASK_ZERO_ARMED,
   ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED,
   ARM_INSTANCED_FETCHED_REFUSED,
};


static const struct {
   const char *name;
   enum arm arm;
} arm_names[] = {
   { "armed", ARM_ARMED },
   { "gate-closed", ARM_GATE_CLOSED },
   { "retention-failure", ARM_RETENTION_FAILURE },
   { "authorization-refused", ARM_AUTHORIZATION_REFUSED },
   { "map-failure", ARM_MAP_FAILURE },
   { "ioctl-refused", ARM_IOCTL_REFUSED },
   { "completion-failure", ARM_COMPLETION_FAILURE },
   { "known-bad-premature-draw", ARM_KNOWN_BAD_PREMATURE_DRAW },
   { "robust-oob-enabled", ARM_ROBUST_OOB_ENABLED },
   { "robust-oob-w0-degenerate", ARM_ROBUST_OOB_W0_DEGENERATE_ARMED },
   { "robust-oob-disabled", ARM_ROBUST_OOB_DISABLED },
   { "gpu-fetched-composed", ARM_GPU_FETCHED_COMPOSED },
   { "gpu-fetched-composed-f32_3", ARM_GPU_FETCHED_COMPOSED_F32_3 },
   { "gpu-fetched-composed-f32_2", ARM_GPU_FETCHED_COMPOSED_F32_2 },
   { "gpu-fetched-compose-failure", ARM_GPU_FETCHED_COMPOSE_FAILURE },
   { "gpu-fetched-wrong-digest", ARM_GPU_FETCHED_WRONG_DIGEST },
   { "gpu-fetched-offset-misaligned", ARM_GPU_FETCHED_OFFSET_MISALIGNED },
   { "gpu-fetched-stride-misaligned", ARM_GPU_FETCHED_STRIDE_MISALIGNED },
   { "gpu-fetched-out-of-domain", ARM_GPU_FETCHED_OUT_OF_DOMAIN },
   { "gpu-fetched-out-of-bounds", ARM_GPU_FETCHED_OUT_OF_BOUNDS },
   { "gpu-fetched-aliased-source", ARM_GPU_FETCHED_ALIASED_SOURCE },
   { "varying-armed", ARM_VARYING_ARMED },
   { "varying-fragment-mismatch", ARM_VARYING_FRAGMENT_MISMATCH },
   { "varying-missing", ARM_VARYING_MISSING },
   { "sampled-armed", ARM_SAMPLED_ARMED },
   { "sampled-layer-armed", ARM_SAMPLED_LAYER_ARMED },
   { "sampled-array-view-refused", ARM_SAMPLED_ARRAY_VIEW_REFUSED },
   { "multi-attribute-armed", ARM_MULTI_ATTRIBUTE_ARMED },
   { "multi-attribute-interleaved-armed",
     ARM_MULTI_ATTRIBUTE_INTERLEAVED_ARMED },
   { "multi-attribute-overlap-refused", ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED },
   { "multi-attribute-unbound-refused", ARM_MULTI_ATTRIBUTE_UNBOUND_REFUSED },
   { "multi-attribute-alias-target-refused",
     ARM_MULTI_ATTRIBUTE_ALIAS_TARGET_REFUSED },
   { "multi-attribute-fetched-refused", ARM_MULTI_ATTRIBUTE_FETCHED_REFUSED },
   { "indexed-armed", ARM_INDEXED_ARMED },
   { "indexed-permuted-armed", ARM_INDEXED_PERMUTED_ARMED },
   { "indexed-robust-restart-armed", ARM_INDEXED_ROBUST_RESTART_ARMED },
   { "indexed-restart-enabled-refused", ARM_INDEXED_RESTART_ENABLED_REFUSED },
   { "indexed-zero-record-degenerate",
     ARM_INDEXED_ZERO_RECORD_DEGENERATE_ARMED },
   { "indexed-range-refused", ARM_INDEXED_RANGE_REFUSED },
   { "indexed-unbound-refused", ARM_INDEXED_UNBOUND_REFUSED },
   { "indexed-uint8-refused", ARM_INDEXED_UINT8_REFUSED },
   { "indexed-alias-target-refused", ARM_INDEXED_ALIAS_TARGET_REFUSED },
   { "indexed-fetched-refused", ARM_INDEXED_FETCHED_REFUSED },
   { "instanced-armed", ARM_INSTANCED_ARMED },
   { "instanced-first-instance-armed", ARM_INSTANCED_FIRST_INSTANCE_ARMED },
   { "instanced-index-armed", ARM_INSTANCED_INDEX_ARMED },
   { "vertex-index-armed", ARM_VERTEX_INDEX_ARMED },
   { "instanced-robust-armed", ARM_INSTANCED_ROBUST_ARMED },
   { "instanced-zero-refused", ARM_INSTANCED_ZERO_REFUSED },
   { "multi-triangle-armed", ARM_MULTI_TRIANGLE_ARMED },
   { "non-triangle-count-refused", ARM_NON_TRIANGLE_COUNT_REFUSED },
   { "cull-back-kept-armed", ARM_CULL_BACK_KEPT_ARMED },
   { "cull-back-dropped-armed", ARM_CULL_BACK_DROPPED_ARMED },
   { "sample-mask-zero-armed", ARM_SAMPLE_MASK_ZERO_ARMED },
   { "write-mask-zero-armed", ARM_WRITE_MASK_ZERO_ARMED },
   { "instanced-out-of-bounds-refused", ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED },
   { "instanced-fetched-refused", ARM_INSTANCED_FETCHED_REFUSED },
};

/* Injection over the transport's ioctl seam: the saved production table
 * (the drm-shim under LD_PRELOAD) serves every call the arm leaves
 * alone. */
static const struct radeon_drm_vk_ioctl_ops *saved_ops;
static enum arm current_arm;
/* The injection is live across the one vkQueueSubmit alone, so the
 * harness's own setup and read-back mappings reach the shim. */
static bool inject_live;
static unsigned cs_ioctls;
static unsigned failed_mmaps;

static int
injected_command_write_read(int fd, unsigned long request, void *data,
                            unsigned size)
{
   if (request == DRM_RADEON_CS) {
      cs_ioctls++;
      if (inject_live && current_arm == ARM_IOCTL_REFUSED)
         return -EINVAL;
   }
   return saved_ops->command_write_read(fd, request, data, size);
}

static int
injected_command_write(int fd, unsigned long request, void *data,
                       unsigned size)
{
   if (request == DRM_RADEON_GEM_WAIT_IDLE && inject_live &&
       current_arm == ARM_COMPLETION_FAILURE)
      return -EIO;
   return saved_ops->command_write(fd, request, data, size);
}

static void *
injected_mmap(size_t size, int fd, uint64_t offset)
{
   if (inject_live && current_arm == ARM_MAP_FAILURE) {
      failed_mmaps++;
      return NULL;
   }
   return saved_ops->mmap(size, fd, offset);
}

static struct radeon_drm_vk_ioctl_ops injected_ops;

static bool
file_present(const char *dir, const char *name)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   struct stat status;
   return stat(path, &status) == 0;
}

/* The retained ib.bin is the little-endian dword stream the kernel parser
 * reads; decoding it dword by dword keeps the digest host-order neutral. */
static void
retained_ib_digest(const char *dir, char out[2 * R300_TRIANGLE_DIGEST_SIZE + 1],
                   uint32_t *out_dwords)
{
   char path[4096];
   snprintf(path, sizeof(path), "%s/ib.bin", dir);
   FILE *file = fopen(path, "rb");
   assert(file != NULL);
   /* Sized past the longest retained stream the arms submit: the
    * composed fetched route. */
   uint8_t bytes[R300_FETCHED_F32_4_ROUTE_IB_DWORDS * 4 + 4];
   const size_t read_bytes = fread(bytes, 1, sizeof(bytes), file);
   fclose(file);
   assert(read_bytes % 4 == 0 && read_bytes < sizeof(bytes));
   uint32_t dwords[R300_FETCHED_F32_4_ROUTE_IB_DWORDS + 1];
   for (size_t i = 0; i < read_bytes / 4; i++) {
      dwords[i] = (uint32_t)bytes[4 * i] | (uint32_t)bytes[4 * i + 1] << 8 |
                  (uint32_t)bytes[4 * i + 2] << 16 |
                  (uint32_t)bytes[4 * i + 3] << 24;
   }
   *out_dwords = (uint32_t)(read_bytes / 4);
   r300_triangle_ib_digest_hex(dwords, *out_dwords, out);
}

#define DEVICE_COMMANDS(f)                                                 \
   f(vkAllocateMemory) f(vkFreeMemory) f(vkMapMemory) f(vkUnmapMemory)     \
   f(vkCreateBuffer) f(vkDestroyBuffer) f(vkBindBufferMemory)              \
   f(vkCreateImage) f(vkDestroyImage) f(vkGetImageMemoryRequirements)      \
   f(vkBindImageMemory) f(vkCreateImageView) f(vkDestroyImageView)         \
   f(vkCreateRenderPass) f(vkDestroyRenderPass) f(vkCreateFramebuffer)     \
   f(vkDestroyFramebuffer) f(vkCreateShaderModule)                         \
   f(vkDestroyShaderModule) f(vkCreatePipelineLayout)                      \
   f(vkDestroyPipelineLayout) f(vkCreateGraphicsPipelines)                 \
   f(vkDestroyPipeline) f(vkCreateCommandPool) f(vkDestroyCommandPool)     \
   f(vkAllocateCommandBuffers) f(vkBeginCommandBuffer)                     \
   f(vkEndCommandBuffer) f(vkCmdBeginRenderPass) f(vkCmdEndRenderPass)     \
   f(vkCmdBindPipeline) f(vkCmdBindVertexBuffers) f(vkCmdDraw)             \
   f(vkCmdBindIndexBuffer) f(vkCmdDrawIndexed)                             \
   f(vkCmdPipelineBarrier) f(vkGetDeviceQueue) f(vkQueueSubmit)            \
   f(vkCreateSampler) f(vkDestroySampler)                                  \
   f(vkCreateDescriptorSetLayout) f(vkDestroyDescriptorSetLayout)          \
   f(vkCreateDescriptorPool) f(vkDestroyDescriptorPool)                    \
   f(vkAllocateDescriptorSets) f(vkUpdateDescriptorSets)                   \
   f(vkCmdBindDescriptorSets)                                              \
   f(vkDestroyDevice)
#define DECLARE(name) static PFN_##name name;
DEVICE_COMMANDS(DECLARE)
#undef DECLARE

static VkShaderModule
make_module(VkDevice device, const uint32_t *words, size_t bytes)
{
   VkShaderModule module = VK_NULL_HANDLE;
   assert(vkCreateShaderModule(
             device,
             &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = bytes,
                .pCode = words,
             },
             NULL, &module) == VK_SUCCESS);
   return module;
}

struct target {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView view;
   VkDeviceSize footprint_bytes;
};

static void
seed_target(VkDevice device, const struct target *target)
{
   uint32_t *map = NULL;
   assert(vkMapMemory(device, target->memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&map) == VK_SUCCESS);
   for (VkDeviceSize i = 0; i < (target->footprint_bytes + 4096) / 4; i++)
      map[i] = COLOR_SEED;
   vkUnmapMemory(device, target->memory);
}

/* The whole allocation: seed everywhere means the clear did not run;
 * sentinel over the footprint and seed past it means it ran once over
 * the declared footprint alone. */
static void
check_target(VkDevice device, const struct target *target, bool cleared,
             const char *label)
{
   uint32_t *map = NULL;
   assert(vkMapMemory(device, target->memory, 0, VK_WHOLE_SIZE, 0,
                      (void **)&map) == VK_SUCCESS);
   const VkDeviceSize footprint_dwords = target->footprint_bytes / 4;
   VkDeviceSize deviations = 0;
   for (VkDeviceSize i = 0; i < footprint_dwords; i++) {
      const uint32_t expected =
         cleared ? R300_TRIANGLE_COLOR_SENTINEL : COLOR_SEED;
      if (map[i] != expected)
         deviations++;
   }
   for (VkDeviceSize i = footprint_dwords;
        i < (target->footprint_bytes + 4096) / 4; i++) {
      if (map[i] != COLOR_SEED)
         deviations++;
   }
   vkUnmapMemory(device, target->memory);
   if (deviations != 0)
      fprintf(stderr, "%s: %llu deviating target dwords\n", label,
              (unsigned long long)deviations);
   assert(deviations == 0);
}

/* The window-space cell a producer route consumes at the reference geometry:
 * the render-shape emitter carrying the bound fragment module's constant.
 */
static void
module_constant_window_cell(struct r300_tcl_bypass_triangle_ib *out)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const uint32_t module_color[4] = R3V_REFERENCE_FRAGMENT_COLOR_BITS;
   memcpy(shape.color_bits, module_color, sizeof(module_color));
   assert(r300_tcl_bypass_triangle_render_shape_emit(&shape, out) == 0);
}

/* The cell a CPU public draw records at the reference geometry: the same
 * module constant with seven fixed-capacity output triangles for clipping.
 */
static void
module_constant_clip_cell(uint32_t source_triangle_count,
                          struct r300_tcl_bypass_triangle_ib *out)
{
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const uint32_t module_color[4] = R3V_REFERENCE_FRAGMENT_COLOR_BITS;
   memcpy(shape.color_bits, module_color, sizeof(module_color));
   assert(r300_tcl_bypass_triangle_clip_space_render_shape_emit(
             &shape, source_triangle_count, out) == 0);
}

/* The composed fetched route the driver submits for a width: the
 * reference producer prefix ahead of the module-constant consumer.  The
 * reference composition is built first and its digest held to the
 * retained silicon pin, so the producer half stays bound to the RS482
 * receipt; the consumer half alone is replaced, which is the half the
 * recorded fragment constant moves.
 */
static void
module_constant_route_digest(int format_id, const char *producer_pin,
                             char out[2 * R300_TRIANGLE_DIGEST_SIZE + 1],
                             uint32_t *out_dwords)
{
   struct r300_r2vb_fetched_route_ib route;
   assert(r300_r2vb_fetched_route_reference_compose(format_id, &route) == 0);
   char reference_hex[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, reference_hex);
   assert(strcmp(reference_hex, producer_pin) == 0);

   struct r300_tcl_bypass_triangle_ib consumer;
   module_constant_window_cell(&consumer);
   assert(route.ib_size_dwords - route.consumer_start_dwords ==
          consumer.ib_size_dwords);
   memcpy(route.ib + route.consumer_start_dwords, consumer.ib,
          (size_t)consumer.ib_size_dwords * sizeof(uint32_t));
   r300_tcl_bypass_triangle_release(&consumer);

   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, out);
   *out_dwords = route.ib_size_dwords;
   r300_r2vb_fetched_route_release(&route);
}

/* The width each fetched arm composes and the silicon pin its producer
 * prefix carries.
 */
static void
arm_fetched_route(enum arm arm, int *format_id, const char **producer_pin)
{
   if (arm == ARM_GPU_FETCHED_COMPOSED_F32_3) {
      *format_id = R300_VERTEX_FORMAT_F32_3;
      *producer_pin = R300_FETCHED_F32_3_ROUTE_IB_BLAKE3;
   } else if (arm == ARM_GPU_FETCHED_COMPOSED_F32_2) {
      *format_id = R300_VERTEX_FORMAT_F32_2;
      *producer_pin = R300_FETCHED_F32_2_ROUTE_IB_BLAKE3;
   } else {
      *format_id = R300_VERTEX_FORMAT_F32_4;
      *producer_pin = R300_FETCHED_F32_4_ROUTE_IB_BLAKE3;
   }
}

static bool
make_manifest_directory(char *manifest_dir, size_t capacity)
{
   const char *temporary_roots[] = { getenv("TMPDIR"), "." };
   static const char manifest_write_suffix[] =
      "/.submit_manifest.json.XXXXXX";

   for (size_t i = 0; i < sizeof(temporary_roots) / sizeof(temporary_roots[0]);
        i++) {
      const char *temporary_root = temporary_roots[i];
      if (temporary_root == NULL || temporary_root[0] == '\0')
         continue;

      const size_t root_length = strlen(temporary_root);
      const int length = snprintf(
         manifest_dir, capacity, "%s%s%s", temporary_root,
         temporary_root[root_length - 1] == '/' ? "" : "/",
         "r3v-native-submit-order-XXXXXX");
      if (length < 0 || (size_t)length >= capacity ||
          (size_t)length + sizeof(manifest_write_suffix) > capacity)
         continue;
      if (mkdtemp(manifest_dir) != NULL)
         return true;
   }

   return false;
}

static int
run_arm(enum arm arm, const char *name)
{
   current_arm = arm;
   cs_ioctls = 0;
   failed_mmaps = 0;

   /* The gate and the evidence directory are read at device creation,
    * so every arm builds its own device under its own environment. */
   char manifest_dir[1024];
   assert(make_manifest_directory(manifest_dir, sizeof(manifest_dir)));
   setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
   if (arm == ARM_GATE_CLOSED || arm == ARM_KNOWN_BAD_PREMATURE_DRAW)
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   else
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);
   char reference_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);
   assert(reference.ib_size_dwords == R300_RETAINED_CPU_ROUTE_IB_DWORDS);
   assert(strcmp(reference_digest, R300_RETAINED_CPU_ROUTE_IB_BLAKE3) == 0);
   r300_tcl_bypass_triangle_release(&reference);
   struct r300_tcl_bypass_triangle_ib module_clip_cell;
   module_constant_clip_cell(1u, &module_clip_cell);
   char module_clip_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(module_clip_cell.ib,
                               module_clip_cell.ib_size_dwords,
                               module_clip_digest);
   const uint32_t module_clip_dwords = module_clip_cell.ib_size_dwords;
   assert(strcmp(module_clip_digest, reference_digest) != 0);
   r300_tcl_bypass_triangle_release(&module_clip_cell);
   const bool fetched_refusal_arm =
      arm == ARM_GPU_FETCHED_OFFSET_MISALIGNED ||
      arm == ARM_GPU_FETCHED_STRIDE_MISALIGNED ||
      arm == ARM_GPU_FETCHED_OUT_OF_DOMAIN ||
      arm == ARM_GPU_FETCHED_OUT_OF_BOUNDS ||
      arm == ARM_GPU_FETCHED_ALIASED_SOURCE;
   const bool fetched_arm = arm == ARM_GPU_FETCHED_COMPOSED ||
                            arm == ARM_GPU_FETCHED_COMPOSED_F32_3 ||
                            arm == ARM_GPU_FETCHED_COMPOSED_F32_2 ||
                            arm == ARM_GPU_FETCHED_COMPOSE_FAILURE ||
                            arm == ARM_GPU_FETCHED_WRONG_DIGEST ||
                            fetched_refusal_arm;
   const bool multi_arm = arm == ARM_MULTI_ATTRIBUTE_ARMED ||
                          arm == ARM_MULTI_ATTRIBUTE_INTERLEAVED_ARMED ||
                          arm == ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED ||
                          arm == ARM_MULTI_ATTRIBUTE_UNBOUND_REFUSED ||
                          arm == ARM_MULTI_ATTRIBUTE_ALIAS_TARGET_REFUSED ||
                          arm == ARM_MULTI_ATTRIBUTE_FETCHED_REFUSED;
   /* The interleaved arm reads both attributes from binding 0; the
    * overlap arm names the interleaved layout with the color record
    * crossing the stride; every other multi arm binds the color through
    * binding 1. */
   const bool interleaved_arm = arm == ARM_MULTI_ATTRIBUTE_INTERLEAVED_ARMED ||
                               arm == ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED;
   const bool indexed_arm = arm == ARM_INDEXED_ARMED ||
                            arm == ARM_INDEXED_PERMUTED_ARMED ||
                            arm == ARM_INDEXED_ROBUST_RESTART_ARMED ||
                            arm == ARM_INDEXED_RESTART_ENABLED_REFUSED ||
                            arm == ARM_INDEXED_ZERO_RECORD_DEGENERATE_ARMED ||
                            arm == ARM_INDEXED_RANGE_REFUSED ||
                            arm == ARM_INDEXED_UNBOUND_REFUSED ||
                            arm == ARM_INDEXED_UINT8_REFUSED ||
                            arm == ARM_INDEXED_ALIAS_TARGET_REFUSED ||
                            arm == ARM_INDEXED_FETCHED_REFUSED;
   /* The instanced arms: the instance-offset arms bind the offset
    * records through binding 1 at the instance rate; the two-triangle
    * arms draw two instances and record the cell family member of two
    * triangles. */
   const bool instance_offset_arm = arm == ARM_INSTANCED_ARMED ||
                                    arm == ARM_INSTANCED_FIRST_INSTANCE_ARMED ||
                                    arm == ARM_INSTANCED_ROBUST_ARMED ||
                                    arm == ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED;
   const bool two_triangle_arm = arm == ARM_INSTANCED_ARMED ||
                                 arm == ARM_INSTANCED_INDEX_ARMED ||
                                 arm == ARM_INSTANCED_FETCHED_REFUSED ||
                                 arm == ARM_MULTI_TRIANGLE_ARMED;
   struct r300_tcl_bypass_triangle_ib two_triangles;
   module_constant_clip_cell(2u, &two_triangles);
   char two_triangle_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(two_triangles.ib, two_triangles.ib_size_dwords,
                               two_triangle_digest);
   const uint32_t two_triangle_dwords = two_triangles.ib_size_dwords;
   /* Both source triangles keep seven output slots in one segmented stream. */
   assert(strcmp(two_triangle_digest, module_clip_digest) != 0);
   r300_tcl_bypass_triangle_release(&two_triangles);
   struct r300_tcl_bypass_triangle_ib varying_cell;
   assert(r300_tcl_bypass_triangle_clip_space_family_emit(
             R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, true, 1,
             &varying_cell) == 0);
   char varying_cell_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(varying_cell.ib,
                               varying_cell.ib_size_dwords,
                               varying_cell_digest);
   const uint32_t varying_cell_dwords = varying_cell.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&varying_cell);
   /* The varying cell is the recorded identity of every arm whose job
    * stores the varying. */
   const bool sampled_arm = arm == ARM_SAMPLED_ARMED ||
                            arm == ARM_SAMPLED_LAYER_ARMED ||
                            arm == ARM_SAMPLED_ARRAY_VIEW_REFUSED;
   /* The layered arm's texture holds three 16x16 layers over the
    * sampling family's 64-byte row pitch, so the selected last layer
    * starts 2048 bytes in. */
   const uint32_t sampled_layers = arm == ARM_SAMPLED_LAYER_ARMED ? 3 : 1;
   const uint32_t sampled_view_layer = sampled_layers - 1;
   const uint32_t sampled_texture_offset = sampled_view_layer * 16 * 16 * 4;
   const bool varying_cell_arm =
      arm == ARM_VARYING_ARMED || sampled_arm || multi_arm;
   char route_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1] = { 0 };
   uint32_t route_dwords = 0;
   if (arm == ARM_AUTHORIZATION_REFUSED) {
      char wrong[BLAKE3_OUT_LEN * 2 + 1];
      memcpy(wrong, module_clip_digest, sizeof(wrong));
      wrong[0] = wrong[0] == '0' ? '1' : '0';
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", wrong, 1);
   } else if (arm == ARM_GPU_FETCHED_WRONG_DIGEST) {
      /* The immediate route's retained identity, declared against the
       * fetched composition.  The composed identity is resolved here
       * too, so the retention check below reads the stream the arm
       * actually retained. */
      int format_id;
      const char *producer_pin;
      arm_fetched_route(arm, &format_id, &producer_pin);
      module_constant_route_digest(format_id, producer_pin, route_digest,
                                   &route_dwords);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
             R300_RETAINED_GPU_ROUTE_IB_BLAKE3, 1);
   } else if (sampled_arm) {
      /* The sampled cell's authorized identity is its own offline
       * emission at this arm's parameters. */
      struct r300_tcl_bypass_triangle_ib sampled_cell;
      assert(r300_tcl_bypass_triangle_clip_space_sampled_emit(
                R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1,
                sampled_texture_offset, 16, 16, 16,
                R300_TRIANGLE_LANES_R8G8B8A8, &sampled_cell) == 0);
      r300_triangle_ib_digest_hex(sampled_cell.ib,
                                  sampled_cell.ib_size_dwords, route_digest);
      r300_tcl_bypass_triangle_release(&sampled_cell);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", route_digest, 1);
   } else if (varying_cell_arm) {
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", varying_cell_digest, 1);
   } else if (two_triangle_arm) {
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", two_triangle_digest, 1);
   } else if (fetched_arm) {
      /* The composition identity of the fetched route over this arm's
       * exact geometry: one-page source at offset zero, the width's
       * record size as stride, one-page slot BO, and the recorded
       * consumer, whose fragment constant is the bound module's. */
      int format_id;
      const char *producer_pin;
      arm_fetched_route(arm, &format_id, &producer_pin);
      module_constant_route_digest(format_id, producer_pin, route_digest,
                                   &route_dwords);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", route_digest, 1);
   } else {
      /* The single-triangle constant-color draw records the
       * render-shape cell, so the authorized identity is the
       * module-constant stream rather than the emitter's oracle
       * color. */
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", module_clip_digest, 1);
   }
   struct utsname host;
   assert(uname(&host) == 0);
   setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
   setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
          R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
   if (fetched_arm || arm == ARM_MULTI_ATTRIBUTE_FETCHED_REFUSED ||
       arm == ARM_INDEXED_FETCHED_REFUSED ||
       arm == ARM_INSTANCED_FETCHED_REFUSED) {
      setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1);
      setenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL", "1", 1);
      setenv("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL", "1", 1);
   } else {
      unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
      unsetenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
      unsetenv("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL");
   }

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo =
                                &(VkApplicationInfo){
                                   .sType =
                                      VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_0,
                                },
                          },
                          NULL, &instance) == VK_SUCCESS);
   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                           "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice create_device =
      (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated = enumerate(instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1);
   const bool robust_arm = arm == ARM_ROBUST_OOB_ENABLED ||
                           arm == ARM_ROBUST_OOB_W0_DEGENERATE_ARMED ||
                           arm == ARM_ROBUST_OOB_DISABLED;
   const bool robust_enabled = arm == ARM_ROBUST_OOB_ENABLED ||
                               arm == ARM_ROBUST_OOB_W0_DEGENERATE_ARMED ||
                               arm == ARM_GPU_FETCHED_OUT_OF_BOUNDS ||
                               arm == ARM_INDEXED_ROBUST_RESTART_ARMED ||
                               arm == ARM_INSTANCED_ROBUST_ARMED;
   /* Per-arm stream geometry: the bind offset, binding stride, record
    * width, buffer size, and whether the buffer binds into the color
    * target's memory. */
   const VkDeviceSize bind_offset =
      arm == ARM_GPU_FETCHED_OFFSET_MISALIGNED ? 2 : 0;
   const uint32_t record_bytes = arm == ARM_GPU_FETCHED_COMPOSED_F32_3 ||
                                       arm == ARM_INDEXED_ROBUST_RESTART_ARMED
                                    ? 12
                                 : arm == ARM_GPU_FETCHED_COMPOSED_F32_2 ? 8
                                                                         : 16;
   const uint32_t binding_stride =
      arm == ARM_GPU_FETCHED_STRIDE_MISALIGNED ? 18
      : interleaved_arm                        ? 32
                                               : record_bytes;
   const bool short_buffer = robust_arm || arm == ARM_GPU_FETCHED_OUT_OF_BOUNDS;
   const bool alias_target = arm == ARM_GPU_FETCHED_ALIASED_SOURCE;
   const VkPhysicalDeviceFeatures robust_features = {
      .robustBufferAccess = VK_TRUE,
   };
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   assert(create_device(
             pdev,
             &(VkDeviceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pEnabledFeatures = robust_enabled ? &robust_features : NULL,
                .queueCreateInfoCount = 1,
                .pQueueCreateInfos =
                   &(VkDeviceQueueCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                      .queueFamilyIndex = 0,
                      .queueCount = 1,
                      .pQueuePriorities = &priority,
                   },
             },
             NULL, &device) == VK_SUCCESS);
#define LOAD(name) name = (PFN_##name)gdpa(device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD

   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   r3v_native_install_shim_arming(native_device);
   /* The injection table wraps the table the device resolved, so every
    * call the arm leaves alone still reaches the shim. */
   saved_ops = native_device->drm.ops;
   injected_ops = *saved_ops;
   injected_ops.command_write_read = injected_command_write_read;
   injected_ops.command_write = injected_command_write;
   injected_ops.mmap = injected_mmap;
   native_device->drm.ops = &injected_ops;

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The qualified target, one seeded page past its footprint. */
   struct target target = { 0 };
   assert(vkCreateImage(
             device,
             &(VkImageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = R3V_NATIVE_TARGET_FORMAT,
                .extent = { R3V_NATIVE_TARGET_WIDTH,
                            R3V_NATIVE_TARGET_HEIGHT, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_LINEAR,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
             },
             NULL, &target.image) == VK_SUCCESS);
   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(device, target.image, &reqs);
   target.footprint_bytes = reqs.size;
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = reqs.size + 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &target.memory) == VK_SUCCESS);
   assert(vkBindImageMemory(device, target.image, target.memory, 0) ==
          VK_SUCCESS);
   seed_target(device, &target);
   assert(vkCreateImageView(
             device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = target.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = R3V_NATIVE_TARGET_FORMAT,
                .subresourceRange = { .aspectMask =
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                      .levelCount = 1,
                                      .layerCount = 1 },
             },
             NULL, &target.view) == VK_SUCCESS);

   /* The application vertex buffer over the reference triangle. */
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &vertex_memory) == VK_SUCCESS);
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   assert(vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            /* Two 16-byte records under a three-vertex
                             * draw for the robust and out-of-bounds
                             * arms. */
                            .size = short_buffer ? 32 : 256,
                            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &vertex_buffer) == VK_SUCCESS);
   if (alias_target) {
      /* The source bound into the color target's allocation, on the
       * seeded page past the image footprint: the admission refuses the
       * aliased handle before it reads a record, so the page keeps its
       * seed and no record is written. */
      const VkDeviceSize alias_offset = (target.footprint_bytes + 4095) &
                                        ~(VkDeviceSize)4095;
      assert(alias_offset + 256 <= target.footprint_bytes + 4096);
      assert(vkBindBufferMemory(device, vertex_buffer, target.memory,
                                alias_offset) == VK_SUCCESS);
   } else {
      assert(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0) ==
             VK_SUCCESS);
      void *map = NULL;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      /* The fetched route declares a window-space carrier and admits
       * FP24 fixed points alone, so the fetched arms bind the reference
       * cell's own window-space records at the arm's bind offset and
       * stride; the out-of-domain arm and every CPU-route arm bind the
       * NDC triangle, whose negative components the CPU route transforms
       * and the fetched route refuses. */
      const float *records = fetched_arm && arm != ARM_GPU_FETCHED_OUT_OF_DOMAIN
                                ? r300_tcl_bypass_triangle_vertices
                                : ndc_triangle;
      /* The permuted indexed arm stores the records as (v1, v2, v0), so
       * only the index order restores the reference triangle; the
       * vertex-index arm stores the triangle at records 2..4 and draws
       * from firstVertex 2. */
      static const unsigned permuted_order[3] = { 1, 2, 0 };
      const unsigned record_base = arm == ARM_VERTEX_INDEX_ARMED ? 2 : 0;
      for (unsigned v = 0; v < 3; v++)
         memcpy((uint8_t *)map + bind_offset +
                   (record_base + v) * binding_stride,
                &records[(arm == ARM_INDEXED_PERMUTED_ARMED
                             ? permuted_order[v]
                             : v) *
                         4],
                record_bytes);
      /* The multi-triangle arm's second triangle: the NDC reference
       * translated by (0.0625, 0, 0, 0), exact in binary32. */
      if (arm == ARM_MULTI_TRIANGLE_ARMED) {
         for (unsigned v = 0; v < 3; v++) {
            float second[4];
            memcpy(second, &ndc_triangle[v * 4], 16);
            second[0] += 0.0625f;
            memcpy((uint8_t *)map + bind_offset +
                      (3 + v) * binding_stride,
                   second, record_bytes);
         }
      }
      /* The interleaved layout carries the varying reference colors as
       * the F32_4 at offset 16 of each 32-byte record. */
      if (interleaved_arm) {
         for (unsigned v = 0; v < 3; v++)
            memcpy((uint8_t *)map + bind_offset + v * binding_stride + 16,
                   &r300_tcl_bypass_triangle_varying_colors[v * 4], 16);
      }
      vkUnmapMemory(device, vertex_memory);
   }
   /* The color binding of the two-binding multi arms: F32_3 records of
    * the varying reference colors (alpha synthesizes to 1) at stride
    * 12, in its own allocation -- or, for the alias arm, bound at the
    * start of the pass target's memory, inside the footprint the clear
    * and the draw write. */
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   VkBuffer color_buffer = VK_NULL_HANDLE;
   if (multi_arm && !interleaved_arm) {
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = 4096,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &color_memory) == VK_SUCCESS);
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 64,
                               .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, &color_buffer) == VK_SUCCESS);
      if (arm == ARM_MULTI_ATTRIBUTE_ALIAS_TARGET_REFUSED) {
         assert(vkBindBufferMemory(device, color_buffer, target.memory, 0) ==
                VK_SUCCESS);
      } else {
         assert(vkBindBufferMemory(device, color_buffer, color_memory, 0) ==
                VK_SUCCESS);
         void *map = NULL;
         assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                            &map) == VK_SUCCESS);
         for (unsigned v = 0; v < 3; v++)
            memcpy((uint8_t *)map + v * 12,
                   &r300_tcl_bypass_triangle_varying_colors[v * 4], 12);
         vkUnmapMemory(device, color_memory);
      }
   }

   /* The instance-offset buffer of the instance-offset arms: two F32_4
    * records, (0, 0, 0, 0) and (0.125, 0.125, 0, 0), at stride 16 --
    * one record alone for the robust and out-of-bounds arms, whose
    * draws read past it. */
   static const float instance_offsets[8] = {
      0.0f, 0.0f, 0.0f, 0.0f, 0.125f, 0.125f, 0.0f, 0.0f,
   };
   VkDeviceMemory offset_memory = VK_NULL_HANDLE;
   VkBuffer offset_buffer = VK_NULL_HANDLE;
   if (instance_offset_arm) {
      const bool one_record = arm == ARM_INSTANCED_ROBUST_ARMED ||
                              arm == ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED;
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = 4096,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &offset_memory) == VK_SUCCESS);
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = one_record ? 16 : 32,
                               .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, &offset_buffer) == VK_SUCCESS);
      assert(vkBindBufferMemory(device, offset_buffer, offset_memory, 0) ==
             VK_SUCCESS);
      void *map = NULL;
      assert(vkMapMemory(device, offset_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS);
      memcpy(map, instance_offsets, sizeof(instance_offsets));
      vkUnmapMemory(device, offset_memory);
   }

   /* The index buffer of the indexed arms: sixteen bytes in its own
    * allocation -- or, for the alias arm, bound at the start of the
    * pass target's memory.  UINT16 0, 1, 2 for the plain arm; UINT32
    * 99, 5, 3, 4 for the permuted arm (read from entry 1 under base
    * vertex -3: vertices 2, 0, 1); UINT16 0, 1, 0xffff for the robust
    * restart-value arm; UINT16 0, 1, 7 for the out-of-bounds arm. */
   VkDeviceMemory index_memory = VK_NULL_HANDLE;
   VkBuffer index_buffer = VK_NULL_HANDLE;
   if (indexed_arm) {
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = 4096,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &index_memory) == VK_SUCCESS);
      assert(vkCreateBuffer(device,
                            &(VkBufferCreateInfo){
                               .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = 16,
                               .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            },
                            NULL, &index_buffer) == VK_SUCCESS);
      if (arm == ARM_INDEXED_ALIAS_TARGET_REFUSED) {
         assert(vkBindBufferMemory(device, index_buffer, target.memory, 0) ==
                VK_SUCCESS);
      } else {
         assert(vkBindBufferMemory(device, index_buffer, index_memory, 0) ==
                VK_SUCCESS);
         void *map = NULL;
         assert(vkMapMemory(device, index_memory, 0, VK_WHOLE_SIZE, 0,
                            &map) == VK_SUCCESS);
         if (arm == ARM_INDEXED_PERMUTED_ARMED) {
            const uint32_t indices[4] = { 99, 5, 3, 4 };
            memcpy(map, indices, sizeof(indices));
         } else {
            const uint16_t indices[4] = {
               0, 1,
               arm == ARM_INDEXED_ROBUST_RESTART_ARMED    ? 0xffffu
               : arm == ARM_INDEXED_ZERO_RECORD_DEGENERATE_ARMED ? 7
                                                           : 2,
               0
            };
            memcpy(map, indices, sizeof(indices));
         }
         vkUnmapMemory(device, index_memory);
      }
   }

   VkRenderPass pass = VK_NULL_HANDLE;
   assert(vkCreateRenderPass(
             device,
             &(VkRenderPassCreateInfo){
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments =
                   &(VkAttachmentDescription){
                      .format = R3V_NATIVE_TARGET_FORMAT,
                      .samples = VK_SAMPLE_COUNT_1_BIT,
                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
                   },
                .subpassCount = 1,
                .pSubpasses =
                   &(VkSubpassDescription){
                      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                      .colorAttachmentCount = 1,
                      .pColorAttachments =
                         &(VkAttachmentReference){
                            .attachment = 0,
                            .layout = VK_IMAGE_LAYOUT_GENERAL,
                         },
                   },
             },
             NULL, &pass) == VK_SUCCESS);
   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   assert(vkCreateFramebuffer(
             device,
             &(VkFramebufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = pass,
                .attachmentCount = 1,
                .pAttachments = &target.view,
                .width = R3V_NATIVE_TARGET_WIDTH,
                .height = R3V_NATIVE_TARGET_HEIGHT,
                .layers = 1,
             },
             NULL, &framebuffer) == VK_SUCCESS);
   /* The sampled arm's texture: a sampled-usage R8G8B8A8 image with
    * bound memory, a view, a nearest/clamp-to-edge sampler, and the
    * set-0 combined-image-sampler write. */
   VkImage tex_image = VK_NULL_HANDLE;
   VkDeviceMemory tex_memory = VK_NULL_HANDLE;
   VkImageView tex_view = VK_NULL_HANDLE;
   VkSampler tex_sampler = VK_NULL_HANDLE;
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   VkDescriptorPool desc_pool = VK_NULL_HANDLE;
   VkDescriptorSet desc_set = VK_NULL_HANDLE;
   if (sampled_arm) {
      assert(vkCreateImage(
                device,
                &(VkImageCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                   .imageType = VK_IMAGE_TYPE_2D,
                   .format = VK_FORMAT_R8G8B8A8_UNORM,
                   .extent = { 16, 16, 1 },
                   .mipLevels = 1,
                   .arrayLayers = sampled_layers,
                   .samples = VK_SAMPLE_COUNT_1_BIT,
                   .tiling = VK_IMAGE_TILING_LINEAR,
                   .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                },
                NULL, &tex_image) == VK_SUCCESS);
      VkMemoryRequirements tex_reqs;
      vkGetImageMemoryRequirements(device, tex_image, &tex_reqs);
      assert(vkAllocateMemory(device,
                              &(VkMemoryAllocateInfo){
                                 .sType =
                                    VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = tex_reqs.size,
                                 .memoryTypeIndex = 0,
                              },
                              NULL, &tex_memory) == VK_SUCCESS);
      assert(vkBindImageMemory(device, tex_image, tex_memory, 0) ==
             VK_SUCCESS);
      assert(vkCreateImageView(
                device,
                &(VkImageViewCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                   .image = tex_image,
                   .viewType = arm == ARM_SAMPLED_ARRAY_VIEW_REFUSED
                                  ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                  : VK_IMAGE_VIEW_TYPE_2D,
                   .format = VK_FORMAT_R8G8B8A8_UNORM,
                   .subresourceRange = { .aspectMask =
                                            VK_IMAGE_ASPECT_COLOR_BIT,
                                         .levelCount = 1,
                                         .baseArrayLayer = sampled_view_layer,
                                         .layerCount = 1 },
                },
                NULL, &tex_view) == VK_SUCCESS);
      assert(vkCreateSampler(
                device,
                &(VkSamplerCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                   .magFilter = VK_FILTER_NEAREST,
                   .minFilter = VK_FILTER_NEAREST,
                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                   .borderColor =
                      VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                },
                NULL, &tex_sampler) == VK_SUCCESS);
      assert(vkCreateDescriptorSetLayout(
                device,
                &(VkDescriptorSetLayoutCreateInfo){
                   .sType =
                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                   .bindingCount = 1,
                   .pBindings =
                      &(VkDescriptorSetLayoutBinding){
                         .binding = 0,
                         .descriptorType =
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .descriptorCount = 1,
                         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                      },
                },
                NULL, &set_layout) == VK_SUCCESS);
      assert(vkCreateDescriptorPool(
                device,
                &(VkDescriptorPoolCreateInfo){
                   .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                   .maxSets = 1,
                   .poolSizeCount = 1,
                   .pPoolSizes =
                      &(VkDescriptorPoolSize){
                         .type =
                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         .descriptorCount = 1,
                      },
                },
                NULL, &desc_pool) == VK_SUCCESS);
      assert(vkAllocateDescriptorSets(
                device,
                &(VkDescriptorSetAllocateInfo){
                   .sType =
                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                   .descriptorPool = desc_pool,
                   .descriptorSetCount = 1,
                   .pSetLayouts = &set_layout,
                },
                &desc_set) == VK_SUCCESS);
      vkUpdateDescriptorSets(
         device, 1,
         &(VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = desc_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo =
               &(VkDescriptorImageInfo){
                  .sampler = tex_sampler,
                  .imageView = tex_view,
                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
               },
         },
         0, NULL);
   }
   VkPipelineLayout layout = VK_NULL_HANDLE;
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = sampled_arm ? 1 : 0,
                .pSetLayouts =
                   sampled_arm ? &set_layout : NULL,
             },
             NULL, &layout) == VK_SUCCESS);
   const bool varying_vs = arm == ARM_VARYING_ARMED ||
                           arm == ARM_VARYING_FRAGMENT_MISMATCH ||
                           sampled_arm;
   const bool varying_fs = arm == ARM_VARYING_ARMED || arm == ARM_VARYING_MISSING;
   VkShaderModule vs =
      multi_arm
         ? make_module(device, r3v_reference_vertex_two_attributes_spirv,
                       sizeof(r3v_reference_vertex_two_attributes_spirv))
      : instance_offset_arm
         ? make_module(device, r3v_reference_vertex_instance_offset_spirv,
                       sizeof(r3v_reference_vertex_instance_offset_spirv))
      : arm == ARM_INSTANCED_INDEX_ARMED
         ? make_module(device, r3v_reference_vertex_instance_index_spirv,
                       sizeof(r3v_reference_vertex_instance_index_spirv))
      : arm == ARM_VERTEX_INDEX_ARMED
         ? make_module(device, r3v_reference_vertex_vertex_index_spirv,
                       sizeof(r3v_reference_vertex_vertex_index_spirv))
      : varying_vs
         ? make_module(device, r3v_reference_vertex_varying_spirv,
                       sizeof(r3v_reference_vertex_varying_spirv))
         : make_module(device, r3v_reference_vertex_spirv,
                       sizeof(r3v_reference_vertex_spirv));
   VkShaderModule fs =
      sampled_arm
         ? make_module(device, r3v_reference_fragment_sampled_spirv,
                       sizeof(r3v_reference_fragment_sampled_spirv))
      : varying_fs || multi_arm
         ? make_module(device, r3v_reference_fragment_varying_spirv,
                       sizeof(r3v_reference_fragment_varying_spirv))
         : make_module(device, r3v_reference_fragment_spirv,
                       sizeof(r3v_reference_fragment_spirv));
   /* The multi arms' vertex input: the color attribute at location 1
    * reads binding 1 (F32_3, stride 12) or, interleaved, binding 0 at
    * offset 16 (F32_4); the overlap arm places it at offset 20, so its
    * 16 bytes cross the 32-byte stride. */
   const VkVertexInputBindingDescription multi_bindings[2] = {
      { .binding = 0, .stride = binding_stride,
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
      { .binding = 1, .stride = 12, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
   };
   const VkVertexInputAttributeDescription multi_attributes[2] = {
      { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .offset = 0 },
      interleaved_arm
         ? (VkVertexInputAttributeDescription){
              .location = 1, .binding = 0,
              .format = VK_FORMAT_R32G32B32A32_SFLOAT,
              .offset = arm == ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED ? 20 : 16 }
         : (VkVertexInputAttributeDescription){
              .location = 1, .binding = 1,
              .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
   };
   const VkPipelineVertexInputStateCreateInfo multi_vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = interleaved_arm ? 1 : 2,
      .pVertexBindingDescriptions = multi_bindings,
      .vertexAttributeDescriptionCount = 2,
      .pVertexAttributeDescriptions = multi_attributes,
   };
   /* The instance-offset arms' vertex input: the offset attribute at
    * location 1 reads binding 1 at the instance rate (F32_4, stride
    * 16). */
   const VkVertexInputBindingDescription offset_bindings[2] = {
      { .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
      { .binding = 1, .stride = 16,
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE },
   };
   const VkVertexInputAttributeDescription offset_attributes[2] = {
      { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .offset = 0 },
      { .location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
        .offset = 0 },
   };
   const VkPipelineVertexInputStateCreateInfo offset_vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 2,
      .pVertexBindingDescriptions = offset_bindings,
      .vertexAttributeDescriptionCount = 2,
      .pVertexAttributeDescriptions = offset_attributes,
   };
   VkPipeline pipeline = VK_NULL_HANDLE;
   const VkResult created = vkCreateGraphicsPipelines(
             device, VK_NULL_HANDLE, 1,
             &(VkGraphicsPipelineCreateInfo){
                .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                .stageCount = 2,
                .pStages =
                   (VkPipelineShaderStageCreateInfo[]){
                      { .sType =
                           VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_VERTEX_BIT,
                        .module = vs,
                        .pName = "main" },
                      { .sType =
                           VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                        .module = fs,
                        .pName = "main" },
                   },
                .pVertexInputState =
                   multi_arm           ? &multi_vertex_input
                   : instance_offset_arm ? &offset_vertex_input
                   : &(VkPipelineVertexInputStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                      .vertexBindingDescriptionCount = 1,
                      .pVertexBindingDescriptions =
                         &(VkVertexInputBindingDescription){
                            .binding = 0,
                            .stride = binding_stride,
                            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                         },
                      .vertexAttributeDescriptionCount = 1,
                      .pVertexAttributeDescriptions =
                         &(VkVertexInputAttributeDescription){
                            .location = 0,
                            .binding = 0,
                            .format = (arm == ARM_ROBUST_OOB_ENABLED ||
                                       arm == ARM_ROBUST_OOB_DISABLED ||
                                       arm == ARM_GPU_FETCHED_COMPOSED_F32_3 ||
                                       arm == ARM_INDEXED_ROBUST_RESTART_ARMED)
                                         ? VK_FORMAT_R32G32B32_SFLOAT
                                      : arm == ARM_GPU_FETCHED_COMPOSED_F32_2
                                         ? VK_FORMAT_R32G32_SFLOAT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = 0,
                         },
                   },
                .pInputAssemblyState =
                   &(VkPipelineInputAssemblyStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                      .primitiveRestartEnable =
                         arm == ARM_INDEXED_RESTART_ENABLED_REFUSED,
                   },
                .pViewportState =
                   &(VkPipelineViewportStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                      .viewportCount = 1,
                      .pViewports =
                         &(VkViewport){
                            .width = (float)R3V_NATIVE_TARGET_WIDTH,
                            .height = (float)R3V_NATIVE_TARGET_HEIGHT,
                            .maxDepth = 1.0f,
                         },
                      .scissorCount = 1,
                      .pScissors =
                         &(VkRect2D){
                            .extent = { R3V_NATIVE_TARGET_WIDTH,
                                        R3V_NATIVE_TARGET_HEIGHT },
                         },
                   },
                .pRasterizationState =
                   &(VkPipelineRasterizationStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                      .polygonMode = VK_POLYGON_MODE_FILL,
                      .cullMode = (arm == ARM_CULL_BACK_KEPT_ARMED ||
                                   arm == ARM_CULL_BACK_DROPPED_ARMED)
                                     ? VK_CULL_MODE_BACK_BIT
                                     : VK_CULL_MODE_NONE,
                      .frontFace =
                         arm == ARM_CULL_BACK_DROPPED_ARMED
                            ? VK_FRONT_FACE_CLOCKWISE
                            : VK_FRONT_FACE_COUNTER_CLOCKWISE,
                      .lineWidth = 1.0f,
                   },
                .pMultisampleState =
                   &(VkPipelineMultisampleStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                      .pSampleMask =
                         arm == ARM_SAMPLE_MASK_ZERO_ARMED
                            ? &(VkSampleMask){ 0 }
                            : NULL,
                   },
                .pColorBlendState =
                   &(VkPipelineColorBlendStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                      .attachmentCount = 1,
                      .pAttachments =
                         &(VkPipelineColorBlendAttachmentState){
                            .colorWriteMask =
                               arm == ARM_WRITE_MASK_ZERO_ARMED
                                  ? 0
                                  : VK_COLOR_COMPONENT_R_BIT |
                                       VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT |
                                       VK_COLOR_COMPONENT_A_BIT,
                         },
                   },
                .layout = layout,
                .renderPass = pass,
             },
             NULL, &pipeline);
   if (arm == ARM_VARYING_FRAGMENT_MISMATCH || arm == ARM_VARYING_MISSING ||
       arm == ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED ||
       arm == ARM_INDEXED_RESTART_ENABLED_REFUSED) {
      /* The stage pair names two fragment shapes for one job, the
       * color attribute's record crosses its binding's stride, or
       * primitive restart is enabled on the list topology, so the
       * pipeline refuses and nothing records. */
      assert(created == R3V_NATIVE_REFUSAL_RESULT);
      assert(pipeline == VK_NULL_HANDLE);
      printf("%s: pipeline refused (%d)\n", name, created);
      return 0;
   }
   assert(created == VK_SUCCESS);

   VkCommandPool pool = VK_NULL_HANDLE;
   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0,
             },
             NULL, &pool) == VK_SUCCESS);
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
             },
             &cmd) == VK_SUCCESS);
   assert(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
                                       .sType =
                                          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    }) == VK_SUCCESS);
   vkCmdPipelineBarrier(
      cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1,
      &(VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = target.image,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .layerCount = 1 },
      });
   vkCmdBeginRenderPass(
      cmd,
      &(VkRenderPassBeginInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = pass,
         .framebuffer = framebuffer,
         .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                     R3V_NATIVE_TARGET_HEIGHT } },
         .clearValueCount = 1,
         .pClearValues =
            &(VkClearValue){
               .color = { .float32 = { CLEAR_SENTINEL, CLEAR_SENTINEL,
                                       CLEAR_SENTINEL, CLEAR_SENTINEL } },
            },
      },
      VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   if (sampled_arm)
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                              0, 1, &desc_set, 0, NULL);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ bind_offset });
   if (multi_arm && !interleaved_arm &&
       arm != ARM_MULTI_ATTRIBUTE_UNBOUND_REFUSED)
      vkCmdBindVertexBuffers(cmd, 1, 1, &color_buffer, &(VkDeviceSize){ 0 });
   if (instance_offset_arm)
      vkCmdBindVertexBuffers(cmd, 1, 1, &offset_buffer, &(VkDeviceSize){ 0 });
   /* The instanced arms' draw parameters: instances from firstInstance,
    * and the vertex-index arm's firstVertex. */
   const uint32_t instance_count =
      arm == ARM_INSTANCED_ZERO_REFUSED    ? 0
      : arm == ARM_MULTI_TRIANGLE_ARMED    ? 1
      : two_triangle_arm                   ? 2
      : arm == ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED ? 2 : 1;
   const uint32_t first_instance =
      arm == ARM_INSTANCED_FIRST_INSTANCE_ARMED ? 1
      : arm == ARM_INSTANCED_INDEX_ARMED ? 3
      : arm == ARM_INSTANCED_ROBUST_ARMED ? 5 : 0;
   const uint32_t first_vertex = arm == ARM_VERTEX_INDEX_ARMED ? 2 : 0;
   if (indexed_arm) {
      if (arm != ARM_INDEXED_UNBOUND_REFUSED) {
         vkCmdBindIndexBuffer(cmd, index_buffer, 0,
                              arm == ARM_INDEXED_PERMUTED_ARMED
                                 ? VK_INDEX_TYPE_UINT32
                              : arm == ARM_INDEXED_UINT8_REFUSED
                                 ? VK_INDEX_TYPE_UINT8_EXT
                                 : VK_INDEX_TYPE_UINT16);
      }
      /* The permuted arm reads entries 1..3 under base vertex -3; the
       * range arm asks for entries 6..8 of an eight-entry buffer. */
      vkCmdDrawIndexed(cmd, 3, 1,
                       arm == ARM_INDEXED_PERMUTED_ARMED ? 1
                       : arm == ARM_INDEXED_RANGE_REFUSED ? 6
                                                          : 0,
                       arm == ARM_INDEXED_PERMUTED_ARMED ? -3 : 0, 0);
   } else {
      vkCmdDraw(cmd,
                arm == ARM_MULTI_TRIANGLE_ARMED          ? 6
                : arm == ARM_NON_TRIANGLE_COUNT_REFUSED ? 4
                                                        : 3,
                instance_count, first_vertex, first_instance);
   }
   vkCmdEndRenderPass(cmd);
   const VkResult ended = vkEndCommandBuffer(cmd);
   if (arm == ARM_ROBUST_OOB_DISABLED ||
       arm == ARM_MULTI_ATTRIBUTE_UNBOUND_REFUSED ||
       arm == ARM_MULTI_ATTRIBUTE_ALIAS_TARGET_REFUSED ||
       arm == ARM_INDEXED_RANGE_REFUSED ||
       arm == ARM_INDEXED_UNBOUND_REFUSED ||
       arm == ARM_INDEXED_UINT8_REFUSED ||
       arm == ARM_INDEXED_ALIAS_TARGET_REFUSED ||
       arm == ARM_INSTANCED_ZERO_REFUSED ||
       arm == ARM_NON_TRIANGLE_COUNT_REFUSED ||
       arm == ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED ||
       arm == ARM_SAMPLED_ARRAY_VIEW_REFUSED) {
      /* A zero instance count and an instance record past the offset
       * buffer's bound with the feature off each poison the draw at
       * recording.  The feature off, the record-time bound proof poisons the
       * recording; the color binding left unbound, and the color stream
       * inside the pass target's footprint, each poison the draw; the
       * index range past the index buffer, the index buffer left
       * unbound, the UINT8 index type, and the index buffer inside the
       * pass target's footprint each poison the indexed draw: the
       * application sees the refusal at end and nothing reaches the
       * queue, the seeded target untouched. */
      assert(ended == R3V_NATIVE_REFUSAL_RESULT);
      check_target(device, &target, false, name);
      printf("%s: record refused at vkEndCommandBuffer (%d)\n", name,
             ended);
      return 0;
   }
   assert(ended == VK_SUCCESS);

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(cmd);
   assert(native_cmd->ib_size_dwords != 0 && native_cmd->owned_carriers[0]);
   /* The recorded cell is the varying cell exactly when the pipeline
    * carries the varying: its dword count and digest are the pinned
    * identity the arm declared. */
   {
      char recorded_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      r300_triangle_ib_digest_hex(native_cmd->ib, native_cmd->ib_size_dwords,
                                  recorded_digest);
      if (sampled_arm) {
         /* The sampled cell's identity is its own offline emission at
          * the recorded parameters: the reference 64x64 target and the
          * bound 16x16 texture at the selected layer's stride over the
          * 16-texel pitch. */
         struct r300_tcl_bypass_triangle_ib offline;
         assert(r300_tcl_bypass_triangle_clip_space_sampled_emit(
                   R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1,
                   sampled_texture_offset, 16, 16, 16,
                   R300_TRIANGLE_LANES_R8G8B8A8, &offline) == 0);
         char offline_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
         r300_triangle_ib_digest_hex(offline.ib, offline.ib_size_dwords,
                                     offline_digest);
         assert(native_cmd->ib_size_dwords == offline.ib_size_dwords);
         assert(strcmp(recorded_digest, offline_digest) == 0);
         r300_tcl_bypass_triangle_release(&offline);
      } else if (varying_cell_arm) {
         assert(native_cmd->ib_size_dwords == varying_cell_dwords);
         assert(strcmp(recorded_digest, varying_cell_digest) == 0);
      } else if (two_triangle_arm) {
         /* Two instances record the two-triangle family member: the
          * reference dword count, the family digest. */
         assert(native_cmd->ib_size_dwords == two_triangle_dwords);
         assert(strcmp(recorded_digest, two_triangle_digest) == 0);
      } else {
         /* The single-triangle constant-color draw records the
          * render-shape cell, so the executed fragment constant is the
          * bound module's and the stream is the module-constant
          * identity rather than the emitter's oracle color.
          */
         assert(native_cmd->ib_size_dwords == module_clip_dwords);
         assert(strcmp(recorded_digest, module_clip_digest) == 0);
      }
   }
   /* The carrier snapshots cover either two position-only source groups
    * or one varying source group, including every clipping-capacity slot. */
   uint32_t carrier_before[2u *
                           R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT *
                           R300_TRIANGLE_VERTEX_DWORDS];
   {
      void *carrier_map = NULL;
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      memcpy(carrier_before, carrier_map, sizeof(carrier_before));
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carriers[0]->bo, carrier_map);
   }

   if (arm == ARM_KNOWN_BAD_PREMATURE_DRAW) {
      assert(r3v_native_cmd_buffer_execute_deferred_draws(
                native_device, native_cmd) == VK_SUCCESS);
   }
   if (arm == ARM_GPU_FETCHED_COMPOSE_FAILURE)
      native_device->gpu_producer_compose_inject_errno = -ENOMEM;
   if (arm == ARM_RETENTION_FAILURE)
      native_device->semantic_cell_retention_inject_errno = -EIO;
   const uint32_t references_before = native_cmd->reference_count;
   const enum r3v_native_cell_kind kind_before = native_cmd->cell_kind;
   const uint32_t ib_dwords_before = native_cmd->ib_size_dwords;
   const uint32_t window_dwords_before =
      native_cmd->window_space_ib_size_dwords;
   uint32_t *ib_before = NULL;
   uint32_t *window_before = NULL;
   if (arm == ARM_GPU_FETCHED_COMPOSE_FAILURE) {
      assert(native_cmd->ib != NULL && ib_dwords_before != 0);
      assert(native_cmd->window_space_ib != NULL && window_dwords_before != 0);
      ib_before = malloc((size_t)ib_dwords_before * sizeof(uint32_t));
      window_before =
         malloc((size_t)window_dwords_before * sizeof(uint32_t));
      assert(ib_before != NULL && window_before != NULL);
      memcpy(ib_before, native_cmd->ib,
             (size_t)ib_dwords_before * sizeof(uint32_t));
      memcpy(window_before, native_cmd->window_space_ib,
             (size_t)window_dwords_before * sizeof(uint32_t));
   }
   assert(references_before == (sampled_arm
                                   ? R300_TRIANGLE_SAMPLED_SLOT_COUNT
                                   : R300_TRIANGLE_RENDER_SLOT_COUNT));
   assert(kind_before == (sampled_arm
                             ? R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED
                             : R3V_NATIVE_CELL_KIND_TRIANGLE));
   assert(native_cmd->owned_slot == NULL);

   inject_live = true;
   const VkResult submitted = vkQueueSubmit(
      queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
      },
      VK_NULL_HANDLE);
   inject_live = false;
   const enum r3v_native_queue_status status =
      r3v_native_queue_submission_status(device);
   native_device->drm.ops = saved_ops;
   native_device->gpu_producer_compose_inject_errno = 0;
   native_device->semantic_cell_retention_inject_errno = 0;

   uint32_t carrier_after[2u *
                          R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT *
                          R300_TRIANGLE_VERTEX_DWORDS];
   {
      void *carrier_map = NULL;
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carriers[0]->bo,
                                  &carrier_map) == 0);
      memcpy(carrier_after, carrier_map, sizeof(carrier_after));
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carriers[0]->bo, carrier_map);
   }
   const bool carrier_untouched =
      memcmp(carrier_before, carrier_after, sizeof(carrier_before)) == 0;
   bool carrier_is_poison = true;
   for (unsigned i = 0; i < R300_TRIANGLE_VERTEX_DWORDS; i++)
      carrier_is_poison &= carrier_after[i] == R300_R2VB_PRODUCER_POISON_DWORD;
   const bool carrier_is_reference =
      memcmp(carrier_after, r300_tcl_bypass_triangle_vertices,
             sizeof(r300_tcl_bypass_triangle_vertices)) == 0;
   /* Each source triangle owns seven output-triangle slots.  Initialize
    * every slot to the clipper's degenerate record, then place each
    * admitted triangle at the start of its source group. */
   float expected_carrier[2u *
                          R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT *
                          R300_TRIANGLE_VERTEX_DWORDS] = {0};
   const unsigned records_per_source =
      R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT * 3u;
   const unsigned expected_source_triangles =
      arm == ARM_INSTANCED_ARMED || arm == ARM_INSTANCED_INDEX_ARMED ||
            arm == ARM_MULTI_TRIANGLE_ARMED
         ? 2u
         : 1u;
   for (unsigned source = 0; source < expected_source_triangles; source++) {
      for (unsigned record = 0; record < records_per_source; record++)
         expected_carrier[(source * records_per_source + record) * 4u + 3u] =
            1.0f;
   }
   if (arm == ARM_CULL_BACK_DROPPED_ARMED ||
       arm == ARM_SAMPLE_MASK_ZERO_ARMED ||
       arm == ARM_WRITE_MASK_ZERO_ARMED) {
      /* The culled triangle collapses to three copies of its first
       * transformed record. */
      float first[4];
      memcpy(first, &ndc_triangle[0], 16);
      first[0] = (first[0] + 1.0f) * (R3V_NATIVE_TARGET_WIDTH / 2.0f);
      first[1] = (first[1] + 1.0f) * (R3V_NATIVE_TARGET_HEIGHT / 2.0f);
      for (unsigned v = 0; v < 3; v++)
         memcpy(&expected_carrier[v * 4], first, 16);
   } else if (arm == ARM_MULTI_TRIANGLE_ARMED) {
      for (unsigned source = 0; source < 2; source++) {
         for (unsigned vertex = 0; vertex < 3; vertex++) {
            float *record =
               &expected_carrier[(source * records_per_source + vertex) * 4];
            memcpy(record, &ndc_triangle[vertex * 4], 16);
            if (source == 1)
               record[0] += 0.0625f;
            record[0] =
               (record[0] + 1.0f) * (R3V_NATIVE_TARGET_WIDTH / 2.0f);
            record[1] =
               (record[1] + 1.0f) * (R3V_NATIVE_TARGET_HEIGHT / 2.0f);
         }
      }
   } else {
      const unsigned instances = instance_count ? instance_count : 1;
      for (unsigned i = 0; i < instances; i++) {
         for (unsigned v = 0; v < 3; v++) {
            float *record =
               &expected_carrier[(i * records_per_source + v) * 4];
            memcpy(record, &ndc_triangle[v * 4], 16);
            if (arm == ARM_INSTANCED_ARMED ||
                arm == ARM_INSTANCED_FIRST_INSTANCE_ARMED) {
               const float *offset = &instance_offsets[(first_instance + i) * 4];
               for (unsigned c = 0; c < 4; c++)
                  record[c] += offset[c];
            } else if (arm == ARM_INSTANCED_INDEX_ARMED) {
               record[0] += 0.0625f * (float)(first_instance + i);
            } else if (arm == ARM_VERTEX_INDEX_ARMED) {
               record[1] += 0.0625f * (float)(first_vertex + v);
            }
            record[0] = (record[0] + 1.0f) * (R3V_NATIVE_TARGET_WIDTH / 2.0f);
            record[1] = (record[1] + 1.0f) * (R3V_NATIVE_TARGET_HEIGHT / 2.0f);
         }
      }
   }
   const bool carrier_is_expected =
      memcmp(carrier_after, expected_carrier,
             expected_source_triangles * records_per_source * 4u *
                sizeof(float)) == 0;
   const bool carrier_is_varying_reference =
      memcmp(carrier_after, r300_tcl_bypass_triangle_varying_vertices,
             sizeof(r300_tcl_bypass_triangle_varying_vertices)) == 0;
   /* The robust delivery: the two in-bounds records transform as the
    * reference, and the third reads (0, 0, 0, 1), the window center. */
   uint32_t robust_expected[R300_TRIANGLE_VERTEX_DWORDS];
   memcpy(robust_expected, r300_tcl_bypass_triangle_vertices,
          sizeof(robust_expected));
   {
      const float center[4] = { (float)R3V_NATIVE_TARGET_WIDTH / 2.0f,
                                (float)R3V_NATIVE_TARGET_HEIGHT / 2.0f,
                                0.0f, 1.0f };
      memcpy(&robust_expected[8], center, sizeof(center));
   }
   const bool carrier_is_robust =
      memcmp(carrier_after, robust_expected, sizeof(robust_expected)) == 0;
   bool carrier_is_degenerate = true;
   for (unsigned i = 0;
        i < R300_TRIANGLE_CLIP_MAX_OUTPUT_TRIANGLES_PER_INPUT *
               R300_TRIANGLE_VERTEX_DWORDS;
        i += 4) {
      carrier_is_degenerate &= carrier_after[i] == 0u;
      carrier_is_degenerate &= carrier_after[i + 1] == 0u;
      carrier_is_degenerate &= carrier_after[i + 2] == 0u;
      carrier_is_degenerate &= carrier_after[i + 3] == 0x3f800000u;
   }
   const bool token = file_present(manifest_dir, "attempt.token");

   printf("%s: result=%d status=%d cs_ioctls=%u failed_mmaps=%u "
          "carrier=%s token=%s\n",
          name, submitted, status, cs_ioctls, failed_mmaps,
          carrier_untouched
             ? "untouched"
             : (carrier_is_varying_reference
                   ? "varying"
                   : (carrier_is_reference
                         ? "reference"
                         : (carrier_is_robust
                               ? "robust"
                               : (carrier_is_degenerate
                                     ? "degenerate"
                                     : (carrier_is_poison ? "poison"
                                                          : "other"))))),
          token ? "spent" : "unspent");
   fflush(stdout);

   switch (arm) {
   case ARM_ARMED:
   case ARM_INDEXED_ARMED:
   case ARM_INDEXED_PERMUTED_ARMED:
   case ARM_CULL_BACK_KEPT_ARMED:
      /* The indexed arms deliver the reference carrier: the plain
       * indices name the records in order, and the permuted arm's
       * dereference, first-index offset, and base-vertex sum restore
       * the reference order from the permuted buffer. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_reference);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_INDEXED_ROBUST_RESTART_ARMED:
      /* Restart disabled, 0xffff is an index past the bound; robust on,
       * the F32_3 record reads (0, 0, 0, 1), the window center. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_robust);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_INDEXED_ZERO_RECORD_DEGENERATE_ARMED:
      /* Index 7 addresses zero-initialized bytes inside the bound vertex
       * buffer.  The homogeneous zero position remains contained during
       * clipping and the stream collapses to degenerate records. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_degenerate);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_INDEXED_RESTART_ENABLED_REFUSED:
      /* Returned above at pipeline creation. */
      assert(!"unreachable");
      break;
   case ARM_INDEXED_RANGE_REFUSED:
   case ARM_INDEXED_UNBOUND_REFUSED:
   case ARM_INDEXED_UINT8_REFUSED:
   case ARM_INDEXED_ALIAS_TARGET_REFUSED:
      /* Returned above at vkEndCommandBuffer. */
      assert(!"unreachable");
      break;
   case ARM_VARYING_ARMED:
   case ARM_MULTI_ATTRIBUTE_ARMED:
   case ARM_MULTI_ATTRIBUTE_INTERLEAVED_ARMED:
      /* The CPU route executed the varying job: each record is the
       * transformed reference position followed by the computed tint --
       * or, for the two-attribute job, the color attribute gathered
       * from its own binding or its interleaved offset -- the payload
       * the varying oracle's vertex colors name. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_varying_reference);
      assert(native_cmd->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE);
      assert(native_cmd->reference_count == R300_TRIANGLE_RENDER_SLOT_COUNT);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_SAMPLED_ARMED:
   case ARM_SAMPLED_LAYER_ARMED:
      /* The sampled cell recorded: the varying carrier executed on the
       * CPU route, and the cell binds three relocations with the
       * texture read beside the vertex and color slots. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(native_cmd->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE_SAMPLED);
      assert(native_cmd->reference_count ==
             R300_TRIANGLE_SAMPLED_SLOT_COUNT);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_VARYING_FRAGMENT_MISMATCH:
   case ARM_VARYING_MISSING:
   case ARM_MULTI_ATTRIBUTE_OVERLAP_REFUSED:
      /* Returned above at pipeline creation. */
      assert(!"unreachable");
      break;
   case ARM_MULTI_ATTRIBUTE_UNBOUND_REFUSED:
   case ARM_MULTI_ATTRIBUTE_ALIAS_TARGET_REFUSED:
   case ARM_SAMPLED_ARRAY_VIEW_REFUSED:
      /* Returned above at vkEndCommandBuffer. */
      assert(!"unreachable");
      break;
   case ARM_INSTANCED_ARMED:
   case ARM_INSTANCED_FIRST_INSTANCE_ARMED:
   case ARM_INSTANCED_INDEX_ARMED:
   case ARM_VERTEX_INDEX_ARMED:
   case ARM_INSTANCED_ROBUST_ARMED:
   case ARM_MULTI_TRIANGLE_ARMED:
   case ARM_CULL_BACK_DROPPED_ARMED:
   case ARM_SAMPLE_MASK_ZERO_ARMED:
   case ARM_WRITE_MASK_ZERO_ARMED:
      /* The CPU route expanded the instances: the carrier holds each
       * instance's transformed triangle in instance order -- the robust
       * arm's out-of-bounds offset record read zeros, so its carrier is
       * the reference. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_expected);
      assert(arm != ARM_INSTANCED_ROBUST_ARMED || carrier_is_reference);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_INSTANCED_ZERO_REFUSED:
   case ARM_NON_TRIANGLE_COUNT_REFUSED:
   case ARM_INSTANCED_OUT_OF_BOUNDS_REFUSED:
      /* Returned above at vkEndCommandBuffer. */
      assert(!"unreachable");
      break;
   case ARM_MULTI_ATTRIBUTE_FETCHED_REFUSED:
   case ARM_INDEXED_FETCHED_REFUSED:
   case ARM_INSTANCED_FETCHED_REFUSED:
      /* The fetched admission refuses the two-slot job, and the
       * producer admission refuses the indexed and the instanced draw,
       * before any allocation, reference, IB, or carrier write: the
       * recording is exactly as recorded, the gate unreached, the token
       * unspent. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 0);
      assert(carrier_untouched);
      assert(native_cmd->cell_kind == kind_before);
      assert(native_cmd->reference_count == references_before);
      assert(native_cmd->owned_slot == NULL);
      if (arm == ARM_GPU_FETCHED_COMPOSE_FAILURE) {
         assert(native_cmd->ib_size_dwords == ib_dwords_before);
         assert(native_cmd->window_space_ib_size_dwords ==
                window_dwords_before);
         assert(memcmp(native_cmd->ib, ib_before,
                       (size_t)ib_dwords_before * sizeof(uint32_t)) == 0);
         assert(memcmp(native_cmd->window_space_ib, window_before,
                       (size_t)window_dwords_before * sizeof(uint32_t)) == 0);
      }
      assert(!native_device->gpu_producer_quarantined);
      check_target(device, &target, false, name);
      assert(!token);
      break;
   case ARM_ROBUST_OOB_ENABLED:
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_robust);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_ROBUST_OOB_DISABLED:
      /* Returned above at vkEndCommandBuffer. */
      assert(!"unreachable");
      break;
   case ARM_ROBUST_OOB_W0_DEGENERATE_ARMED:
      /* Robust F32_4 reads supply the homogeneous zero position.  The
       * clipper contains it and emits only degenerate records. */
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_degenerate);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_GATE_CLOSED:
   case ARM_KNOWN_BAD_PREMATURE_DRAW:
   case ARM_RETENTION_FAILURE:
   case ARM_AUTHORIZATION_REFUSED:
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 0);
      assert(carrier_untouched);
      check_target(device, &target, false, name);
      assert(!token);
      break;
   case ARM_MAP_FAILURE:
      /* The vertex mapping fails inside the deferred draw, after every
       * gate and before any write; the disarm follows the draw, so the
       * authorization is unspent.  The driver returns host exhaustion and
       * the runtime folds every driver_submit failure through
       * vk_queue_set_lost, so the application observes device loss with
       * the queue status still at its refusal entry value. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(failed_mmaps >= 1);
      assert(cs_ioctls == 0);
      assert(carrier_untouched);
      check_target(device, &target, false, name);
      assert(!token);
      break;
   case ARM_IOCTL_REFUSED:
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 1);
      assert(carrier_is_reference);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_COMPLETION_FAILURE:
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE);
      assert(cs_ioctls == 1);
      assert(carrier_is_reference);
      check_target(device, &target, true, name);
      assert(token);
      break;
   case ARM_GPU_FETCHED_COMPOSED:
   case ARM_GPU_FETCHED_COMPOSED_F32_3:
   case ARM_GPU_FETCHED_COMPOSED_F32_2:
      /* The ioctl ran on the composed stream and the token was spent;
       * the shim executes no producer, so the carrier still holds the
       * poison the admission published, the read-back verdict reports
       * device loss, and the capability quarantines.  The completion
       * wait retires the transport before the wrong-result verdict.
       */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_poison);
      assert(native_device->gpu_producer_quarantined);
      assert(native_cmd->cell_kind ==
             R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED);
      assert(native_cmd->reference_count == 4);
      assert(native_cmd->owned_slot != NULL);
      assert(native_cmd->references[2].memory == native_cmd->owned_slot);
      assert(native_cmd->references[3].handle ==
             r3v_native_memory_from_handle(vertex_memory)->bo.handle);
      {
         void *slot_map = NULL;
         assert(radeon_drm_vk_bo_map(&native_device->drm,
                                     &native_cmd->owned_slot->bo,
                                     &slot_map) == 0);
         uint32_t slot_expected[12];
         assert(r300_r2vb_fetched_producer_slot_positions(
                   3, slot_expected, 12) == 0);
         assert(memcmp(slot_map, slot_expected, sizeof(slot_expected)) == 0);
         radeon_drm_vk_bo_unmap(&native_device->drm,
                                &native_cmd->owned_slot->bo, slot_map);
      }
      check_target(device, &target, true, name);
      assert(token);
      assert(file_present(manifest_dir, "gpu_carrier_observed.bin"));
      break;
   case ARM_GPU_FETCHED_COMPOSE_FAILURE:
   case ARM_GPU_FETCHED_OFFSET_MISALIGNED:
   case ARM_GPU_FETCHED_STRIDE_MISALIGNED:
   case ARM_GPU_FETCHED_OUT_OF_DOMAIN:
   case ARM_GPU_FETCHED_OUT_OF_BOUNDS:
   case ARM_GPU_FETCHED_ALIASED_SOURCE:
      /* The injected composition failure and each named admission
       * refusal stop before any allocation, reference, IB, or carrier
       * write: the recording is exactly as recorded. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 0);
      assert(carrier_untouched);
      assert(native_cmd->cell_kind == kind_before);
      assert(native_cmd->reference_count == references_before);
      assert(native_cmd->owned_slot == NULL);
      assert(!native_device->gpu_producer_quarantined);
      check_target(device, &target, false, name);
      assert(!token);
      break;
   case ARM_GPU_FETCHED_WRONG_DIGEST:
      /* The admission composed the fetched stream and poisoned the
       * driver-owned carrier; the arming gate then refused the stream
       * against the immediate route's identity, so no ioctl ran, the
       * token is unspent, and the application-visible target is
       * untouched. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 0);
      assert(carrier_is_poison);
      assert(native_cmd->cell_kind ==
             R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED);
      assert(native_cmd->reference_count == 4);
      assert(!native_device->gpu_producer_quarantined);
      check_target(device, &target, false, name);
      assert(!token);
      break;
   }

   /* Semantic-cell retention precedes the final gate verdict.  Every arm
    * that reaches it retains ib.bin, including the closed gate; admission
    * refusals before command submission and the injected write failure do
    * not retain the file. */
   const bool ib_retained = file_present(manifest_dir, "ib.bin");
   if (arm == ARM_RETENTION_FAILURE ||
       arm == ARM_GPU_FETCHED_COMPOSE_FAILURE || fetched_refusal_arm ||
       arm == ARM_MULTI_ATTRIBUTE_FETCHED_REFUSED ||
       arm == ARM_INDEXED_FETCHED_REFUSED ||
       arm == ARM_INSTANCED_FETCHED_REFUSED)
      assert(!ib_retained);
   else
      assert(ib_retained);

   /* The bytes the armed submit retained and handed to the ioctl are the
    * module-constant identity the recording installed, so the
    * re-sequenced submit path moves no dword of the recorded cell. */
   if (arm == ARM_ARMED || arm == ARM_INDEXED_ARMED ||
       arm == ARM_INDEXED_PERMUTED_ARMED ||
       arm == ARM_INSTANCED_FIRST_INSTANCE_ARMED ||
       arm == ARM_VERTEX_INDEX_ARMED || arm == ARM_INSTANCED_ROBUST_ARMED) {
      char submitted_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      uint32_t submitted_dwords;
      retained_ib_digest(manifest_dir, submitted_digest, &submitted_dwords);
      assert(submitted_dwords == module_clip_dwords);
      assert(strcmp(submitted_digest, module_clip_digest) == 0);
   }
   /* The two-instance armed arms submitted the two-triangle family
    * member the recording installed. */
   if (arm == ARM_INSTANCED_ARMED || arm == ARM_INSTANCED_INDEX_ARMED ||
       arm == ARM_MULTI_TRIANGLE_ARMED) {
      char submitted_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      uint32_t submitted_dwords;
      retained_ib_digest(manifest_dir, submitted_digest, &submitted_dwords);
      assert(submitted_dwords == two_triangle_dwords);
      assert(strcmp(submitted_digest, two_triangle_digest) == 0);
   }
   /* The fetched arms that reached retention retained the submit-time
    * composition, and its bytes are the offline no-submit composition's
    * -- the reference producer prefix, itself held to the retained
    * silicon pin, ahead of the recorded module-constant consumer; that
    * is the digest the composed arm's gate matched and the wrong-digest
    * arm's gate refused against the immediate route's identity. */
   if (arm == ARM_GPU_FETCHED_COMPOSED ||
       arm == ARM_GPU_FETCHED_COMPOSED_F32_3 ||
       arm == ARM_GPU_FETCHED_COMPOSED_F32_2 ||
       arm == ARM_GPU_FETCHED_WRONG_DIGEST) {
      char submitted_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      uint32_t submitted_dwords;
      retained_ib_digest(manifest_dir, submitted_digest, &submitted_dwords);
      assert(submitted_dwords == R300_FETCHED_F32_4_ROUTE_IB_DWORDS);
      assert(route_dwords == submitted_dwords);
      assert(strcmp(submitted_digest, route_digest) == 0);
      assert(strcmp(submitted_digest, R300_RETAINED_GPU_ROUTE_IB_BLAKE3) != 0);
      /* The three widths are three cells: one stream geometry, three
       * digests. */
      assert(strcmp(R300_FETCHED_F32_4_ROUTE_IB_BLAKE3,
                    R300_FETCHED_F32_3_ROUTE_IB_BLAKE3) != 0);
      assert(strcmp(R300_FETCHED_F32_3_ROUTE_IB_BLAKE3,
                    R300_FETCHED_F32_2_ROUTE_IB_BLAKE3) != 0);
   }

   free(window_before);
   free(ib_before);

   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyBuffer(device, color_buffer, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyBuffer(device, index_buffer, NULL);
   vkFreeMemory(device, index_memory, NULL);
   vkDestroyImageView(device, target.view, NULL);
   vkDestroyImage(device, target.image, NULL);
   vkFreeMemory(device, target.memory, NULL);
   vkDestroyDevice(device, NULL);
   destroy_instance(instance, NULL);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <arm>\n", argv[0]);
      return 2;
   }
   for (size_t i = 0; i < sizeof(arm_names) / sizeof(arm_names[0]); i++) {
      if (strcmp(argv[1], arm_names[i].name) == 0) {
         run_arm(arm_names[i].arm, arm_names[i].name);
         printf("r3v-native-submit-order-%s: PASS\n", argv[1]);
         return 0;
      }
   }
   fprintf(stderr, "unknown arm: %s\n", argv[1]);
   return 2;
}
