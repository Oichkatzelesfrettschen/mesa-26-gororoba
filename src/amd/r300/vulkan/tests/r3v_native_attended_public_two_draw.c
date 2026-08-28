/*
 * SPDX-License-Identifier: MIT
 *
 * Attended public two-draw cell: records two render passes with a draw
 * each through the application-shaped Vulkan surface -- two images,
 * one render pass, two framebuffers, one pipeline over each admitted
 * fragment module, one vertex buffer, two draws -- and drives the
 * concatenated stream to a live DRM_RADEON_CS on RS482 silicon.  The
 * recorder route's two-pass arm proved the appended cell executes; this
 * arm proves the public route records that same stream: before the
 * submission the runner digests the recorded indirect buffer and
 * refuses unless it equals the offline emitter's, so the authorization
 * names the bytes the command processor reads.  Each pass's constant
 * names the pass that wrote its target.  Runs only under the
 * authorization and procedure in
 * docs/hardware/r3v-native-attended-public-two-draw-procedure.md; every
 * stage prints and flushes before it runs.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_watchdog_guard.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native_multi_pass_arms.h"

#include "util/mesa-blake3.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

static bool
same_directory(const char *a, const char *b)
{
   if (strcmp(a, b) == 0)
      return true;
   char resolved_a[PATH_MAX];
   char resolved_b[PATH_MAX];
   return realpath(a, resolved_a) != NULL && realpath(b, resolved_b) != NULL &&
          strcmp(resolved_a, resolved_b) == 0;
}

static void
report(const char *label, const struct r300_triangle_coverage_verdict *v)
{
   printf("[oracle] %s judged=%d coverage_exact=%d canary=%d interior=%u "
          "analytic=%u exterior=%u ambiguous=%u mismatch=%u\n",
          label, v->judged, v->coverage_exact, v->canary_pass,
          v->interior_pixels, v->analytic_pixels, v->exterior_pixels,
          v->ambiguous_pixels, v->mismatch_pixels);
   if (!v->judged)
      printf("[oracle] %s verdict refused: the shape or the retained "
             "footprint left the producer's domain, so the zero counters "
             "carry no claim about the render\n",
             label);
   fflush(stdout);
}

/* One pass's public objects: the image, its memory, its view, and its
 * framebuffer over the shared render pass.
 */
struct pass_target {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView view;
   VkFramebuffer framebuffer;
};

int
main(int argc, char **argv)
{
   /* --record-only builds every object and records the command buffer,
    * then stops at the recording boundary after comparing the recorded
    * stream's digest against the emitter's.  The public recording
    * contract -- image family, pass shape, pipeline admission, the
    * two-pass concatenation and its binding -- is what this program can
    * get wrong with no device present, so the shim fixture calibrates
    * it here and the attended run inherits a proven sequence.
    */
   bool record_only = false;
   const char *waiver_path = NULL;
   bool usage_error = argc < 2;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--record-only") == 0)
         record_only = true;
      else if (strcmp(argv[i], "--waiver") == 0 && i + 1 < argc)
         waiver_path = argv[++i];
      else
         usage_error = true;
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <evidence-directory> [--record-only] "
              "[--waiver <path>]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return 1;
   }

   /* The stream the public route records: both passes the reference
    * shape, the first under the reference fragment module's green and
    * the second under the blue module's, bound at merged indices 2 and
    * 3.
    */
   struct r300_triangle_multi_pass mp;
   r3v_native_multi_pass_public_reference(&mp);

   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&mp.pass[0]);
   const uint32_t first_dword =
      r300_tcl_bypass_triangle_render_shape_draw_dword(&mp.pass[0]);
   const uint32_t second_dword =
      r300_tcl_bypass_triangle_render_shape_draw_dword(&mp.pass[1]);

   struct r300_tcl_bypass_triangle_ib armed;
   if (r300_tcl_bypass_triangle_multi_pass_emit(&mp, &armed) != 0) {
      fprintf(stderr, "the two-pass cell refused to emit\n");
      return 1;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed.ib, armed.ib_size_dwords, digest);
   const uint32_t ib_dwords = armed.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&armed);

   printf("[shape] public two-draw, two passes %ux%u pitch %u, binding "
          "(%u, %u), %u IB dwords, cell blake3 %.8s\n",
          mp.pass[0].width, mp.pass[0].height, mp.pass[0].pitch_pixels,
          mp.second_vertex_index, mp.second_color_index, ib_dwords, digest);
   printf("[predict] first target: interior 0x%08x over the analytic "
          "triangle, exterior 0x%08x, canary clean; second target: "
          "interior 0x%08x, exterior 0x%08x, canary clean\n",
          first_dword, R300_TRIANGLE_COLOR_SENTINEL, second_dword,
          R300_TRIANGLE_COLOR_SENTINEL);
   printf("[predict] falsifier: a recorded stream whose digest differs "
          "from the emitter's refuses before any ioctl; a target holding "
          "the other pass's constant names state crossing the pass "
          "boundary; a second target holding the sentinel names a second "
          "cell the command processor never reached\n");
   fflush(stdout);

   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared != NULL && declared[0] != '\0' &&
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s\n",
              declared, evidence_dir);
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", evidence_dir, 1);

   struct r3v_native_watchdog_guard guard = {0};
   if (!record_only) {
      stage("watchdog");
      if (r3v_native_watchdog_guard_open(&guard, waiver_path, evidence_dir,
                                         digest) != 0)
         return 2;
   }

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", result);
      return 1;
   }

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || pdev_count != 1 ||
       pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS482\n");
      return 1;
   }

   stage("device");
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = vkCreateDevice(
      pdev,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos =
            &(VkDeviceQueueCreateInfo){
               .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
               .queueFamilyIndex = 0,
               .queueCount = 1,
               .pQueuePriorities = &priority,
            },
      },
      NULL, &device);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %d\n", result);
      return 1;
   }

   r3v_native_watchdog_guard_install(&guard, device);

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkCreateBuffer);
   LOAD_DEVICE(vkDestroyBuffer);
   LOAD_DEVICE(vkBindBufferMemory);
   LOAD_DEVICE(vkCreateImage);
   LOAD_DEVICE(vkDestroyImage);
   LOAD_DEVICE(vkGetImageMemoryRequirements);
   LOAD_DEVICE(vkBindImageMemory);
   LOAD_DEVICE(vkCreateImageView);
   LOAD_DEVICE(vkDestroyImageView);
   LOAD_DEVICE(vkCreateRenderPass);
   LOAD_DEVICE(vkDestroyRenderPass);
   LOAD_DEVICE(vkCreateFramebuffer);
   LOAD_DEVICE(vkDestroyFramebuffer);
   LOAD_DEVICE(vkCreateShaderModule);
   LOAD_DEVICE(vkDestroyShaderModule);
   LOAD_DEVICE(vkCreatePipelineLayout);
   LOAD_DEVICE(vkDestroyPipelineLayout);
   LOAD_DEVICE(vkCreateGraphicsPipelines);
   LOAD_DEVICE(vkDestroyPipeline);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkCmdBeginRenderPass);
   LOAD_DEVICE(vkCmdEndRenderPass);
   LOAD_DEVICE(vkCmdBindPipeline);
   LOAD_DEVICE(vkCmdBindVertexBuffers);
   LOAD_DEVICE(vkCmdDraw);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

#define CHECK(call)                                        \
   do {                                                    \
      VkResult check_result = (call);                      \
      if (check_result != VK_SUCCESS) {                    \
         fprintf(stderr, "%s: %d\n", #call, check_result); \
         return 1;                                         \
      }                                                    \
   } while (0)

   /* Two color targets, each the 64x64 B8G8R8A8 surface whose memory
    * requirement is the cell footprint with the canary row.  Both carry
    * the sentinel before the submission, so every dword either target
    * holds afterward names its writer.
    */
   stage("targets");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = R3V_NATIVE_TARGET_FORMAT,
      .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   struct pass_target target[2] = { { VK_NULL_HANDLE } };
   for (unsigned i = 0; i < 2; i++) {
      CHECK(vkCreateImage(device, &image_info, NULL, &target[i].image));
      VkMemoryRequirements reqs;
      vkGetImageMemoryRequirements(device, target[i].image, &reqs);
      if (reqs.size != R3V_NATIVE_TARGET_MEMORY_BYTES ||
          reqs.size < color_bytes) {
         fprintf(stderr, "target %u requirement %llu is not the cell "
                 "footprint %u\n",
                 i, (unsigned long long)reqs.size, color_bytes);
         return 1;
      }
      CHECK(vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = 0,
         },
         NULL, &target[i].memory));
      CHECK(vkBindImageMemory(device, target[i].image, target[i].memory, 0));
      void *map = NULL;
      CHECK(vkMapMemory(device, target[i].memory, 0, VK_WHOLE_SIZE, 0, &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < reqs.size / 4; p++)
         pixels[p] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, target[i].memory);
   }

   /* The application's vertex records: the reference triangle's
    * pretransformed positions as F32_4, one buffer both passes bind.
    * Each pass's deferred draw gathers them into its own carrier, so
    * the buffer's memory is no reference of the submission.
    */
   stage("vertex stream");
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   CHECK(vkAllocateMemory(device,
                          &(VkMemoryAllocateInfo){
                             .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = 4096,
                             .memoryTypeIndex = 0,
                          },
                          NULL, &vertex_memory));
   CHECK(vkCreateBuffer(device,
                        &(VkBufferCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = sizeof(float) * R300_TRIANGLE_VERTEX_DWORDS,
                           .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                        },
                        NULL, &vertex_buffer));
   CHECK(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
   {
      float vertices[R300_TRIANGLE_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_render_shape_vertices(&mp.pass[0], vertices);
      void *map = NULL;
      CHECK(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map));
      memcpy(map, vertices, sizeof(vertices));
      vkUnmapMemory(device, vertex_memory);
   }

   stage("pipelines");
   VkRenderPass pass = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   CHECK(vkCreateRenderPass(
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
      NULL, &pass));
   for (unsigned i = 0; i < 2; i++) {
      CHECK(vkCreateImageView(
         device,
         &(VkImageViewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = target[i].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = R3V_NATIVE_TARGET_FORMAT,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .levelCount = 1,
                                  .layerCount = 1 },
         },
         NULL, &target[i].view));
      CHECK(vkCreateFramebuffer(
         device,
         &(VkFramebufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = pass,
            .attachmentCount = 1,
            .pAttachments = &target[i].view,
            .width = R3V_NATIVE_TARGET_WIDTH,
            .height = R3V_NATIVE_TARGET_HEIGHT,
            .layers = 1,
         },
         NULL, &target[i].framebuffer));
   }
   CHECK(vkCreatePipelineLayout(
      device,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      },
      NULL, &layout));

   /* One pipeline per admitted fragment module: the reference module's
    * green for the first pass, the blue module's for the second, so the
    * stream is r3v_native_multi_pass_public_reference's.
    */
   VkShaderModule vs = VK_NULL_HANDLE;
   CHECK(vkCreateShaderModule(
      device,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(r3v_reference_vertex_spirv),
         .pCode = r3v_reference_vertex_spirv,
      },
      NULL, &vs));
   const uint32_t *const fragment_words[2] = {
      r3v_reference_fragment_spirv, r3v_reference_fragment_blue_spirv
   };
   const size_t fragment_bytes[2] = { sizeof(r3v_reference_fragment_spirv),
                                      sizeof(r3v_reference_fragment_blue_spirv) };
   VkPipeline pipeline[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   for (unsigned i = 0; i < 2; i++) {
      VkShaderModule fs = VK_NULL_HANDLE;
      CHECK(vkCreateShaderModule(
         device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = fragment_bytes[i],
            .pCode = fragment_words[i],
         },
         NULL, &fs));
      const VkGraphicsPipelineCreateInfo pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages =
            (VkPipelineShaderStageCreateInfo[]){
               { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                 .module = vs,
                 .pName = "main" },
               { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
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
                     .stride = 16,
                     .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                  },
               .vertexAttributeDescriptionCount = 1,
               .pVertexAttributeDescriptions =
                  &(VkVertexInputAttributeDescription){
                     .location = 0,
                     .binding = 0,
                     .format = VK_FORMAT_R32G32B32A32_SFLOAT,
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
               .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
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
                     .colorWriteMask =
                        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                  },
            },
         .layout = layout,
         .renderPass = pass,
      };
      CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                      &pipeline_info, NULL, &pipeline[i]));
      vkDestroyShaderModule(device, fs, NULL);
   }
   vkDestroyShaderModule(device, vs, NULL);

   /* Two render passes with a draw each.  Each pass's load-op clear is
    * the sentinel the oracle reads as exterior and canary; the public
    * draw path realizes it as a host fill of the target footprint ahead
    * of the device draw, so the exterior prediction is the sentinel on
    * either realization.
    */
   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   CHECK(vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool));
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   CHECK(vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd));
   CHECK(vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           }));
   const float sentinel = (float)0xa5 / 255.0f;
   const VkClearValue clear = { .color = { .float32 = { sentinel, sentinel,
                                                        sentinel,
                                                        sentinel } } };
   for (unsigned i = 0; i < 2; i++) {
      vkCmdBeginRenderPass(
         cmd,
         &(VkRenderPassBeginInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass,
            .framebuffer = target[i].framebuffer,
            .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                        R3V_NATIVE_TARGET_HEIGHT } },
            .clearValueCount = 1,
            .pClearValues = &clear,
         },
         VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline[i]);
      vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &(VkDeviceSize){ 0 });
      vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
   }
   CHECK(vkEndCommandBuffer(cmd));

   /* The recorded stream against the emitter's: the authorization names
    * the emitter's digest, so a recording that differs refuses here,
    * ahead of any ioctl, with the differing dwords named.
    */
   stage("recorded stream");
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, cmd);
   char recorded_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(native->ib, native->ib_size_dwords,
                               recorded_digest);
   printf("[record] kind=%d references=%u deferred_draws=%u ib_dwords=%u "
          "recorded blake3 %.8s emitted blake3 %.8s\n",
          (int)native->cell_kind, native->reference_count,
          native->deferred_draw_count, native->ib_size_dwords,
          recorded_digest, digest);
   fflush(stdout);
   const bool stream_agrees =
      native->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS &&
      native->reference_count == 4 && native->ib_size_dwords == ib_dwords &&
      strcmp(recorded_digest, digest) == 0;
   if (!stream_agrees) {
      struct r300_tcl_bypass_triangle_ib offline;
      if (r300_tcl_bypass_triangle_multi_pass_emit(&mp, &offline) == 0) {
         uint32_t differing = 0;
         const uint32_t common = offline.ib_size_dwords < native->ib_size_dwords
                                    ? offline.ib_size_dwords
                                    : native->ib_size_dwords;
         for (uint32_t i = 0; i < common; i++) {
            if (offline.ib[i] != native->ib[i]) {
               if (differing < 8)
                  fprintf(stderr,
                          "dword %u: recorded 0x%08x emitted 0x%08x\n", i,
                          native->ib[i], offline.ib[i]);
               differing++;
            }
         }
         fprintf(stderr, "%u differing dwords over %u common\n", differing,
                 common);
         r300_tcl_bypass_triangle_release(&offline);
      }
      fprintf(stderr, "the public recording is not the authorized stream; "
              "refusing ahead of the ioctl\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }

   if (record_only) {
      printf("record: ACCEPTED\n");
      fflush(stdout);
      vkDestroyCommandPool(device, pool, NULL);
      for (unsigned i = 0; i < 2; i++) {
         vkDestroyPipeline(device, pipeline[i], NULL);
         vkDestroyFramebuffer(device, target[i].framebuffer, NULL);
         vkDestroyImageView(device, target[i].view, NULL);
         vkDestroyImage(device, target[i].image, NULL);
         vkFreeMemory(device, target[i].memory, NULL);
      }
      vkDestroyPipelineLayout(device, layout, NULL);
      vkDestroyRenderPass(device, pass, NULL);
      vkDestroyBuffer(device, vertex_buffer, NULL);
      vkFreeMemory(device, vertex_memory, NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 0;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: the two load-op clears realize on the host, the two
    * carriers fill, and one live DRM_RADEON_CS reaches the command
    * processor here, with the bounded completion wait after it.
    */
   stage("submit");
   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   printf("[submit] vkQueueSubmit returned %d\n", result);
   fflush(stdout);

   if (r3v_native_watchdog_guard_close(&guard, result) != 0)
      return 1;

   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *first_map = NULL;
   void *second_map = NULL;
   CHECK(vkMapMemory(device, target[0].memory, 0, VK_WHOLE_SIZE, 0,
                     &first_map));
   CHECK(vkMapMemory(device, target[1].memory, 0, VK_WHOLE_SIZE, 0,
                     &second_map));
   if (r3v_native_evidence_write_file(evidence_dir, "first_target.bin",
                                      first_map, color_bytes) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "second_target.bin",
                                      second_map, color_bytes) != 0) {
      fprintf(stderr, "target retention failed\n");
      return 1;
   }

   struct r300_triangle_coverage_verdict first_verdict, second_verdict;
   struct r300_triangle_coverage_verdict first_crossed, second_crossed;
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[0], &first_dword, 1, R300_TRIANGLE_COLOR_SENTINEL, first_map,
      color_bytes, &first_verdict);
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[1], &second_dword, 1, R300_TRIANGLE_COLOR_SENTINEL,
      second_map, color_bytes, &second_verdict);
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[0], &second_dword, 1, R300_TRIANGLE_COLOR_SENTINEL, first_map,
      color_bytes, &first_crossed);
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[1], &first_dword, 1, R300_TRIANGLE_COLOR_SENTINEL, second_map,
      color_bytes, &second_crossed);
   report("first", &first_verdict);
   report("second", &second_verdict);
   report("first-under-second-constant", &first_crossed);
   report("second-under-first-constant", &second_crossed);

   const uint32_t *second_pixels = second_map;
   const uint32_t cx = mp.pass[1].width / 2;
   const uint32_t cy = (mp.pass[1].height * 3) / 8;
   printf("[oracle] second centroid (%u,%u)=0x%08x predicted 0x%08x "
          "corner (0,0)=0x%08x\n",
          cx, cy, second_pixels[cy * mp.pass[1].pitch_pixels + cx],
          second_dword, second_pixels[0]);
   fflush(stdout);

   const bool second_unreached =
      second_verdict.judged && second_verdict.analytic_pixels != 0 &&
      second_verdict.interior_pixels == 0 &&
      second_verdict.exterior_pixels ==
         mp.pass[1].width * mp.pass[1].height;
   const bool crossed = first_crossed.coverage_exact ||
                        second_crossed.coverage_exact;
   printf("[classify] %s\n",
          crossed ? "a target carries the other pass's constant; state "
                    "crossed the pass boundary"
          : second_unreached
             ? "the second target holds the sentinel; the command "
               "processor never reached the second cell"
             : "each target carries its own pass's constant over the "
               "analytic triangle");
   fflush(stdout);

   const bool pass_verdict =
      first_verdict.judged && first_verdict.coverage_exact &&
      first_verdict.canary_pass && second_verdict.judged &&
      second_verdict.coverage_exact && second_verdict.canary_pass && !crossed;

   stage("teardown");
   vkUnmapMemory(device, target[0].memory);
   vkUnmapMemory(device, target[1].memory);
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 2; i++) {
      vkDestroyPipeline(device, pipeline[i], NULL);
      vkDestroyFramebuffer(device, target[i].framebuffer, NULL);
      vkDestroyImageView(device, target[i].view, NULL);
      vkDestroyImage(device, target[i].image, NULL);
      vkFreeMemory(device, target[i].memory, NULL);
   }
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("[verdict] %s\n",
          pass_verdict
             ? "the public two-draw command buffer executed through one "
               "indirect buffer, each target holding its own pass's "
               "constant"
          : crossed ? "state crossed the pass boundary; the finding is the "
                      "concatenation's contract"
          : second_unreached
             ? "the second cell was not reached; the finding is the "
               "concatenation"
             : "prediction deviated; the deviation is the finding");
   return pass_verdict ? 0 : 1;
}
