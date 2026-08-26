/*
 * SPDX-License-Identifier: MIT
 *
 * Attended sampled cell: drives the public sampling surface -- the
 * varying vertex module, the sampled fragment module, and the set-0
 * combined image sampler over a one-color texture -- to a live
 * DRM_RADEON_CS on RS482 silicon and reports the render-shape oracle's
 * verdict.  A uniform texture makes every sampled coordinate return
 * one texel, so the interior equals that texel whatever the
 * interpolated coordinates are; a wrong TX program cannot reproduce
 * it.  Runs only under the authorization and procedure in
 * docs/hardware/r3v-native-attended-render-shape-procedure.md; every
 * stage prints and flushes before it runs.
 */

#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The one texel every sampled coordinate returns: distinct in each
 * byte lane from the clear sentinel and the seeds.
 */
#define R3V_SAMPLED_TEXEL_R 0x20
#define R3V_SAMPLED_TEXEL_G 0x60
#define R3V_SAMPLED_TEXEL_B 0xa0
#define R3V_SAMPLED_TEXEL_A 0xe0

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

static int
write_target(const char *dir, const void *data, size_t size)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/color_target.bin", dir);
   FILE *f = fopen(path, "wb");
   if (f == NULL)
      return 1;
   size_t written = fwrite(data, 1, size, f);
   return fclose(f) != 0 || written != size;
}

int
main(int argc, char **argv)
{
   if (argc != 2 && !(argc == 3 && strcmp(argv[2], "bgra") == 0)) {
      fprintf(stderr, "usage: %s <evidence-directory> [bgra]\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];
   /* The optional bgra arm binds the same texel values through the
    * B8G8R8A8 memory order; the predicted target dword is unchanged,
    * and unrouted selects would instead replicate byte X (0xa0).
    */
   const bool texture_bgra = argc == 3;
   const VkFormat texture_format =
      texture_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;

   /* The oracle's expected interior is the texel through the reference
    * target's UNORM8 conversion; the reference shape carries geometry,
    * pitch, and lane order, and its color_bits take the texel.
    */
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const float texel_rgba[4] = {
      R3V_SAMPLED_TEXEL_R / 255.0f, R3V_SAMPLED_TEXEL_G / 255.0f,
      R3V_SAMPLED_TEXEL_B / 255.0f, R3V_SAMPLED_TEXEL_A / 255.0f,
   };
   memcpy(shape.color_bits, texel_rgba, sizeof(texel_rgba));
   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   const uint32_t predicted_dword =
      r300_tcl_bypass_triangle_pack_unorm8_dword(shape.lanes, texel_rgba);
   printf("[shape] sampled reference 64x64, %s uniform texel "
          "(%02x,%02x,%02x,%02x), predicted interior 0x%08x\n",
          texture_bgra ? "B8G8R8A8" : "R8G8B8A8", R3V_SAMPLED_TEXEL_R,
          R3V_SAMPLED_TEXEL_G, R3V_SAMPLED_TEXEL_B, R3V_SAMPLED_TEXEL_A,
          predicted_dword);
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
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       pdev_count != 1 || pdev == VK_NULL_HANDLE) {
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
   LOAD_DEVICE(vkCreateSampler);
   LOAD_DEVICE(vkDestroySampler);
   LOAD_DEVICE(vkCreateDescriptorSetLayout);
   LOAD_DEVICE(vkDestroyDescriptorSetLayout);
   LOAD_DEVICE(vkCreateDescriptorPool);
   LOAD_DEVICE(vkDestroyDescriptorPool);
   LOAD_DEVICE(vkAllocateDescriptorSets);
   LOAD_DEVICE(vkUpdateDescriptorSets);
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
   LOAD_DEVICE(vkCmdPipelineBarrier);
   LOAD_DEVICE(vkCmdBeginRenderPass);
   LOAD_DEVICE(vkCmdEndRenderPass);
   LOAD_DEVICE(vkCmdBindPipeline);
   LOAD_DEVICE(vkCmdBindDescriptorSets);
   LOAD_DEVICE(vkCmdBindVertexBuffers);
   LOAD_DEVICE(vkCmdDraw);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

#define CHECK(call)                                                        \
   do {                                                                    \
      VkResult check_result = (call);                                      \
      if (check_result != VK_SUCCESS) {                                    \
         fprintf(stderr, "%s: %d\n", #call, check_result);                 \
         return 1;                                                         \
      }                                                                    \
   } while (0)

   stage("texture");
   VkImage tex_image = VK_NULL_HANDLE;
   CHECK(vkCreateImage(
      device,
      &(VkImageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = texture_format,
         .extent = { 16, 16, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &tex_image));
   VkMemoryRequirements tex_reqs;
   vkGetImageMemoryRequirements(device, tex_image, &tex_reqs);
   VkDeviceMemory tex_memory = VK_NULL_HANDLE;
   CHECK(vkAllocateMemory(device,
                          &(VkMemoryAllocateInfo){
                             .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = tex_reqs.size,
                             .memoryTypeIndex = 0,
                          },
                          NULL, &tex_memory));
   CHECK(vkBindImageMemory(device, tex_image, tex_memory, 0));
   {
      void *map = NULL;
      CHECK(vkMapMemory(device, tex_memory, 0, VK_WHOLE_SIZE, 0, &map));
      uint8_t *texels = map;
      for (size_t t = 0; t < tex_reqs.size / 4; t++) {
         texels[4 * t + 0] =
            texture_bgra ? R3V_SAMPLED_TEXEL_B : R3V_SAMPLED_TEXEL_R;
         texels[4 * t + 1] = R3V_SAMPLED_TEXEL_G;
         texels[4 * t + 2] =
            texture_bgra ? R3V_SAMPLED_TEXEL_R : R3V_SAMPLED_TEXEL_B;
         texels[4 * t + 3] = R3V_SAMPLED_TEXEL_A;
      }
      vkUnmapMemory(device, tex_memory);
   }
   VkImageView tex_view = VK_NULL_HANDLE;
   CHECK(vkCreateImageView(
      device,
      &(VkImageViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = tex_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = texture_format,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .layerCount = 1 },
      },
      NULL, &tex_view));
   VkSampler sampler = VK_NULL_HANDLE;
   CHECK(vkCreateSampler(
      device,
      &(VkSamplerCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
         .magFilter = VK_FILTER_NEAREST,
         .minFilter = VK_FILTER_NEAREST,
         .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
         .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
         .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
      },
      NULL, &sampler));

   stage("descriptor set");
   VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
   CHECK(vkCreateDescriptorSetLayout(
      device,
      &(VkDescriptorSetLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings =
            &(VkDescriptorSetLayoutBinding){
               .binding = 0,
               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1,
               .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            },
      },
      NULL, &set_layout));
   VkDescriptorPool desc_pool = VK_NULL_HANDLE;
   CHECK(vkCreateDescriptorPool(
      device,
      &(VkDescriptorPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes =
            &(VkDescriptorPoolSize){
               .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
               .descriptorCount = 1,
            },
      },
      NULL, &desc_pool));
   VkDescriptorSet desc_set = VK_NULL_HANDLE;
   CHECK(vkAllocateDescriptorSets(
      device,
      &(VkDescriptorSetAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = desc_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &set_layout,
      },
      &desc_set));
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
               .sampler = sampler,
               .imageView = tex_view,
               .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            },
      },
      0, NULL);

   stage("target");
   VkImage target_image = VK_NULL_HANDLE;
   CHECK(vkCreateImage(
      device,
      &(VkImageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .extent = { shape.width, shape.height, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &target_image));
   VkDeviceMemory target_memory = VK_NULL_HANDLE;
   CHECK(vkAllocateMemory(device,
                          &(VkMemoryAllocateInfo){
                             .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = color_bytes,
                             .memoryTypeIndex = 0,
                          },
                          NULL, &target_memory));
   CHECK(vkBindImageMemory(device, target_image, target_memory, 0));
   {
      void *map = NULL;
      CHECK(vkMapMemory(device, target_memory, 0, VK_WHOLE_SIZE, 0, &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < color_bytes / 4; p++)
         pixels[p] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, target_memory);
   }
   VkImageView target_view = VK_NULL_HANDLE;
   CHECK(vkCreateImageView(
      device,
      &(VkImageViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = target_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .layerCount = 1 },
      },
      NULL, &target_view));

   stage("pipeline");
   VkRenderPass pass = VK_NULL_HANDLE;
   CHECK(vkCreateRenderPass(
      device,
      &(VkRenderPassCreateInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments =
            &(VkAttachmentDescription){
               .format = VK_FORMAT_B8G8R8A8_UNORM,
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
   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   CHECK(vkCreateFramebuffer(device,
                             &(VkFramebufferCreateInfo){
                                .sType =
                                   VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                .renderPass = pass,
                                .attachmentCount = 1,
                                .pAttachments = &target_view,
                                .width = shape.width,
                                .height = shape.height,
                                .layers = 1,
                             },
                             NULL, &framebuffer));
   VkPipelineLayout layout = VK_NULL_HANDLE;
   CHECK(vkCreatePipelineLayout(
      device,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &set_layout,
      },
      NULL, &layout));
   VkShaderModule vs = VK_NULL_HANDLE;
   VkShaderModule fs = VK_NULL_HANDLE;
   CHECK(vkCreateShaderModule(
      device,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(r3v_reference_vertex_varying_spirv),
         .pCode = r3v_reference_vertex_varying_spirv,
      },
      NULL, &vs));
   CHECK(vkCreateShaderModule(
      device,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(r3v_reference_fragment_sampled_spirv),
         .pCode = r3v_reference_fragment_sampled_spirv,
      },
      NULL, &fs));
   VkPipeline pipeline = VK_NULL_HANDLE;
   CHECK(vkCreateGraphicsPipelines(
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
                     .width = (float)shape.width,
                     .height = (float)shape.height,
                     .maxDepth = 1.0f,
                  },
               .scissorCount = 1,
               .pScissors =
                  &(VkRect2D){
                     .extent = { shape.width, shape.height },
                  },
            },
         .pRasterizationState =
            &(VkPipelineRasterizationStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
               .polygonMode = VK_POLYGON_MODE_FILL,
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
                     .colorWriteMask =
                        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                  },
            },
         .layout = layout,
         .renderPass = pass,
      },
      NULL, &pipeline));

   stage("vertex buffer");
   static const float ndc_triangle[12] = {
      -0.75f, -0.75f, 0.0f, 1.0f,
       0.75f, -0.75f, 0.0f, 1.0f,
       0.00f,  0.75f, 0.0f, 1.0f,
   };
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   CHECK(vkAllocateMemory(device,
                          &(VkMemoryAllocateInfo){
                             .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = 4096,
                             .memoryTypeIndex = 0,
                          },
                          NULL, &vertex_memory));
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   CHECK(vkCreateBuffer(device,
                        &(VkBufferCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = 256,
                           .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                        },
                        NULL, &vertex_buffer));
   CHECK(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
   {
      void *map = NULL;
      CHECK(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map));
      memcpy(map, ndc_triangle, sizeof(ndc_triangle));
      vkUnmapMemory(device, vertex_memory);
   }

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
         .image = target_image,
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
         .renderArea = { .extent = { shape.width, shape.height } },
         .clearValueCount = 1,
         .pClearValues =
            &(VkClearValue){
               .color = { .float32 = { 0.25f, 0.25f, 0.25f, 0.25f } },
            },
      },
      VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                           1, &desc_set, 0, NULL);
   vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &(VkDeviceSize){ 0 });
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdEndRenderPass(cmd);
   CHECK(vkEndCommandBuffer(cmd));

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: a live DRM_RADEON_CS reaches the command processor
    * here, and the bounded completion wait follows it.
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
   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *color_map = NULL;
   CHECK(vkMapMemory(device, target_memory, 0, VK_WHOLE_SIZE, 0,
                     &color_map));
   if (write_target(evidence_dir, color_map, color_bytes) != 0) {
      fprintf(stderr, "color target retention failed\n");
      return 1;
   }

   /* The load-op clear paints (0.25, 0.25, 0.25, 0.25) rather than the
    * sentinel, so the render-shape oracle's exterior expectation is the
    * clear dword; a shape whose color_bits carry the texel and whose
    * clear rides the pass gives the oracle both expectations through
    * the family's own conversion.
    */
   struct r300_triangle_oracle_verdict verdict;
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, color_map,
                                                color_bytes, &verdict);
   const uint32_t *pixels = color_map;
   const uint32_t cx = shape.width / 2, cy = (shape.height * 3) / 8;
   printf("[oracle] executed=%d interior=%d interior_samples=%u\n",
          verdict.executed, verdict.interior_pass, verdict.interior_samples);
   printf("[oracle] centroid (%u,%u)=0x%08x predicted 0x%08x corner "
          "(0,0)=0x%08x\n",
          cx, cy, pixels[cy * shape.pitch_pixels + cx], predicted_dword,
          pixels[0]);
   fflush(stdout);
   const bool centroid_pass =
      pixels[cy * shape.pitch_pixels + cx] == predicted_dword;

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyImageView(device, target_view, NULL);
   vkDestroyImage(device, target_image, NULL);
   vkFreeMemory(device, target_memory, NULL);
   vkDestroyDescriptorPool(device, desc_pool, NULL);
   vkDestroyDescriptorSetLayout(device, set_layout, NULL);
   vkDestroySampler(device, sampler, NULL);
   vkDestroyImageView(device, tex_view, NULL);
   vkDestroyImage(device, tex_image, NULL);
   vkFreeMemory(device, tex_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("[verdict] %s\n",
          centroid_pass ? "sampled cell rendered the texel as predicted"
                        : "prediction deviated; the deviation is the "
                          "finding");
   return centroid_pass ? 0 : 1;
}
