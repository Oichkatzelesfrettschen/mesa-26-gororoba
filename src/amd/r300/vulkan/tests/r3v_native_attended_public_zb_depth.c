/*
 * SPDX-License-Identifier: MIT
 *
 * Public Vulkan RS485M tiled Z24/S8 qualification.  The host packs an
 * independently defined logical image, records ordinary depth/stencil
 * attachment draws through Vulkan entrypoints, and retains complete color and
 * depth allocations before classifying logical and physical results.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_vertex_spirv.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/radeon_legacy_2d_reg.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "amd/r300/common/r300_zb_depth_layout.h"
#include "amd/r300/common/r300_zb_depth_surface.h"
#include "util/mesa-blake3.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

#define VERTEX_BYTES 4096u
#define COLOR_BYTES 16640u
#define DEPTH_BYTES 28672u
#define DEPTH_BASE 2048u
#define DEPTH_STORAGE_END 22528u
#define PIXELS 4096u
#define STORAGE_WORDS 5120u
#define DEPTH_LOW 0x200000u
#define DEPTH_HIGH 0x600000u
#define DEPTH_HALF 0x400000u
#define DEPTH_GUARD 0xa3u

enum run_mode {
   MODE_READ,
   MODE_WRITE,
   MODE_NEAR_FAR,
   MODE_FAR_NEAR,
};

enum outcome {
   OUTCOME_PASS,
   OUTCOME_FAILED,
   OUTCOME_CONTAINMENT_FAILURE,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_PASS] = "PASS",
   [OUTCOME_FAILED] = "FAILED",
   [OUTCOME_CONTAINMENT_FAILURE] = "CONTAINMENT_FAILURE",
   [OUTCOME_SUBMISSION_REFUSED] = "SUBMISSION_REFUSED",
   [OUTCOME_COMPLETION_FAILURE] = "COMPLETION_FAILURE",
   [OUTCOME_RETENTION_FAILURE] = "RETENTION_FAILURE",
};

static const char *const mode_names[] = {
   [MODE_READ] = "read",
   [MODE_WRITE] = "write",
   [MODE_NEAR_FAR] = "nearfar",
   [MODE_FAR_NEAR] = "farnear",
};

static int
finish(enum outcome outcome)
{
   printf("verdict: %s\n", outcome_names[outcome]);
   fflush(stdout);
   return outcome == OUTCOME_PASS ? 0 : 1;
}

static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

static bool
same_directory(const char *first, const char *second)
{
   if (strcmp(first, second) == 0)
      return true;
   char resolved_first[PATH_MAX];
   char resolved_second[PATH_MAX];
   return realpath(first, resolved_first) != NULL &&
          realpath(second, resolved_second) != NULL &&
          strcmp(resolved_first, resolved_second) == 0;
}

static bool
create_result_directory(const char *evidence_dir, const char *name,
                        char result_dir[PATH_MAX])
{
   const int path_length = snprintf(result_dir, PATH_MAX, "%s/%s",
                                    evidence_dir, name);
   return path_length > 0 && path_length < PATH_MAX &&
          mkdir(result_dir, 0700) == 0;
}

static bool
parse_pattern(const char *text, uint32_t *pattern)
{
   char *end = NULL;
   if (text == NULL || text[0] == '\0')
      return false;
   unsigned long value = strtoul(text, &end, 10);
   if (*end != '\0' || value > 19u)
      return false;
   *pattern = (uint32_t)value;
   return true;
}

static bool
parse_mode(const char *text, enum run_mode *mode)
{
   for (unsigned candidate = 0; candidate < 4; candidate++) {
      if (strcmp(text, mode_names[candidate]) == 0) {
         *mode = (enum run_mode)candidate;
         return true;
      }
   }
   return false;
}

static bool
logical_high(uint32_t pattern, uint32_t x, uint32_t y)
{
   switch (pattern) {
   case 0:
      return ((x / 4u) + (y / 2u)) & 1u;
   case 1: {
      const bool macro_parity = ((x / 32u) ^ (y / 16u)) & 1u;
      const bool asymmetric_quadrant =
         (x >= 32u && y < 32u) || (x < 16u && y >= 48u);
      return macro_parity ^ asymmetric_quadrant;
   }
   case 2:
      return (((x * 0x45d9f3bu) ^ (y * 0x119de1f3u) ^
               (x * y * 0x27d4eb2du) ^ (x << 19) ^ (y << 11)) >>
              ((x + 3u * y) & 15u)) & 1u;
   case 19:
      return (x < 11u) || (y >= 9u && y < 23u && x >= 17u) ||
             (x >= 45u && y >= 37u) || (x + 2u * y == 79u);
   default:
      return false;
   }
}

static bool
storage_high(uint32_t pattern, uint32_t storage_slot,
             const struct r300_zb_depth_address_coordinate *coordinate)
{
   if (pattern <= 2u || pattern == 19u)
      return logical_high(pattern, coordinate->x, coordinate->y);
   if (pattern <= 15u)
      return (storage_slot >> (pattern - 3u)) & 1u;
   if (pattern == 16u)
      return false;
   if (pattern == 17u)
      return true;
   return storage_slot & 1u;
}

static uint8_t
stencil_for_coordinate(uint32_t x, uint32_t y)
{
   return (uint8_t)((x * 37u + y * 101u + (x ^ (y << 1))) & 0xffu);
}

static uint32_t
load_word(const uint8_t *bytes, uint32_t offset)
{
   uint32_t word;
   memcpy(&word, bytes + offset, sizeof(word));
   return word;
}

static void
store_word(uint8_t *bytes, uint32_t offset, uint32_t word)
{
   memcpy(bytes + offset, &word, sizeof(word));
}

static void
blake3_hex(const void *data, size_t size,
           char text[BLAKE3_OUT_LEN * 2u + 1u])
{
   struct mesa_blake3 context;
   blake3_hash digest;
   _mesa_blake3_init(&context);
   _mesa_blake3_update(&context, data, size);
   _mesa_blake3_final(&context, digest);
   _mesa_blake3_format(text, digest);
}

static bool
fill_depth_image(uint8_t bytes[DEPTH_BYTES], uint32_t pattern,
                 enum run_mode mode)
{
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   memset(bytes, DEPTH_GUARD, DEPTH_BYTES);
   for (uint32_t slot = 0; slot < STORAGE_WORDS; slot++) {
      const uint32_t offset = DEPTH_BASE + slot * 4u;
      struct r300_zb_depth_address_coordinate coordinate;
      if (surface->address_resolver->coordinate(surface, DEPTH_BASE,
                                                 DEPTH_BYTES, offset,
                                                 &coordinate) != 0)
         return false;
      const bool overlap = mode == MODE_NEAR_FAR || mode == MODE_FAR_NEAR;
      const bool high = overlap || storage_high(pattern, slot, &coordinate);
      const uint32_t packed = ((high ? DEPTH_HIGH : DEPTH_LOW) << 8) |
                              stencil_for_coordinate(coordinate.x,
                                                     coordinate.y);
      store_word(bytes, offset, packed);
   }
   return true;
}

static bool
fill_depth_image_codes(uint8_t bytes[DEPTH_BYTES], uint32_t pattern,
                       uint32_t low_code, uint32_t high_code)
{
   if (!fill_depth_image(bytes, pattern, MODE_READ))
      return false;
   for (uint32_t slot = 0; slot < STORAGE_WORDS; slot++) {
      const uint32_t offset = DEPTH_BASE + slot * 4u;
      const uint32_t word = load_word(bytes, offset);
      const uint32_t depth = word >> 8;
      const uint32_t replacement = depth == DEPTH_HIGH ? high_code : low_code;
      store_word(bytes, offset, (replacement << 8) | (word & 0xffu));
   }
   return true;
}

static bool
expected_pixel_high(uint32_t pattern, enum run_mode mode, uint32_t x,
                    uint32_t y)
{
   if (mode == MODE_NEAR_FAR || mode == MODE_FAR_NEAR)
      return true;
   if (pattern <= 2u || pattern == 19u)
      return logical_high(pattern, x, y);
   uint64_t offset;
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   if (r300_zb_depth_address_checked(surface, DEPTH_BASE, DEPTH_BYTES, x, y,
                                     &offset) != 0)
      return false;
   return storage_high(pattern, (uint32_t)((offset - DEPTH_BASE) / 4u),
                       &(struct r300_zb_depth_address_coordinate){x, y,
                          R300_ZB_DEPTH_ADDRESS_LOGICAL});
}

static bool
classify_color(const uint32_t *color, uint32_t pattern, enum run_mode mode,
               uint8_t observed_bits[PIXELS / 8u], uint32_t *colored_out,
               uint32_t *mismatches_out, bool *containment_out)
{
   uint32_t colored = 0;
   uint32_t mismatches = 0;
   bool containment = false;
   memset(observed_bits, 0, PIXELS / 8u);
   for (uint32_t pixel = 0; pixel < COLOR_BYTES / 4u; pixel++) {
      const bool inside = pixel < PIXELS;
      const bool drawn =
         color[pixel] == R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
      const bool sentinel = color[pixel] == R300_TRIANGLE_COLOR_SENTINEL;
      if (inside) {
         if (drawn) {
            observed_bits[pixel / 8u] |= 1u << (pixel & 7u);
            colored++;
         }
         const uint32_t x = pixel % 64u;
         const uint32_t y = pixel / 64u;
         if (!sentinel && !drawn)
            mismatches++;
         else if (drawn != expected_pixel_high(pattern, mode, x, y))
            mismatches++;
      } else if (!sentinel) {
         containment = true;
      }
   }
   *colored_out = colored;
   *mismatches_out = mismatches;
   *containment_out = containment;
   return mismatches == 0 && !containment;
}

static bool
classify_depth(const uint8_t before[DEPTH_BYTES],
               const uint8_t after[DEPTH_BYTES], uint32_t pattern,
               enum run_mode mode, uint32_t *mismatches_out,
               bool *containment_out)
{
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   uint32_t mismatches = 0;
   bool containment = false;
   for (uint32_t offset = 0; offset < DEPTH_BYTES; offset++) {
      if ((offset < DEPTH_BASE || offset >= DEPTH_STORAGE_END) &&
          before[offset] != after[offset])
         containment = true;
   }
   if (mode == MODE_READ) {
      for (uint32_t offset = 0; offset < DEPTH_BYTES; offset++)
         mismatches += before[offset] != after[offset];
   } else {
      for (uint32_t slot = 0; slot < STORAGE_WORDS; slot++) {
         const uint32_t offset = DEPTH_BASE + slot * 4u;
         struct r300_zb_depth_address_coordinate coordinate;
         if (surface->address_resolver->coordinate(surface, DEPTH_BASE,
                                                    DEPTH_BYTES, offset,
                                                    &coordinate) != 0)
            return false;
         uint32_t expected = load_word(before, offset);
         if (coordinate.region == R300_ZB_DEPTH_ADDRESS_LOGICAL &&
             expected_pixel_high(pattern, mode, coordinate.x, coordinate.y)) {
            const uint32_t expected_depth = DEPTH_HALF;
            expected = (expected_depth << 8) | (expected & 0xffu);
         }
         mismatches += load_word(after, offset) != expected;
      }
   }
   *mismatches_out = mismatches;
   *containment_out = containment;
   return mismatches == 0 && !containment;
}

static void
make_expected_color(uint32_t color[COLOR_BYTES / 4u], uint32_t pattern,
                    enum run_mode mode)
{
   for (uint32_t pixel = 0; pixel < COLOR_BYTES / 4u; pixel++)
      color[pixel] = R300_TRIANGLE_COLOR_SENTINEL;
   for (uint32_t y = 0; y < 64u; y++)
      for (uint32_t x = 0; x < 64u; x++)
         if (expected_pixel_high(pattern, mode, x, y))
            color[y * 64u + x] =
               R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
}

static bool
make_expected_depth(const uint8_t before[DEPTH_BYTES], uint32_t pattern,
                    enum run_mode mode, uint8_t after[DEPTH_BYTES])
{
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   memcpy(after, before, DEPTH_BYTES);
   if (mode == MODE_READ)
      return true;
   for (uint32_t slot = 0; slot < STORAGE_WORDS; slot++) {
      const uint32_t offset = DEPTH_BASE + slot * 4u;
      struct r300_zb_depth_address_coordinate coordinate;
      if (surface->address_resolver->coordinate(surface, DEPTH_BASE,
                                                 DEPTH_BYTES, offset,
                                                 &coordinate) != 0)
         return false;
      if (coordinate.region == R300_ZB_DEPTH_ADDRESS_LOGICAL &&
          expected_pixel_high(pattern, mode, coordinate.x, coordinate.y))
         store_word(after, offset,
                    (DEPTH_HALF << 8) | (load_word(before, offset) & 0xffu));
   }
   return true;
}

static bool
selftest_rejects_depth_mutation(const uint8_t before[DEPTH_BYTES],
                                const uint8_t expected[DEPTH_BYTES],
                                uint32_t pattern, enum run_mode mode,
                                uint32_t offset)
{
   uint8_t mutated[DEPTH_BYTES];
   memcpy(mutated, expected, DEPTH_BYTES);
   mutated[offset] ^= 1u;
   uint32_t mismatches;
   bool containment;
   return !classify_depth(before, mutated, pattern, mode, &mismatches,
                          &containment);
}

static int
run_selftest(void)
{
   const struct r300_zb_depth_surface *surface =
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical;
   uint64_t selected_offset;
   if (r300_zb_depth_address_checked(surface, DEPTH_BASE, DEPTH_BYTES, 7u, 11u,
                                     &selected_offset) != 0)
      return 1;
   uint32_t padding_offset = 0;
   for (uint32_t slot = 0; slot < STORAGE_WORDS; slot++) {
      struct r300_zb_depth_address_coordinate coordinate;
      const uint32_t offset = DEPTH_BASE + slot * 4u;
      if (surface->address_resolver->coordinate(surface, DEPTH_BASE,
                                                 DEPTH_BYTES, offset,
                                                 &coordinate) != 0)
         return 1;
      if (coordinate.region == R300_ZB_DEPTH_ADDRESS_PADDING) {
         padding_offset = offset;
         break;
      }
   }
   if (padding_offset == 0)
      return 1;

   for (uint32_t pattern = 0; pattern <= 19u; pattern++) {
      for (unsigned mode_index = 0; mode_index < 4u; mode_index++) {
         const enum run_mode mode = (enum run_mode)mode_index;
         uint8_t before[DEPTH_BYTES];
         uint8_t after[DEPTH_BYTES];
         uint32_t color[COLOR_BYTES / 4u];
         if (!fill_depth_image(before, pattern, mode) ||
             !make_expected_depth(before, pattern, mode, after))
            return 1;
         make_expected_color(color, pattern, mode);
         uint8_t bits[PIXELS / 8u];
         uint32_t colored, color_mismatches, depth_mismatches;
         bool color_containment, depth_containment;
         if (!classify_color(color, pattern, mode, bits, &colored,
                             &color_mismatches, &color_containment) ||
             !classify_depth(before, after, pattern, mode, &depth_mismatches,
                             &depth_containment))
            return 1;

         color[0] = color[0] == R300_TRIANGLE_COLOR_SENTINEL
                       ? R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM
                       : R300_TRIANGLE_COLOR_SENTINEL;
         if (classify_color(color, pattern, mode, bits, &colored,
                            &color_mismatches, &color_containment))
            return 1;
         make_expected_color(color, pattern, mode);
         color[PIXELS] ^= 1u;
         if (classify_color(color, pattern, mode, bits, &colored,
                            &color_mismatches, &color_containment))
            return 1;

         const uint32_t mutations[] = {
            (uint32_t)selected_offset, (uint32_t)selected_offset + 3u,
            padding_offset,            0u,
            DEPTH_BASE - 1u,           DEPTH_STORAGE_END,
            DEPTH_BYTES - 1u,
         };
         for (unsigned mutation = 0;
              mutation < sizeof(mutations) / sizeof(mutations[0]); mutation++)
            if (!selftest_rejects_depth_mutation(before, after, pattern, mode,
                                                 mutations[mutation]))
               return 1;
      }
   }
   printf("selftest: PASS (20 patterns x 4 modes; color, depth, stencil, "
          "padding, prefix, suffix, and tail mutations rejected)\n");
   return 0;
}

struct public_api {
   PFN_vkAllocateMemory allocate_memory;
   PFN_vkFreeMemory free_memory;
   PFN_vkMapMemory map_memory;
   PFN_vkUnmapMemory unmap_memory;
   PFN_vkCreateBuffer create_buffer;
   PFN_vkDestroyBuffer destroy_buffer;
   PFN_vkBindBufferMemory bind_buffer_memory;
   PFN_vkCreateImage create_image;
   PFN_vkDestroyImage destroy_image;
   PFN_vkGetImageMemoryRequirements get_image_memory_requirements;
   PFN_vkBindImageMemory bind_image_memory;
   PFN_vkCreateImageView create_image_view;
   PFN_vkDestroyImageView destroy_image_view;
   PFN_vkCreateRenderPass create_render_pass;
   PFN_vkDestroyRenderPass destroy_render_pass;
   PFN_vkCreateFramebuffer create_framebuffer;
   PFN_vkDestroyFramebuffer destroy_framebuffer;
   PFN_vkCreateShaderModule create_shader_module;
   PFN_vkDestroyShaderModule destroy_shader_module;
   PFN_vkCreatePipelineLayout create_pipeline_layout;
   PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
   PFN_vkCreateGraphicsPipelines create_graphics_pipelines;
   PFN_vkDestroyPipeline destroy_pipeline;
   PFN_vkCreateCommandPool create_command_pool;
   PFN_vkDestroyCommandPool destroy_command_pool;
   PFN_vkAllocateCommandBuffers allocate_command_buffers;
   PFN_vkBeginCommandBuffer begin_command_buffer;
   PFN_vkEndCommandBuffer end_command_buffer;
   PFN_vkCmdBeginRenderPass cmd_begin_render_pass;
   PFN_vkCmdEndRenderPass cmd_end_render_pass;
   PFN_vkCmdBindPipeline cmd_bind_pipeline;
   PFN_vkCmdBindVertexBuffers cmd_bind_vertex_buffers;
   PFN_vkCmdDraw cmd_draw;
   PFN_vkGetDeviceQueue get_device_queue;
   PFN_vkQueueSubmit queue_submit;
   PFN_vkDestroyDevice destroy_device;
};

struct public_depth_target {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView view;
   VkFramebuffer framebuffer;
};

struct public_context {
   VkInstance instance;
   VkDevice device;
   VkQueue queue;
   struct public_api api;
   VkImage color_image;
   VkDeviceMemory color_memory;
   VkImageView color_view;
   VkDeviceMemory vertex_memory;
   VkBuffer vertex_buffer;
   VkRenderPass render_pass;
   VkPipelineLayout pipeline_layout;
   VkPipeline read_pipeline;
   VkPipeline write_pipeline;
   VkCommandPool command_pool;
   struct public_depth_target depth[2];
   uint32_t depth_count;
};

static bool
load_public_api(PFN_vkGetDeviceProcAddr get_device_proc_addr,
                struct public_context *context)
{
#define LOAD(field, name)                                                       \
   context->api.field =                                                        \
      (PFN_##name)get_device_proc_addr(context->device, #name)
   LOAD(allocate_memory, vkAllocateMemory);
   LOAD(free_memory, vkFreeMemory);
   LOAD(map_memory, vkMapMemory);
   LOAD(unmap_memory, vkUnmapMemory);
   LOAD(create_buffer, vkCreateBuffer);
   LOAD(destroy_buffer, vkDestroyBuffer);
   LOAD(bind_buffer_memory, vkBindBufferMemory);
   LOAD(create_image, vkCreateImage);
   LOAD(destroy_image, vkDestroyImage);
   LOAD(get_image_memory_requirements, vkGetImageMemoryRequirements);
   LOAD(bind_image_memory, vkBindImageMemory);
   LOAD(create_image_view, vkCreateImageView);
   LOAD(destroy_image_view, vkDestroyImageView);
   LOAD(create_render_pass, vkCreateRenderPass);
   LOAD(destroy_render_pass, vkDestroyRenderPass);
   LOAD(create_framebuffer, vkCreateFramebuffer);
   LOAD(destroy_framebuffer, vkDestroyFramebuffer);
   LOAD(create_shader_module, vkCreateShaderModule);
   LOAD(destroy_shader_module, vkDestroyShaderModule);
   LOAD(create_pipeline_layout, vkCreatePipelineLayout);
   LOAD(destroy_pipeline_layout, vkDestroyPipelineLayout);
   LOAD(create_graphics_pipelines, vkCreateGraphicsPipelines);
   LOAD(destroy_pipeline, vkDestroyPipeline);
   LOAD(create_command_pool, vkCreateCommandPool);
   LOAD(destroy_command_pool, vkDestroyCommandPool);
   LOAD(allocate_command_buffers, vkAllocateCommandBuffers);
   LOAD(begin_command_buffer, vkBeginCommandBuffer);
   LOAD(end_command_buffer, vkEndCommandBuffer);
   LOAD(cmd_begin_render_pass, vkCmdBeginRenderPass);
   LOAD(cmd_end_render_pass, vkCmdEndRenderPass);
   LOAD(cmd_bind_pipeline, vkCmdBindPipeline);
   LOAD(cmd_bind_vertex_buffers, vkCmdBindVertexBuffers);
   LOAD(cmd_draw, vkCmdDraw);
   LOAD(get_device_queue, vkGetDeviceQueue);
   LOAD(queue_submit, vkQueueSubmit);
   LOAD(destroy_device, vkDestroyDevice);
#undef LOAD
   return context->api.allocate_memory && context->api.free_memory &&
          context->api.map_memory && context->api.unmap_memory &&
          context->api.create_buffer && context->api.destroy_buffer &&
          context->api.bind_buffer_memory && context->api.create_image &&
          context->api.destroy_image &&
          context->api.get_image_memory_requirements &&
          context->api.bind_image_memory && context->api.create_image_view &&
          context->api.destroy_image_view && context->api.create_render_pass &&
          context->api.destroy_render_pass &&
          context->api.create_framebuffer &&
          context->api.destroy_framebuffer &&
          context->api.create_shader_module &&
          context->api.destroy_shader_module &&
          context->api.create_pipeline_layout &&
          context->api.destroy_pipeline_layout &&
          context->api.create_graphics_pipelines &&
          context->api.destroy_pipeline && context->api.create_command_pool &&
          context->api.destroy_command_pool &&
          context->api.allocate_command_buffers &&
          context->api.begin_command_buffer &&
          context->api.end_command_buffer &&
          context->api.cmd_begin_render_pass &&
          context->api.cmd_end_render_pass &&
          context->api.cmd_bind_pipeline &&
          context->api.cmd_bind_vertex_buffers && context->api.cmd_draw &&
          context->api.get_device_queue && context->api.queue_submit &&
          context->api.destroy_device;
}

static void
fill_public_vertices(enum run_mode mode, float vertices[48])
{
   float first_depth = 0.25f;
   float second_depth = 0.25f;
   if (mode == MODE_NEAR_FAR)
      second_depth = 0.5f;
   else if (mode == MODE_FAR_NEAR)
      first_depth = 0.5f;
   static const float positions[24] = {
      -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
      -1.0f, -1.0f, 1.0f,  1.0f, -1.0f, 1.0f,
   };
   for (uint32_t pass = 0; pass < 2; pass++) {
      const float depth = pass == 0 ? first_depth : second_depth;
      for (uint32_t vertex = 0; vertex < 6; vertex++) {
         const uint32_t destination = pass * 24u + vertex * 4u;
         vertices[destination] = positions[vertex * 2u];
         vertices[destination + 1u] = positions[vertex * 2u + 1u];
         vertices[destination + 2u] = depth;
         vertices[destination + 3u] = 1.0f;
      }
   }
}

static VkResult
create_depth_pipeline(struct public_context *context, bool depth_write,
                      VkPipeline *pipeline)
{
   VkShaderModule vertex_shader = VK_NULL_HANDLE;
   VkShaderModule fragment_shader = VK_NULL_HANDLE;
   VkResult result = context->api.create_shader_module(
      context->device,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(r3v_reference_vertex_spirv),
         .pCode = r3v_reference_vertex_spirv,
      }, NULL, &vertex_shader);
   if (result == VK_SUCCESS)
      result = context->api.create_shader_module(
         context->device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(r3v_reference_fragment_spirv),
            .pCode = r3v_reference_fragment_spirv,
         }, NULL, &fragment_shader);
   if (result != VK_SUCCESS)
      goto out;

   const VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertex_shader,
        .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment_shader,
        .pName = "main" },
   };
   const VkVertexInputBindingDescription binding = {
      .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
   };
   const VkVertexInputAttributeDescription attribute = {
      .location = 0, .binding = 0,
      .format = VK_FORMAT_R32G32B32A32_SFLOAT,
   };
   const VkPipelineColorBlendAttachmentState blend = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                        VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT,
   };
   const VkGraphicsPipelineCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
         .vertexBindingDescriptionCount = 1,
         .pVertexBindingDescriptions = &binding,
         .vertexAttributeDescriptionCount = 1,
         .pVertexAttributeDescriptions = &attribute,
      },
      .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
         .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
      },
      .pViewportState = &(VkPipelineViewportStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
         .viewportCount = 1,
         .pViewports = &(VkViewport){ .width = 64.0f, .height = 64.0f,
                                      .maxDepth = 1.0f },
         .scissorCount = 1,
         .pScissors = &(VkRect2D){ .extent = { 64, 64 } },
      },
      .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
         .polygonMode = VK_POLYGON_MODE_FILL,
         .cullMode = VK_CULL_MODE_NONE,
         .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
         .lineWidth = 1.0f,
      },
      .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
         .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
      },
      .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
         .depthTestEnable = VK_TRUE,
         .depthWriteEnable = depth_write,
         .depthCompareOp = VK_COMPARE_OP_LESS,
      },
      .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
         .attachmentCount = 1, .pAttachments = &blend,
      },
      .layout = context->pipeline_layout,
      .renderPass = context->render_pass,
   };
   result = context->api.create_graphics_pipelines(
      context->device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);

out:
   if (fragment_shader)
      context->api.destroy_shader_module(context->device, fragment_shader,
                                         NULL);
   if (vertex_shader)
      context->api.destroy_shader_module(context->device, vertex_shader,
                                         NULL);
   return result;
}

static bool
create_public_context(struct public_context *context, uint32_t depth_count)
{
   memset(context, 0, sizeof(*context));
   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
      vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (!create_instance || create_instance(&(VkInstanceCreateInfo){
          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }, NULL,
          &context->instance) != VK_SUCCESS)
      return false;
#define LOAD_INSTANCE(name)                                                    \
   PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(                    \
      context->instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
#undef LOAD_INSTANCE
   if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
       !vkCreateDevice || !vkGetDeviceProcAddr)
      return false;
   uint32_t physical_count = 1;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkResult result = vkEnumeratePhysicalDevices(
      context->instance, &physical_count, &physical);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       physical_count != 1 || !physical)
      return false;
   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(physical, &properties);
   printf("[identity] vendor=0x%04x device=0x%04x name=%s\n",
          properties.vendorID, properties.deviceID, properties.deviceName);
   if (properties.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       properties.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE)
      return false;
   const float priority = 1.0f;
   result = vkCreateDevice(
      physical,
      &(VkDeviceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
         .queueCreateInfoCount = 1,
         .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = 0, .queueCount = 1,
            .pQueuePriorities = &priority,
         },
      }, NULL, &context->device);
   if (result != VK_SUCCESS ||
       !load_public_api(vkGetDeviceProcAddr, context))
      return false;
   context->api.get_device_queue(context->device, 0, 0, &context->queue);

   result = context->api.create_command_pool(
      context->device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      }, NULL, &context->command_pool);
   if (result != VK_SUCCESS)
      return false;
   result = context->api.create_pipeline_layout(
      context->device,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      }, NULL, &context->pipeline_layout);
   if (result != VK_SUCCESS)
      return false;

   const VkAttachmentDescription attachments[2] = {
      { .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
      { .format = VK_FORMAT_D24_UNORM_S8_UINT,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
   };
   const VkAttachmentReference color_reference = {
      .attachment = 0, .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkAttachmentReference depth_reference = {
      .attachment = 1, .layout = VK_IMAGE_LAYOUT_GENERAL,
   };
   result = context->api.create_render_pass(
      context->device,
      &(VkRenderPassCreateInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 2, .pAttachments = attachments,
         .subpassCount = 1,
         .pSubpasses = &(VkSubpassDescription){
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_reference,
            .pDepthStencilAttachment = &depth_reference,
         },
      }, NULL, &context->render_pass);
   if (result != VK_SUCCESS)
      return false;

   const VkImageCreateInfo color_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 64, 64, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   result = context->api.create_image(context->device, &color_info, NULL,
                                      &context->color_image);
   VkMemoryRequirements color_requirements;
   if (result == VK_SUCCESS)
      context->api.get_image_memory_requirements(
         context->device, context->color_image, &color_requirements);
   if (result != VK_SUCCESS || color_requirements.size != COLOR_BYTES)
      return false;
   result = context->api.allocate_memory(
      context->device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = color_requirements.size,
         .memoryTypeIndex = 0,
      }, NULL, &context->color_memory);
   if (result == VK_SUCCESS)
      result = context->api.bind_image_memory(
         context->device, context->color_image, context->color_memory, 0);
   if (result == VK_SUCCESS)
      result = context->api.create_image_view(
         context->device,
         &(VkImageViewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = context->color_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .subresourceRange = {
               .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
               .levelCount = 1, .layerCount = 1,
            },
         }, NULL, &context->color_view);
   if (result != VK_SUCCESS)
      return false;

   const VkImageCreateInfo depth_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_D24_UNORM_S8_UINT,
      .extent = { 64, 64, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   context->depth_count = depth_count;
   for (uint32_t index = 0; index < depth_count; index++) {
      struct public_depth_target *target = &context->depth[index];
      result = context->api.create_image(context->device, &depth_info, NULL,
                                         &target->image);
      VkMemoryRequirements depth_requirements;
      if (result == VK_SUCCESS)
         context->api.get_image_memory_requirements(
            context->device, target->image, &depth_requirements);
      if (result != VK_SUCCESS || depth_requirements.size != DEPTH_BYTES)
         return false;
      result = context->api.allocate_memory(
         context->device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = depth_requirements.size,
            .memoryTypeIndex = 0,
         }, NULL, &target->memory);
      if (result == VK_SUCCESS)
         result = context->api.bind_image_memory(
            context->device, target->image, target->memory, 0);
      if (result == VK_SUCCESS)
         result = context->api.create_image_view(
            context->device,
            &(VkImageViewCreateInfo){
               .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
               .image = target->image,
               .viewType = VK_IMAGE_VIEW_TYPE_2D,
               .format = VK_FORMAT_D24_UNORM_S8_UINT,
               .subresourceRange = {
                  .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                VK_IMAGE_ASPECT_STENCIL_BIT,
                  .levelCount = 1, .layerCount = 1,
               },
            }, NULL, &target->view);
      VkImageView views[2] = { context->color_view, target->view };
      if (result == VK_SUCCESS)
         result = context->api.create_framebuffer(
            context->device,
            &(VkFramebufferCreateInfo){
               .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
               .renderPass = context->render_pass,
               .attachmentCount = 2, .pAttachments = views,
               .width = 64, .height = 64, .layers = 1,
            }, NULL, &target->framebuffer);
      if (result != VK_SUCCESS)
         return false;
   }

   result = context->api.allocate_memory(
      context->device,
      &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = VERTEX_BYTES, .memoryTypeIndex = 0,
      }, NULL, &context->vertex_memory);
   if (result == VK_SUCCESS)
      result = context->api.create_buffer(
         context->device,
         &(VkBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(float) * 48u,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         }, NULL, &context->vertex_buffer);
   if (result == VK_SUCCESS)
      result = context->api.bind_buffer_memory(
         context->device, context->vertex_buffer, context->vertex_memory, 0);
   if (result != VK_SUCCESS)
      return false;
   if (create_depth_pipeline(context, false, &context->read_pipeline) !=
          VK_SUCCESS ||
       create_depth_pipeline(context, true, &context->write_pipeline) !=
          VK_SUCCESS)
      return false;
   return true;
}

static bool
seed_color(struct public_context *context)
{
   uint32_t *words = NULL;
   if (context->api.map_memory(context->device, context->color_memory, 0,
                               VK_WHOLE_SIZE, 0, (void **)&words) != VK_SUCCESS)
      return false;
   for (uint32_t word = 0; word < COLOR_BYTES / sizeof(*words); word++)
      words[word] = R300_TRIANGLE_COLOR_SENTINEL;
   context->api.unmap_memory(context->device, context->color_memory);
   return true;
}

static bool
seed_depth(struct public_context *context, uint32_t target_index,
           const uint8_t image[DEPTH_BYTES])
{
   void *bytes = NULL;
   if (context->api.map_memory(context->device,
                               context->depth[target_index].memory, 0,
                               VK_WHOLE_SIZE, 0, &bytes) != VK_SUCCESS)
      return false;
   memcpy(bytes, image, DEPTH_BYTES);
   context->api.unmap_memory(context->device,
                             context->depth[target_index].memory);
   return true;
}

static bool
upload_vertices(struct public_context *context, enum run_mode mode)
{
   float vertices[48];
   fill_public_vertices(mode, vertices);
   void *mapped = NULL;
   if (context->api.map_memory(context->device, context->vertex_memory, 0,
                               VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
      return false;
   memcpy(mapped, vertices, sizeof(vertices));
   context->api.unmap_memory(context->device, context->vertex_memory);
   return true;
}

static bool
record_public_command(struct public_context *context, uint32_t target_index,
                      enum run_mode mode, VkCommandBuffer *command_out)
{
   VkCommandBuffer command = VK_NULL_HANDLE;
   VkResult result = context->api.allocate_command_buffers(
      context->device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = context->command_pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      }, &command);
   if (result == VK_SUCCESS)
      result = context->api.begin_command_buffer(
         command,
         &(VkCommandBufferBeginInfo){
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         });
   if (result != VK_SUCCESS)
      return false;

   const VkClearValue color_clear = {
      .color = { .float32 = { 0.64705884f, 0.64705884f,
                              0.64705884f, 0.64705884f } },
   };
   const VkRenderPassBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = context->render_pass,
      .framebuffer = context->depth[target_index].framebuffer,
      .renderArea = { .extent = { 64, 64 } },
      .clearValueCount = 1,
      .pClearValues = &color_clear,
   };
   const bool overlap = mode == MODE_NEAR_FAR || mode == MODE_FAR_NEAR;
   const uint32_t pass_count = overlap ? 2u : 1u;
   const VkPipeline pipeline = mode == MODE_READ ? context->read_pipeline
                                                 : context->write_pipeline;
   for (uint32_t pass = 0; pass < pass_count; pass++) {
      context->api.cmd_begin_render_pass(command, &begin,
                                         VK_SUBPASS_CONTENTS_INLINE);
      context->api.cmd_bind_pipeline(command,
                                     VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     pipeline);
      context->api.cmd_bind_vertex_buffers(
         command, 0, 1, &context->vertex_buffer, &(VkDeviceSize){ 0 });
      context->api.cmd_draw(command, 6, 1, pass * 6u, 0);
      context->api.cmd_end_render_pass(command);
   }
   if (context->api.end_command_buffer(command) != VK_SUCCESS)
      return false;
   *command_out = command;
   return true;
}

static bool
recorded_load_contract(VkCommandBuffer command,
                       VkDeviceMemory depth_memory, enum run_mode mode)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_command, command);
   VK_FROM_HANDLE(r3v_native_memory, native_depth, depth_memory);
   bool has_depth_state = false;
   bool has_zmask_state = false;
   uint32_t bandwidth_writes = 0;
   uint32_t color_load_launches = 0;
   bool ordered_color_relocation = false;
   const bool overlap = mode == MODE_NEAR_FAR || mode == MODE_FAR_NEAR;
   const uint32_t second_span =
      overlap ? native_command->deferred_draws[1].ib_span_offset : 0;
   uint32_t color_reference = UINT32_MAX;
   if (overlap) {
      for (uint32_t index = 0; index < native_command->reference_count;
           index++) {
         if (native_command->references[index].memory ==
                native_command->deferred_draws[1].target_memory &&
             native_command->references[index].write_domain ==
                RADEON_GEM_DOMAIN_GTT) {
            color_reference = index;
            break;
         }
      }
   }
   for (uint32_t word = 0; word + 1u < native_command->ib_size_dwords;
        word++) {
      has_depth_state |= native_command->ib[word] == CP_PACKET0(R300_ZB_FORMAT, 0);
      if (native_command->ib[word] ==
          CP_PACKET0(RADEON_DST_WIDTH_HEIGHT, 0)) {
         if (!overlap || word < second_span)
            return false;
         color_load_launches++;
      }
      if (overlap && word >= second_span && color_reference != UINT32_MAX &&
          native_command->ib[word] ==
             CP_PACKET3(R300_PM4_PACKET3_NOP, 0) &&
          native_command->ib[word + 1u] == color_reference * 4u)
         ordered_color_relocation = true;
      has_zmask_state |=
         native_command->ib[word] == CP_PACKET0(R300_ZB_ZMASK_OFFSET, 0) ||
         native_command->ib[word] == CP_PACKET0(R300_ZB_ZMASK_PITCH, 0);
      if (native_command->ib[word] == CP_PACKET0(R300_ZB_BW_CNTL, 0)) {
         if (native_command->ib[word + 1u] !=
             (R300_HIZ_DISABLE | R300_FAST_FILL_DISABLE |
              R300_RD_COMP_DISABLE | R300_WR_COMP_DISABLE))
            return false;
         bandwidth_writes++;
      }
   }
   uint32_t depth_references = 0;
   for (uint32_t index = 0; index < native_command->reference_count; index++)
      depth_references += native_command->references[index].handle ==
                          native_depth->bo.handle;
   return has_depth_state && !has_zmask_state && bandwidth_writes != 0u &&
          depth_references == 1u &&
          color_load_launches == (overlap ? 1u : 0u) &&
          (!overlap ||
           (ordered_color_relocation &&
            !native_command->deferred_draws[0].color_load_in_ib &&
            native_command->deferred_draws[1].color_load_in_ib));
}

static bool
recorded_overlap_load_mutation_rejected(VkCommandBuffer command,
                                        VkDeviceMemory depth_memory,
                                        enum run_mode mode)
{
   if (mode != MODE_NEAR_FAR && mode != MODE_FAR_NEAR)
      return true;
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_command, command);
   const uint32_t second_span =
      native_command->deferred_draws[1].ib_span_offset;
   for (uint32_t word = second_span;
        word + 1u < native_command->ib_size_dwords; word++) {
      if (native_command->ib[word] !=
          CP_PACKET0(RADEON_DST_WIDTH_HEIGHT, 0))
         continue;
      const uint32_t saved = native_command->ib[word];
      native_command->ib[word] = CP_PACKET0(RADEON_DST_Y_X, 0);
      const bool rejected =
         !recorded_load_contract(command, depth_memory, mode);
      native_command->ib[word] = saved;
      return rejected;
   }
   return false;
}

static bool
command_digest(VkCommandBuffer command,
               char digest[BLAKE3_OUT_LEN * 2u + 1u],
               uint32_t *dwords)
{
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_command, command);
   if (native_command->ib_size_dwords == 0u)
      return false;
   r300_triangle_ib_digest_hex(native_command->ib,
                               native_command->ib_size_dwords, digest);
   *dwords = native_command->ib_size_dwords;
   return true;
}

static bool
classify_public_color(const uint32_t *color, uint32_t pattern,
                      enum run_mode mode, uint32_t *mismatches_out,
                      bool *containment_out)
{
   uint32_t mismatches = 0;
   bool containment = false;
   for (uint32_t pixel = 0; pixel < COLOR_BYTES / sizeof(*color); pixel++) {
      if (pixel >= PIXELS) {
         containment |= color[pixel] != R300_TRIANGLE_COLOR_SENTINEL;
         continue;
      }
      const uint32_t x = pixel % 64u;
      const uint32_t y = pixel / 64u;
      const bool expected = mode == MODE_NEAR_FAR
                               ? false
                               : mode == MODE_FAR_NEAR
                                    ? true
                                    : expected_pixel_high(pattern, mode, x, y);
      const bool drawn =
         color[pixel] == R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
      const bool sentinel = color[pixel] == R300_TRIANGLE_COLOR_SENTINEL;
      mismatches += (!drawn && !sentinel) || drawn != expected;
   }
   *mismatches_out = mismatches;
   *containment_out = containment;
   return mismatches == 0 && !containment;
}

static enum outcome
classify_public_result(struct public_context *context, uint32_t target_index,
                       const uint8_t before[DEPTH_BYTES], uint32_t pattern,
                       enum run_mode mode, const char *evidence_dir,
                       const char *name, VkResult submit_result)
{
   uint32_t *color = NULL;
   uint8_t *depth = NULL;
   if (context->api.map_memory(context->device, context->color_memory, 0,
                               VK_WHOLE_SIZE, 0,
                               (void **)&color) != VK_SUCCESS)
      return OUTCOME_RETENTION_FAILURE;
   if (context->api.map_memory(context->device,
                               context->depth[target_index].memory, 0,
                               VK_WHOLE_SIZE, 0,
                               (void **)&depth) != VK_SUCCESS) {
      context->api.unmap_memory(context->device, context->color_memory);
      return OUTCOME_RETENTION_FAILURE;
   }
   char result_dir[PATH_MAX];
   const int path_length = snprintf(result_dir, sizeof(result_dir), "%s/%s",
                                    evidence_dir, name);
   if (path_length < 0 || (size_t)path_length >= sizeof(result_dir) ||
       r3v_native_evidence_write_file(result_dir, "color_after.bin", color,
                                      COLOR_BYTES) != 0 ||
       r3v_native_evidence_write_file(result_dir, "depth_after.bin", depth,
                                      DEPTH_BYTES) != 0) {
      context->api.unmap_memory(context->device, context->color_memory);
      context->api.unmap_memory(context->device,
                                context->depth[target_index].memory);
      return OUTCOME_RETENTION_FAILURE;
   }
   uint32_t color_mismatches = 0, depth_mismatches = 0;
   bool color_containment = false, depth_containment = false;
   const bool color_exact = classify_public_color(
      color, pattern, mode, &color_mismatches, &color_containment);
   const bool depth_exact = classify_depth(before, depth, pattern, mode,
      &depth_mismatches, &depth_containment);
   context->api.unmap_memory(context->device, context->color_memory);
   context->api.unmap_memory(context->device,
                             context->depth[target_index].memory);
   const enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(context->device);
   enum outcome outcome = OUTCOME_FAILED;
   if (color_containment || depth_containment)
      outcome = OUTCOME_CONTAINMENT_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (submit_result != VK_SUCCESS ||
            queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED && color_exact &&
            depth_exact)
      outcome = OUTCOME_PASS;
   char json[1024];
   const int json_length = snprintf(
      json, sizeof(json),
      "{\n  \"schema\": \"r3v-public-zb-tiled-result/1\",\n"
      "  \"name\": \"%s\",\n  \"verdict\": \"%s\",\n"
      "  \"pattern\": %u,\n  \"mode\": \"%s\",\n"
      "  \"submit_result\": %d,\n  \"queue_status\": \"%s\",\n"
      "  \"color_exact\": %s,\n  \"color_mismatches\": %u,\n"
      "  \"raw_depth_exact\": %s,\n  \"raw_depth_mismatches\": %u\n}\n",
      name, outcome_names[outcome], pattern, mode_names[mode], submit_result,
      r3v_native_queue_status_name(queue_status),
      color_exact ? "true" : "false", color_mismatches,
      depth_exact ? "true" : "false", depth_mismatches);
   if (json_length <= 0 || (size_t)json_length >= sizeof(json) ||
       r3v_native_evidence_write_file(result_dir, "outcome.json", json,
                                      (size_t)json_length) != 0)
      return OUTCOME_RETENTION_FAILURE;
   printf("[oracle] %s color_exact=%d color_mismatches=%u "
          "depth_exact=%d depth_mismatches=%u\n",
          name, color_exact, color_mismatches, depth_exact, depth_mismatches);
   return outcome;
}

static void
destroy_public_context(struct public_context *context)
{
   if (context->device && context->command_pool &&
       context->api.destroy_command_pool)
      context->api.destroy_command_pool(context->device,
                                        context->command_pool, NULL);
   if (context->device && context->read_pipeline &&
       context->api.destroy_pipeline)
      context->api.destroy_pipeline(context->device,
                                    context->read_pipeline, NULL);
   if (context->device && context->write_pipeline &&
       context->api.destroy_pipeline)
      context->api.destroy_pipeline(context->device,
                                    context->write_pipeline, NULL);
   for (uint32_t index = 0; index < context->depth_count; index++) {
      struct public_depth_target *target = &context->depth[index];
      if (context->device && target->framebuffer &&
          context->api.destroy_framebuffer)
         context->api.destroy_framebuffer(context->device,
                                           target->framebuffer, NULL);
      if (context->device && target->view && context->api.destroy_image_view)
         context->api.destroy_image_view(context->device, target->view, NULL);
      if (context->device && target->image && context->api.destroy_image)
         context->api.destroy_image(context->device, target->image, NULL);
      if (context->device && target->memory && context->api.free_memory)
         context->api.free_memory(context->device, target->memory, NULL);
   }
   if (context->device && context->vertex_buffer &&
       context->api.destroy_buffer)
      context->api.destroy_buffer(context->device, context->vertex_buffer,
                                  NULL);
   if (context->device && context->vertex_memory && context->api.free_memory)
      context->api.free_memory(context->device, context->vertex_memory, NULL);
   if (context->device && context->color_view &&
       context->api.destroy_image_view)
      context->api.destroy_image_view(context->device, context->color_view,
                                      NULL);
   if (context->device && context->color_image && context->api.destroy_image)
      context->api.destroy_image(context->device, context->color_image, NULL);
   if (context->device && context->color_memory && context->api.free_memory)
      context->api.free_memory(context->device, context->color_memory, NULL);
   if (context->device && context->pipeline_layout &&
       context->api.destroy_pipeline_layout)
      context->api.destroy_pipeline_layout(context->device,
                                            context->pipeline_layout, NULL);
   if (context->device && context->render_pass &&
       context->api.destroy_render_pass)
      context->api.destroy_render_pass(context->device, context->render_pass,
                                       NULL);
   if (context->device && context->api.destroy_device)
      context->api.destroy_device(context->device, NULL);
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(
         context->instance, "vkDestroyInstance");
   if (destroy_instance)
      destroy_instance(context->instance, NULL);
}

static bool
hardware_environment_matches(const char *evidence_dir,
                             const char *submission_count)
{
   const char *preload = getenv("LD_PRELOAD");
   const char *manifest = getenv("R3V_NATIVE_MANIFEST_DIR");
   const char *authorized =
      getenv("R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS");
   return (preload == NULL || preload[0] == '\0') && manifest != NULL &&
          manifest[0] != '\0' && same_directory(manifest, evidence_dir) &&
          authorized != NULL && strcmp(authorized, submission_count) == 0;
}

static int
run_public_single(const char *evidence_dir, uint32_t pattern,
                  enum run_mode mode, bool record_only, bool prepare_only)
{
   uint8_t before[DEPTH_BYTES];
   if (!fill_depth_image(before, pattern, mode))
      return 2;
   if (!record_only && !prepare_only &&
       !hardware_environment_matches(evidence_dir, "1")) {
      fprintf(stderr, "hardware environment does not authorize one public "
                      "submission\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   struct public_context context;
   if (!create_public_context(&context, 1) || !seed_color(&context) ||
       !seed_depth(&context, 0, before) || !upload_vertices(&context, mode)) {
      fprintf(stderr, "public Vulkan objects did not construct\n");
      destroy_public_context(&context);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkCommandBuffer command = VK_NULL_HANDLE;
   if (!record_public_command(&context, 0, mode, &command) ||
       !recorded_load_contract(command, context.depth[0].memory, mode) ||
       !recorded_overlap_load_mutation_rejected(
          command, context.depth[0].memory, mode)) {
      fprintf(stderr, "public LOAD command misses its depth contract\n");
      destroy_public_context(&context);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   printf("[record] pattern=%u mode=%s public_load_contract=PASS\n", pattern,
          mode_names[mode]);
   if (prepare_only) {
      float vertices[48];
      char ib_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char initial_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char vertex_digest[BLAKE3_OUT_LEN * 2u + 1u];
      uint32_t ib_dwords;
      fill_public_vertices(mode, vertices);
      if (!command_digest(command, ib_digest, &ib_dwords)) {
         destroy_public_context(&context);
         return 2;
      }
      blake3_hex(before, sizeof(before), initial_digest);
      blake3_hex(vertices, sizeof(vertices), vertex_digest);
      printf("pattern=%u\nmode=%s\nib_dwords=%u\nib_blake3=%s\n"
             "initial_image_blake3=%s\nvertex_blake3=%s\n",
             pattern, mode_names[mode], ib_dwords, ib_digest,
             initial_digest, vertex_digest);
      destroy_public_context(&context);
      return 0;
   }
   if (record_only) {
      destroy_public_context(&context);
      return finish(OUTCOME_PASS);
   }

   char result_name[64];
   const int name_length = snprintf(result_name, sizeof(result_name),
                                    "pattern-%u-%s", pattern,
                                    mode_names[mode]);
   if (name_length <= 0 || (size_t)name_length >= sizeof(result_name)) {
      destroy_public_context(&context);
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   char result_dir[PATH_MAX];
   if (!create_result_directory(evidence_dir, result_name, result_dir) ||
       r3v_native_evidence_write_file(result_dir, "depth_before.bin", before,
                                      DEPTH_BYTES) != 0) {
      destroy_public_context(&context);
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   stage("submit public depth attachment");
   const VkResult submit_result = context.api.queue_submit(
      context.queue, 1,
      &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1, .pCommandBuffers = &command,
      }, VK_NULL_HANDLE);
   const enum outcome outcome = classify_public_result(
      &context, 0, before, pattern, mode, evidence_dir, result_name,
      submit_result);
   destroy_public_context(&context);
   return finish(outcome);
}

static bool
retain_and_check_other_depth(struct public_context *context,
                             uint32_t target_index,
                             const uint8_t expected[DEPTH_BYTES],
                             const char *result_dir, const char *filename)
{
   uint8_t *mapped = NULL;
   if (context->api.map_memory(context->device,
                               context->depth[target_index].memory, 0,
                               VK_WHOLE_SIZE, 0,
                               (void **)&mapped) != VK_SUCCESS)
      return false;
   const bool exact = memcmp(mapped, expected, DEPTH_BYTES) == 0;
   const bool retained = r3v_native_evidence_write_file(
      result_dir, filename, mapped, DEPTH_BYTES) == 0;
   context->api.unmap_memory(context->device,
                             context->depth[target_index].memory);
   return exact && retained;
}

static int
run_public_persistence(const char *evidence_dir, bool record_only,
                       bool prepare_only)
{
   uint8_t initial[2][DEPTH_BYTES];
   if (!fill_depth_image_codes(initial[0], 2u, 0x200000u, 0x600000u) ||
       !fill_depth_image_codes(initial[1], 19u, 0x100000u, 0x700000u))
      return 2;
   if (!record_only && !prepare_only &&
       !hardware_environment_matches(evidence_dir, "3")) {
      fprintf(stderr, "hardware environment does not authorize three public "
                      "submissions\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   struct public_context context;
   if (!create_public_context(&context, 2) || !seed_color(&context) ||
       !seed_depth(&context, 0, initial[0]) ||
       !seed_depth(&context, 1, initial[1]) ||
       !upload_vertices(&context, MODE_READ)) {
      destroy_public_context(&context);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   const uint32_t targets[3] = { 0, 1, 0 };
   const uint32_t patterns[3] = { 2, 19, 2 };
   VkCommandBuffer commands[3] = { VK_NULL_HANDLE };
   for (uint32_t ordinal = 0; ordinal < 3; ordinal++) {
      if (!record_public_command(&context, targets[ordinal], MODE_READ,
                                 &commands[ordinal]) ||
          !recorded_load_contract(commands[ordinal],
                                  context.depth[targets[ordinal]].memory,
                                  MODE_READ)) {
         destroy_public_context(&context);
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
   }
   printf("[record] persistence=A2,B19,A2 public_load_contract=PASS\n");
   if (prepare_only) {
      char ib_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char candidate_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char a_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char b_digest[BLAKE3_OUT_LEN * 2u + 1u];
      char vertex_digest[BLAKE3_OUT_LEN * 2u + 1u];
      float vertices[48];
      uint32_t ib_dwords;
      uint32_t candidate_dwords;
      if (!command_digest(commands[0], ib_digest, &ib_dwords)) {
         destroy_public_context(&context);
         return 2;
      }
      for (uint32_t ordinal = 1; ordinal < 3; ordinal++) {
         if (!command_digest(commands[ordinal], candidate_digest,
                             &candidate_dwords) ||
             candidate_dwords != ib_dwords ||
             strcmp(candidate_digest, ib_digest) != 0) {
            destroy_public_context(&context);
            return 2;
         }
      }
      fill_public_vertices(MODE_READ, vertices);
      blake3_hex(initial[0], sizeof(initial[0]), a_digest);
      blake3_hex(initial[1], sizeof(initial[1]), b_digest);
      blake3_hex(vertices, sizeof(vertices), vertex_digest);
      printf("persistence_sequence=A2,B19,A2\nib_dwords=%u\n"
             "ib_blake3=%s\ninitial_a_blake3=%s\n"
             "initial_b_blake3=%s\nvertex_blake3=%s\n",
             ib_dwords, ib_digest, a_digest, b_digest, vertex_digest);
      destroy_public_context(&context);
      return 0;
   }
   if (record_only) {
      destroy_public_context(&context);
      return finish(OUTCOME_PASS);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "depth_a_before.bin",
                                      initial[0], DEPTH_BYTES) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "depth_b_before.bin",
                                      initial[1], DEPTH_BYTES) != 0) {
      destroy_public_context(&context);
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   static const char *const names[3] = {
      "stage-0-a", "stage-1-b", "stage-2-a",
   };
   enum outcome final_outcome = OUTCOME_PASS;
   for (uint32_t ordinal = 0; ordinal < 3; ordinal++) {
      if (!seed_color(&context)) {
         final_outcome = OUTCOME_RETENTION_FAILURE;
         break;
      }
      const VkResult submit_result = context.api.queue_submit(
         context.queue, 1,
         &(VkSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commands[ordinal],
         }, VK_NULL_HANDLE);
      final_outcome = classify_public_result(
         &context, targets[ordinal], initial[targets[ordinal]],
         patterns[ordinal], MODE_READ, evidence_dir, names[ordinal],
         submit_result);
      char result_dir[PATH_MAX];
      const int path_length = snprintf(result_dir, sizeof(result_dir),
                                       "%s/%s", evidence_dir,
                                       names[ordinal]);
      const uint32_t other = targets[ordinal] ^ 1u;
      const char *other_name = other == 0 ? "depth_a_other_after.bin"
                                          : "depth_b_other_after.bin";
      if (path_length <= 0 || (size_t)path_length >= sizeof(result_dir) ||
          !retain_and_check_other_depth(&context, other, initial[other],
                                        result_dir, other_name))
         final_outcome = OUTCOME_CONTAINMENT_FAILURE;
      if (final_outcome != OUTCOME_PASS)
         break;
   }
   destroy_public_context(&context);
   return finish(final_outcome);
}

static bool
public_reference_fragment_color_selftest(void)
{
   const uint32_t expected_bits[4] = R3V_REFERENCE_FRAGMENT_COLOR_BITS;
   uint32_t observed_bits[4];
   const char *reason = NULL;
   if (!r3v_fragment_constant_color_from_spirv(
          r3v_reference_fragment_spirv,
          sizeof(r3v_reference_fragment_spirv) /
             sizeof(r3v_reference_fragment_spirv[0]),
          "main", observed_bits, &reason) ||
       memcmp(observed_bits, expected_bits, sizeof(observed_bits)) != 0)
      return false;

   uint32_t channels[4];
   for (uint32_t channel = 0; channel < 4; channel++) {
      if (observed_bits[channel] == 0x00000000u)
         channels[channel] = 0u;
      else if (observed_bits[channel] == 0x3f800000u)
         channels[channel] = 0xffu;
      else
         return false;
   }
   const uint32_t packed = channels[2] | (channels[1] << 8) |
                           (channels[0] << 16) | (channels[3] << 24);
   return packed == R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM &&
          packed != R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
}

static bool
public_color_oracle_selftest(void)
{
   float vertices[48];
   fill_public_vertices(MODE_READ, vertices);
   for (uint32_t y = 0; y < 64; y++) {
      for (uint32_t x = 0; x < 64; x++) {
         const float point_x = ((float)x + 0.5f) / 32.0f - 1.0f;
         const float point_y = ((float)y + 0.5f) / 32.0f - 1.0f;
         bool covered = false;
         for (uint32_t triangle = 0; triangle < 2; triangle++) {
            const float *first = &vertices[triangle * 12u];
            const float *second = first + 4;
            const float *third = second + 4;
            const float edge0 = (second[0] - first[0]) *
                                   (point_y - first[1]) -
                                (second[1] - first[1]) *
                                   (point_x - first[0]);
            const float edge1 = (third[0] - second[0]) *
                                   (point_y - second[1]) -
                                (third[1] - second[1]) *
                                   (point_x - second[0]);
            const float edge2 = (first[0] - third[0]) *
                                   (point_y - third[1]) -
                                (first[1] - third[1]) *
                                   (point_x - third[0]);
            covered |= edge0 >= 0.0f && edge1 >= 0.0f && edge2 >= 0.0f;
         }
         if (!covered)
            return false;
      }
   }
   uint32_t color[COLOR_BYTES / sizeof(uint32_t)];
   uint32_t mismatches;
   bool containment;
   for (uint32_t pixel = 0; pixel < COLOR_BYTES / sizeof(uint32_t); pixel++)
      color[pixel] = R300_TRIANGLE_COLOR_SENTINEL;
   if (!classify_public_color(color, 2, MODE_NEAR_FAR, &mismatches,
                              &containment))
      return false;
   color[0] = R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
   if (classify_public_color(color, 2, MODE_NEAR_FAR, &mismatches,
                             &containment))
      return false;
   for (uint32_t pixel = 0; pixel < PIXELS; pixel++)
      color[pixel] = R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
   for (uint32_t pixel = PIXELS;
        pixel < COLOR_BYTES / sizeof(uint32_t); pixel++)
      color[pixel] = R300_TRIANGLE_COLOR_SENTINEL;
   if (!classify_public_color(color, 2, MODE_FAR_NEAR, &mismatches,
                              &containment))
      return false;
   color[0] = R300_TRIANGLE_COLOR_SENTINEL;
   if (classify_public_color(color, 2, MODE_FAR_NEAR, &mismatches,
                             &containment))
      return false;
   color[0] = R3V_REFERENCE_FRAGMENT_B8G8R8A8_UNORM;
   color[PIXELS] ^= 1u;
   return !classify_public_color(color, 2, MODE_FAR_NEAR, &mismatches,
                                 &containment);
}

static bool
public_result_directory_selftest(void)
{
   const char *temporary_root = getenv("TMPDIR");
   if (temporary_root == NULL || temporary_root[0] == '\0')
      temporary_root = ".";
   char root_template[PATH_MAX];
   const int template_length = snprintf(root_template, sizeof(root_template),
                                        "%s/r3v-public-zb-retention-XXXXXX",
                                        temporary_root);
   if (template_length <= 0 ||
       (size_t)template_length >= sizeof(root_template) ||
       mkdtemp(root_template) == NULL)
      return false;
   char result_dir[PATH_MAX];
   struct stat result_stat;
   const bool created =
      create_result_directory(root_template, "pattern-2-read", result_dir);
   const bool exact =
      created && stat(result_dir, &result_stat) == 0 &&
      S_ISDIR(result_stat.st_mode) && (result_stat.st_mode & 0777) == 0700;
   const bool reuse_refused =
      !create_result_directory(root_template, "pattern-2-read", result_dir);
   const bool removed = (!created || rmdir(result_dir) == 0) &&
                        rmdir(root_template) == 0;
   return exact && reuse_refused && removed;
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--selftest") == 0) {
      if (run_selftest() != 0 ||
          !public_reference_fragment_color_selftest() ||
          !public_color_oracle_selftest() ||
          !public_result_directory_selftest())
         return 1;
      printf("public color order selftest: PASS\n");
      return 0;
   }
   bool record_only = false;
   bool prepare_only = false;
   if (argc > 2 && strcmp(argv[argc - 1], "--record-only") == 0) {
      record_only = true;
      argc--;
   } else if (argc > 2 && strcmp(argv[argc - 1], "--prepare") == 0) {
      prepare_only = true;
      argc--;
   }
   if (argc == 3 && strcmp(argv[2], "--persistence") == 0)
      return run_public_persistence(argv[1], record_only, prepare_only);
   uint32_t pattern;
   enum run_mode mode;
   if (argc != 4 || !parse_pattern(argv[2], &pattern) ||
       !parse_mode(argv[3], &mode)) {
      fprintf(stderr,
              "usage: %s <evidence-directory> <pattern 0..19> "
              "<read|write|nearfar|farnear> [--prepare|--record-only]\n"
              "       %s <evidence-directory> --persistence "
              "[--prepare|--record-only]\n       %s --selftest\n",
              argv[0], argv[0], argv[0]);
      return 2;
   }
   return run_public_single(argv[1], pattern, mode, record_only,
                            prepare_only);
}
