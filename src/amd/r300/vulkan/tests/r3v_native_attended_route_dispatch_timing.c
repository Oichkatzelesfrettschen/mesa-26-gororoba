/*
 * SPDX-License-Identifier: MIT
 *
 * Attended route dispatch timing cell: drives the application-shaped
 * Vulkan surface -- render pass, pipeline, vertex buffer, draw, submit
 * -- through one declared delivery route and retains two intervals per
 * run.  The deciding one is the driver's own transport bracket,
 * DRM_RADEON_CS plus the bounded completion wait; the vkQueueSubmit
 * wall around it also spans the semantic-cell and submit-object
 * evidence writes, the IB digest, the completion BO allocation, and
 * the arming facts read from sysfs and uname, so the wall bounds this
 * run's evidence I/O rather than its delivery.
 *
 * A run cannot time one route under the other's name.  The gate state
 * this process sets is what the device caches, so reading it back
 * proves nothing; instead the driver reports the route its resolver
 * and submit-time admission actually took, and a completed submission
 * whose observed route differs from the declared one reports the
 * disagreement rather than a timing row.  --route gpu takes both
 * delivery gates at their exact opt-in values and submits the composed
 * producer-plus-consumer stream; --route cpu takes both closed and
 * submits the consumer alone over the CPU-executed vertex job.  The
 * color oracle decides that the timed submission delivered, so every
 * timing row rests on verified bytes.  One submission, one verdict, one
 * timing row per process; repetition lives with the orchestrator, which
 * arms each repetition in a fresh evidence directory.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"

#include "amd/r300/common/r300_r2vb_public_route.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The run's outcome classes.  Every class except TARGET_DELIVERED exits
 * nonzero, and the run never retries: one submission, one verdict.
 */
enum outcome {
   OUTCOME_TARGET_DELIVERED,
   OUTCOME_ROUTE_DISAGREEMENT,
   OUTCOME_TARGET_MISMATCH,
   OUTCOME_CANARY_DISTURBED,
   OUTCOME_CARRIER_DIVERGED,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_TARGET_DELIVERED] = "TARGET_DELIVERED",
   [OUTCOME_ROUTE_DISAGREEMENT] = "ROUTE_DISAGREEMENT",
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

/* One raw monotonic reading in nanoseconds; the raw clock stands
 * outside NTP slew, so two readings subtract to a wall interval.  A
 * clock this thread cannot read yields 0, which reads downstream as an
 * unmeasured interval rather than as an unsigned subtraction that
 * wrapped.
 */
static uint64_t
now_raw_ns(void)
{
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
      return 0;
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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
   const bool digest_only = argc == 2 && strcmp(argv[1], "--digest-only") == 0;
   const bool record_only =
      argc == 4 && strcmp(argv[3], "--record-only") == 0;
   const bool timed_run = argc == 3;
   if (!digest_only && !record_only && !timed_run) {
      fprintf(stderr,
              "usage: %s <evidence-directory> cpu|gpu [--record-only]\n"
              "       %s --digest-only\n",
              argv[0], argv[0]);
      return 2;
   }
   const char *evidence_dir = digest_only ? NULL : argv[1];
   bool gpu_route = false;
   if (!digest_only) {
      /* The route rides as its own word so the declared route is
       * visible in the process line an evidence bundle retains.
       */
      const char *route_arg = argv[2];
      if (strncmp(route_arg, "--route=", 8) == 0)
         route_arg += 8;
      if (strcmp(route_arg, "gpu") == 0)
         gpu_route = true;
      else if (strcmp(route_arg, "cpu") != 0) {
         fprintf(stderr, "unknown route %s; the routes are cpu and gpu\n",
                 route_arg);
         return 2;
      }
   }

   /* A silicon result binds to the real libc entry points.  A preloaded
    * interposer -- the drm-shim fixture or any other -- would let the
    * run report a silicon verdict it never earned, so any LD_PRELOAD
    * refuses before the first Vulkan call.  The recording mode reaches
    * no ioctl and reports no verdict, so it runs on the fixture.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && !digest_only && preload != NULL &&
       preload[0] != '\0') {
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
   if (!record_only && !digest_only &&
       (declared == NULL || declared[0] == '\0' ||
        !same_directory(declared, evidence_dir))) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s; "
              "the armed directory and the readback directory are one "
              "directory\n",
              declared != NULL ? declared : "(unset)", evidence_dir);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The declared route must be the route the device resolves: the gpu
    * route takes both delivery gates at their exact values, and the cpu
    * route takes both closed, so a gate state naming the other route
    * refuses before any token is spent on a mislabeled timing row.
    */
   if (!record_only && !digest_only) {
      const bool gates_open =
         gate_open("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") &&
         gate_open("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
      const bool gates_closed =
         !gate_open("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") &&
         !gate_open("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
      if (gpu_route && !gates_open) {
         fprintf(stderr,
                 "--route gpu requires both "
                 "R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL and "
                 "R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL at 1\n");
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
      if (!gpu_route && !gates_closed) {
         fprintf(stderr,
                 "--route cpu requires both delivery gates closed so the "
                 "resolver takes the CPU route\n");
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
   }

   /* The stream this run submits, composed here so its digest reaches
    * the console beside the authorization the operator declared.
    */
   struct r300_r2vb_public_route_ib route;
   if (r300_r2vb_public_route_reference_compose(&route) != 0 ||
       r300_r2vb_public_route_validate_reloc_sites(&route) != 0) {
      fprintf(stderr, "route composition failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   char gpu_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, gpu_digest);
   /* The consumer slice of the composed stream is the recorded consumer
    * cell verbatim, so its digest is what the arming gate reads on the
    * CPU route, where the consumer submits alone; the shim calibration
    * leg proves that equality before any hardware declaration rests on
    * it.
    */
   char cpu_digest[2 * R300_TRIANGLE_DIGEST_SIZE + 1];
   r300_triangle_ib_digest_hex(route.ib + route.consumer_start_dwords,
                               route.ib_size_dwords -
                                  route.consumer_start_dwords,
                               cpu_digest);
   printf("route ib_dwords=%u consumer_start_dwords=%u\n",
          route.ib_size_dwords, route.consumer_start_dwords);
   printf("gpu_ib_blake3=%s\ncpu_ib_blake3=%s\n", gpu_digest, cpu_digest);
   fflush(stdout);
   const char *route_digest = digest_only    ? NULL
                              : gpu_route    ? gpu_digest
                                             : cpu_digest;
   r300_r2vb_public_route_release(&route);
   if (digest_only)
      return 0;

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
                         .size = sizeof(r300_tcl_bypass_triangle_vertices),
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
   memcpy(map, r300_tcl_bypass_triangle_vertices,
          sizeof(r300_tcl_bypass_triangle_vertices));
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
   const uint64_t t_record_done_ns = now_raw_ns();

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
   const uint64_t t_submit_enter_ns = now_raw_ns();
   VkResult submit_result =
      vkQueueSubmit(queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd,
                    },
                    VK_NULL_HANDLE);
   const uint64_t t_submit_return_ns = now_raw_ns();
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   /* The route that ran, as the resolver and the submit-time admission
    * decided it.  The delivery gates this process set are what the
    * driver cached, so reading them back here would prove nothing; the
    * resolver additionally requires the F32_4 stream format, and a
    * declared gpu route over any other format resolves to a host
    * delivery.  Comparing the observed route against the declared one is
    * what makes a mislabeled timing row impossible.
    */
   const bool observed_gpu = r3v_native_queue_observed_gpu_producer(device);
   /* The transport bracket: DRM_RADEON_CS plus the bounded completion
    * wait.  The vkQueueSubmit wall around it also spans the semantic-cell
    * and submit-object evidence writes, the IB digest, the completion BO
    * allocation, and the arming facts read from sysfs and uname, so the
    * wall bounds this run's evidence I/O rather than its delivery.  Both
    * readings retain; the transport interval is the deciding one.
    */
   const unsigned long long transport_wall_ns =
      (unsigned long long)r3v_native_queue_transport_wall_ns(device);
   printf("[submit] vkQueueSubmit returned %d status=%s\n", submit_result,
          r3v_native_queue_status_name(queue_status));
   const unsigned long long submit_wall_ns =
      t_submit_return_ns >= t_submit_enter_ns && t_submit_enter_ns != 0
         ? (unsigned long long)(t_submit_return_ns - t_submit_enter_ns)
         : 0ull;
   printf("[timing] route=%s observed=%s transport_wall_ns=%llu "
          "submit_wall_ns=%llu\n",
          gpu_route ? "gpu" : "cpu", observed_gpu ? "gpu" : "cpu",
          transport_wall_ns, submit_wall_ns);
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
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            observed_gpu != gpu_route)
      /* A completed submission whose delivery route is not the declared
       * one carries a timing row for the other route; it reports the
       * disagreement rather than the row.
       */
      outcome = OUTCOME_ROUTE_DISAGREEMENT;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (submit_result == VK_ERROR_DEVICE_LOST)
      /* Carrier divergence is the GPU route's own falsifier; the CPU
       * route writes the carrier itself, so its device loss is a
       * transport failure.
       */
      outcome = gpu_route ? OUTCOME_CARRIER_DIVERGED
                          : OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            verdict.executed && verdict.interior_pass &&
            verdict.exterior_pass)
      outcome = OUTCOME_TARGET_DELIVERED;
   else
      outcome = OUTCOME_TARGET_MISMATCH;

   /* The 64-bit timestamps ride as decimal strings, the JSON convention
    * every reducer in the evidence lane already parses.
    */
   char outcome_json[1536];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-route-dispatch-timing-outcome/2\",\n"
      "  \"route\": \"%s\",\n"
      "  \"observed_route\": \"%s\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"t_record_done_ns\": \"%llu\",\n"
      "  \"t_submit_enter_ns\": \"%llu\",\n"
      "  \"t_submit_return_ns\": \"%llu\",\n"
      "  \"submit_wall_ns\": \"%llu\",\n"
      "  \"transport_wall_ns\": \"%llu\",\n"
      "  \"color_size_bytes\": %u,\n"
      "  \"executed\": %s,\n"
      "  \"interior_pass\": %s,\n"
      "  \"exterior_pass\": %s,\n"
      "  \"canary_pass\": %s,\n"
      "  \"interior_samples\": %u,\n"
      "  \"exterior_samples\": %u\n"
      "}\n",
      gpu_route ? "gpu" : "cpu", observed_gpu ? "gpu" : "cpu",
      outcome_names[outcome], submit_result,
      r3v_native_queue_status_name(queue_status), route_digest,
      (unsigned long long)t_record_done_ns,
      (unsigned long long)t_submit_enter_ns,
      (unsigned long long)t_submit_return_ns,
      submit_wall_ns, transport_wall_ns,
      (unsigned)R3V_NATIVE_TARGET_MEMORY_BYTES,
      verdict.executed ? "true" : "false",
      verdict.interior_pass ? "true" : "false",
      verdict.exterior_pass ? "true" : "false",
      verdict.canary_pass ? "true" : "false", verdict.interior_samples,
      verdict.exterior_samples);
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "route_dispatch_timing_outcome.json",
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
