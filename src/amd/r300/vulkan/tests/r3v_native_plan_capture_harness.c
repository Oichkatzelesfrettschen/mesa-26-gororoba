/*
 * SPDX-License-Identifier: MIT
 *
 * Plan capture under the drm-shim: two distinct draws submit through a
 * capture device, the transcript carries both entries in order with
 * the retained digests, a composed plan binds, and the capture shape
 * refuses an open hazard gate.
 */

#undef NDEBUG
#define VK_NO_PROTOTYPES

#include "r3v_native.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_shim_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/tests/r300_retained_route_digests.h"
#include "vk_instance.h"
#include "vk_physical_device.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The recorded pass admits the sentinel clear alone: a target seeded
 * with any other value proves the load-op clear wrote it.
 */
#define CLEAR_SENTINEL ((float)0xa5 / 255.0f)

static const float ndc_triangle[24] = {
   -0.75f, -0.75f, 0.0f, 1.0f, 0.75f, -0.75f, 0.0f, 1.0f,
   0.0f,   0.75f,  0.0f, 1.0f, -0.6875f, -0.75f, 0.0f, 1.0f,
   0.8125f, -0.75f, 0.0f, 1.0f, 0.0625f, 0.75f, 0.0f, 1.0f,
};

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
   f(vkCmdPipelineBarrier) f(vkGetDeviceQueue) f(vkQueueSubmit)\
   f(vkQueueWaitIdle)                 \
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

static char *
read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   assert(f != NULL);
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)n + 1);
   assert(buf != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n);
   fclose(f);
   buf[n] = '\0';
   *size = (size_t)n;
   return buf;
}

/* Creates the instance and device; returns the device-creation result so
 * the refusal arm can assert on it.
 */
static VkResult
create(VkInstance *instance_out, VkDevice *device_out,
       PFN_vkDestroyInstance *destroy_instance_out)
{
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
   *destroy_instance_out =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated = enumerate(instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1);
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   VkResult result = create_device(
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
   *instance_out = instance;
   *device_out = device;
   if (result != VK_SUCCESS)
      return result;

   /* The shim device reports no subsystem or DMI identity, so it resolves
    * to no board.  A harness that replaces the fact provider declares the
    * platform it stands in for, which is what keeps the capture path's
    * identity gate exercised here rather than bypassed.
    */
   r3v_native_install_shim_arming(r3v_native_device_from_handle(device));
#define LOAD(name) name = (PFN_##name)gdpa(device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD
   return VK_SUCCESS;
}

/* The image, buffer, pass, and pipeline one draw submits against; built
 * once and reused across the draws a capture arm records, so the arm
 * exercises the plan-capture path (not pipeline construction) between
 * submissions.
 */
struct triangle_resources {
   VkImage image;
   VkDeviceMemory image_memory;
   VkImageView view;
   VkDeviceMemory vertex_memory;
   VkBuffer vertex_buffer;
   VkRenderPass pass;
   VkFramebuffer framebuffer;
   VkPipelineLayout layout;
   VkShaderModule vs, fs;
   VkPipeline pipeline;
   VkCommandPool pool;
   VkMemoryRequirements image_reqs;
};

static struct triangle_resources
create_triangle_resources(VkDevice device)
{
   struct triangle_resources r = { 0 };
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
             NULL, &r.image) == VK_SUCCESS);
   vkGetImageMemoryRequirements(device, r.image, &r.image_reqs);
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = r.image_reqs.size,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &r.image_memory) == VK_SUCCESS);
   assert(vkBindImageMemory(device, r.image, r.image_memory, 0) == VK_SUCCESS);
   assert(vkCreateImageView(
             device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = r.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = R3V_NATIVE_TARGET_FORMAT,
                .subresourceRange = { .aspectMask =
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                      .levelCount = 1,
                                      .layerCount = 1 },
             },
             NULL, &r.view) == VK_SUCCESS);
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &r.vertex_memory) == VK_SUCCESS);
   assert(vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = 256,
                            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &r.vertex_buffer) == VK_SUCCESS);
   assert(vkBindBufferMemory(device, r.vertex_buffer, r.vertex_memory, 0) ==
          VK_SUCCESS);
   void *map = NULL;
   assert(vkMapMemory(device, r.vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
          VK_SUCCESS);
   memcpy(map, ndc_triangle, sizeof(ndc_triangle));
   vkUnmapMemory(device, r.vertex_memory);

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
             NULL, &r.pass) == VK_SUCCESS);
   assert(vkCreateFramebuffer(
             device,
             &(VkFramebufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = r.pass,
                .attachmentCount = 1,
                .pAttachments = &r.view,
                .width = R3V_NATIVE_TARGET_WIDTH,
                .height = R3V_NATIVE_TARGET_HEIGHT,
                .layers = 1,
             },
             NULL, &r.framebuffer) == VK_SUCCESS);
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             },
             NULL, &r.layout) == VK_SUCCESS);
   r.vs = make_module(device, r3v_reference_vertex_spirv,
                      sizeof(r3v_reference_vertex_spirv));
   r.fs = make_module(device, r3v_reference_fragment_spirv,
                      sizeof(r3v_reference_fragment_spirv));
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
                        .module = r.vs,
                        .pName = "main" },
                      { .sType =
                           VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                        .module = r.fs,
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
                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
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
                .layout = r.layout,
                .renderPass = r.pass,
             },
             NULL, &r.pipeline) == VK_SUCCESS);
   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0,
             },
             NULL, &r.pool) == VK_SUCCESS);
   return r;
}

static void
destroy_triangle_resources(VkDevice device, struct triangle_resources *r)
{
   vkDestroyCommandPool(device, r->pool, NULL);
   vkDestroyPipeline(device, r->pipeline, NULL);
   vkDestroyShaderModule(device, r->vs, NULL);
   vkDestroyShaderModule(device, r->fs, NULL);
   vkDestroyPipelineLayout(device, r->layout, NULL);
   vkDestroyFramebuffer(device, r->framebuffer, NULL);
   vkDestroyRenderPass(device, r->pass, NULL);
   vkDestroyBuffer(device, r->vertex_buffer, NULL);
   vkFreeMemory(device, r->vertex_memory, NULL);
   vkDestroyImageView(device, r->view, NULL);
   vkDestroyImage(device, r->image, NULL);
   vkFreeMemory(device, r->image_memory, NULL);
}

/* Records one draw of vertex_count vertices into an already-allocated,
 * already-begun cmd, leaving it ready for vkEndCommandBuffer.
 */
static void
record_triangle_draw(VkCommandBuffer cmd, struct triangle_resources *r,
                     uint32_t vertex_count)
{
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
         .image = r->image,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .layerCount = 1 },
      });
   vkCmdBeginRenderPass(
      cmd,
      &(VkRenderPassBeginInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = r->pass,
         .framebuffer = r->framebuffer,
         .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                     R3V_NATIVE_TARGET_HEIGHT } },
         .clearValueCount = 1,
         .pClearValues = &(VkClearValue){
            .color = { .float32 = { CLEAR_SENTINEL, CLEAR_SENTINEL,
                                    CLEAR_SENTINEL, CLEAR_SENTINEL } } },
      },
      VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
   vkCmdBindVertexBuffers(cmd, 0, 1, &r->vertex_buffer, &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, vertex_count, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
}

/* Records one complete command buffer against r.  The command buffer
 * allocates from r's pool and remains live until the pool is destroyed,
 * matching the harness's single-shot arms and letting refusal arms inspect
 * or submit the exact recorded object.
 */
static VkCommandBuffer
record_triangle_draw_command(VkDevice device, struct triangle_resources *r,
                             uint32_t vertex_count)
{
   VkCommandBuffer cmd;
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = r->pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
             },
             &cmd) == VK_SUCCESS);
   assert(vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
                                       .sType =
                                          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    }) == VK_SUCCESS);
   record_triangle_draw(cmd, r, vertex_count);
   assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);
   return cmd;
}

static VkResult
submit_triangle_draw_result(VkDevice device, VkQueue queue,
                            struct triangle_resources *r,
                            uint32_t vertex_count)
{
   VkCommandBuffer cmd =
      record_triangle_draw_command(device, r, vertex_count);
   return vkQueueSubmit(queue, 1,
                        &(VkSubmitInfo){
                           .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd,
                        },
                        VK_NULL_HANDLE);
}

static void
submit_triangle_draw(VkDevice device, VkQueue queue,
                     struct triangle_resources *r, uint32_t vertex_count)
{
   assert(submit_triangle_draw_result(device, queue, r, vertex_count) ==
          VK_SUCCESS);
   assert(vkQueueWaitIdle(queue) == VK_SUCCESS);
}

int
main(int argc, char **argv)
{
   const char *arm = argc > 1 ? argv[1] : "capture";
   char dir[] = "/tmp/r3v-native-plan-capture-XXXXXX";
   assert(mkdtemp(dir) != NULL);
   char transcript[4096];
   snprintf(transcript, sizeof(transcript), "%s/transcript.plan", dir);
   char diagnostic[4096];
   snprintf(diagnostic, sizeof(diagnostic), "%s/diagnostic.log", dir);
   if (strcmp(arm, "relocation-count-diagnostic") == 0)
      setenv("MESA_LOG_FILE", diagnostic, 1);
   setenv("R3V_NATIVE_PLAN_CAPTURE_FILE", transcript, 1);
   unsetenv("R3V_NATIVE_MANIFEST_DIR");
   unsetenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
   unsetenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
   unsetenv("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL");

   VkInstance instance;
   VkDevice device;
   PFN_vkDestroyInstance destroy_instance;
   if (strcmp(arm, "gate-open-refused") == 0) {
      /* An open hazard gate and a capture path refuse each other. */
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
      VkResult r = create(&instance, &device, &destroy_instance);
      assert(r == VK_ERROR_INITIALIZATION_FAILED && device == VK_NULL_HANDLE);
      assert(access(transcript, F_OK) != 0);
      destroy_instance(instance, NULL);
      rmdir(dir);
      printf("gate-open-refused: capture with an open gate refused\n");
      return 0;
   }
   if (strcmp(arm, "manifest-dir-refused") == 0) {
      /* A capture session and an attended-evidence directory refuse
       * each other: the semantic-cell retention belongs to the open
       * gate, and a capture pass must not occupy that directory.
       */
      setenv("R3V_NATIVE_MANIFEST_DIR", dir, 1);
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
      VkResult r = create(&instance, &device, &destroy_instance);
      assert(r == VK_ERROR_INITIALIZATION_FAILED && device == VK_NULL_HANDLE);
      assert(access(transcript, F_OK) != 0);
      destroy_instance(instance, NULL);
      rmdir(dir);
      printf("manifest-dir-refused: capture with an evidence directory "
             "refused\n");
      return 0;
   }
   if (strcmp(arm, "second-device-ordinal") == 0) {
      /* Two devices in one process, both under the same declared
       * capture path: dEQP's own robustness cases create a device
       * ahead of the one the case drives (createRobustBufferAccessDevice
       * in vktRobustnessBufferAccessTests.cpp), so the first device
       * claims ordinal 0 and the declared path but submits nothing,
       * and the second claims ordinal 1 and its own draw lands at
       * <path>.1.  The base path stays absent: the driver writes no
       * transcript for a device that never submits.
       */
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
      assert(create(&instance, &device, &destroy_instance) == VK_SUCCESS);
      struct r3v_native_device *first = r3v_native_device_from_handle(device);
      assert(first->plan_capture_active);

      VkInstance instance2;
      VkDevice device2;
      PFN_vkDestroyInstance destroy2;
      assert(create(&instance2, &device2, &destroy2) == VK_SUCCESS);
      struct r3v_native_device *second =
         r3v_native_device_from_handle(device2);
      assert(second->plan_capture_active);

      VkQueue queue2 = VK_NULL_HANDLE;
      vkGetDeviceQueue(device2, 0, 0, &queue2);
      struct triangle_resources res2 = create_triangle_resources(device2);
      submit_triangle_draw(device2, queue2, &res2, 3);
      destroy_triangle_resources(device2, &res2);

      vkDestroyDevice(device2, NULL);
      destroy2(instance2, NULL);
      vkDestroyDevice(device, NULL);
      destroy_instance(instance, NULL);

      char ordinal_one[4096];
      snprintf(ordinal_one, sizeof(ordinal_one), "%s.1", transcript);
      assert(access(transcript, F_OK) != 0);
      size_t n;
      char *text = read_file(ordinal_one, &n);
      struct r3v_native_plan p;
      assert(r3v_native_plan_parse(text, n, &p) == R3V_NATIVE_PLAN_PARSE_OK);
      assert(p.submission_count == 1);
      r3v_native_plan_finish(&p);
      free(text);
      unlink(ordinal_one);
      rmdir(dir);
      printf("second-device-ordinal: base path absent, second device's "
             "draw landed at %s\n", ordinal_one);
      return 0;
   }
   if (strcmp(arm, "derived-path-exists-refused") == 0) {
      /* The second device's own derived path is subject to the same
       * pre-existence refusal as the declared path: a stale <path>.1
       * from a prior run refuses the device that would claim it.
       */
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
      assert(create(&instance, &device, &destroy_instance) == VK_SUCCESS);
      char ordinal_one[4096];
      snprintf(ordinal_one, sizeof(ordinal_one), "%s.1", transcript);
      FILE *stale = fopen(ordinal_one, "wb");
      assert(stale != NULL);
      fclose(stale);
      VkInstance instance2;
      VkDevice device2;
      PFN_vkDestroyInstance destroy2;
      VkResult r = create(&instance2, &device2, &destroy2);
      assert(r == VK_ERROR_INITIALIZATION_FAILED && device2 == VK_NULL_HANDLE);
      destroy2(instance2, NULL);
      vkDestroyDevice(device, NULL);
      destroy_instance(instance, NULL);
      unlink(ordinal_one);
      assert(access(transcript, F_OK) != 0);
      rmdir(dir);
      printf("derived-path-exists-refused: a stale ordinal-1 path refused "
             "the second device\n");
      return 0;
   }
   if (strcmp(arm, "missing-directory-refused") == 0) {
      setenv("R3V_NATIVE_PLAN_CAPTURE_FILE", "/nonexistent-r3v/x.plan", 1);
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
      VkResult r = create(&instance, &device, &destroy_instance);
      assert(r == VK_ERROR_INITIALIZATION_FAILED);
      destroy_instance(instance, NULL);
      rmdir(dir);
      printf("missing-directory-refused: unreachable transcript refused\n");
      return 0;
   }
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   if (strcmp(arm, "slot-roles") == 0) {
      /* The role tables name each cell kind's reference slots: the
       * public GPU producer rides the triangle slots with its carrier
       * at the vertex slot, the fetched producer binds carrier, slot,
       * and source, and a slot past a table is named by index.
       */
      char role[R3V_NATIVE_PLAN_NAME_MAX + 1];
      struct {
         enum r3v_native_cell_kind kind;
         uint32_t slot;
         const char *want;
      } rows[] = {
         {R3V_NATIVE_CELL_KIND_TRIANGLE, 0, "vertex"},
         {R3V_NATIVE_CELL_KIND_TRIANGLE, 1, "color"},
         {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC, 0, "carrier"},
         {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC, 1, "color"},
         {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED, 0, "carrier"},
         {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED, 1, "slot"},
         {R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED, 2, "source"},
         {R3V_NATIVE_CELL_KIND_ZB_DEPTH_CONTROL, 2, "depth"},
         {R3V_NATIVE_CELL_KIND_COMPUTE_IDENTITY_CARRIER, 1, "command1"},
         {R3V_NATIVE_CELL_KIND_TRIANGLE, 5, "command5"},
      };
      for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
         r3v_native_plan_capture_slot_role(rows[i].kind, rows[i].slot, role);
         assert(strcmp(role, rows[i].want) == 0);
      }
      rmdir(dir);
      printf("slot-roles: role tables name every cell kind's slots\n");
      return 0;
   }
   assert(create(&instance, &device, &destroy_instance) == VK_SUCCESS);
   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   assert(native_device->plan_capture_active);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   struct triangle_resources res = create_triangle_resources(device);
   if (strcmp(arm, "relocation-count-diagnostic") == 0) {
      /* A 64-reference command plus the completion BO crosses the plan
       * schema ceiling by one.  Capture refuses before dereferencing the
       * synthetic reference roles or reaching the ioctl, so the harness can
       * exercise the diagnostic over distinct live BO handles without
       * inventing a hardware command-stream shape.
       */
      native_device->vk.physical->instance->enable_debug_logging = true;

      VkCommandBuffer cmd = record_triangle_draw_command(device, &res, 3);
      VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cmd, cmd);
      free(native_cmd->references);
      native_cmd->references =
         calloc(R3V_NATIVE_PLAN_RELOC_MAX, sizeof(*native_cmd->references));
      assert(native_cmd->references != NULL);
      native_cmd->reference_count = R3V_NATIVE_PLAN_RELOC_MAX;

      VkDeviceMemory memories[R3V_NATIVE_PLAN_RELOC_MAX];
      for (uint32_t i = 0; i < R3V_NATIVE_PLAN_RELOC_MAX; i++) {
         assert(vkAllocateMemory(device,
                                 &(VkMemoryAllocateInfo){
                                    .sType =
                                       VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize =
                                       R3V_NATIVE_MEMORY_ALIGNMENT,
                                    .memoryTypeIndex = 0,
                                 },
                                 NULL, &memories[i]) == VK_SUCCESS);
         VK_FROM_HANDLE(r3v_native_memory, memory, memories[i]);
         native_cmd->references[i] = (struct r3v_native_bo_reference){
            .handle = memory->bo.handle,
            .read_domains = RADEON_GEM_DOMAIN_GTT,
            .memory = memory,
         };
      }

      assert(vkQueueSubmit(queue, 1,
                           &(VkSubmitInfo){
                              .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                              .commandBufferCount = 1,
                              .pCommandBuffers = &cmd,
                           },
                           VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);
      size_t diagnostic_size;
      char *diagnostic_text = read_file(diagnostic, &diagnostic_size);
      char expected_diagnostic[128];
      snprintf(expected_diagnostic, sizeof(expected_diagnostic),
               "submission carries %u relocations, outside the plan schema",
               R3V_NATIVE_PLAN_RELOC_MAX + 1);
      assert(strstr(diagnostic_text, expected_diagnostic) != NULL);
      free(diagnostic_text);

      destroy_triangle_resources(device, &res);
      for (uint32_t i = 0; i < R3V_NATIVE_PLAN_RELOC_MAX; i++)
         vkFreeMemory(device, memories[i], NULL);
      vkDestroyDevice(device, NULL);
      destroy_instance(instance, NULL);
      assert(access(transcript, F_OK) != 0);
      char empty_marker[sizeof(transcript) + sizeof(".no_nonempty_ib")];
      snprintf(empty_marker, sizeof(empty_marker), "%s.no_nonempty_ib",
               transcript);
      assert(unlink(empty_marker) == 0);
      assert(unlink(diagnostic) == 0);
      assert(rmdir(dir) == 0);
      printf("relocation-count-diagnostic: schema refusal reports %u "
             "relocations\n",
             R3V_NATIVE_PLAN_RELOC_MAX + 1);
      return 0;
   }
   if (strcmp(arm, "write-failure-status") == 0) {
      /* Device creation proves the capture parent exists.  Removing the
       * still-empty parent afterward makes the post-completion transcript
       * landing fail without changing the transport result.
       */
      assert(access(transcript, F_OK) != 0);
      assert(rmdir(dir) == 0);
      assert(submit_triangle_draw_result(device, queue, &res, 3) ==
             VK_ERROR_DEVICE_LOST);
      assert(r3v_native_queue_submission_status(device) ==
             R3V_NATIVE_QUEUE_STATUS_COMPLETED);

      /* Device destruction lands the retained entry again.  Restoring the
       * parent lets teardown release capture state without a second I/O
       * failure obscuring the queue-status verdict above.
       */
      assert(mkdir(dir, 0700) == 0);
      destroy_triangle_resources(device, &res);
      vkDestroyDevice(device, NULL);
      destroy_instance(instance, NULL);
      assert(access(transcript, F_OK) == 0);
      assert(unlink(transcript) == 0);
      assert(rmdir(dir) == 0);
      printf("write-failure-status: completed transport precedes capture "
             "I/O failure\n");
      return 0;
   }
   VkMemoryRequirements reqs = res.image_reqs;
   /* Two distinct executable IBs: one and two clip-space source groups,
    * each retaining seven output-triangle slots per source triangle.
    */
   const uint32_t vertex_counts[2] = { 3, 6 };
   for (unsigned i = 0; i < 2; i++) {
      submit_triangle_draw(device, queue, &res, vertex_counts[i]);
      /* The transcript lands after every completed submission. */
      struct r3v_native_plan p;
      size_t n;
      char *text = read_file(transcript, &n);
      assert(r3v_native_plan_parse(text, n, &p) == R3V_NATIVE_PLAN_PARSE_OK);
      assert(p.submission_count == i + 1);
      r3v_native_plan_finish(&p);
      free(text);
   }
   /* Two executable buffers in one submit refuse under capture as under
    * the open gate: a plan entry is one submission.
    */
   VkCommandBuffer cmds[2];
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = res.pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 2,
             },
             cmds) == VK_SUCCESS);
   for (unsigned i = 0; i < 2; i++) {
      assert(vkBeginCommandBuffer(cmds[i], &(VkCommandBufferBeginInfo){
                                       .sType =
                                          VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    }) == VK_SUCCESS);
      record_triangle_draw(cmds[i], &res, vertex_counts[i]);
      assert(vkEndCommandBuffer(cmds[i]) == VK_SUCCESS);
   }
   assert(vkQueueSubmit(queue, 1,
                        &(VkSubmitInfo){
                           .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 2,
                           .pCommandBuffers = cmds,
                        },
                        VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);

   struct r300_triangle_render_shape module_shape;
   r300_tcl_bypass_triangle_render_shape_reference(&module_shape);
   const uint32_t module_color[4] = R3V_REFERENCE_FRAGMENT_COLOR_BITS;
   memcpy(module_shape.color_bits, module_color, sizeof(module_color));
   struct r300_tcl_bypass_triangle_ib one;
   struct r300_tcl_bypass_triangle_ib two;
   assert(r300_tcl_bypass_triangle_clip_space_render_shape_emit(
             &module_shape, 1u, &one) == 0);
   assert(r300_tcl_bypass_triangle_clip_space_render_shape_emit(
             &module_shape, 2u, &two) == 0);
   char one_digest[BLAKE3_OUT_LEN * 2 + 1];
   char two_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(one.ib, one.ib_size_dwords, one_digest);
   r300_triangle_ib_digest_hex(two.ib, two.ib_size_dwords, two_digest);
   const uint32_t one_dwords = one.ib_size_dwords;
   const uint32_t two_dwords = two.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&one);
   r300_tcl_bypass_triangle_release(&two);

   destroy_triangle_resources(device, &res);
   vkDestroyDevice(device, NULL);
   destroy_instance(instance, NULL);

   /* The transcript: two entries in submission order, the one-source
    * digest then the two-source digest, each with the target, vertex, and
    * completion relocations, the host identity the driver knows, and
    * placeholders for the run identities.
    */
   size_t n;
   char *text = read_file(transcript, &n);
   struct r3v_native_plan p;
   assert(r3v_native_plan_parse(text, n, &p) == R3V_NATIVE_PLAN_PARSE_OK);
   assert(p.submission_count == 2 && p.max_submissions == 2);
   assert(strcmp(p.submissions[0].ib_blake3, one_digest) == 0);
   assert(p.submissions[0].ib_dwords == one_dwords);
   assert(strcmp(p.submissions[1].ib_blake3, two_digest) == 0);
   assert(p.submissions[1].ib_dwords == two_dwords);
   assert(strcmp(p.submissions[0].ib_blake3, p.submissions[1].ib_blake3) != 0);
   for (unsigned i = 0; i < 2; i++) {
      const struct r3v_native_plan_submission *s = &p.submissions[i];
      assert(s->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE);
      assert(strcmp(s->emitter, "r3v") == 0);
      assert(s->reloc_count == 3);
      assert(strcmp(s->relocs[s->reloc_count - 1].role, "completion") == 0);
      assert(s->relocs[s->reloc_count - 1].size == 4);
      /* The triangle cell binds the vertex stream first and the color
       * target second, and the roles name those slots.
       */
      assert(strcmp(s->relocs[0].role, "vertex") == 0);
      assert(strcmp(s->relocs[1].role, "color") == 0);
      uint64_t command_bytes = 0;
      for (uint32_t r = 0; r + 1 < s->reloc_count; r++) {
         assert(s->relocs[r].size >= 4096);
         command_bytes += s->relocs[r].size;
      }
      assert(command_bytes == reqs.size + 4096);
   }
   assert(p.max_ib_dwords == two_dwords);
   assert(p.max_relocs == 3);
   assert(p.max_cumulative_bytes == 2 * (reqs.size + 4096 + 4));
   struct utsname host;
   assert(uname(&host) == 0 && strcmp(p.kernel_release, host.release) == 0);
   assert(strcmp(p.module_srcversion, "unloaded") == 0);
   assert(p.pci_vendor_id == R3V_NATIVE_ARMING_PCI_VENDOR &&
          p.pci_device_id == R3V_NATIVE_ARMING_PCI_DEVICE);
   assert(strcmp(p.source_sha, "0000000000000000000000000000000000000000") == 0);
   assert(!p.source_clean);
   assert(strspn(p.dso_blake3, "0123456789abcdef") == 64 &&
          strcmp(p.dso_blake3,
                 "0000000000000000000000000000000000000000000000000000000000000000") != 0);
   /* A transcript binds to no run: its placeholders refuse. */
   struct r3v_native_plan_identity id = {
      .source_sha = "0000000000000000000000000000000000000000",
      .source_clean = true,
   };
   assert(r3v_native_plan_bind(&p, &id) == R3V_NATIVE_PLAN_BIND_SOURCE_DIRTY);
   r3v_native_plan_finish(&p);
   free(text);
   if (argc > 2 && strcmp(argv[2], "--keep") == 0) {
      printf("capture: %s\n", transcript);
   } else {
      unlink(transcript);
      rmdir(dir);
      printf("capture: two entries recorded and removed\n");
   }
   return 0;
}
