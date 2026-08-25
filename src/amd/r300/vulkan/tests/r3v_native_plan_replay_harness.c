/*
 * SPDX-License-Identifier: MIT
 *
 * Plan replay under the drm-shim: a captured two-draw transcript composes
 * into a plan bound to this process's identity, a plan device replays it
 * to exhaustion, and every single defect refuses with its own name.
 */

#undef NDEBUG
#define VK_NO_PROTOTYPES

#include "r3v_native.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_shim_arming.h"

#include "amd/radeon/drm_vk/radeon_drm_vk_ioctl.h"
#include "git_sha1.h"

#include <assert.h>
#include <errno.h>
#include <radeon_drm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

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
   f(vkCmdPipelineBarrier) f(vkGetDeviceQueue) f(vkQueueSubmit)            \
   f(vkQueueWaitIdle) f(vkDestroyDevice)
#define DECLARE(name) static PFN_##name name;
DEVICE_COMMANDS(DECLARE)
#undef DECLARE

/* One device with its scene: everything a draw needs, torn down together. */
struct scene {
   VkInstance instance;
   PFN_vkDestroyInstance destroy_instance;
   VkDevice device;
   VkQueue queue;
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
   VkCommandBuffer cmds[3];
};

static VkResult
create_device(struct scene *s)
{
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                             .pApplicationInfo =
                                &(VkApplicationInfo){
                                   .sType =
                                      VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .apiVersion = VK_API_VERSION_1_0,
                                },
                          },
                          NULL, &s->instance) == VK_SUCCESS);
   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(s->instance,
                                           "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice create =
      (PFN_vkCreateDevice)gipa(s->instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(s->instance, "vkGetDeviceProcAddr");
   s->destroy_instance =
      (PFN_vkDestroyInstance)gipa(s->instance, "vkDestroyInstance");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated = enumerate(s->instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1);
   const float priority = 1.0f;
   VkResult result = create(
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
      NULL, &s->device);
   if (result != VK_SUCCESS)
      return result;
#define LOAD(name) name = (PFN_##name)gdpa(s->device, #name); assert(name);
   DEVICE_COMMANDS(LOAD)
#undef LOAD
   vkGetDeviceQueue(s->device, 0, 0, &s->queue);
   return VK_SUCCESS;
}

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

static void
build_scene(struct scene *s)
{
   VkDevice device = s->device;
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
             NULL, &s->image) == VK_SUCCESS);
   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(device, s->image, &reqs);
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = reqs.size,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &s->image_memory) == VK_SUCCESS);
   assert(vkBindImageMemory(device, s->image, s->image_memory, 0) ==
          VK_SUCCESS);
   assert(vkCreateImageView(
             device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = s->image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = R3V_NATIVE_TARGET_FORMAT,
                .subresourceRange = { .aspectMask =
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                      .levelCount = 1,
                                      .layerCount = 1 },
             },
             NULL, &s->view) == VK_SUCCESS);
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &s->vertex_memory) == VK_SUCCESS);
   assert(vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = 256,
                            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &s->vertex_buffer) == VK_SUCCESS);
   assert(vkBindBufferMemory(device, s->vertex_buffer, s->vertex_memory,
                             0) == VK_SUCCESS);
   void *map = NULL;
   assert(vkMapMemory(device, s->vertex_memory, 0, VK_WHOLE_SIZE, 0,
                      &map) == VK_SUCCESS);
   memcpy(map, ndc_triangle, sizeof(ndc_triangle));
   vkUnmapMemory(device, s->vertex_memory);
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
             NULL, &s->pass) == VK_SUCCESS);
   assert(vkCreateFramebuffer(
             device,
             &(VkFramebufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                .renderPass = s->pass,
                .attachmentCount = 1,
                .pAttachments = &s->view,
                .width = R3V_NATIVE_TARGET_WIDTH,
                .height = R3V_NATIVE_TARGET_HEIGHT,
                .layers = 1,
             },
             NULL, &s->framebuffer) == VK_SUCCESS);
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             },
             NULL, &s->layout) == VK_SUCCESS);
   s->vs = make_module(device, r3v_reference_vertex_spirv,
                       sizeof(r3v_reference_vertex_spirv));
   s->fs = make_module(device, r3v_reference_fragment_spirv,
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
                        .module = s->vs,
                        .pName = "main" },
                      { .sType =
                           VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                        .module = s->fs,
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
                .layout = s->layout,
                .renderPass = s->pass,
             },
             NULL, &s->pipeline) == VK_SUCCESS);
   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0,
             },
             NULL, &s->pool) == VK_SUCCESS);
   assert(vkAllocateCommandBuffers(
             device,
             &(VkCommandBufferAllocateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = s->pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 3,
             },
             s->cmds) == VK_SUCCESS);
}

/* Records a draw of vertex_count vertices into cmd: the three-vertex
 * cell and the two-triangle family member are the two distinct IBs.
 */
static void
record_draw(struct scene *s, VkCommandBuffer cmd, uint32_t vertex_count)
{
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
         .image = s->image,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .layerCount = 1 },
      });
   vkCmdBeginRenderPass(
      cmd,
      &(VkRenderPassBeginInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = s->pass,
         .framebuffer = s->framebuffer,
         .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                     R3V_NATIVE_TARGET_HEIGHT } },
         .clearValueCount = 1,
         .pClearValues = &(VkClearValue){
            .color = { .float32 = { CLEAR_SENTINEL, CLEAR_SENTINEL,
                                    CLEAR_SENTINEL, CLEAR_SENTINEL } } },
      },
      VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pipeline);
   vkCmdBindVertexBuffers(cmd, 0, 1, &s->vertex_buffer, &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, vertex_count, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);
}

static VkResult
submit(struct scene *s, VkCommandBuffer *cmds, uint32_t count)
{
   VkResult r = vkQueueSubmit(s->queue, 1,
                              &(VkSubmitInfo){
                                 .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .commandBufferCount = count,
                                 .pCommandBuffers = cmds,
                              },
                              VK_NULL_HANDLE);
   if (r == VK_SUCCESS)
      assert(vkQueueWaitIdle(s->queue) == VK_SUCCESS);
   return r;
}

static void
teardown(struct scene *s)
{
   VkDevice device = s->device;
   vkDestroyCommandPool(device, s->pool, NULL);
   vkDestroyPipeline(device, s->pipeline, NULL);
   vkDestroyShaderModule(device, s->vs, NULL);
   vkDestroyShaderModule(device, s->fs, NULL);
   vkDestroyPipelineLayout(device, s->layout, NULL);
   vkDestroyFramebuffer(device, s->framebuffer, NULL);
   vkDestroyRenderPass(device, s->pass, NULL);
   vkDestroyBuffer(device, s->vertex_buffer, NULL);
   vkFreeMemory(device, s->vertex_memory, NULL);
   vkDestroyImageView(device, s->view, NULL);
   vkDestroyImage(device, s->image, NULL);
   vkFreeMemory(device, s->image_memory, NULL);
   vkDestroyDevice(device, NULL);
   s->destroy_instance(s->instance, NULL);
}

static char *
read_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return NULL;
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)n + 1);
   assert(buf != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n);
   buf[n] = '\0';
   fclose(f);
   *size = (size_t)n;
   return buf;
}

static void
write_file(const char *path, const char *text, size_t size)
{
   FILE *f = fopen(path, "wb");
   assert(f != NULL && fwrite(text, 1, size, f) == size && fclose(f) == 0);
}

static const char *
state_of(const char *evidence_dir)
{
   static char line[512];
   char path[4096];
   snprintf(path, sizeof(path), "%s/session.state", evidence_dir);
   size_t n;
   char *text = read_file(path, &n);
   if (text == NULL)
      return "absent";
   snprintf(line, sizeof(line), "%s", text);
   free(text);
   char *nl = strchr(line, '\n');
   if (nl)
      *nl = '\0';
   return line;
}

/* The drm.ops seam: one arm refuses the first CS ioctl to prove the
 * latch after a transport failure.
 */
static const struct radeon_drm_vk_ioctl_ops *saved_ops;
static struct radeon_drm_vk_ioctl_ops injected_ops;
static bool refuse_next_cs;

static int
injected_command_write_read(int fd, unsigned long request, void *data,
                            unsigned int size)
{
   if (request == DRM_RADEON_CS && refuse_next_cs) {
      refuse_next_cs = false;
      return -EINVAL;
   }
   return saved_ops->command_write_read(fd, request, data, size);
}

static const char *
source_sha_for_this_build(char out[41])
{
   const char *tag = strstr(MESA_GIT_SHA1, "git-");
   assert(tag != NULL);
   size_t n = strspn(tag + 4, "0123456789abcdef");
   memcpy(out, tag + 4, n);
   memset(out + n, '0', 40 - n);
   out[40] = '\0';
   return out;
}

int
main(int argc, char **argv)
{
   const char *arm = argc > 1 ? argv[1] : "replay";
   char dir[] = "/tmp/r3v-native-plan-replay-XXXXXX";
   assert(mkdtemp(dir) != NULL);
   char transcript[4096], plan_path[4096], evidence[4096];
   snprintf(transcript, sizeof(transcript), "%s/transcript.plan", dir);
   snprintf(plan_path, sizeof(plan_path), "%s/shard.plan", dir);
   snprintf(evidence, sizeof(evidence), "%s/evidence", dir);
   assert(mkdir(evidence, 0755) == 0);
   /* Every gate the replay bind enumerates starts closed, so an arm
    * tests what it names whatever the operator's shell carries.
    */
   {
      extern char **environ;
      char **e = environ;
      char *names[256];
      unsigned count = 0;
      for (; *e != NULL && count < 256; e++) {
         if (strncmp(*e, "R3V_NATIVE_", 11) == 0) {
            size_t n = strcspn(*e, "=");
            names[count] = strndup(*e, n);
            count++;
         }
      }
      for (unsigned i = 0; i < count; i++) {
         unsetenv(names[i]);
         free(names[i]);
      }
   }
   unsetenv("R3V_NATIVE_MANIFEST_DIR");

   /* Capture: two distinct draws through a capture device. */
   setenv("R3V_NATIVE_PLAN_CAPTURE_FILE", transcript, 1);
   struct scene cap = {0};
   assert(create_device(&cap) == VK_SUCCESS);
   build_scene(&cap);
   record_draw(&cap, cap.cmds[0], 3);
   record_draw(&cap, cap.cmds[1], 6);
   assert(submit(&cap, &cap.cmds[0], 1) == VK_SUCCESS);
   assert(submit(&cap, &cap.cmds[1], 1) == VK_SUCCESS);
   teardown(&cap);
   unsetenv("R3V_NATIVE_PLAN_CAPTURE_FILE");

   /* Compose: the transcript plus this process's identity. */
   size_t n;
   char *text = read_file(transcript, &n);
   struct r3v_native_plan plan;
   assert(r3v_native_plan_parse(text, n, &plan) == R3V_NATIVE_PLAN_PARSE_OK);
   free(text);
   assert(plan.submission_count == 2);
   char source[41];
   source_sha_for_this_build(source);
   strcpy(plan.source_sha, source);
   plan.source_clean = true;
   memset(plan.deqp_sha256, '1', 64);
   memset(plan.partition_sha256, '2', 64);
   memset(plan.caselist_sha256, '3', 64);
   strcpy(plan.deqp_release, "opengl-cts-fixture");
   struct utsname host;
   assert(uname(&host) == 0);
   strcpy(plan.kernel_release, host.release);
   strcpy(plan.module_srcversion, R3V_NATIVE_SHIM_MODULE_SRCVERSION);
   const char *nonce = "00112233445566778899aabbccddeeff";
   strcpy(plan.nonce, nonce);
   strcpy(plan.evidence_dir, evidence);
   plan.max_runtime_seconds = 600;
   plan.queue_claim = R3V_NATIVE_PLAN_QUEUE_DEFAULT_GRAPHICS_ONLY;
   /* Per-arm plan mutations, applied before sealing. */
   if (strcmp(arm, "wrong-kernel") == 0)
      strcpy(plan.kernel_release, "0.0.0-none");
   else if (strcmp(arm, "wrong-dso") == 0)
      memset(plan.dso_blake3, 'f', 64);
   else if (strcmp(arm, "wrong-pci") == 0)
      plan.pci_device_id = 0x5975;
   else if (strcmp(arm, "wrong-source") == 0)
      memset(plan.source_sha, 'e', 40);
   else if (strcmp(arm, "deadline") == 0)
      plan.max_runtime_seconds = 1;
   else if (strcmp(arm, "mutated-entry") == 0)
      plan.submissions[1].ib_dwords -= 1;
   else if (strcmp(arm, "missing-terminal-entry") == 0) {
      plan.submission_count = 1;
      plan.max_submissions = 1;
   }
   long size = r3v_native_plan_write(&plan, NULL, 0);
   assert(size > 0);
   char *out = malloc((size_t)size);
   assert(r3v_native_plan_write(&plan, out, (size_t)size) == size);
   if (strcmp(arm, "truncated-plan") == 0)
      size -= 40;
   write_file(plan_path, out, (size_t)size);
   free(out);
   r3v_native_plan_finish(&plan);

   /* Replay device. */
   setenv("R3V_NATIVE_PLAN_FILE", plan_path, 1);
   setenv("R3V_NATIVE_PLAN_NONCE",
          strcmp(arm, "wrong-nonce") == 0
             ? "ffeeddccbbaa99887766554433221100" : nonce, 1);
   if (strcmp(arm, "hazard-gate-open") == 0)
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   if (strcmp(arm, "capture-and-plan") == 0)
      setenv("R3V_NATIVE_PLAN_CAPTURE_FILE", transcript, 1);
   if (strcmp(arm, "gate-contamination") == 0)
      setenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL", "1", 1);
   if (strcmp(arm, "used-evidence-dir") == 0) {
      char stale[4096];
      snprintf(stale, sizeof(stale), "%s/session.state", evidence);
      write_file(stale, "complete\tx\t2\t-\n", 16);
   }
   struct scene rep = {0};
   VkResult created = create_device(&rep);
   if (strcmp(arm, "truncated-plan") == 0 ||
       strcmp(arm, "hazard-gate-open") == 0 ||
       strcmp(arm, "capture-and-plan") == 0) {
      assert(created == VK_ERROR_INITIALIZATION_FAILED);
      rep.destroy_instance(rep.instance, NULL);
      printf("%s: plan device refused at creation\n", arm);
      goto done;
   }
   assert(created == VK_SUCCESS);
   struct r3v_native_device *native = r3v_native_device_from_handle(rep.device);
   assert(native->plan_replay_active);
   native->arming_provider = &r3v_native_shim_arming_provider;
   saved_ops = native->drm.ops;
   injected_ops = *saved_ops;
   injected_ops.command_write_read = injected_command_write_read;
   native->drm.ops = &injected_ops;
   build_scene(&rep);
   record_draw(&rep, rep.cmds[0], 3);
   record_draw(&rep, rep.cmds[1], 6);
   record_draw(&rep, rep.cmds[2], 3);

   const char *expect_state = "complete";
   if (strcmp(arm, "replay") == 0) {
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      assert(strncmp(state_of(evidence), "bound", 5) == 0);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_SUCCESS);
      char path[4096];
      snprintf(path, sizeof(path), "%s/chain.log", evidence);
      char *chain = read_file(path, &n);
      assert(chain != NULL);
      unsigned lines = 0;
      for (const char *p = chain; (p = strchr(p, '\n')) != NULL; p++)
         lines++;
      assert(lines == 2);
      free(chain);
      snprintf(path, sizeof(path), "%s/ib", evidence);
      struct stat st;
      assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
   } else if (strcmp(arm, "reordered") == 0) {
      assert(submit(&rep, &rep.cmds[1], 1) == VK_ERROR_DEVICE_LOST);
      assert(strncmp(state_of(evidence), "terminal", 8) == 0 &&
             strstr(state_of(evidence), "mismatch:digest") != NULL);
      assert(submit(&rep, &rep.cmds[0], 1) == VK_ERROR_DEVICE_LOST);
      expect_state = "terminal";
   } else if (strcmp(arm, "duplicated") == 0) {
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      assert(submit(&rep, &rep.cmds[2], 1) == VK_ERROR_DEVICE_LOST);
      assert(strstr(state_of(evidence), "mismatch:digest") != NULL);
      expect_state = "terminal";
   } else if (strcmp(arm, "extra") == 0) {
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_SUCCESS);
      assert(submit(&rep, &rep.cmds[2], 1) == VK_ERROR_DEVICE_LOST);
      assert(strstr(state_of(evidence), "exhausted") != NULL);
      expect_state = "terminal";
   } else if (strcmp(arm, "missing-terminal") == 0) {
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      expect_state = "incomplete";
   } else if (strcmp(arm, "missing-terminal-entry") == 0) {
      /* The plan lists one submission; the second live draw is extra. */
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_ERROR_DEVICE_LOST);
      assert(strstr(state_of(evidence), "exhausted") != NULL);
      expect_state = "terminal";
   } else if (strcmp(arm, "mutated-entry") == 0) {
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_ERROR_DEVICE_LOST);
      assert(strstr(state_of(evidence), "mismatch:dwords") != NULL);
      expect_state = "terminal";
   } else if (strcmp(arm, "two-buffers") == 0) {
      assert(submit(&rep, &rep.cmds[0], 2) == VK_ERROR_DEVICE_LOST);
      /* Refused ahead of the bind: the directory stays unclaimed. */
      assert(strcmp(state_of(evidence), "absent") == 0);
      expect_state = "unbound";
   } else if (strcmp(arm, "deadline") == 0) {
      assert(submit(&rep, &rep.cmds[0], 1) == VK_SUCCESS);
      sleep(2);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_ERROR_DEVICE_LOST);
      assert(strstr(state_of(evidence), "runtime_exceeded") != NULL);
      expect_state = "terminal";
   } else if (strcmp(arm, "ioctl-failure-then-continue") == 0) {
      refuse_next_cs = true;
      assert(submit(&rep, &rep.cmds[0], 1) == VK_ERROR_DEVICE_LOST);
      assert(strstr(state_of(evidence), "terminal\t") != NULL &&
             strstr(state_of(evidence), "ioctl:-22:") != NULL);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_ERROR_DEVICE_LOST);
      expect_state = "terminal";
   } else {
      /* Bind refusals: the first submission refuses with the bind
       * result's name and the directory stays unclaimed.
       */
      const char *want = strcmp(arm, "wrong-nonce") == 0    ? "nonce"
                         : strcmp(arm, "wrong-kernel") == 0 ? "kernel"
                         : strcmp(arm, "wrong-dso") == 0    ? "dso"
                         : strcmp(arm, "wrong-pci") == 0    ? "pci"
                         : strcmp(arm, "wrong-source") == 0 ? "source"
                         : strcmp(arm, "used-evidence-dir") == 0
                            ? "evidence_dir"
                         : strcmp(arm, "gate-contamination") == 0
                            ? "gate_contamination"
                                                            : NULL;
      assert(want != NULL);
      assert(submit(&rep, &rep.cmds[0], 1) == VK_ERROR_DEVICE_LOST);
      assert(native->plan_replay.refused &&
             strcmp(r3v_native_plan_bind_result_name(
                       native->plan_replay.bind_result), want) == 0);
      assert(submit(&rep, &rep.cmds[1], 1) == VK_ERROR_DEVICE_LOST);
      expect_state = strcmp(arm, "used-evidence-dir") == 0 ? "complete"
                                                            : "absent";
   }
   native->drm.ops = saved_ops;
   teardown(&rep);
   if (strcmp(arm, "unbound") != 0)
      assert(strncmp(state_of(evidence), expect_state,
                     strlen(expect_state)) == 0 ||
             (strcmp(expect_state, "unbound") == 0 &&
              strcmp(state_of(evidence), "absent") == 0));
   printf("%s: %s\n", arm, state_of(evidence));
done:
   {
      char cmd[8192];
      snprintf(cmd, sizeof(cmd), "rm -r '%s'", dir);
      assert(system(cmd) == 0);
   }
   return 0;
}
