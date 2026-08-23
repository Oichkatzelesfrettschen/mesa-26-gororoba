/*
 * SPDX-License-Identifier: MIT
 *
 * Submit-order matrix on the drm-shim fixture: one public
 * render-pass/pipeline/draw recording with a load-op clear over a seeded
 * target, submitted once per arm on a fresh device.  The armed arm is the
 * positive control (the shim absorbs the CS ioctl; the draw executes and
 * the one-shot token is spent).  Every pre-commit failure -- closed gate,
 * unwritable evidence directory, refused authorization, failed vertex
 * mapping -- leaves the target and the carrier byte-identical to their
 * pre-submit content and spends no authorization; the two transport
 * failures -- refused CS ioctl, failed completion wait -- come after the
 * draw, so the draw has executed exactly once and the token is spent.
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
#include "amd/r300/common/tests/r300_fetched_route_digests.h"
#include "amd/r300/common/tests/r300_retained_route_digests.h"
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
   ARM_RETENTION_UNWRITABLE,
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
    * F32_4 attribute, the third record reads w = 0, outside the CPU
    * route's admitted clip volume, and the draw refuses by name;
    * disabled, the record-time bound proof refuses the recording. */
   ARM_ROBUST_OOB_ENABLED,
   ARM_ROBUST_OOB_W0_REFUSED,
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
};


static const struct {
   const char *name;
   enum arm arm;
} arm_names[] = {
   { "armed", ARM_ARMED },
   { "gate-closed", ARM_GATE_CLOSED },
   { "retention-unwritable", ARM_RETENTION_UNWRITABLE },
   { "authorization-refused", ARM_AUTHORIZATION_REFUSED },
   { "map-failure", ARM_MAP_FAILURE },
   { "ioctl-refused", ARM_IOCTL_REFUSED },
   { "completion-failure", ARM_COMPLETION_FAILURE },
   { "known-bad-premature-draw", ARM_KNOWN_BAD_PREMATURE_DRAW },
   { "robust-oob-enabled", ARM_ROBUST_OOB_ENABLED },
   { "robust-oob-w0-refused", ARM_ROBUST_OOB_W0_REFUSED },
   { "robust-oob-disabled", ARM_ROBUST_OOB_DISABLED },
   { "gpu-fetched-composed", ARM_GPU_FETCHED_COMPOSED },
   { "gpu-fetched-compose-failure", ARM_GPU_FETCHED_COMPOSE_FAILURE },
   { "gpu-fetched-wrong-digest", ARM_GPU_FETCHED_WRONG_DIGEST },
   { "gpu-fetched-offset-misaligned", ARM_GPU_FETCHED_OFFSET_MISALIGNED },
   { "gpu-fetched-stride-misaligned", ARM_GPU_FETCHED_STRIDE_MISALIGNED },
   { "gpu-fetched-out-of-domain", ARM_GPU_FETCHED_OUT_OF_DOMAIN },
   { "gpu-fetched-out-of-bounds", ARM_GPU_FETCHED_OUT_OF_BOUNDS },
   { "gpu-fetched-aliased-source", ARM_GPU_FETCHED_ALIASED_SOURCE },
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
   f(vkCmdPipelineBarrier) f(vkGetDeviceQueue) f(vkQueueSubmit)            \
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

static int
run_arm(enum arm arm, const char *name)
{
   current_arm = arm;
   cs_ioctls = 0;
   failed_mmaps = 0;

   /* The gate and the evidence directory are read at device creation,
    * so every arm builds its own device under its own environment. */
   char manifest_dir[] = "/tmp/r3v-native-submit-order-XXXXXX";
   assert(mkdtemp(manifest_dir) != NULL);
   setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
   if (arm == ARM_GATE_CLOSED || arm == ARM_KNOWN_BAD_PREMATURE_DRAW)
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   else
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   if (arm == ARM_RETENTION_UNWRITABLE)
      assert(chmod(manifest_dir, 0500) == 0);

   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);
   char reference_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);
   assert(reference.ib_size_dwords == R300_RETAINED_CPU_ROUTE_IB_DWORDS);
   assert(strcmp(reference_digest, R300_RETAINED_CPU_ROUTE_IB_BLAKE3) == 0);
   r300_tcl_bypass_triangle_release(&reference);
   const bool fetched_refusal_arm =
      arm == ARM_GPU_FETCHED_OFFSET_MISALIGNED ||
      arm == ARM_GPU_FETCHED_STRIDE_MISALIGNED ||
      arm == ARM_GPU_FETCHED_OUT_OF_DOMAIN ||
      arm == ARM_GPU_FETCHED_OUT_OF_BOUNDS ||
      arm == ARM_GPU_FETCHED_ALIASED_SOURCE;
   const bool fetched_arm = arm == ARM_GPU_FETCHED_COMPOSED ||
                            arm == ARM_GPU_FETCHED_COMPOSE_FAILURE ||
                            arm == ARM_GPU_FETCHED_WRONG_DIGEST ||
                            fetched_refusal_arm;
   if (arm == ARM_AUTHORIZATION_REFUSED) {
      char wrong[BLAKE3_OUT_LEN * 2 + 1];
      memcpy(wrong, reference_digest, sizeof(wrong));
      wrong[0] = wrong[0] == '0' ? '1' : '0';
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", wrong, 1);
   } else if (arm == ARM_GPU_FETCHED_WRONG_DIGEST) {
      /* The immediate route's retained identity, declared against the
       * fetched composition. */
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
             R300_RETAINED_GPU_ROUTE_IB_BLAKE3, 1);
   } else if (fetched_arm) {
      /* The offline no-submit composition identity of the fetched F32_4
       * route over this arm's exact geometry: one-page source at offset
       * zero, stride 16, one-page slot BO, the reference consumer. */
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3",
             R300_FETCHED_F32_4_ROUTE_IB_BLAKE3, 1);
   } else {
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", reference_digest, 1);
   }
   struct utsname host;
   assert(uname(&host) == 0);
   setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
   setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
          R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
   if (fetched_arm) {
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
                           arm == ARM_ROBUST_OOB_W0_REFUSED ||
                           arm == ARM_ROBUST_OOB_DISABLED;
   const bool robust_enabled = arm == ARM_ROBUST_OOB_ENABLED ||
                               arm == ARM_ROBUST_OOB_W0_REFUSED ||
                               arm == ARM_GPU_FETCHED_OUT_OF_BOUNDS;
   /* Per-arm stream geometry: the bind offset, binding stride, buffer
    * size, and whether the buffer binds into the color target's memory. */
   const VkDeviceSize bind_offset =
      arm == ARM_GPU_FETCHED_OFFSET_MISALIGNED ? 2 : 0;
   const uint32_t binding_stride =
      arm == ARM_GPU_FETCHED_STRIDE_MISALIGNED ? 18 : 16;
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
   native_device->arming_provider = &r3v_native_shim_arming_provider;
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
      for (unsigned v = 0; v < 3; v++)
         memcpy((uint8_t *)map + bind_offset + v * binding_stride,
                &records[v * 4], 4 * sizeof(float));
      vkUnmapMemory(device, vertex_memory);
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
   VkPipelineLayout layout = VK_NULL_HANDLE;
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             },
             NULL, &layout) == VK_SUCCESS);
   VkShaderModule vs = make_module(device, r3v_reference_vertex_spirv,
                                   sizeof(r3v_reference_vertex_spirv));
   VkShaderModule fs = make_module(device, r3v_reference_fragment_spirv,
                                   sizeof(r3v_reference_fragment_spirv));
   VkPipeline pipeline = VK_NULL_HANDLE;
   assert(vkCreateGraphicsPipelines(
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
                   &(VkPipelineVertexInputStateCreateInfo){
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
                                       arm == ARM_ROBUST_OOB_DISABLED)
                                         ? VK_FORMAT_R32G32B32_SFLOAT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT,
                            .offset = 0,
                         },
                   },
                .pInputAssemblyState =
                   &(VkPipelineInputAssemblyStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
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
                      .cullMode = VK_CULL_MODE_NONE,
                      .lineWidth = 1.0f,
                   },
                .pMultisampleState =
                   &(VkPipelineMultisampleStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                   },
                .pColorBlendState =
                   &(VkPipelineColorBlendStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                      .attachmentCount = 1,
                      .pAttachments =
                         &(VkPipelineColorBlendAttachmentState){
                            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                              VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT |
                                              VK_COLOR_COMPONENT_A_BIT,
                         },
                   },
                .layout = layout,
                .renderPass = pass,
             },
             NULL, &pipeline) == VK_SUCCESS);

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
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ bind_offset });
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   const VkResult ended = vkEndCommandBuffer(cmd);
   if (arm == ARM_ROBUST_OOB_DISABLED) {
      /* The feature off, the record-time bound proof poisons the
       * recording, so the application sees the refusal at end and
       * nothing reaches the queue. */
      assert(ended == R3V_NATIVE_REFUSAL_RESULT);
      printf("%s: record refused at vkEndCommandBuffer (%d)\n", name,
             ended);
      return 0;
   }
   assert(ended == VK_SUCCESS);

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(cmd);
   assert(native_cmd->ib_size_dwords != 0 && native_cmd->owned_carrier);
   uint32_t carrier_before[R300_TRIANGLE_VERTEX_DWORDS];
   {
      void *carrier_map = NULL;
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carrier->bo,
                                  &carrier_map) == 0);
      memcpy(carrier_before, carrier_map, sizeof(carrier_before));
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carrier->bo, carrier_map);
   }

   if (arm == ARM_KNOWN_BAD_PREMATURE_DRAW) {
      assert(r3v_native_cmd_buffer_execute_deferred_draw(
                native_device, native_cmd) == VK_SUCCESS);
   }
   if (arm == ARM_GPU_FETCHED_COMPOSE_FAILURE)
      native_device->gpu_producer_compose_inject_errno = -ENOMEM;
   const uint32_t references_before = native_cmd->reference_count;
   const enum r3v_native_cell_kind kind_before = native_cmd->cell_kind;
   assert(references_before == R300_TRIANGLE_SLOT_COUNT);
   assert(kind_before == R3V_NATIVE_CELL_KIND_TRIANGLE);
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
   if (arm == ARM_RETENTION_UNWRITABLE)
      assert(chmod(manifest_dir, 0700) == 0);

   uint32_t carrier_after[R300_TRIANGLE_VERTEX_DWORDS];
   {
      void *carrier_map = NULL;
      assert(radeon_drm_vk_bo_map(&native_device->drm,
                                  &native_cmd->owned_carrier->bo,
                                  &carrier_map) == 0);
      memcpy(carrier_after, carrier_map, sizeof(carrier_after));
      radeon_drm_vk_bo_unmap(&native_device->drm,
                             &native_cmd->owned_carrier->bo, carrier_map);
   }
   const bool carrier_untouched =
      memcmp(carrier_before, carrier_after, sizeof(carrier_before)) == 0;
   bool carrier_is_poison = true;
   for (unsigned i = 0; i < R300_TRIANGLE_VERTEX_DWORDS; i++)
      carrier_is_poison &= carrier_after[i] == R300_R2VB_PRODUCER_POISON_DWORD;
   const bool carrier_is_reference =
      memcmp(carrier_after, r300_tcl_bypass_triangle_vertices,
             sizeof(carrier_after)) == 0;
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
      memcmp(carrier_after, robust_expected, sizeof(carrier_after)) == 0;
   const bool token = file_present(manifest_dir, "attempt.token");

   printf("%s: result=%d status=%d cs_ioctls=%u failed_mmaps=%u "
          "carrier=%s token=%s\n",
          name, submitted, status, cs_ioctls, failed_mmaps,
          carrier_untouched
             ? "untouched"
             : (carrier_is_reference
                   ? "reference"
                   : (carrier_is_robust
                         ? "robust"
                         : (carrier_is_poison ? "poison" : "other"))),
          token ? "spent" : "unspent");
   fflush(stdout);

   switch (arm) {
   case ARM_ARMED:
      assert(submitted == VK_SUCCESS);
      assert(status == R3V_NATIVE_QUEUE_STATUS_COMPLETED);
      assert(cs_ioctls == 1);
      assert(carrier_is_reference);
      check_target(device, &target, true, name);
      assert(token);
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
   case ARM_ROBUST_OOB_W0_REFUSED:
      /* Refuses inside the deferred draw, after every gate and before
       * any write: the w = 0 record leaves the admitted clip volume. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
      assert(cs_ioctls == 0);
      assert(carrier_untouched);
      check_target(device, &target, false, name);
      assert(!token);
      break;
   case ARM_GATE_CLOSED:
   case ARM_KNOWN_BAD_PREMATURE_DRAW:
   case ARM_RETENTION_UNWRITABLE:
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
      /* The ioctl ran on the composed stream and the token was spent;
       * the shim executes no producer, so the carrier still holds the
       * poison the admission published, the read-back verdict reports
       * device loss, and the capability quarantines.  The status stays
       * at SUBMITTED: the verdict returns before the completion status
       * is recorded. */
      assert(submitted == VK_ERROR_DEVICE_LOST);
      assert(status == R3V_NATIVE_QUEUE_STATUS_SUBMITTED);
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

   /* Evidence retention precedes the gate verdict: every armed-gate arm
    * that reached retention keeps ib.bin; the closed gate, the
    * unwritable directory, and the admission refused ahead of retention
    * retain none. */
   const bool ib_retained = file_present(manifest_dir, "ib.bin");
   if (arm == ARM_RETENTION_UNWRITABLE ||
       arm == ARM_GPU_FETCHED_COMPOSE_FAILURE || fetched_refusal_arm)
      assert(!ib_retained);
   else if (arm != ARM_GATE_CLOSED && arm != ARM_KNOWN_BAD_PREMATURE_DRAW)
      assert(ib_retained);

   /* The bytes the armed submit retained and handed to the ioctl are the
    * retained CPU route identity, so the re-sequenced submit path moves
    * no dword of the reference cell. */
   if (arm == ARM_ARMED) {
      char submitted_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      uint32_t submitted_dwords;
      retained_ib_digest(manifest_dir, submitted_digest, &submitted_dwords);
      assert(submitted_dwords == R300_RETAINED_CPU_ROUTE_IB_DWORDS);
      assert(strcmp(submitted_digest, R300_RETAINED_CPU_ROUTE_IB_BLAKE3) == 0);
   }
   /* The fetched arms that reached retention retained the submit-time
    * composition, and its bytes are the offline no-submit composition's
    * -- the digest the composed arm's gate matched and the wrong-digest
    * arm's gate refused against the immediate route's identity. */
   if (arm == ARM_GPU_FETCHED_COMPOSED || arm == ARM_GPU_FETCHED_WRONG_DIGEST) {
      char submitted_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
      uint32_t submitted_dwords;
      retained_ib_digest(manifest_dir, submitted_digest, &submitted_dwords);
      assert(submitted_dwords == R300_FETCHED_F32_4_ROUTE_IB_DWORDS);
      assert(strcmp(submitted_digest, R300_FETCHED_F32_4_ROUTE_IB_BLAKE3) ==
             0);
      assert(strcmp(submitted_digest, R300_RETAINED_GPU_ROUTE_IB_BLAKE3) != 0);
   }

   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
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
