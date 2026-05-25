/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

struct probe_context {
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   VkQueue queue;
   uint32_t queue_family;
};

static void
probe_result(const char *case_name, const char *status, const char *detail)
{
   printf("{\"case\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"}\n",
          case_name, status, detail ? detail : "");
}

static int
probe_fail(const char *case_name, VkResult result)
{
   char detail[96];
   snprintf(detail, sizeof(detail), "VkResult=%d", result);
   probe_result(case_name, "fail", detail);
   return 1;
}

static uint32_t
find_memory_type(VkPhysicalDevice physical_device,
                 uint32_t type_bits,
                 VkMemoryPropertyFlags required_flags)
{
   VkPhysicalDeviceMemoryProperties props;
   vkGetPhysicalDeviceMemoryProperties(physical_device, &props);

   for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
      if (!(type_bits & (1u << i)))
         continue;

      if ((props.memoryTypes[i].propertyFlags & required_flags) == required_flags)
         return i;
   }

   return UINT32_MAX;
}

static int
init_probe_context(struct probe_context *ctx)
{
   const VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "r300vk_4096_image_probe",
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app_info,
   };

   VkResult result = vkCreateInstance(&instance_info, NULL, &ctx->instance);
   if (result != VK_SUCCESS)
      return probe_fail("create_instance", result);
   probe_result("create_instance", "pass", "instance created");

   uint32_t physical_device_count = 0;
   result = vkEnumeratePhysicalDevices(ctx->instance, &physical_device_count, NULL);
   if (result != VK_SUCCESS || physical_device_count == 0)
      return probe_fail("enumerate_physical_devices", result);

   VkPhysicalDevice *physical_devices =
      calloc(physical_device_count, sizeof(*physical_devices));
   if (!physical_devices) {
      probe_result("enumerate_physical_devices", "fail", "calloc failed");
      return 1;
   }

   result = vkEnumeratePhysicalDevices(ctx->instance, &physical_device_count,
                                       physical_devices);
   if (result != VK_SUCCESS) {
      free(physical_devices);
      return probe_fail("enumerate_physical_devices", result);
   }
   ctx->physical_device = physical_devices[0];
   free(physical_devices);

   VkPhysicalDeviceProperties device_props;
   vkGetPhysicalDeviceProperties(ctx->physical_device, &device_props);
   char device_detail[384];
   snprintf(device_detail, sizeof(device_detail),
            "device=%.255s maxImage2D=%u framebuffer=%ux%u",
            device_props.deviceName,
            device_props.limits.maxImageDimension2D,
            device_props.limits.maxFramebufferWidth,
            device_props.limits.maxFramebufferHeight);
   probe_result("device_properties", "pass", device_detail);

   uint32_t queue_family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device,
                                            &queue_family_count, NULL);
   VkQueueFamilyProperties *queue_props =
      calloc(queue_family_count, sizeof(*queue_props));
   if (!queue_props) {
      probe_result("queue_family", "fail", "calloc failed");
      return 1;
   }

   vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device,
                                            &queue_family_count, queue_props);
   ctx->queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_family_count; i++) {
      if (queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         ctx->queue_family = i;
         break;
      }
   }
   free(queue_props);

   if (ctx->queue_family == UINT32_MAX) {
      probe_result("queue_family", "fail", "no graphics queue");
      return 1;
   }
   probe_result("queue_family", "pass", "graphics queue found");

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = ctx->queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkPhysicalDeviceFeatures features = {
      .robustBufferAccess = VK_TRUE,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .pEnabledFeatures = &features,
   };

   result = vkCreateDevice(ctx->physical_device, &device_info, NULL, &ctx->device);
   if (result != VK_SUCCESS)
      return probe_fail("create_device", result);
   vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);
   probe_result("create_device", "pass", "device created");
   return 0;
}

static void
finish_probe_context(struct probe_context *ctx)
{
   if (ctx->device)
      vkDestroyDevice(ctx->device, NULL);
   if (ctx->instance)
      vkDestroyInstance(ctx->instance, NULL);
}

static int
check_format_contract(struct probe_context *ctx)
{
   VkImageFormatProperties props;
   VkResult result =
      vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device,
                                               VK_FORMAT_B8G8R8A8_UNORM,
                                               VK_IMAGE_TYPE_2D,
                                               VK_IMAGE_TILING_OPTIMAL,
                                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                               0, &props);
   if (result != VK_SUCCESS)
      return probe_fail("format_4096_contract", result);

   if (props.maxExtent.width < 4096 || props.maxExtent.height < 4096 ||
       props.maxMipLevels != 1 || props.maxArrayLayers != 1 ||
       !(props.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
      char detail[192];
      snprintf(detail, sizeof(detail),
               "extent=%ux%u mips=%u layers=%u samples=0x%x",
               props.maxExtent.width, props.maxExtent.height,
               props.maxMipLevels, props.maxArrayLayers, props.sampleCounts);
      probe_result("format_4096_contract", "fail", detail);
      return 1;
   }

   probe_result("format_4096_contract", "pass",
                "4096 extent, one mip, one layer, 1x sample supported");
   return 0;
}

static int
allocate_and_bind_image(struct probe_context *ctx,
                        VkImage image,
                        VkDeviceMemory *memory_out)
{
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(ctx->device, image, &req);
   const uint32_t memory_type =
      find_memory_type(ctx->physical_device, req.memoryTypeBits, 0);
   if (memory_type == UINT32_MAX) {
      probe_result("bind_image_memory", "fail", "no compatible memory type");
      return 1;
   }

   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type,
   };
   VkResult result =
      vkAllocateMemory(ctx->device, &alloc_info, NULL, memory_out);
   if (result != VK_SUCCESS)
      return probe_fail("allocate_image_memory", result);

   result = vkBindImageMemory(ctx->device, image, *memory_out, 0);
   if (result != VK_SUCCESS)
      return probe_fail("bind_image_memory", result);

   probe_result("bind_image_memory", "pass", "image memory bound");
   return 0;
}

static int
allocate_and_bind_buffer(struct probe_context *ctx,
                         VkBuffer buffer,
                         VkDeviceMemory *memory_out)
{
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(ctx->device, buffer, &req);
   const uint32_t memory_type =
      find_memory_type(ctx->physical_device, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (memory_type == UINT32_MAX) {
      probe_result("bind_buffer_memory", "fail", "no host-visible memory type");
      return 1;
   }

   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memory_type,
   };
   VkResult result =
      vkAllocateMemory(ctx->device, &alloc_info, NULL, memory_out);
   if (result != VK_SUCCESS)
      return probe_fail("allocate_buffer_memory", result);

   result = vkBindBufferMemory(ctx->device, buffer, *memory_out, 0);
   if (result != VK_SUCCESS)
      return probe_fail("bind_buffer_memory", result);

   probe_result("bind_buffer_memory", "pass", "buffer memory bound");
   return 0;
}

static int
verify_readback(struct probe_context *ctx,
                VkDeviceMemory buffer_memory,
                VkDeviceSize buffer_size)
{
   void *mapped = NULL;
   VkResult result =
      vkMapMemory(ctx->device, buffer_memory, 0, buffer_size, 0, &mapped);
   if (result != VK_SUCCESS)
      return probe_fail("map_readback_buffer", result);

   const uint8_t expected[4] = { 255, 0, 255, 255 };
   const uint8_t *bytes = mapped;
   bool ok = true;
   VkDeviceSize first_mismatch = 0;
   for (VkDeviceSize i = 0; i + 3 < buffer_size; i += 4) {
      if (memcmp(bytes + i, expected, sizeof(expected)) != 0) {
         ok = false;
         first_mismatch = i;
         break;
      }
   }

   if (!ok) {
      char detail[192];
      snprintf(detail, sizeof(detail),
               "offset=%" PRIu64 " actual=%u,%u,%u,%u expected=%u,%u,%u,%u",
               (uint64_t)first_mismatch,
               bytes[first_mismatch + 0], bytes[first_mismatch + 1],
               bytes[first_mismatch + 2], bytes[first_mismatch + 3],
               expected[0], expected[1], expected[2], expected[3]);
      vkUnmapMemory(ctx->device, buffer_memory);
      probe_result("verify_tile_readback", "fail", detail);
      return 1;
   }

   vkUnmapMemory(ctx->device, buffer_memory);
   probe_result("verify_tile_readback", "pass",
                "tile-boundary readback matches clear color");
   return 0;
}

static int
run_4096_clear_copy_probe(struct probe_context *ctx)
{
   VkImage image = VK_NULL_HANDLE;
   VkImageView image_view = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
   VkRenderPass render_pass = VK_NULL_HANDLE;
   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   int status = 1;

   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 4096, 4096, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkResult result = vkCreateImage(ctx->device, &image_info, NULL, &image);
   if (result != VK_SUCCESS)
      goto out_fail_create_image;
   probe_result("create_4096_image", "pass", "4096x4096 image created");

   if (allocate_and_bind_image(ctx, image, &image_memory))
      goto out;

   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = image,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = 0,
         .levelCount = 1,
         .baseArrayLayer = 0,
         .layerCount = 1,
      },
   };
   result = vkCreateImageView(ctx->device, &view_info, NULL, &image_view);
   if (result != VK_SUCCESS)
      goto out_fail_image_view;

   const VkAttachmentDescription attachment = {
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   };
   const VkAttachmentReference color_ref = {
      .attachment = 0,
      .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
   };
   const VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
   };
   const VkRenderPassCreateInfo render_pass_info = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &attachment,
      .subpassCount = 1,
      .pSubpasses = &subpass,
   };
   result = vkCreateRenderPass(ctx->device, &render_pass_info, NULL,
                               &render_pass);
   if (result != VK_SUCCESS)
      goto out_fail_render_pass;

   const VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = render_pass,
      .attachmentCount = 1,
      .pAttachments = &image_view,
      .width = 4096,
      .height = 4096,
      .layers = 1,
   };
   result = vkCreateFramebuffer(ctx->device, &framebuffer_info, NULL,
                                &framebuffer);
   if (result != VK_SUCCESS)
      goto out_fail_framebuffer;

   const VkDeviceSize copy_region_size = 4u * 4u * 4u;
   const VkDeviceSize buffer_size = copy_region_size * 3u;
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = vkCreateBuffer(ctx->device, &buffer_info, NULL, &buffer);
   if (result != VK_SUCCESS)
      goto out_fail_buffer;
   if (allocate_and_bind_buffer(ctx, buffer, &buffer_memory))
      goto out;

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->queue_family,
   };
   result = vkCreateCommandPool(ctx->device, &pool_info, NULL, &command_pool);
   if (result != VK_SUCCESS)
      goto out_fail_command_pool;

   const VkCommandBufferAllocateInfo command_buffer_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   result = vkAllocateCommandBuffers(ctx->device, &command_buffer_info,
                                     &command_buffer);
   if (result != VK_SUCCESS)
      goto out_fail_command_buffer;

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   result = vkBeginCommandBuffer(command_buffer, &begin_info);
   if (result != VK_SUCCESS)
      goto out_fail_begin_command_buffer;

   const VkClearValue clear_value = {
      .color = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } },
   };
   const VkRenderPassBeginInfo render_begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = render_pass,
      .framebuffer = framebuffer,
      .renderArea = {
         .offset = { 0, 0 },
         .extent = { 4096, 4096 },
      },
      .clearValueCount = 1,
      .pClearValues = &clear_value,
   };
   vkCmdBeginRenderPass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(command_buffer);

   const VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = 0,
         .levelCount = 1,
         .baseArrayLayer = 0,
         .layerCount = 1,
      },
   };
   vkCmdPipelineBarrier(command_buffer,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, NULL, 0, NULL, 1, &barrier);

   const VkBufferImageCopy copies[3] = {
      {
         .bufferOffset = 0,
         .bufferRowLength = 4,
         .bufferImageHeight = 4,
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageOffset = { 0, 0, 0 },
         .imageExtent = { 4, 4, 1 },
      },
      {
         .bufferOffset = copy_region_size,
         .bufferRowLength = 4,
         .bufferImageHeight = 4,
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageOffset = { 2558, 2046, 0 },
         .imageExtent = { 4, 4, 1 },
      },
      {
         .bufferOffset = copy_region_size * 2,
         .bufferRowLength = 4,
         .bufferImageHeight = 4,
         .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
         .imageOffset = { 4092, 4092, 0 },
         .imageExtent = { 4, 4, 1 },
      },
   };
   vkCmdCopyImageToBuffer(command_buffer, image,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                          3, copies);

   result = vkEndCommandBuffer(command_buffer);
   if (result != VK_SUCCESS)
      goto out_fail_end_command_buffer;

   const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   result = vkCreateFence(ctx->device, &fence_info, NULL, &fence);
   if (result != VK_SUCCESS)
      goto out_fail_fence;

   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &command_buffer,
   };
   result = vkQueueSubmit(ctx->queue, 1, &submit_info, fence);
   if (result != VK_SUCCESS)
      goto out_fail_queue_submit;

   result = vkWaitForFences(ctx->device, 1, &fence, VK_TRUE, UINT64_MAX);
   if (result != VK_SUCCESS)
      goto out_fail_wait_fence;
   probe_result("submit_clear_copy", "pass", "queue submit completed");

   status = verify_readback(ctx, buffer_memory, buffer_size);
   goto out;

out_fail_create_image:
   probe_fail("create_4096_image", result);
   goto out;
out_fail_image_view:
   probe_fail("create_image_view", result);
   goto out;
out_fail_render_pass:
   probe_fail("create_render_pass", result);
   goto out;
out_fail_framebuffer:
   probe_fail("create_framebuffer", result);
   goto out;
out_fail_buffer:
   probe_fail("create_readback_buffer", result);
   goto out;
out_fail_command_pool:
   probe_fail("create_command_pool", result);
   goto out;
out_fail_command_buffer:
   probe_fail("allocate_command_buffer", result);
   goto out;
out_fail_begin_command_buffer:
   probe_fail("begin_command_buffer", result);
   goto out;
out_fail_end_command_buffer:
   probe_fail("end_command_buffer", result);
   goto out;
out_fail_fence:
   probe_fail("create_fence", result);
   goto out;
out_fail_queue_submit:
   probe_fail("queue_submit", result);
   goto out;
out_fail_wait_fence:
   probe_fail("wait_fence", result);
   goto out;

out:
   if (fence)
      vkDestroyFence(ctx->device, fence, NULL);
   if (command_pool)
      vkDestroyCommandPool(ctx->device, command_pool, NULL);
   if (buffer)
      vkDestroyBuffer(ctx->device, buffer, NULL);
   if (buffer_memory)
      vkFreeMemory(ctx->device, buffer_memory, NULL);
   if (framebuffer)
      vkDestroyFramebuffer(ctx->device, framebuffer, NULL);
   if (render_pass)
      vkDestroyRenderPass(ctx->device, render_pass, NULL);
   if (image_view)
      vkDestroyImageView(ctx->device, image_view, NULL);
   if (image)
      vkDestroyImage(ctx->device, image, NULL);
   if (image_memory)
      vkFreeMemory(ctx->device, image_memory, NULL);
   return status;
}

int
main(void)
{
   struct probe_context ctx = {0};
   int status = init_probe_context(&ctx);
   if (!status)
      status |= check_format_contract(&ctx);
   if (!status)
      status |= run_4096_clear_copy_probe(&ctx);

   finish_probe_context(&ctx);
   return status ? EXIT_FAILURE : EXIT_SUCCESS;
}
