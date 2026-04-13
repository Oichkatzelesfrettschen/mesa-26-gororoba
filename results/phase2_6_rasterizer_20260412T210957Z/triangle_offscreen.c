#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define WIDTH 128
#define HEIGHT 128

#define VK_CHECK(x) do { \
  VkResult _r = (x); \
  if (_r != VK_SUCCESS) { \
    fprintf(stderr, "VK error %d at %s:%d\\n", _r, __FILE__, __LINE__); \
    return 1; \
  } \
} while (0)

static uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties *mem_props,
                                 uint32_t bits,
                                 VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < mem_props->memoryTypeCount; ++i) {
    if ((bits & (1u << i)) &&
        (mem_props->memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return UINT32_MAX;
}

static int load_spv(const char *path, uint32_t **words, size_t *word_count) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "Failed to open %s\\n", path);
    return 0;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long size = ftell(f);
  if (size <= 0 || (size % 4) != 0) {
    fclose(f);
    return 0;
  }
  rewind(f);
  uint32_t *buf = (uint32_t *)malloc((size_t)size);
  if (!buf) {
    fclose(f);
    return 0;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    fclose(f);
    return 0;
  }
  fclose(f);
  *words = buf;
  *word_count = (size_t)size / 4;
  return 1;
}

static void write_ppm(const char *path, const uint8_t *rgba) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    return;
  }
  fprintf(f, "P6\\n%d %d\\n255\\n", WIDTH, HEIGHT);
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      const uint8_t *p = &rgba[(y * WIDTH + x) * 4];
      fwrite(p, 1, 3, f);
    }
  }
  fclose(f);
}

int main(int argc, char **argv) {
  const char *vert_path = argc > 1 ? argv[1] : "triangle.vert.spv";
  const char *frag_path = argc > 2 ? argv[2] : "triangle.frag.spv";
  const char *ppm_path = argc > 3 ? argv[3] : "triangle.ppm";

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkImage color_image = VK_NULL_HANDLE;
  VkDeviceMemory color_mem = VK_NULL_HANDLE;
  VkImageView color_view = VK_NULL_HANDLE;
  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readback_mem = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule vs = VK_NULL_HANDLE;
  VkShaderModule fs = VK_NULL_HANDLE;

  VkApplicationInfo app_info = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "terakan_triangle_probe",
    .apiVersion = VK_API_VERSION_1_0,
  };
  VkInstanceCreateInfo instance_info = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app_info,
  };
  VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));

  uint32_t physical_count = 0;
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, NULL));
  if (physical_count == 0) {
    fprintf(stderr, "No Vulkan physical devices\\n");
    return 1;
  }
  VkPhysicalDevice devices[8];
  if (physical_count > 8) physical_count = 8;
  VK_CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, devices));
  physical = devices[0];

  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical, &props);
  fprintf(stderr, "Using GPU: %s\\n", props.deviceName);

  uint32_t qf_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qf_count, NULL);
  VkQueueFamilyProperties qf[16];
  if (qf_count > 16) qf_count = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qf_count, qf);
  uint32_t gfx_qf = UINT32_MAX;
  for (uint32_t i = 0; i < qf_count; ++i) {
    if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      gfx_qf = i;
      break;
    }
  }
  if (gfx_qf == UINT32_MAX) {
    fprintf(stderr, "No graphics queue family\\n");
    return 1;
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = gfx_qf,
    .queueCount = 1,
    .pQueuePriorities = &prio,
  };
  VkDeviceCreateInfo dci = {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &qci,
  };
  VK_CHECK(vkCreateDevice(physical, &dci, NULL, &device));
  vkGetDeviceQueue(device, gfx_qf, 0, &queue);

  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);

  VkImageCreateInfo image_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .extent = { WIDTH, HEIGHT, 1 },
    .mipLevels = 1,
    .arrayLayers = 1,
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VK_CHECK(vkCreateImage(device, &image_info, NULL, &color_image));

  VkMemoryRequirements image_mem_req;
  vkGetImageMemoryRequirements(device, color_image, &image_mem_req);
  uint32_t image_mem_type = find_memory_type(
      &mem_props, image_mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (image_mem_type == UINT32_MAX) {
    image_mem_type = find_memory_type(&mem_props, image_mem_req.memoryTypeBits, 0);
  }
  if (image_mem_type == UINT32_MAX) {
    fprintf(stderr, "No image memory type\\n");
    return 1;
  }
  VkMemoryAllocateInfo image_alloc = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = image_mem_req.size,
    .memoryTypeIndex = image_mem_type,
  };
  VK_CHECK(vkAllocateMemory(device, &image_alloc, NULL, &color_mem));
  VK_CHECK(vkBindImageMemory(device, color_image, color_mem, 0));

  VkImageViewCreateInfo view_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = color_image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
  };
  VK_CHECK(vkCreateImageView(device, &view_info, NULL, &color_view));

  VkBufferCreateInfo buf_info = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = WIDTH * HEIGHT * 4,
    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VK_CHECK(vkCreateBuffer(device, &buf_info, NULL, &readback));

  VkMemoryRequirements buf_mem_req;
  vkGetBufferMemoryRequirements(device, readback, &buf_mem_req);
  uint32_t buf_mem_type = find_memory_type(
      &mem_props, buf_mem_req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (buf_mem_type == UINT32_MAX) {
    fprintf(stderr, "No host-visible buffer memory type\\n");
    return 1;
  }
  VkMemoryAllocateInfo buf_alloc = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = buf_mem_req.size,
    .memoryTypeIndex = buf_mem_type,
  };
  VK_CHECK(vkAllocateMemory(device, &buf_alloc, NULL, &readback_mem));
  VK_CHECK(vkBindBufferMemory(device, readback, readback_mem, 0));

  VkAttachmentDescription attachment = {
    .format = VK_FORMAT_R8G8B8A8_UNORM,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };
  VkAttachmentReference color_ref = {
    .attachment = 0,
    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
  };
  VkSubpassDescription subpass = {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments = &color_ref,
  };
  VkRenderPassCreateInfo rp_info = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &attachment,
    .subpassCount = 1,
    .pSubpasses = &subpass,
  };
  VK_CHECK(vkCreateRenderPass(device, &rp_info, NULL, &render_pass));

  VkFramebufferCreateInfo fb_info = {
    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
    .renderPass = render_pass,
    .attachmentCount = 1,
    .pAttachments = &color_view,
    .width = WIDTH,
    .height = HEIGHT,
    .layers = 1,
  };
  VK_CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &framebuffer));

  uint32_t *vs_words = NULL;
  uint32_t *fs_words = NULL;
  size_t vs_word_count = 0, fs_word_count = 0;
  if (!load_spv(vert_path, &vs_words, &vs_word_count) ||
      !load_spv(frag_path, &fs_words, &fs_word_count)) {
    fprintf(stderr, "Failed to load SPIR-V shaders\\n");
    return 1;
  }

  VkShaderModuleCreateInfo smci = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = vs_word_count * 4,
    .pCode = vs_words,
  };
  VK_CHECK(vkCreateShaderModule(device, &smci, NULL, &vs));
  smci.codeSize = fs_word_count * 4;
  smci.pCode = fs_words;
  VK_CHECK(vkCreateShaderModule(device, &smci, NULL, &fs));
  free(vs_words);
  free(fs_words);

  VkPipelineShaderStageCreateInfo stages[2] = {
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_VERTEX_BIT,
      .module = vs,
      .pName = "main",
    },
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
      .module = fs,
      .pName = "main",
    }
  };

  VkPipelineVertexInputStateCreateInfo vi = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo ia = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkViewport viewport = {
    .x = 0.0f,
    .y = 0.0f,
    .width = (float)WIDTH,
    .height = (float)HEIGHT,
    .minDepth = 0.0f,
    .maxDepth = 1.0f,
  };
  VkRect2D scissor = {
    .offset = {0, 0},
    .extent = {WIDTH, HEIGHT},
  };
  VkPipelineViewportStateCreateInfo vp = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .pViewports = &viewport,
    .scissorCount = 1,
    .pScissors = &scissor,
  };
  VkPipelineRasterizationStateCreateInfo rs = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_NONE,
    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
    .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo ms = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState cb_att = {
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo cb = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &cb_att,
  };

  VkPipelineLayoutCreateInfo pl_info = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
  };
  VK_CHECK(vkCreatePipelineLayout(device, &pl_info, NULL, &pipeline_layout));

  VkGraphicsPipelineCreateInfo gp = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .stageCount = 2,
    .pStages = stages,
    .pVertexInputState = &vi,
    .pInputAssemblyState = &ia,
    .pViewportState = &vp,
    .pRasterizationState = &rs,
    .pMultisampleState = &ms,
    .pColorBlendState = &cb,
    .layout = pipeline_layout,
    .renderPass = render_pass,
    .subpass = 0,
  };
  VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL, &pipeline));

  VkCommandPoolCreateInfo cp_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = gfx_qf,
  };
  VK_CHECK(vkCreateCommandPool(device, &cp_info, NULL, &cmd_pool));

  VkCommandBufferAllocateInfo cba = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = cmd_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1,
  };
  VK_CHECK(vkAllocateCommandBuffers(device, &cba, &cmd));

  VkCommandBufferBeginInfo cb_begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
  };
  VK_CHECK(vkBeginCommandBuffer(cmd, &cb_begin));

  VkImageMemoryBarrier to_color = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = 0,
    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = color_image,
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
  };
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, 0, NULL, 0, NULL, 1, &to_color);

  VkClearValue clear = { .color = {{0.0f, 0.0f, 0.0f, 1.0f}} };
  VkRenderPassBeginInfo rp_begin = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = render_pass,
    .framebuffer = framebuffer,
    .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
    .clearValueCount = 1,
    .pClearValues = &clear,
  };
  vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);

  VkImageMemoryBarrier to_copy = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = color_image,
    .subresourceRange = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .baseMipLevel = 0,
      .levelCount = 1,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
  };
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, NULL, 0, NULL, 1, &to_copy);

  VkBufferImageCopy copy = {
    .bufferOffset = 0,
    .bufferRowLength = 0,
    .bufferImageHeight = 0,
    .imageSubresource = {
      .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
      .mipLevel = 0,
      .baseArrayLayer = 0,
      .layerCount = 1,
    },
    .imageOffset = {0, 0, 0},
    .imageExtent = {WIDTH, HEIGHT, 1},
  };
  vkCmdCopyImageToBuffer(cmd, color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback, 1, &copy);

  VkBufferMemoryBarrier to_host = {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = readback,
    .offset = 0,
    .size = VK_WHOLE_SIZE,
  };
  vkCmdPipelineBarrier(cmd,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0, 0, NULL, 1, &to_host, 0, NULL);

  VK_CHECK(vkEndCommandBuffer(cmd));

  VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &cmd,
  };
  VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(queue));

  uint8_t *pixels = NULL;
  VK_CHECK(vkMapMemory(device, readback_mem, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));

  uint32_t non_black = 0;
  uint32_t red_dominant = 0;
  for (int y = 0; y < HEIGHT; ++y) {
    for (int x = 0; x < WIDTH; ++x) {
      const uint8_t *p = &pixels[(y * WIDTH + x) * 4];
      if (p[0] || p[1] || p[2]) {
        ++non_black;
      }
      if (p[0] > 200 && p[1] < 50 && p[2] < 50) {
        ++red_dominant;
      }
    }
  }
  const uint8_t *center = &pixels[((HEIGHT / 2) * WIDTH + (WIDTH / 2)) * 4];

  write_ppm(ppm_path, pixels);

  printf("RESULT non_black=%u red_dominant=%u center_rgba=%u,%u,%u,%u\\n",
         non_black, red_dominant, center[0], center[1], center[2], center[3]);

  vkUnmapMemory(device, readback_mem);

  vkDestroyPipeline(device, pipeline, NULL);
  vkDestroyPipelineLayout(device, pipeline_layout, NULL);
  vkDestroyShaderModule(device, fs, NULL);
  vkDestroyShaderModule(device, vs, NULL);
  vkDestroyFramebuffer(device, framebuffer, NULL);
  vkDestroyRenderPass(device, render_pass, NULL);
  vkDestroyBuffer(device, readback, NULL);
  vkFreeMemory(device, readback_mem, NULL);
  vkDestroyImageView(device, color_view, NULL);
  vkDestroyImage(device, color_image, NULL);
  vkFreeMemory(device, color_mem, NULL);
  vkDestroyCommandPool(device, cmd_pool, NULL);
  vkDestroyDevice(device, NULL);
  vkDestroyInstance(instance, NULL);

  return (non_black > 0 && red_dominant > 0) ? 0 : 2;
}
