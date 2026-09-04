/*
 * SPDX-License-Identifier: MIT
 *
 * Attended sampled cell: drives the public sampling surface -- the
 * varying vertex module, the sampled fragment module, and the set-0
 * combined image sampler over a one-color texture -- to a live
 * DRM_RADEON_CS on RS485M silicon and reports the render-shape oracle's
 * verdict.  A uniform texture makes every sampled coordinate return
 * one texel, so the interior equals that texel whatever the
 * interpolated coordinates are; a wrong TX program cannot reproduce
 * it.  Runs only under the authorization and procedure in
 * docs/hardware/r3v-native-attended-render-shape-procedure.md; every
 * stage prints and flushes before it runs.
 */

#include "r3v_native_arming.h"
#include "r3v_native_watchdog_guard.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_sampled_arms.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <inttypes.h>
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

/* The rows arm's lower-half texel: distinct from the upper texel, the
 * clear, and the seeds in every byte lane.
 */
#define R3V_SAMPLED_LOWER_R 0x90
#define R3V_SAMPLED_LOWER_G 0x50
#define R3V_SAMPLED_LOWER_B 0x10
#define R3V_SAMPLED_LOWER_A 0xd0

/* The split-row arms' own model of the fragment source: TEX0's t is
 * 0.125 at both base vertices, which sit at window y 8, and 0.875 at
 * the apex at y 56 (r300_tcl_bypass_triangle_varying_vertices), so t
 * carries no x term and reads 0.125 + (y - 8) / 64.  The texture holds
 * the upper texel below its midpoint row and the lower texel from it,
 * so the predicted dword at an interior center follows t alone and the
 * verdict judges where each texel lands rather than that both appear.
 */
static uint32_t
split_expectation(void *data, uint32_t x, uint32_t y)
{
   const uint32_t *texel = data;
   (void)x;
   const float t = 0.125f + ((float)y + 0.5f - 8.0f) / 64.0f;
   return t < 0.5f ? texel[0] : texel[1];
}

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
   const struct r3v_sampled_arm *arm = &r3v_sampled_arms[0];
   const char *waiver_path = NULL;
   bool usage_error = argc < 2;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--waiver") == 0 && i + 1 < argc)
         waiver_path = argv[++i];
      else if (argv[i][0] != '-' && (arm = r3v_sampled_arm_find(argv[i])))
         continue;
      else
         usage_error = true;
   }
   if (usage_error || arm == NULL) {
      fprintf(stderr,
              "usage: %s <evidence-directory> "
              "[rgba|bgra|rows|wide|layer|row1] [--waiver <path>]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];
   /* The bgra arm binds the same texel values through the B8G8R8A8
    * memory order; the predicted target dword is unchanged, and
    * unrouted selects would instead replicate byte X (0xa0).  The
    * split-row arms halve the texture, so the two oracle pixels prove
    * address-dependent fetch: the varying TEX0 interpolation puts pixel
    * (32,24) at texture fraction 0.41 and pixel (32,44) at 0.72, each
    * clear of the half boundary at every admitted height.  The layer
    * arm selects the last of three layers with the other two holding
    * the lower texel, so a dropped TX_OFFSET_0 stride reads that texel;
    * the row1 arm is the height-one 1D shape.
    */
   const bool texture_bgra = arm->lanes == R300_TRIANGLE_LANES_B8G8R8A8;
   const bool texture_rows = arm->split_rows;
   const VkFormat texture_format = texture_bgra
                                      ? VK_FORMAT_B8G8R8A8_UNORM
                                      : VK_FORMAT_R8G8B8A8_UNORM;

   /* The oracle's expected interior is the texel through the reference
    * target's UNORM8 conversion, passed to the verdict as an admitted
    * value.  The reference shape carries geometry, pitch, and lane
    * order alone: this cell's fragment color arrives through the TX
    * unit, so it drives no R300_PFS_PARAM_0 constant and color_bits
    * name nothing it executes.
    */
   struct r300_triangle_render_shape shape;
   r300_tcl_bypass_triangle_render_shape_reference(&shape);
   const float texel_rgba[4] = {
      R3V_SAMPLED_TEXEL_R / 255.0f, R3V_SAMPLED_TEXEL_G / 255.0f,
      R3V_SAMPLED_TEXEL_B / 255.0f, R3V_SAMPLED_TEXEL_A / 255.0f,
   };
   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   const uint32_t predicted_dword =
      r300_tcl_bypass_triangle_pack_unorm8_dword(shape.lanes, texel_rgba);
   const float lower_rgba[4] = {
      R3V_SAMPLED_LOWER_R / 255.0f, R3V_SAMPLED_LOWER_G / 255.0f,
      R3V_SAMPLED_LOWER_B / 255.0f, R3V_SAMPLED_LOWER_A / 255.0f,
   };
   const uint32_t predicted_lower_dword =
      r300_tcl_bypass_triangle_pack_unorm8_dword(shape.lanes, lower_rgba);
   printf("[shape] arm %s, target 64x64, texture %s %ux%u %u layer(s) "
          "view layer %u, %s %s texel (%02x,%02x,%02x,%02x), predicted "
          "interior 0x%08x\n",
          arm->name, arm->one_dimensional ? "1D" : "2D", arm->width,
          arm->height, arm->array_layers, arm->view_layer,
          texture_bgra ? "B8G8R8A8" : "R8G8B8A8",
          texture_rows ? "split-rows upper" : "uniform", R3V_SAMPLED_TEXEL_R,
          R3V_SAMPLED_TEXEL_G, R3V_SAMPLED_TEXEL_B, R3V_SAMPLED_TEXEL_A,
          predicted_dword);
   if (arm->decoy_layers)
      printf("[shape] unselected layers hold (%02x,%02x,%02x,%02x), "
             "falsifier 0x%08x\n",
             R3V_SAMPLED_LOWER_R, R3V_SAMPLED_LOWER_G, R3V_SAMPLED_LOWER_B,
             R3V_SAMPLED_LOWER_A, predicted_lower_dword);
   if (texture_rows)
      printf("[shape] lower texel (%02x,%02x,%02x,%02x), predicted lower "
             "0x%08x at (32,44)\n",
             R3V_SAMPLED_LOWER_R, R3V_SAMPLED_LOWER_G, R3V_SAMPLED_LOWER_B,
             R3V_SAMPLED_LOWER_A, predicted_lower_dword);
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

   /* The qualified control submission the guarded interval is measured
    * on: this cell holds silicon evidence, so the interval it reports
    * bounds the bracket rather than the cell.  The arming digest comes
    * from the separate arming runner, so no cell digest is present here
    * to bind a waiver, and the bracket is the only admission.
    */
   struct r3v_native_watchdog_guard guard = {0};
   stage("watchdog");
   if (r3v_native_watchdog_guard_open(&guard, waiver_path, evidence_dir,
                                      NULL) != 0)
      return 2;

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
      fprintf(stderr, "enumerated chip is not the authorized RS485M\n");
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
   LOAD_DEVICE(vkGetImageSubresourceLayout);
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
         .imageType = arm->one_dimensional ? VK_IMAGE_TYPE_1D
                                           : VK_IMAGE_TYPE_2D,
         .format = texture_format,
         .extent = { arm->width, arm->height, 1 },
         .mipLevels = 1,
         .arrayLayers = arm->array_layers,
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
   VkSubresourceLayout tex_layout;
   vkGetImageSubresourceLayout(
      device, tex_image,
      &(VkImageSubresource){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT },
      &tex_layout);
   printf("[texture] rowPitch %" PRIu64 " arrayPitch %" PRIu64 " size %" PRIu64
          ", arm pitch %u texels, arm layer offset %u\n",
          (uint64_t)tex_layout.rowPitch, (uint64_t)tex_layout.arrayPitch,
          (uint64_t)tex_reqs.size, r3v_sampled_arm_row_pitch_texels(arm),
          r3v_sampled_arm_texture_offset(arm));
   fflush(stdout);
   /* The arming digest was computed from the arm's own geometry, so a
    * layout the driver derives differently would submit a stream the
    * report never named.
    */
   if (tex_layout.rowPitch != r3v_sampled_arm_row_pitch_texels(arm) * 4u ||
       (arm->array_layers > 1 &&
        tex_layout.arrayPitch != r3v_sampled_arm_layer_pitch_bytes(arm))) {
      fprintf(stderr, "driver layout disagrees with the arm geometry\n");
      return 1;
   }
   {
      void *map = NULL;
      CHECK(vkMapMemory(device, tex_memory, 0, VK_WHOLE_SIZE, 0, &map));
      uint8_t *texels = map;
      const uint64_t layer_pitch =
         arm->array_layers > 1 ? tex_layout.arrayPitch : 0;
      for (uint32_t layer = 0; layer < arm->array_layers; layer++) {
         for (uint32_t y = 0; y < arm->height; y++) {
            uint8_t *row =
               texels + layer * layer_pitch + (uint64_t)y * tex_layout.rowPitch;
            const bool lower =
               (texture_rows && y >= arm->height / 2) ||
               (arm->decoy_layers && layer != arm->view_layer);
            const uint8_t r =
               lower ? R3V_SAMPLED_LOWER_R : R3V_SAMPLED_TEXEL_R;
            const uint8_t g =
               lower ? R3V_SAMPLED_LOWER_G : R3V_SAMPLED_TEXEL_G;
            const uint8_t b =
               lower ? R3V_SAMPLED_LOWER_B : R3V_SAMPLED_TEXEL_B;
            const uint8_t a =
               lower ? R3V_SAMPLED_LOWER_A : R3V_SAMPLED_TEXEL_A;
            for (uint32_t x = 0; x < arm->width; x++) {
               row[4 * x + 0] = texture_bgra ? b : r;
               row[4 * x + 1] = g;
               row[4 * x + 2] = texture_bgra ? r : b;
               row[4 * x + 3] = a;
            }
         }
      }
      vkUnmapMemory(device, tex_memory);
   }
   VkImageView tex_view = VK_NULL_HANDLE;
   CHECK(vkCreateImageView(
      device,
      &(VkImageViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = tex_image,
         .viewType = arm->one_dimensional ? VK_IMAGE_VIEW_TYPE_1D
                                          : VK_IMAGE_VIEW_TYPE_2D,
         .format = texture_format,
         .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .levelCount = 1,
                               .baseArrayLayer = arm->view_layer,
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

   /* The counter comes to rest before the first target read, because a
    * fire after a good submission destroys the result and spends the
    * attempt.
    */
   if (r3v_native_watchdog_guard_close(&guard, result) != 0)
      return 1;

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

   /* The coverage verdict classifies every dword of the footprint: the
    * admitted interior values are the texels the fragment source can
    * deliver, and the exterior is the pass's own load-op clear, which
    * paints (0.25, 0.25, 0.25, 0.25) through the family's UNORM8
    * conversion.  Their placement inside the drawn region is the two
    * point checks below, so the verdict carries the region's shape and
    * the point checks carry the addressing.
    */
   const uint32_t admitted_interior[2] = { predicted_dword,
                                           predicted_lower_dword };
   const uint32_t clear_dword = r300_tcl_bypass_triangle_pack_unorm8_dword(
      shape.lanes, (const float[4]){ 0.25f, 0.25f, 0.25f, 0.25f });
   struct r300_triangle_coverage_verdict verdict;
   r300_tcl_bypass_triangle_coverage_oracle_predicted(
      &shape, admitted_interior, texture_rows ? 2u : 1u,
      texture_rows ? split_expectation : NULL, (void *)admitted_interior,
      clear_dword, color_map, color_bytes, &verdict);
   const uint32_t *pixels = color_map;
   const uint32_t cx = shape.width / 2, cy = (shape.height * 3) / 8;
   printf("[oracle] judged=%d coverage_exact=%d canary=%d interior=%u "
          "analytic=%u exterior=%u ambiguous=%u mismatch=%u\n",
          verdict.judged, verdict.coverage_exact, verdict.canary_pass,
          verdict.interior_pixels, verdict.analytic_pixels,
          verdict.exterior_pixels, verdict.ambiguous_pixels,
          verdict.mismatch_pixels);
   if (!verdict.judged)
      printf("[oracle] verdict refused: the shape or the retained "
             "footprint left the producer's domain, so the zero counters "
             "carry no claim about the render\n");
   printf("[oracle] centroid (%u,%u)=0x%08x predicted 0x%08x corner "
          "(0,0)=0x%08x\n",
          cx, cy, pixels[cy * shape.pitch_pixels + cx], predicted_dword,
          pixels[0]);
   fflush(stdout);
   bool oracle_pass = verdict.judged && verdict.coverage_exact &&
                      verdict.canary_pass &&
                      pixels[cy * shape.pitch_pixels + cx] == predicted_dword;
   if (texture_rows) {
      const uint32_t lower_read = pixels[44 * shape.pitch_pixels + 32];
      printf("[oracle] lower (32,44)=0x%08x predicted 0x%08x\n", lower_read,
             predicted_lower_dword);
      fflush(stdout);
      oracle_pass = oracle_pass && lower_read == predicted_lower_dword;
   }

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
          oracle_pass ? "sampled cell covered the analytic triangle with the predicted texel"
                        : "prediction deviated; the deviation is the "
                          "finding");
   return oracle_pass ? 0 : 1;
}
