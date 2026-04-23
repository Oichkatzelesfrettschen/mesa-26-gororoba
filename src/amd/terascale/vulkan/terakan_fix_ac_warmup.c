/*
 * Copyright © 2024-2026 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "terakan_fix_ac_warmup.h"

#include "terakan_command_buffer.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "terakan_pipeline_compute.h"
#include "terakan_queue.h"

#include "util/list.h"
#include "vk_alloc.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Pre-compiled SPIR-V for:
 *
 *   #version 460
 *   layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
 *   layout(set = 0, binding = 0, r32ui) coherent uniform uimage2D warmup_img;
 *   void main() {
 *       imageStore(warmup_img, ivec2(0, 0), uvec4(0xDEADBEEFu, 0u, 0u, 1u));
 *   }
 *
 * Compiled on x130e with glslangValidator -V /tmp/warmup.comp -o /tmp/warmup.spv
 * and extracted via python3 -c "import struct,sys; d=open('/tmp/warmup.spv','rb').read();
 * [print(hex(x)+'u,', end=' ') for x in struct.unpack('<'+str(len(d)//4)+'I',d)]"
 * 2026-04-22.  147 dwords.
 */
static const uint32_t fix_ac_warmup_spv[] = {
   0x07230203u, 0x00010000u, 0x0008000bu, 0x00000016u, 0x00000000u,
   0x00020011u, 0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u,
   0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u,
   0x00000001u, 0x0005000fu, 0x00000005u, 0x00000004u, 0x6e69616du,
   0x00000000u, 0x00060010u, 0x00000004u, 0x00000011u, 0x00000001u,
   0x00000001u, 0x00000001u, 0x00030003u, 0x00000002u, 0x000001ccu,
   0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00050005u,
   0x00000009u, 0x6d726177u, 0x695f7075u, 0x0000676du, 0x00030047u,
   0x00000009u, 0x00000017u, 0x00040047u, 0x00000009u, 0x00000021u,
   0x00000000u, 0x00040047u, 0x00000009u, 0x00000022u, 0x00000000u,
   0x00040047u, 0x00000015u, 0x0000000bu, 0x00000019u, 0x00020013u,
   0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u,
   0x00000006u, 0x00000020u, 0x00000000u, 0x00090019u, 0x00000007u,
   0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
   0x00000002u, 0x00000021u, 0x00040020u, 0x00000008u, 0x00000000u,
   0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000000u,
   0x00040015u, 0x0000000bu, 0x00000020u, 0x00000001u, 0x00040017u,
   0x0000000cu, 0x0000000bu, 0x00000002u, 0x0004002bu, 0x0000000bu,
   0x0000000du, 0x00000000u, 0x0005002cu, 0x0000000cu, 0x0000000eu,
   0x0000000du, 0x0000000du, 0x00040017u, 0x0000000fu, 0x00000006u,
   0x00000004u, 0x0004002bu, 0x00000006u, 0x00000010u, 0xdeadbeefu,
   0x0004002bu, 0x00000006u, 0x00000011u, 0x00000000u, 0x0004002bu,
   0x00000006u, 0x00000012u, 0x00000001u, 0x0007002cu, 0x0000000fu,
   0x00000013u, 0x00000010u, 0x00000011u, 0x00000011u, 0x00000012u,
   0x00040017u, 0x00000014u, 0x00000006u, 0x00000003u, 0x0006002cu,
   0x00000014u, 0x00000015u, 0x00000012u, 0x00000012u, 0x00000012u,
   0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
   0x000200f8u, 0x00000005u, 0x0004003du, 0x00000007u, 0x0000000au,
   0x00000009u, 0x00040063u, 0x0000000au, 0x0000000eu, 0x00000013u,
   0x000100fdu, 0x00010038u,
};

VkResult
terakan_fix_ac_warmup_create(struct terakan_device *const device,
                             struct terakan_fix_ac_warmup **const out)
{
   VkDevice const vk_dev = terakan_device_to_handle(device);
   VkResult result;

   struct terakan_fix_ac_warmup *const warmup =
      vk_zalloc(&device->vk.alloc, sizeof(*warmup), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!warmup)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Descriptor set layout: binding 0 = r32ui storage image, compute stage. */
   {
      VkDescriptorSetLayoutBinding const binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };
      VkDescriptorSetLayoutCreateInfo const dsl_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings = &binding,
      };
      result = terakan_CreateDescriptorSetLayout(vk_dev, &dsl_info, NULL,
                                                 &warmup->descriptor_set_layout);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Pipeline layout. */
   {
      VkPipelineLayoutCreateInfo const pl_info = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1,
         .pSetLayouts = &warmup->descriptor_set_layout,
      };
      result = terakan_CreatePipelineLayout(vk_dev, &pl_info, NULL,
                                            &warmup->pipeline_layout);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Compute pipeline from pre-compiled SPIR-V.
    * Pass cache=NULL to terakan_pipeline_compute_create so the warmup
    * shader never touches device->vk.mem_cache.  This prevents
    * any potential interference with subsequent user pipeline lookups
    * caused by inserting warmup ISA into the shared device cache.
    */
   {
      VkShaderModule shader_module = VK_NULL_HANDLE;
      VkShaderModuleCreateInfo const sm_info = {
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(fix_ac_warmup_spv),
         .pCode = fix_ac_warmup_spv,
      };
      result = device->vk.dispatch_table.CreateShaderModule(vk_dev, &sm_info, NULL,
                                                             &shader_module);
      if (result != VK_SUCCESS)
         goto fail;

      VkComputePipelineCreateInfo const cs_info = {
         .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
         .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader_module,
            .pName = "main",
         },
         .layout = warmup->pipeline_layout,
      };
      result = terakan_pipeline_compute_create(device, NULL, &cs_info,
                                               NULL, &warmup->pipeline);
      device->vk.dispatch_table.DestroyShaderModule(vk_dev, shader_module, NULL);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* 1x1 r32ui device-local image as the warmup write target. */
   {
      VkImageCreateInfo const img_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R32_UINT,
         .extent = { .width = 1, .height = 1, .depth = 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_STORAGE_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      result = terakan_CreateImage(vk_dev, &img_info, NULL, &warmup->image);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Allocate device-local backing memory. */
   {
      VkMemoryRequirements mem_req;
      device->vk.dispatch_table.GetImageMemoryRequirements(vk_dev, warmup->image, &mem_req);

      struct terakan_physical_device *const phys_dev =
         terakan_device_physical_device(device);
      VkPhysicalDeviceMemoryProperties const *const mem_props =
         &phys_dev->memory_properties;

      uint32_t mem_type_idx = UINT32_MAX;
      for (uint32_t i = 0; i < mem_props->memoryTypeCount; i++) {
         if (!(mem_req.memoryTypeBits & (1u << i)))
            continue;
         if (mem_props->memoryTypes[i].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            mem_type_idx = i;
            break;
         }
      }
      if (mem_type_idx == UINT32_MAX) {
         /* Fallback: any compatible type (should not happen on Evergreen). */
         assert(mem_req.memoryTypeBits != 0);
         for (uint32_t i = 0; i < mem_props->memoryTypeCount; i++) {
            if (mem_req.memoryTypeBits & (1u << i)) {
               mem_type_idx = i;
               break;
            }
         }
      }

      VkMemoryAllocateInfo const alloc_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = mem_req.size,
         .memoryTypeIndex = mem_type_idx,
      };
      result = terakan_AllocateMemory(vk_dev, &alloc_info, NULL, &warmup->memory);
      if (result != VK_SUCCESS)
         goto fail;

      result = device->vk.dispatch_table.BindImageMemory(vk_dev, warmup->image,
                                                          warmup->memory, 0);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Image view. */
   {
      VkImageViewCreateInfo const iv_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = warmup->image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_R32_UINT,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
      };
      result = terakan_CreateImageView(vk_dev, &iv_info, NULL, &warmup->image_view);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Descriptor pool (1 storage image). */
   {
      VkDescriptorPoolSize const pool_size = {
         .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
      };
      VkDescriptorPoolCreateInfo const pool_info = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes = &pool_size,
      };
      result = terakan_CreateDescriptorPool(vk_dev, &pool_info, NULL,
                                            &warmup->descriptor_pool);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Allocate and populate the descriptor set. */
   {
      VkDescriptorSetAllocateInfo const ds_alloc = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = warmup->descriptor_pool,
         .descriptorSetCount = 1,
         .pSetLayouts = &warmup->descriptor_set_layout,
      };
      result = terakan_AllocateDescriptorSets(vk_dev, &ds_alloc,
                                              &warmup->descriptor_set);
      if (result != VK_SUCCESS)
         goto fail;

      VkDescriptorImageInfo const ds_img = {
         .imageView = warmup->image_view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
      };
      VkWriteDescriptorSet const ds_write = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = warmup->descriptor_set,
         .dstBinding = 0,
         .dstArrayElement = 0,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &ds_img,
      };
      terakan_UpdateDescriptorSets(vk_dev, 1, &ds_write, 0, NULL);
   }

   /* Command pool (queue family 0 = the graphics+compute queue on Evergreen). */
   {
      VkCommandPoolCreateInfo const cp_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      };
      result = terakan_CreateCommandPool(vk_dev, &cp_info, NULL,
                                         &warmup->command_pool);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Allocate the pre-recorded warmup command buffer. */
   {
      VkCommandBufferAllocateInfo const cb_alloc = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = warmup->command_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      };
      result = device->vk.dispatch_table.AllocateCommandBuffers(vk_dev, &cb_alloc,
                                                                 &warmup->command_buffer);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Record: layout transition -> bind pipeline -> bind ds -> 2x dispatch. */
   {
      VkCommandBufferBeginInfo const begin_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
      };
      result = terakan_BeginCommandBuffer(warmup->command_buffer, &begin_info);
      if (result != VK_SUCCESS)
         goto fail;

      VkImageMemoryBarrier const img_barrier = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = 0,
         .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = warmup->image,
         .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
         },
      };
      device->vk.dispatch_table.CmdPipelineBarrier(warmup->command_buffer,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &img_barrier);

      terakan_CmdBindPipeline(warmup->command_buffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE, warmup->pipeline);
      terakan_CmdBindDescriptorSets(warmup->command_buffer,
                                    VK_PIPELINE_BIND_POINT_COMPUTE,
                                    warmup->pipeline_layout, 0, 1,
                                    &warmup->descriptor_set, 0, NULL);

      /*
       * Two dispatches: first warms the CB-exporter out of COLD, second
       * confirms the WARM state.  Handles the intermediate case where a
       * single dispatch is insufficient (tranche 19 DEEP_COLD observation).
       */
      terakan_CmdDispatch(warmup->command_buffer, 1, 1, 1);
      terakan_CmdDispatch(warmup->command_buffer, 1, 1, 1);

      result = terakan_EndCommandBuffer(warmup->command_buffer);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *out = warmup;
   return VK_SUCCESS;

fail:
   terakan_fix_ac_warmup_destroy(device, warmup);
   return result;
}

void
terakan_fix_ac_warmup_destroy(struct terakan_device *const device,
                               struct terakan_fix_ac_warmup *const warmup)
{
   if (!warmup)
      return;

   VkDevice const vk_dev = terakan_device_to_handle(device);

   /* DestroyCommandPool frees all command buffers allocated from the pool. */
   if (warmup->command_pool != VK_NULL_HANDLE)
      terakan_DestroyCommandPool(vk_dev, warmup->command_pool, NULL);

   /* DestroyDescriptorPool frees all descriptor sets allocated from the pool. */
   if (warmup->descriptor_pool != VK_NULL_HANDLE)
      terakan_DestroyDescriptorPool(vk_dev, warmup->descriptor_pool, NULL);

   if (warmup->image_view != VK_NULL_HANDLE)
      terakan_DestroyImageView(vk_dev, warmup->image_view, NULL);

   if (warmup->memory != VK_NULL_HANDLE)
      terakan_FreeMemory(vk_dev, warmup->memory, NULL);

   if (warmup->image != VK_NULL_HANDLE)
      terakan_DestroyImage(vk_dev, warmup->image, NULL);

   if (warmup->pipeline != VK_NULL_HANDLE)
      terakan_DestroyPipeline(vk_dev, warmup->pipeline, NULL);

   if (warmup->pipeline_layout != VK_NULL_HANDLE)
      device->vk.dispatch_table.DestroyPipelineLayout(vk_dev, warmup->pipeline_layout, NULL);

   if (warmup->descriptor_set_layout != VK_NULL_HANDLE)
      device->vk.dispatch_table.DestroyDescriptorSetLayout(vk_dev,
                                                            warmup->descriptor_set_layout,
                                                            NULL);

   vk_free(&device->vk.alloc, warmup);
}

void
terakan_fix_ac_warmup_submit_prelude(struct terakan_device *const device,
                                     struct terakan_fix_ac_warmup *const warmup,
                                     struct terakan_queue *const queue)
{
   if (warmup->command_buffer == VK_NULL_HANDLE)
      return;

   struct terakan_command_buffer const *const cb =
      terakan_command_buffer_from_handle(warmup->command_buffer);

   /* Submit the warmup IB 3 times: DEEP_COLD->COLD (pass 1), COLD->WARM (pass 2),
    * WARM confirmed (pass 3).  Single dispatch is insufficient on cold silicon.
    */
   for (uint32_t warmup_pass = 0; warmup_pass < 3; ++warmup_pass) {
      list_for_each_entry(struct terakan_command_buffer_indirect_buffer const, ib,
                          &cb->indirect_buffers, link) {
         VkResult const r = device->winsys_fn->queue->submit(
            queue->submission_context,
            ib->bo_reference_count, ib->bo_references,
            ib->indirect_buffer_size_dwords, ib->indirect_buffer,
            ib->relocation_count, ib->relocations);
         if (r != VK_SUCCESS) {
            fprintf(stderr,
                    "terakan/fix-ac: warmup pass %u IB submit failed (VkResult=%d); silicon may remain in cold state\\n",
                    warmup_pass, (int)r);
            return;
         }
      }
   }
}
