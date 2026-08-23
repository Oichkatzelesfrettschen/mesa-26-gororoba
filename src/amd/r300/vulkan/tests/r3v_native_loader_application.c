/*
 * SPDX-License-Identifier: MIT
 *
 * Loader-only application over the native R3V public surface: the
 * complete instance-to-submit sequence an external Vulkan program
 * performs, linked against the installed loader alone and reaching the
 * ICD only through its manifest.  The one driver artifact compiled in
 * is the reference SPIR-V data header, the application's own shader
 * bytes; the symbol audit proves the binary references no driver
 * symbol, and the check wrapper proves the recorded IB byte-identical
 * to the independently emitted reference cell.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_native_reference_spirv.h"

#include "amd/r300/common/r300_chip_identity.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* The public contract values the driver advertises: the 64x64 linear
 * B8G8R8A8_UNORM color target, the F32_4 vertex stream, and the
 * sentinel clear whose identically evaluated expression rounds to the
 * one binary32 value the cell realizes.
 */
#define TARGET_WIDTH 64
#define TARGET_HEIGHT 64
#define TARGET_FORMAT VK_FORMAT_B8G8R8A8_UNORM
#define CLEAR_SENTINEL ((float)0xa5 / 255.0f)
#define SENTINEL_PIXEL 0xa5a5a5a5u
#define COLOR_SEED 0x5c5c5c5cu

/* The reference triangle in NDC: the driver's CPU vertex node applies
 * the Vulkan viewport transform, and over the 64x64 target these
 * positions map byte-exactly onto the window-space payload the silicon
 * witness rendered -- (x + 1) * 32 lands on 8, 56, and 32 in binary32
 * with no rounding.
 */
static const float triangle_vertices[12] = {
   -0.75f, -0.75f, 0.0f, 1.0f,
    0.75f, -0.75f, 0.0f, 1.0f,
    0.00f,  0.75f, 0.0f, 1.0f,
};

/* The loader resolves the ICD through its manifest, so the proof that
 * this run exercised the intended driver is the mapped DSO itself:
 * R3V_EXPECTED_ICD_DSO names the built library, and the maps scan
 * refuses a run the loader satisfied from another ICD.
 */
static void
assert_icd_dso_mapped(void)
{
   const char *expected = getenv("R3V_EXPECTED_ICD_DSO");
   assert(expected != NULL && expected[0] != '\0');

   FILE *maps = fopen("/proc/self/maps", "r");
   assert(maps != NULL);
   char line[4096];
   int found = 0;
   while (fgets(line, sizeof(line), maps) != NULL) {
      if (strstr(line, expected) != NULL) {
         found = 1;
         break;
      }
   }
   fclose(maps);
   assert(found);
}

int
main(void)
{
   /* The gate stays closed by construction: this application proves the
    * public route up to the authorization boundary and never opens the
    * hazard environment.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");

   VkInstance instance = VK_NULL_HANDLE;
   assert(vkCreateInstance(&(VkInstanceCreateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                           },
                           NULL, &instance) == VK_SUCCESS);

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated =
      vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1 && pdev != VK_NULL_HANDLE);

   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   assert(props.vendorID == R300_PCI_VENDOR_ATI &&
          props.deviceID == R300_PCI_DEVICE_RS482);

   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   assert(vkCreateDevice(
             pdev,
             &(VkDeviceCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
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
   assert_icd_dso_mapped();

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   assert(queue != VK_NULL_HANDLE);

   /* The qualified color target: size and memory contract discovered
    * through the public requirements query, one page of seeded tail
    * past the footprint so the load-op clear's bound is observable.
    */
   VkImage image = VK_NULL_HANDLE;
   assert(vkCreateImage(
             device,
             &(VkImageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = TARGET_FORMAT,
                .extent = { TARGET_WIDTH, TARGET_HEIGHT, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_LINEAR,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
             },
             NULL, &image) == VK_SUCCESS);

   VkMemoryRequirements reqs;
   vkGetImageMemoryRequirements(device, image, &reqs);
   assert(reqs.size > 0 && reqs.size % 4 == 0 &&
          (reqs.memoryTypeBits & 1) != 0);

   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   assert(vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = reqs.size + 4096,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &color_memory) == VK_SUCCESS);
   assert(vkBindImageMemory(device, image, color_memory, 0) == VK_SUCCESS);

   {
      uint32_t *seed_map = NULL;
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&seed_map) == VK_SUCCESS);
      for (VkDeviceSize i = 0; i < (reqs.size + 4096) / 4; i++)
         seed_map[i] = COLOR_SEED;
      vkUnmapMemory(device, color_memory);
   }

   VkImageView view = VK_NULL_HANDLE;
   assert(vkCreateImageView(
             device,
             &(VkImageViewCreateInfo){
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = TARGET_FORMAT,
                .subresourceRange = { .aspectMask =
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                      .levelCount = 1,
                                      .layerCount = 1 },
             },
             NULL, &view) == VK_SUCCESS);

   /* The application vertex stream through the public map. */
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
                            .sType =
                               VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = 256,
                            .usage =
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &vertex_buffer) == VK_SUCCESS);
   assert(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0) ==
          VK_SUCCESS);
   {
      void *map = NULL;
      assert(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                         &map) == VK_SUCCESS);
      memcpy(map, triangle_vertices, sizeof(triangle_vertices));
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
                      .format = TARGET_FORMAT,
                      .samples = VK_SAMPLE_COUNT_1_BIT,
                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                      .stencilStoreOp =
                         VK_ATTACHMENT_STORE_OP_DONT_CARE,
                      .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
                   },
                .subpassCount = 1,
                .pSubpasses =
                   &(VkSubpassDescription){
                      .pipelineBindPoint =
                         VK_PIPELINE_BIND_POINT_GRAPHICS,
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
                .pAttachments = &view,
                .width = TARGET_WIDTH,
                .height = TARGET_HEIGHT,
                .layers = 1,
             },
             NULL, &framebuffer) == VK_SUCCESS);

   VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
   assert(vkCreateShaderModule(
             device,
             &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = sizeof(r3v_reference_vertex_spirv),
                .pCode = r3v_reference_vertex_spirv,
             },
             NULL, &vs) == VK_SUCCESS);
   assert(vkCreateShaderModule(
             device,
             &(VkShaderModuleCreateInfo){
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = sizeof(r3v_reference_fragment_spirv),
                .pCode = r3v_reference_fragment_spirv,
             },
             NULL, &fs) == VK_SUCCESS);

   VkPipelineLayout layout = VK_NULL_HANDLE;
   assert(vkCreatePipelineLayout(
             device,
             &(VkPipelineLayoutCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
             },
             NULL, &layout) == VK_SUCCESS);

   VkPipeline pipeline = VK_NULL_HANDLE;
   assert(vkCreateGraphicsPipelines(
             device, VK_NULL_HANDLE, 1,
             &(VkGraphicsPipelineCreateInfo){
                .sType =
                   VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
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
                      .topology =
                         VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                   },
                .pViewportState =
                   &(VkPipelineViewportStateCreateInfo){
                      .sType =
                         VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                      .viewportCount = 1,
                      .pViewports =
                         &(VkViewport){
                            .width = TARGET_WIDTH,
                            .height = TARGET_HEIGHT,
                            .maxDepth = 1.0f,
                         },
                      .scissorCount = 1,
                      .pScissors =
                         &(VkRect2D){
                            .extent = { TARGET_WIDTH, TARGET_HEIGHT },
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
                               VK_COLOR_COMPONENT_R_BIT |
                               VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT,
                         },
                   },
                .layout = layout,
                .renderPass = pass,
             },
             NULL, &pipeline) == VK_SUCCESS);
   assert(pipeline != VK_NULL_HANDLE);

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
                .sType =
                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
             },
             &cmd) == VK_SUCCESS);

   assert(vkBeginCommandBuffer(
             cmd, &(VkCommandBufferBeginInfo){
                     .sType =
                        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                  }) == VK_SUCCESS);
   vkCmdBeginRenderPass(
      cmd,
      &(VkRenderPassBeginInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = pass,
         .framebuffer = framebuffer,
         .renderArea = { .extent = { TARGET_WIDTH, TARGET_HEIGHT } },
         .clearValueCount = 1,
         .pClearValues =
            &(VkClearValue){
               .color = { .float32 = { CLEAR_SENTINEL, CLEAR_SENTINEL,
                                       CLEAR_SENTINEL,
                                       CLEAR_SENTINEL } },
            },
      },
      VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer,
                          &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);

   /* The closed hazard gate refuses before the deferred draw executes,
    * so the loader-observed verdict is device loss with the application's
    * memory untouched; the IB manifest the submit retained before the
    * gate is the wrapper's byte-equality evidence.
    */
   assert(vkQueueSubmit(queue, 1,
                        &(VkSubmitInfo){
                           .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd,
                        },
                        VK_NULL_HANDLE) == VK_ERROR_DEVICE_LOST);

   /* Readback oracle: the refused submit left the whole allocation at
    * its seed -- the load-op clear did not run, and neither did anything
    * past the footprint.  The footprint is the declared memory contract
    * -- the row pitch times the height plus one oracle-headroom row --
    * pinned against the requirements query so the oracle and the driver
    * share one definition, then swept in full together with the tail.
    * An oracle failure exits with status 3, its own verdict class; the
    * corrupt fixtures write one sentinel dword through the application's
    * own mapping before the sweep, inside the footprint or past it, so
    * the exact-status calibration legs prove the sweep still judges
    * bytes.
    */
   {
      const VkDeviceSize footprint_bytes =
         (VkDeviceSize)TARGET_WIDTH * 4 * (TARGET_HEIGHT + 1);
      assert(reqs.size == footprint_bytes);
      uint32_t *color_map = NULL;
      assert(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                         (void **)&color_map) == VK_SUCCESS);
      const char *corrupt_footprint =
         getenv("R3V_LOADER_APP_FIXTURE_CORRUPT_FOOTPRINT");
      const char *corrupt_tail =
         getenv("R3V_LOADER_APP_FIXTURE_CORRUPT_TAIL");
      const VkDeviceSize footprint_dwords = footprint_bytes / 4;
      if (corrupt_footprint != NULL && strcmp(corrupt_footprint, "1") == 0)
         color_map[footprint_dwords / 2] = SENTINEL_PIXEL;
      if (corrupt_tail != NULL && strcmp(corrupt_tail, "1") == 0)
         color_map[footprint_dwords + 1] = SENTINEL_PIXEL;
      VkDeviceSize mismatches = 0;
      for (VkDeviceSize i = 0; i < (footprint_bytes + 4096) / 4; i++) {
         if (color_map[i] != COLOR_SEED)
            mismatches++;
      }
      vkUnmapMemory(device, color_memory);
      if (mismatches != 0) {
         fprintf(stderr,
                 "readback oracle: %llu deviating dwords in the seeded "
                 "footprint or tail after the refused submit\n",
                 (unsigned long long)mismatches);
         return 3;
      }
   }

   /* Full public teardown. */
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyImageView(device, view, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("r3v-native-loader-application: PASS\n");
   return 0;
}
