/*
 * SPDX-License-Identifier: MIT
 *
 * RS485M tiled Z24/S8 qualification.  The host constructs a physical
 * allocation from either an independently defined logical image or a storage
 * signature, submits one native depth-test draw, and retains both complete
 * surfaces before classifying logical color and raw depth separately.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
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

static void
fill_vertices(enum run_mode mode, uint32_t vertices[24])
{
   const float first_depth = mode == MODE_FAR_NEAR ? 0.5f : 0.25f;
   const float second_depth = mode == MODE_NEAR_FAR ? 0.5f : 0.25f;
   const float values[24] = {
      -64.0f, -64.0f, first_depth, 1.0f,
      192.0f, -64.0f, first_depth, 1.0f,
      -64.0f, 192.0f, first_depth, 1.0f,
      -64.0f, -64.0f, second_depth, 1.0f,
      192.0f, -64.0f, second_depth, 1.0f,
      -64.0f, 192.0f, second_depth, 1.0f,
   };
   memcpy(vertices, values, sizeof(values));
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
      const bool drawn = color[pixel] == R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
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
bits_hex(const uint8_t bits[PIXELS / 8u], char text[PIXELS / 4u + 1u])
{
   static const char digits[] = "0123456789abcdef";
   for (uint32_t i = 0; i < PIXELS / 8u; i++) {
      text[i * 2u] = digits[bits[i] >> 4];
      text[i * 2u + 1u] = digits[bits[i] & 15u];
   }
   text[PIXELS / 4u] = '\0';
}

static void
blake3_hex(const void *data, size_t size, char text[BLAKE3_OUT_LEN * 2 + 1])
{
   struct mesa_blake3 context;
   blake3_hash digest;
   _mesa_blake3_init(&context);
   _mesa_blake3_update(&context, data, size);
   _mesa_blake3_final(&context, digest);
   _mesa_blake3_format(text, digest);
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
            color[y * 64u + x] = R300_TRIANGLE_DRAW_COLOR_B8G8R8A8;
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
                       ? R300_TRIANGLE_DRAW_COLOR_B8G8R8A8
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

static int
run_persistence(const char *evidence_dir, bool prepare)
{
   uint8_t initial_a[DEPTH_BYTES];
   uint8_t initial_b[DEPTH_BYTES];
   uint32_t vertices[24];
   if (!fill_depth_image_codes(initial_a, 2u, 0x200000u, 0x600000u) ||
       !fill_depth_image_codes(initial_b, 19u, 0x100000u, 0x700000u))
      return 2;
   fill_vertices(MODE_READ, vertices);
   struct r300_zb_depth_control_ib reference;
   if (r300_zb_depth_tiled_validation_emit(false, &reference) != 0 ||
       r300_zb_depth_control_validate_reloc_sites(&reference) != 0)
      return 2;
   char ib_digest[65];
   char a_digest[65];
   char b_digest[65];
   char vertex_digest[65];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               ib_digest);
   blake3_hex(initial_a, sizeof(initial_a), a_digest);
   blake3_hex(initial_b, sizeof(initial_b), b_digest);
   blake3_hex(vertices, sizeof(vertices), vertex_digest);
   printf("persistence_sequence=A2,B19,A2\nib_dwords=%u\nib_blake3=%s\n"
          "initial_a_blake3=%s\ninitial_b_blake3=%s\nvertex_blake3=%s\n",
          reference.ib_size_dwords, ib_digest, a_digest, b_digest,
          vertex_digest);
   r300_zb_depth_control_release(&reference);
   if (prepare) {
      fflush(stdout);
      return 0;
   }

   const char *serial = getenv("R3V_NATIVE_AUTHORIZED_SERIAL_SUBMISSIONS");
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   const char *preload = getenv("LD_PRELOAD");
   if (serial == NULL || strcmp(serial, "3") != 0 ||
       declared == NULL || declared[0] == '\0' ||
       !same_directory(declared, evidence_dir) ||
       (preload != NULL && preload[0] != '\0'))
      return finish(OUTCOME_SUBMISSION_REFUSED);

   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
      vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (create_instance == NULL)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(&(VkInstanceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}, NULL, &instance);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)
      vk_icdGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties get_properties =
      (PFN_vkGetPhysicalDeviceProperties)vk_icdGetInstanceProcAddr(
         instance, "vkGetPhysicalDeviceProperties");
   PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)
      vk_icdGetInstanceProcAddr(instance, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr get_device_proc_addr = (PFN_vkGetDeviceProcAddr)
      vk_icdGetInstanceProcAddr(instance, "vkGetDeviceProcAddr");
   PFN_vkDestroyInstance destroy_instance = (PFN_vkDestroyInstance)
      vk_icdGetInstanceProcAddr(instance, "vkDestroyInstance");
   if (!enumerate || !get_properties || !create_device ||
       !get_device_proc_addr || !destroy_instance)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   uint32_t physical_count = 1;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   result = enumerate(instance, &physical_count, &physical);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       physical_count != 1 || physical == VK_NULL_HANDLE)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   VkPhysicalDeviceProperties properties;
   get_properties(physical, &properties);
   if (properties.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       properties.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = create_device(physical, &(VkDeviceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0, .queueCount = 1,
         .pQueuePriorities = &priority}}, NULL, &device);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);

#define PERSISTENCE_DEVICE_PROC(type, variable, name)                         \
   type variable = (type)get_device_proc_addr(device, name)
   PERSISTENCE_DEVICE_PROC(PFN_vkAllocateMemory, allocate_memory,
                           "vkAllocateMemory");
   PERSISTENCE_DEVICE_PROC(PFN_vkFreeMemory, free_memory, "vkFreeMemory");
   PERSISTENCE_DEVICE_PROC(PFN_vkMapMemory, map_memory, "vkMapMemory");
   PERSISTENCE_DEVICE_PROC(PFN_vkGetDeviceQueue, get_queue,
                           "vkGetDeviceQueue");
   PERSISTENCE_DEVICE_PROC(PFN_vkCreateCommandPool, create_pool,
                           "vkCreateCommandPool");
   PERSISTENCE_DEVICE_PROC(PFN_vkDestroyCommandPool, destroy_pool,
                           "vkDestroyCommandPool");
   PERSISTENCE_DEVICE_PROC(PFN_vkAllocateCommandBuffers, allocate_commands,
                           "vkAllocateCommandBuffers");
   PERSISTENCE_DEVICE_PROC(PFN_vkBeginCommandBuffer, begin_command,
                           "vkBeginCommandBuffer");
   PERSISTENCE_DEVICE_PROC(PFN_vkEndCommandBuffer, end_command,
                           "vkEndCommandBuffer");
   PERSISTENCE_DEVICE_PROC(PFN_vkQueueSubmit, queue_submit, "vkQueueSubmit");
   PERSISTENCE_DEVICE_PROC(PFN_vkDestroyDevice, destroy_device,
                           "vkDestroyDevice");
#undef PERSISTENCE_DEVICE_PROC
   if (!allocate_memory || !free_memory || !map_memory || !get_queue ||
       !create_pool || !destroy_pool || !allocate_commands || !begin_command ||
       !end_command || !queue_submit || !destroy_device)
      return finish(OUTCOME_SUBMISSION_REFUSED);

   struct allocation {
      VkDeviceSize size;
      VkDeviceMemory memory;
   } allocations[4] = {{VERTEX_BYTES, VK_NULL_HANDLE},
                       {COLOR_BYTES, VK_NULL_HANDLE},
                       {DEPTH_BYTES, VK_NULL_HANDLE},
                       {DEPTH_BYTES, VK_NULL_HANDLE}};
   for (unsigned index = 0; index < 4; index++) {
      result = allocate_memory(device, &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = allocations[index].size, .memoryTypeIndex = 0},
         NULL, &allocations[index].memory);
      if (result != VK_SUCCESS)
         return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkCommandPool pool = VK_NULL_HANDLE;
   result = create_pool(device, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0}, NULL, &pool);
   VkCommandBuffer commands[3] = {VK_NULL_HANDLE};
   if (result == VK_SUCCESS)
      result = allocate_commands(device, &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 3}, commands);
   for (uint32_t ordinal = 0; result == VK_SUCCESS && ordinal < 3; ordinal++) {
      result = begin_command(commands[ordinal], &(VkCommandBufferBeginInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
      if (result == VK_SUCCESS)
         result = r3v_native_record_zb_tiled_persistence(
            commands[ordinal], allocations[0].memory, allocations[1].memory,
            allocations[2].memory, allocations[3].memory, vertices,
            (enum r3v_native_zb_persistence_ordinal)ordinal);
      if (result == VK_SUCCESS)
         result = end_command(commands[ordinal]);
   }
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);

   void *color_map = NULL;
   void *depth_a_map = NULL;
   void *depth_b_map = NULL;
   if (map_memory(device, allocations[1].memory, 0, VK_WHOLE_SIZE, 0,
                  &color_map) != VK_SUCCESS || !color_map ||
       map_memory(device, allocations[2].memory, 0, VK_WHOLE_SIZE, 0,
                  &depth_a_map) != VK_SUCCESS || !depth_a_map ||
       map_memory(device, allocations[3].memory, 0, VK_WHOLE_SIZE, 0,
                  &depth_b_map) != VK_SUCCESS || !depth_b_map)
      return finish(OUTCOME_RETENTION_FAILURE);
   memcpy(depth_a_map, initial_a, DEPTH_BYTES);
   memcpy(depth_b_map, initial_b, DEPTH_BYTES);
   if (r3v_native_evidence_write_file(evidence_dir, "depth_a_before.bin",
                                      initial_a, DEPTH_BYTES) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "depth_b_before.bin",
                                      initial_b, DEPTH_BYTES) != 0)
      return finish(OUTCOME_RETENTION_FAILURE);

   VkQueue queue = VK_NULL_HANDLE;
   get_queue(device, 0, 0, &queue);
   enum outcome outcome = OUTCOME_PASS;
   static const char *const stage_names[] = {
      "stage-0-a", "stage-1-b", "stage-2-a",
   };
   static const uint32_t patterns[] = {2u, 19u, 2u};
   for (uint32_t ordinal = 0; ordinal < 3; ordinal++) {
      for (uint32_t pixel = 0; pixel < COLOR_BYTES / 4u; pixel++)
         ((uint32_t *)color_map)[pixel] = R300_TRIANGLE_COLOR_SENTINEL;
      result = queue_submit(queue, 1, &(VkSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
         .pCommandBuffers = &commands[ordinal]}, VK_NULL_HANDLE);
      const enum r3v_native_queue_status queue_status =
         r3v_native_queue_submission_status(device);
      char stage_dir[PATH_MAX];
      const int stage_length = snprintf(stage_dir, sizeof(stage_dir), "%s/%s",
                                        evidence_dir, stage_names[ordinal]);
      if (stage_length < 0 || (size_t)stage_length >= sizeof(stage_dir) ||
          r3v_native_evidence_write_file(stage_dir, "color_after.bin",
                                         color_map, COLOR_BYTES) != 0 ||
          r3v_native_evidence_write_file(stage_dir, "depth_a_after.bin",
                                         depth_a_map, DEPTH_BYTES) != 0 ||
          r3v_native_evidence_write_file(stage_dir, "depth_b_after.bin",
                                         depth_b_map, DEPTH_BYTES) != 0)
         return finish(OUTCOME_RETENTION_FAILURE);
      uint8_t bits[PIXELS / 8u];
      uint32_t colored, color_mismatches, a_mismatches, b_mismatches;
      bool color_containment, a_containment, b_containment;
      const bool color_exact = classify_color(
         color_map, patterns[ordinal], MODE_READ, bits, &colored,
         &color_mismatches, &color_containment);
      const bool a_exact = classify_depth(initial_a, depth_a_map, 2u, MODE_READ,
         &a_mismatches, &a_containment);
      const bool b_exact = classify_depth(initial_b, depth_b_map, 19u,
         MODE_READ, &b_mismatches, &b_containment);
      char outcome_json[768];
      const int outcome_length = snprintf(
         outcome_json, sizeof(outcome_json),
         "{\n  \"schema\": \"r3v-native-zb-tiled-persistence-stage/1\",\n"
         "  \"ordinal\": %u,\n  \"image\": \"%c\",\n"
         "  \"pattern\": %u,\n  \"submit_result\": %d,\n"
         "  \"queue_status\": \"%s\",\n  \"color_exact\": %s,\n"
         "  \"depth_a_exact\": %s,\n  \"depth_b_exact\": %s\n}\n",
         ordinal, ordinal == 1 ? 'B' : 'A', patterns[ordinal], result,
         r3v_native_queue_status_name(queue_status),
         color_exact ? "true" : "false", a_exact ? "true" : "false",
         b_exact ? "true" : "false");
      if (outcome_length <= 0 ||
          (size_t)outcome_length >= sizeof(outcome_json) ||
          r3v_native_evidence_write_file(stage_dir, "outcome.json",
             outcome_json, (size_t)outcome_length) != 0)
         return finish(OUTCOME_RETENTION_FAILURE);
      if (color_containment || a_containment || b_containment)
         outcome = OUTCOME_CONTAINMENT_FAILURE;
      else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
         outcome = OUTCOME_COMPLETION_FAILURE;
      else if (result != VK_SUCCESS ||
               queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED)
         outcome = OUTCOME_SUBMISSION_REFUSED;
      else if (queue_status != R3V_NATIVE_QUEUE_STATUS_COMPLETED ||
               !color_exact || !a_exact || !b_exact)
         outcome = OUTCOME_FAILED;
      if (outcome != OUTCOME_PASS)
         break;
   }

   destroy_pool(device, pool, NULL);
   for (unsigned index = 0; index < 4; index++)
      free_memory(device, allocations[index].memory, NULL);
   destroy_device(device, NULL);
   destroy_instance(instance, NULL);
   return finish(outcome);
}

int
main(int argc, char **argv)
{
   if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
      return run_selftest();
   if ((argc == 3 || argc == 4) && strcmp(argv[2], "--persistence") == 0 &&
       (argc == 3 || strcmp(argv[3], "--prepare") == 0))
      return run_persistence(argv[1], argc == 4);
   const bool prepare = argc == 5 && strcmp(argv[4], "--prepare") == 0;
   uint32_t pattern;
   enum run_mode mode;
   if ((argc != 4 && !prepare) || !parse_pattern(argv[2], &pattern) ||
       !parse_mode(argv[3], &mode)) {
      fprintf(stderr,
              "usage: %s <evidence-directory> <pattern 0..19> "
              "<read|write|nearfar|farnear> [--prepare]\n"
              "       %s <evidence-directory> --persistence [--prepare]\n"
              "       %s --selftest\n",
              argv[0], argv[0], argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];
   const bool depth_write = mode != MODE_READ;
   struct r300_zb_depth_control_ib reference;
   if (r300_zb_depth_tiled_validation_emit(depth_write, &reference) != 0 ||
       r300_zb_depth_control_validate_reloc_sites(&reference) != 0) {
      fprintf(stderr, "tiled validation IB does not construct\n");
      return 2;
   }
   char digest[65];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords, digest);
   printf("pattern=%u\nmode=%s\nib_dwords=%u\nib_blake3=%s\n", pattern,
          mode_names[mode], reference.ib_size_dwords, digest);
   r300_zb_depth_control_release(&reference);
   uint8_t prepared_depth[DEPTH_BYTES];
   uint32_t prepared_vertices[24];
   if (!fill_depth_image(prepared_depth, pattern, mode))
      return 2;
   fill_vertices(mode, prepared_vertices);
   char initial_digest[BLAKE3_OUT_LEN * 2 + 1];
   char vertex_digest[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(prepared_depth, sizeof(prepared_depth), initial_digest);
   blake3_hex(prepared_vertices, sizeof(prepared_vertices), vertex_digest);
   printf("initial_image_blake3=%s\nvertex_blake3=%s\n", initial_digest,
          vertex_digest);
   if (prepare) {
      fflush(stdout);
      return 0;
   }

   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr, "LD_PRELOAD is set; hardware qualification refused\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared == NULL || declared[0] == '\0' ||
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr, "manifest and evidence directories differ\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("instance");
   PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
      vk_icdGetInstanceProcAddr(NULL, "vkCreateInstance");
   if (create_instance == NULL)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(&(VkInstanceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}, NULL, &instance);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name) \
   vk_icdGetInstanceProcAddr(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);
   if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceProperties ||
       !vkCreateDevice || !vkGetDeviceProcAddr || !vkDestroyInstance)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   uint32_t physical_count = 1;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &physical_count, &physical);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       physical_count != 1 || physical == VK_NULL_HANDLE)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(physical, &properties);
   printf("[identity] vendor=0x%04x device=0x%04x name=%s\n",
          properties.vendorID, properties.deviceID, properties.deviceName);
   if (properties.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       properties.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE)
      return finish(OUTCOME_SUBMISSION_REFUSED);

   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = vkCreateDevice(physical, &(VkDeviceCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &(VkDeviceQueueCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = 0, .queueCount = 1,
         .pQueuePriorities = &priority}}, NULL, &device);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);

#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name) \
   vkGetDeviceProcAddr(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);
   if (!vkAllocateMemory || !vkFreeMemory || !vkMapMemory ||
       !vkGetDeviceQueue || !vkCreateCommandPool || !vkDestroyCommandPool ||
       !vkAllocateCommandBuffers || !vkBeginCommandBuffer ||
       !vkEndCommandBuffer || !vkQueueSubmit || !vkDestroyDevice)
      return finish(OUTCOME_SUBMISSION_REFUSED);

   struct allocation { VkDeviceSize size; VkDeviceMemory memory; } memory[3] = {
      {VERTEX_BYTES, VK_NULL_HANDLE}, {COLOR_BYTES, VK_NULL_HANDLE},
      {DEPTH_BYTES, VK_NULL_HANDLE}};
   for (unsigned index = 0; index < 3; index++) {
      result = vkAllocateMemory(device, &(VkMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .allocationSize = memory[index].size, .memoryTypeIndex = 0},
         NULL, &memory[index].memory);
      if (result != VK_SUCCESS)
         return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkCommandPool pool = VK_NULL_HANDLE;
   result = vkCreateCommandPool(device, &(VkCommandPoolCreateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0}, NULL, &pool);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);
   VkCommandBuffer command = VK_NULL_HANDLE;
   result = vkAllocateCommandBuffers(device, &(VkCommandBufferAllocateInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1}, &command);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);

   result = vkBeginCommandBuffer(command, &(VkCommandBufferBeginInfo){
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO});
   if (result == VK_SUCCESS)
      result = r3v_native_record_zb_tiled_validation(
         command, memory[0].memory, memory[1].memory, memory[2].memory,
         prepared_vertices, depth_write);
   if (result == VK_SUCCESS)
      result = vkEndCommandBuffer(command);
   if (result != VK_SUCCESS)
      return finish(OUTCOME_SUBMISSION_REFUSED);

   void *color_map = NULL;
   void *depth_map = NULL;
   if (vkMapMemory(device, memory[1].memory, 0, VK_WHOLE_SIZE, 0,
                   &color_map) != VK_SUCCESS || color_map == NULL ||
       vkMapMemory(device, memory[2].memory, 0, VK_WHOLE_SIZE, 0,
                   &depth_map) != VK_SUCCESS || depth_map == NULL)
      return finish(OUTCOME_RETENTION_FAILURE);
   uint8_t before[DEPTH_BYTES];
   memcpy(before, prepared_depth, DEPTH_BYTES);
   memcpy(depth_map, before, DEPTH_BYTES);
   for (uint32_t pixel = 0; pixel < COLOR_BYTES / 4u; pixel++)
      ((uint32_t *)color_map)[pixel] = R300_TRIANGLE_COLOR_SENTINEL;
   if (r3v_native_evidence_write_file(evidence_dir, "depth_before.bin",
                                      before, DEPTH_BYTES) != 0)
      return finish(OUTCOME_RETENTION_FAILURE);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   stage("submit");
   VkResult submit_result = vkQueueSubmit(queue, 1, &(VkSubmitInfo){
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
      .pCommandBuffers = &command}, VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);

   if (r3v_native_evidence_write_file(evidence_dir, "color_after.bin",
                                      color_map, COLOR_BYTES) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "depth_after.bin",
                                      depth_map, DEPTH_BYTES) != 0)
      return finish(OUTCOME_RETENTION_FAILURE);

   uint8_t observed_bits[PIXELS / 8u];
   uint32_t colored, color_mismatches, depth_mismatches;
   bool color_containment, depth_containment;
   const bool color_exact = classify_color(color_map, pattern, mode,
      observed_bits, &colored, &color_mismatches, &color_containment);
   const bool depth_exact = classify_depth(before, depth_map, pattern, mode,
      &depth_mismatches, &depth_containment);
   char observed_hex[PIXELS / 4u + 1u];
   bits_hex(observed_bits, observed_hex);

   enum outcome outcome;
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
   else
      outcome = OUTCOME_FAILED;

   char outcome_json[4096];
   const int outcome_length = snprintf(outcome_json, sizeof(outcome_json),
      "{\n  \"schema\": \"r3v-native-zb-tiled-validation-outcome/1\",\n"
      "  \"verdict\": \"%s\",\n  \"pattern\": %u,\n  \"mode\": \"%s\",\n"
      "  \"ib_blake3\": \"%s\",\n  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n  \"color_exact\": %s,\n"
      "  \"color_containment\": %s,\n  \"color_colored\": %u,\n"
      "  \"color_mismatches\": %u,\n  \"observed_color_bits_lsb0\": \"%s\",\n"
      "  \"expected_classification\": \"%s\",\n  \"raw_depth_exact\": %s,\n"
      "  \"raw_depth_containment\": %s,\n  \"raw_depth_mismatches\": %u\n}\n",
      outcome_names[outcome], pattern, mode_names[mode], digest, submit_result,
      r3v_native_queue_status_name(queue_status), color_exact ? "true" : "false",
      color_containment ? "true" : "false", colored, color_mismatches,
      observed_hex, color_exact ? "candidate_match" : "candidate_mismatch",
      depth_exact ? "true" : "false",
      depth_containment ? "true" : "false", depth_mismatches);
   if (outcome_length <= 0 || (size_t)outcome_length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
          "zb_tiled_validation_outcome.json", outcome_json,
          (size_t)outcome_length) != 0)
      return finish(OUTCOME_RETENTION_FAILURE);

   printf("[oracle] color_exact=%d colored=%u mismatches=%u "
          "raw_depth_exact=%d mismatches=%u\n", color_exact, colored,
          color_mismatches, depth_exact, depth_mismatches);
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned index = 0; index < 3; index++)
      vkFreeMemory(device, memory[index].memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return finish(outcome);
}
