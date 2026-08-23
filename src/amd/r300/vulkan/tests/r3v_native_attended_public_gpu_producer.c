/*
 * SPDX-License-Identifier: MIT
 *
 * Attended public GPU-producer cell: drives the application-shaped
 * Vulkan surface -- render pass, pipeline, vertex buffer, draw, submit
 * -- so RS482 silicon executes the composed route.  The producer pass
 * rasterizes the application's records into the carrier and the
 * consumer cell fetches that same buffer object as its vertex stream,
 * both inside one DRM_RADEON_CS, so two independent oracles decide the
 * run: the driver compares the carrier read-back against the CPU
 * gather, and this runner compares the color target against the
 * analytic triangle.  A carrier divergence quarantines the capability
 * inside the driver and surfaces here as device loss.  The producer,
 * triangle, and direct-write cells keep their own runners; no runner
 * records another's cell.  --fetched drives the fetched GPU-producer
 * cell over the same recording: the admission composes the fetched
 * producer under the third gate and the authorization names that
 * composition.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"

#include "amd/r300/common/r300_r2vb_fetched_producer.h"
#include "amd/r300/common/r300_r2vb_public_route.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The run's outcome classes.  Every class except TARGET_DELIVERED exits
 * nonzero, and the run never retries: one submission, one verdict.
 */
enum outcome {
   OUTCOME_TARGET_DELIVERED,
   OUTCOME_TARGET_MISMATCH,
   OUTCOME_CANARY_DISTURBED,
   OUTCOME_CARRIER_DIVERGED,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_TARGET_DELIVERED] = "TARGET_DELIVERED",
   [OUTCOME_TARGET_MISMATCH] = "TARGET_MISMATCH",
   [OUTCOME_CANARY_DISTURBED] = "CANARY_DISTURBED",
   [OUTCOME_CARRIER_DIVERGED] = "CARRIER_DIVERGED",
   [OUTCOME_SUBMISSION_REFUSED] = "SUBMISSION_REFUSED",
   [OUTCOME_COMPLETION_FAILURE] = "COMPLETION_FAILURE",
   [OUTCOME_RETENTION_FAILURE] = "RETENTION_FAILURE",
};

static int
finish(enum outcome outcome)
{
   printf("verdict: %s\n", outcome_names[outcome]);
   fflush(stdout);
   return outcome == OUTCOME_TARGET_DELIVERED ? 0 : 1;
}

/* Names the stage about to run.  A hang leaves its stage as the last
 * line on the console and in the off-box log.
 */
static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

/* One directory reached by two spellings is still one directory, so the
 * comparison resolves both paths when they exist.
 */
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

/* The exact-value delivery gates.  Both open the GPU producer route;
 * either one closed selects the CPU route, which submits the consumer
 * alone under an authorization naming the composed stream.
 */
static bool
gate_open(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0;
}

int
main(int argc, char **argv)
{
   /* --record-only builds every object and records the command buffer,
    * then stops at the recording boundary.  The recording contract the
    * driver enforces -- pass shape, sentinel clear, pipeline extent,
    * draw arguments, bound stream range -- is what this program can get
    * wrong without any device, so the shim fixture calibrates it there
    * and the hardware mode inherits a proven sequence.
    */
   /* --fetched selects the fetched GPU-producer cell: the same recording
    * and the same records, with the submit-time admission composing the
    * fetched producer (slot BO + the bound vertex BO through the
    * two-array fetched body) under the third gate, and the authorization
    * naming that composition's digest.  --fetched=f32_3 and
    * --fetched=f32_2 bind the same three positions as narrower records
    * -- the stride and the attribute format are the width's record size,
    * the record holds the leading components, and the fetch swizzle
    * fills z and w -- so each width is its own cell with its own stream
    * digest over the same target oracle.
    */
   bool record_only = false;
   bool fetched = false;
   int source_format = R300_VERTEX_FORMAT_F32_4;
   bool usage_error = argc < 2 || argc > 4;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--record-only") == 0 && !record_only)
         record_only = true;
      else if (strcmp(argv[i], "--fetched") == 0 && !fetched)
         fetched = true;
      else if (strcmp(argv[i], "--fetched=f32_4") == 0 && !fetched)
         fetched = true;
      else if (strcmp(argv[i], "--fetched=f32_3") == 0 && !fetched) {
         fetched = true;
         source_format = R300_VERTEX_FORMAT_F32_3;
      } else if (strcmp(argv[i], "--fetched=f32_2") == 0 && !fetched) {
         fetched = true;
         source_format = R300_VERTEX_FORMAT_F32_2;
      } else
         usage_error = true;
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <evidence-directory> [--record-only] "
              "[--fetched[=f32_4|f32_3|f32_2]]\n",
              argv[0]);
      return 2;
   }
   const struct r300_vertex_format_semantics *source_semantics =
      r300_vertex_format_semantics((enum r300_vertex_format_id)source_format);
   const uint32_t source_record_bytes = source_semantics->semantic_record_bytes;
   const VkFormat source_vk_format =
      source_format == R300_VERTEX_FORMAT_F32_4   ? VK_FORMAT_R32G32B32A32_SFLOAT
      : source_format == R300_VERTEX_FORMAT_F32_3 ? VK_FORMAT_R32G32B32_SFLOAT
                                                  : VK_FORMAT_R32G32_SFLOAT;
   const char *source_format_name =
      source_format == R300_VERTEX_FORMAT_F32_4   ? "F32_4"
      : source_format == R300_VERTEX_FORMAT_F32_3 ? "F32_3"
                                                  : "F32_2";
   const char *evidence_dir = argv[1];

   /* A silicon result binds to the real libc entry points.  A preloaded
    * interposer -- the drm-shim fixture or any other -- would let the
    * run report a silicon verdict it never earned, so any LD_PRELOAD
    * refuses before the first Vulkan call.  The recording mode reaches
    * no ioctl and reports no verdict, so it runs on the fixture.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The driver reads the evidence directory from the environment, and
    * the arming conjunction consumes the one-shot token in whichever
    * directory the environment names.  The armed directory and the
    * readback directory are one directory, so a disagreement refuses.
    */
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (!record_only &&
       (declared == NULL || declared[0] == '\0' ||
        !same_directory(declared, evidence_dir))) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s; "
              "the armed directory and the readback directory are one "
              "directory\n",
              declared != NULL ? declared : "(unset)", evidence_dir);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The authorization names the composed stream, so a run whose gates
    * would select the CPU route refuses before the ioctl rather than
    * spending the one-shot token on the consumer alone.
    */
   if (!record_only &&
       (!gate_open("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") ||
        !gate_open("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL"))) {
      fprintf(stderr,
              "both R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL and "
              "R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL carry the exact "
              "value 1 on this route\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   /* The third gate selects the fetched composition; a run declared for
    * one producer form with the gate set for the other would submit a
    * stream its authorization does not name, so it refuses by name.
    */
   if (!record_only &&
       gate_open("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL") != fetched) {
      fprintf(stderr,
              "R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL %s on the %s "
              "cell\n",
              fetched ? "carries the exact value 1" : "stays unset",
              fetched ? "fetched" : "immediate");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The stream this run submits, composed here so its digest reaches
    * the console beside the authorization the operator declared.
    */
   char route_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   uint32_t route_dwords = 0;
   uint32_t route_split = 0;
   if (fetched) {
      struct r300_r2vb_fetched_route_ib route;
      if (r300_r2vb_fetched_route_reference_compose(source_format,
                                                    &route) != 0) {
         fprintf(stderr, "fetched route composition failed\n");
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
      r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords,
                                  route_digest);
      route_dwords = route.ib_size_dwords;
      route_split = route.consumer_start_dwords;
      r300_r2vb_fetched_route_release(&route);
   } else {
      struct r300_r2vb_public_route_ib route;
      if (r300_r2vb_public_route_reference_compose(&route) != 0 ||
          r300_r2vb_public_route_validate_reloc_sites(&route) != 0) {
         fprintf(stderr, "route composition failed\n");
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
      r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords,
                                  route_digest);
      route_dwords = route.ib_size_dwords;
      route_split = route.consumer_start_dwords;
      r300_r2vb_public_route_release(&route);
   }
   printf("route %s source_format=%s ib_dwords=%u consumer_start_dwords=%u "
          "ib_blake3=%s\n",
          fetched ? "fetched" : "immediate", source_format_name,
          route_dwords, route_split, route_digest);
   fflush(stdout);

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   if (create_instance(&(VkInstanceCreateInfo){
                          .sType =
                             VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                       },
                       NULL, &instance) != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
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
   VkResult result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       pdev_count != 1 || pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   /* The arming gate enforces this too; refusing here keeps the run off
    * a chip whose falsifiers were not written.
    */
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS482\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("device");
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   if (vkCreateDevice(
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
          NULL, &device) != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkCreateBuffer);
   LOAD_DEVICE(vkBindBufferMemory);
   LOAD_DEVICE(vkCreateImage);
   LOAD_DEVICE(vkGetImageMemoryRequirements);
   LOAD_DEVICE(vkBindImageMemory);
   LOAD_DEVICE(vkCreateImageView);
   LOAD_DEVICE(vkCreateRenderPass);
   LOAD_DEVICE(vkCreateFramebuffer);
   LOAD_DEVICE(vkCreateShaderModule);
   LOAD_DEVICE(vkDestroyShaderModule);
   LOAD_DEVICE(vkCreatePipelineLayout);
   LOAD_DEVICE(vkCreateGraphicsPipelines);
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

   /* The color target: the consumer cell's 64x64 B8G8R8A8 surface with
    * the canary row the oracle reads past the render extent.
    */
   stage("target");
   VkImage image = VK_NULL_HANDLE;
   if (vkCreateImage(
          device,
          &(VkImageCreateInfo){
             .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
             .imageType = VK_IMAGE_TYPE_2D,
             .format = R3V_NATIVE_TARGET_FORMAT,
             .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT,
                         1 },
             .mipLevels = 1,
             .arrayLayers = 1,
             .samples = VK_SAMPLE_COUNT_1_BIT,
             .tiling = VK_IMAGE_TILING_LINEAR,
             .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
          },
          NULL, &image) != VK_SUCCESS) {
      fprintf(stderr, "color image creation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(device, image, &reqs);
   if (reqs.size != R3V_NATIVE_TARGET_MEMORY_BYTES) {
      fprintf(stderr, "color requirement %llu is not the cell footprint\n",
              (unsigned long long)reqs.size);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   if (vkAllocateMemory(device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = reqs.size,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &color_memory) != VK_SUCCESS ||
       vkBindImageMemory(device, image, color_memory, 0) != VK_SUCCESS) {
      fprintf(stderr, "color allocation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The application's vertex records: the fixed triangle's own
    * pretransformed positions, the payload the qualified producer pass
    * embeds and the consumer cell fetches.  Writing them through the
    * public map is what makes this run an application draw rather than
    * a recorder's fixed stream.
    */
   stage("vertex stream");
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   if (vkAllocateMemory(device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = 4096,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &vertex_memory) != VK_SUCCESS) {
      fprintf(stderr, "vertex allocation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   if (vkCreateBuffer(device,
                      &(VkBufferCreateInfo){
                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                         .size = 3 * source_record_bytes,
                         .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                      },
                      NULL, &vertex_buffer) != VK_SUCCESS ||
       vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0) !=
          VK_SUCCESS) {
      fprintf(stderr, "vertex buffer binding failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   void *map = NULL;
   if (vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) !=
          VK_SUCCESS ||
       map == NULL) {
      fprintf(stderr, "vertex map failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   /* Each record carries the leading components of its F32_4 position;
    * the fetch swizzle restores z = 0 and w = 1 on the narrower widths,
    * which is what the reference triangle carries there.
    */
   for (unsigned v = 0; v < 3; v++)
      memcpy((uint8_t *)map + v * source_record_bytes,
             &r300_tcl_bypass_triangle_vertices[v * 4], source_record_bytes);
   vkUnmapMemory(device, vertex_memory);

   stage("pipeline");
   VkImageView view = VK_NULL_HANDLE;
   VkRenderPass pass = VK_NULL_HANDLE;
   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   if (vkCreateImageView(
          device,
          &(VkImageViewCreateInfo){
             .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
             .image = image,
             .viewType = VK_IMAGE_VIEW_TYPE_2D,
             .format = R3V_NATIVE_TARGET_FORMAT,
             .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                   .levelCount = 1,
                                   .layerCount = 1 },
          },
          NULL, &view) != VK_SUCCESS ||
       vkCreateRenderPass(
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
          NULL, &pass) != VK_SUCCESS ||
       vkCreateFramebuffer(
          device,
          &(VkFramebufferCreateInfo){
             .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
             .renderPass = pass,
             .attachmentCount = 1,
             .pAttachments = &view,
             .width = R3V_NATIVE_TARGET_WIDTH,
             .height = R3V_NATIVE_TARGET_HEIGHT,
             .layers = 1,
          },
          NULL, &framebuffer) != VK_SUCCESS ||
       vkCreatePipelineLayout(
          device,
          &(VkPipelineLayoutCreateInfo){
             .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
          },
          NULL, &layout) != VK_SUCCESS) {
      fprintf(stderr, "render-pass construction failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   VkShaderModule vs = VK_NULL_HANDLE;
   VkShaderModule fs = VK_NULL_HANDLE;
   if (vkCreateShaderModule(
          device,
          &(VkShaderModuleCreateInfo){
             .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
             .codeSize = sizeof(r3v_reference_vertex_spirv),
             .pCode = r3v_reference_vertex_spirv,
          },
          NULL, &vs) != VK_SUCCESS ||
       vkCreateShaderModule(
          device,
          &(VkShaderModuleCreateInfo){
             .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
             .codeSize = sizeof(r3v_reference_fragment_spirv),
             .pCode = r3v_reference_fragment_spirv,
          },
          NULL, &fs) != VK_SUCCESS) {
      fprintf(stderr, "shader module creation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   VkPipeline pipeline = VK_NULL_HANDLE;
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
                  .stride = source_record_bytes,
                  .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
               },
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions =
               &(VkVertexInputAttributeDescription){
                  .location = 0,
                  .binding = 0,
                  .format = source_vk_format,
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
   if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                 NULL, &pipeline) != VK_SUCCESS) {
      fprintf(stderr, "pipeline creation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);

   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkCreateCommandPool(
          device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0,
          },
          NULL, &pool) != VK_SUCCESS ||
       vkAllocateCommandBuffers(
          device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1,
          },
          &cmd) != VK_SUCCESS ||
       vkBeginCommandBuffer(
          cmd, &(VkCommandBufferBeginInfo){
                  .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
               }) != VK_SUCCESS) {
      fprintf(stderr, "command recording boundary failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The cell realizes one load-op clear, the 0xa5a5a5a5 sentinel the
    * target oracle reads as its exterior and canary value, so the
    * recording admits that color alone (r3v_native_draw.c
    * clear_is_sentinel).  Both sides evaluate the same expression, so
    * the comparison lands on identical bits.
    */
   const float sentinel = (float)0xa5 / 255.0f;
   const VkClearValue clear = { .color = { .float32 = { sentinel, sentinel,
                                                        sentinel,
                                                        sentinel } } };
   vkCmdBeginRenderPass(cmd,
                        &(VkRenderPassBeginInfo){
                           .sType =
                              VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                           .renderPass = pass,
                           .framebuffer = framebuffer,
                           .renderArea = { .extent =
                                              { R3V_NATIVE_TARGET_WIDTH,
                                                R3V_NATIVE_TARGET_HEIGHT } },
                           .clearValueCount = 1,
                           .pClearValues = &clear,
                        },
                        VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   /* Recording latches the first refusal and vkEndCommandBuffer returns
    * it, so the code names which contract the sequence missed.
    */
   VkResult end_result = vkEndCommandBuffer(cmd);
   if (end_result != VK_SUCCESS) {
      fprintf(stderr, "vkEndCommandBuffer: %d\n", end_result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   if (record_only) {
      printf("record: ACCEPTED\n");
      fflush(stdout);
      vkDestroyCommandPool(device, pool, NULL);
      vkFreeMemory(device, vertex_memory, NULL);
      vkFreeMemory(device, color_memory, NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 0;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: the submit-time admission composes the route, poisons
    * the carrier, and a live DRM_RADEON_CS reaches the command
    * processor here.  The carrier read-back and its verdict run inside
    * the queue after the bounded completion wait.  The submission is
    * one-shot; whatever it returns, no resubmission follows.
    */
   stage("submit");
   VkResult submit_result =
      vkQueueSubmit(queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd,
                    },
                    VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   printf("[submit] vkQueueSubmit returned %d status=%s\n", submit_result,
          r3v_native_queue_status_name(queue_status));
   fflush(stdout);

   /* Readback and retention run for every submit result: a refused or
    * incomplete submission still leaves the target's state as evidence.
    * The driver has already retained the carrier read-back beside its
    * expectation in this same directory.
    */
   stage("readback");
   uint32_t *color_map = NULL;
   if (vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                   (void **)&color_map) != VK_SUCCESS ||
       color_map == NULL) {
      fprintf(stderr, "color readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "color.bin", color_map,
                                      R3V_NATIVE_TARGET_MEMORY_BYTES) != 0) {
      fprintf(stderr, "color retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("oracle");
   struct r300_triangle_oracle_verdict verdict;
   r300_tcl_bypass_triangle_extent_oracle(
      R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, color_map,
      R3V_NATIVE_TARGET_MEMORY_BYTES, &verdict);
   printf("[oracle] executed=%d interior_pass=%d exterior_pass=%d "
          "canary_pass=%d interior_samples=%u exterior_samples=%u\n",
          verdict.executed, verdict.interior_pass, verdict.exterior_pass,
          verdict.canary_pass, verdict.interior_samples,
          verdict.exterior_samples);
   fflush(stdout);

   /* Classification order: a disturbed canary stops the sequence
    * whatever else passed; then the transport's own failures, with the
    * carrier divergence separated from every other refusal because it
    * is the route's own falsifier; then the target verdict.
    */
   enum outcome outcome;
   if (!verdict.canary_pass)
      outcome = OUTCOME_CANARY_DISTURBED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (submit_result == VK_ERROR_DEVICE_LOST)
      outcome = OUTCOME_CARRIER_DIVERGED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            verdict.executed && verdict.interior_pass &&
            verdict.exterior_pass)
      outcome = OUTCOME_TARGET_DELIVERED;
   else
      outcome = OUTCOME_TARGET_MISMATCH;

   char outcome_json[1024];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"%s\",\n"
      "  \"route\": \"%s\",\n"
      "  \"source_format\": \"%s\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"color_size_bytes\": %u,\n"
      "  \"executed\": %s,\n"
      "  \"interior_pass\": %s,\n"
      "  \"exterior_pass\": %s,\n"
      "  \"canary_pass\": %s,\n"
      "  \"interior_samples\": %u,\n"
      "  \"exterior_samples\": %u\n"
      "}\n",
      fetched ? "r3v-native-r2vb-fetched-route-outcome/1"
              : "r3v-native-r2vb-public-route-outcome/1",
      fetched ? "fetched" : "immediate", source_format_name,
      outcome_names[outcome],
      submit_result, r3v_native_queue_status_name(queue_status),
      route_digest,
      (unsigned)R3V_NATIVE_TARGET_MEMORY_BYTES,
      verdict.executed ? "true" : "false",
      verdict.interior_pass ? "true" : "false",
      verdict.exterior_pass ? "true" : "false",
      verdict.canary_pass ? "true" : "false", verdict.interior_samples,
      verdict.exterior_samples);
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      fetched ? "fetched_route_outcome.json"
                                              : "public_route_outcome.json",
                                      outcome_json, (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   return finish(outcome);
}
