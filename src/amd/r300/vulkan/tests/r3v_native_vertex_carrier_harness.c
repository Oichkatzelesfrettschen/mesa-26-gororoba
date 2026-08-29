/*
 * SPDX-License-Identifier: MIT
 *
 * Carrier-delivery harness on the drm-shim fixture: proves the
 * stream-fed recorder writes the same carrier bytes as the frozen
 * reference copy for every F32 delivery shape, and the same cell IB
 * regardless of route.  Recording-only: the hazard gate stays closed
 * and no submission is attempted.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_native.h"

#include "amd/r300/cpu/r300_cpu_vertex.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

#define COLOR_BYTES (64 * 65 * 4)
#define VERTEX_BYTES 4096
#define REFERENCE_BYTES (R300_TRIANGLE_VERTEX_DWORDS * sizeof(uint32_t))

static VkDevice device;
static VkDeviceMemory vertex_memory;
static VkDeviceMemory color_memory;
static VkCommandPool pool;
static PFN_vkAllocateCommandBuffers alloc_cmd;
static PFN_vkBeginCommandBuffer begin_cmd;
static PFN_vkEndCommandBuffer end_cmd;
static PFN_vkMapMemory map_memory;
static PFN_vkUnmapMemory unmap_memory;

/* Encode the canonical triangle table as the little-endian bytes consumed by
 * the VAP.  The common table remains the oracle; the conversion makes the
 * comparison independent of the host float object representation. */
static void
encode_float_carrier_bytes(uint8_t output[REFERENCE_BYTES],
                           const float vertices[R300_TRIANGLE_VERTEX_DWORDS])
{
   for (unsigned component = 0; component < R300_TRIANGLE_VERTEX_DWORDS;
        component++) {
      uint32_t bits;
      memcpy(&bits, &vertices[component], sizeof(bits));
      output[component * 4 + 0] = (uint8_t)(bits >> 0);
      output[component * 4 + 1] = (uint8_t)(bits >> 8);
      output[component * 4 + 2] = (uint8_t)(bits >> 16);
      output[component * 4 + 3] = (uint8_t)(bits >> 24);
   }
}

static VkCommandBuffer
fresh_cmd(void)
{
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   assert(alloc_cmd(device,
                    &(VkCommandBufferAllocateInfo){
                       .sType =
                          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                       .commandPool = pool,
                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                       .commandBufferCount = 1,
                    },
                    &cmd) == VK_SUCCESS);
   begin_cmd(cmd, &(VkCommandBufferBeginInfo){
                     .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                  });
   return cmd;
}

/* Records through the stream route, then checks the carrier bytes in
 * the vertex BO and the installed IB against the frozen references.
 */
static void
check_delivery(const struct r3v_native_vertex_stream_desc *stream,
               const void *expected_carrier, uint32_t carrier_bytes)
{
   VkCommandBuffer cmd = fresh_cmd();
   assert(r3v_native_record_tcl_bypass_triangle_from_stream(
             cmd, vertex_memory, color_memory, stream) == VK_SUCCESS);
   end_cmd(cmd);

   void *map = NULL;
   assert(map_memory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS &&
          map != NULL);
   assert(memcmp(map, expected_carrier, carrier_bytes) == 0);
   unmap_memory(device, vertex_memory);

   /* The harness links the implementation, so the installed IB is
    * directly readable: every delivery route records the reference
    * cell's exact dwords, keeping the qualified digest independent of
    * how the carrier was written.
    */
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native_cmd, cmd);
   struct r300_tcl_bypass_triangle_ib reference;
   assert(r300_tcl_bypass_triangle_reference_emit(&reference) == 0);
   assert(native_cmd->ib_size_dwords == reference.ib_size_dwords);
   assert(memcmp(native_cmd->ib, reference.ib,
                 reference.ib_size_dwords * sizeof(uint32_t)) == 0);
   r300_tcl_bypass_triangle_release(&reference);
}

int
main(void)
{
   /* The gate stays closed by construction: recording is submit-free
    * and this harness never opens the hazard environment.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   assert(create_instance(&(VkInstanceCreateInfo){
                             .sType =
                                VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                          },
                          NULL, &instance) == VK_SUCCESS);

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated =
      vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   assert((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
          pdev_count == 1);

   const float priority = 1.0f;
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

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   alloc_cmd = vkAllocateCommandBuffers;
   begin_cmd = vkBeginCommandBuffer;
   end_cmd = vkEndCommandBuffer;
   map_memory = vkMapMemory;
   unmap_memory = vkUnmapMemory;

   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = VERTEX_BYTES,
      .memoryTypeIndex = 0,
   };
   assert(vkAllocateMemory(device, &alloc, NULL, &vertex_memory) ==
          VK_SUCCESS);
   alloc.allocationSize = COLOR_BYTES;
   assert(vkAllocateMemory(device, &alloc, NULL, &color_memory) ==
          VK_SUCCESS);
   assert(vkCreateCommandPool(
             device,
             &(VkCommandPoolCreateInfo){
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .queueFamilyIndex = 0,
             },
             NULL, &pool) == VK_SUCCESS);

   /* The reference vertices carry z = 0 and w = 1 on every vertex, so
    * the F32_3 (XYZ1) and F32_2 (XY01) deliveries must reproduce the
    * frozen carrier byte-identically, and the F32_4 delivery is the
    * pass-through identity.
   */
   const float *ref = r300_tcl_bypass_triangle_vertices;
   uint8_t reference_bytes[REFERENCE_BYTES];
   encode_float_carrier_bytes(reference_bytes,
                              r300_tcl_bypass_triangle_vertices);

   check_delivery(&(struct r3v_native_vertex_stream_desc){
                     .records = ref,
                     .size_bytes = 48,
                     .stride = 16,
                     .format_id = R300_VERTEX_FORMAT_F32_4,
                  },
                  reference_bytes, sizeof(reference_bytes));

   /* The VAP fetch contract consumes little-endian component bytes.  The
    * r300_cpu_vertex_gather_baseline oracle (rg --fixed-strings
    * r300_cpu_vertex_gather_baseline src/) copies physical component bytes
    * verbatim and writes explicit synthesized lanes, while R300_VAP_VTX_SIZE
    * (rg --fixed-strings R300_VAP_VTX_SIZE src/) fixes the dword fetch width.
    * This independent packed leg exercises that representation directly
    * against the canonical triangle table instead of relying on the host
    * float object representation.
    */
   static const uint8_t byte_defined_ref[48] = {
      0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x41,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
      0x00, 0x00, 0x60, 0x42, 0x00, 0x00, 0x00, 0x41,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
      0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x60, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f,
   };
   assert(memcmp(byte_defined_ref, reference_bytes,
                 sizeof(byte_defined_ref)) == 0);
   check_delivery(&(struct r3v_native_vertex_stream_desc){
                     .records = byte_defined_ref,
                     .size_bytes = sizeof(byte_defined_ref),
                     .stride = 16,
                     .format_id = R300_VERTEX_FORMAT_F32_4,
                  },
                  reference_bytes, sizeof(reference_bytes));

   float xyz[9];
   float xy[6];
   for (unsigned v = 0; v < 3; v++) {
      memcpy(&xyz[v * 3], &ref[v * 4], 12);
      memcpy(&xy[v * 2], &ref[v * 4], 8);
   }
   check_delivery(&(struct r3v_native_vertex_stream_desc){
                     .records = xyz,
                     .size_bytes = sizeof(xyz),
                     .stride = 12,
                     .format_id = R300_VERTEX_FORMAT_F32_3,
                  },
                  reference_bytes, sizeof(reference_bytes));
   check_delivery(&(struct r3v_native_vertex_stream_desc){
                     .records = xy,
                     .size_bytes = sizeof(xy),
                     .stride = 8,
                     .format_id = R300_VERTEX_FORMAT_F32_2,
                  },
                  reference_bytes, sizeof(reference_bytes));

   /* Padded stride and nonzero first_vertex reach the same records. */
   uint8_t padded[4 * 24];
   memset(padded, 0x5a, sizeof(padded));
   for (unsigned v = 0; v < 3; v++)
      memcpy(padded + (v + 1) * 24, &ref[v * 4], 16);
   check_delivery(&(struct r3v_native_vertex_stream_desc){
                     .records = padded,
                     .size_bytes = sizeof(padded),
                     .stride = 24,
                     .first_vertex = 1,
                     .format_id = R300_VERTEX_FORMAT_F32_4,
                  },
                  reference_bytes, sizeof(reference_bytes));

   /* Known-bad calibration: a stream with a mutated W writes a carrier
    * the comparison rejects, so the identity legs above are
    * non-vacuous.
    */
   float mutated[12];
   memcpy(mutated, ref, sizeof(mutated));
   mutated[7] = 2.0f;
   uint8_t mutated_bytes[REFERENCE_BYTES];
   encode_float_carrier_bytes(mutated_bytes, mutated);
   VkCommandBuffer cmd = fresh_cmd();
   assert(r3v_native_record_tcl_bypass_triangle_from_stream(
             cmd, vertex_memory, color_memory,
             &(struct r3v_native_vertex_stream_desc){
                .records = mutated,
                .size_bytes = sizeof(mutated),
                .stride = 16,
                .format_id = R300_VERTEX_FORMAT_F32_4,
             }) == VK_SUCCESS);
   end_cmd(cmd);
   void *map = NULL;
   assert(map_memory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS &&
          map != NULL);
   assert(memcmp(map, reference_bytes, sizeof(reference_bytes)) != 0);
   unmap_memory(device, vertex_memory);

   /* Refusals report before any BO write: a bound too short for the
    * three records and an unknown format both refuse.
    */
   cmd = fresh_cmd();
   assert(r3v_native_record_tcl_bypass_triangle_from_stream(
             cmd, vertex_memory, color_memory,
             &(struct r3v_native_vertex_stream_desc){
                .records = ref,
                .size_bytes = 40,
                .stride = 16,
                .format_id = R300_VERTEX_FORMAT_F32_4,
             }) == VK_ERROR_INITIALIZATION_FAILED);
   assert(r3v_native_record_tcl_bypass_triangle_from_stream(
             cmd, vertex_memory, color_memory,
             &(struct r3v_native_vertex_stream_desc){
                .records = ref,
                .size_bytes = 48,
                .stride = 16,
                .format_id = 99,
             }) == VK_ERROR_INITIALIZATION_FAILED);

   /* Refusal preserves the carrier: the last accepted delivery wrote
    * the mutated stream, and both refused calls above left those exact
    * bytes in place, so the refusal path performed no BO write.
    */
   assert(map_memory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS &&
          map != NULL);
   assert(memcmp(map, mutated_bytes, sizeof(mutated_bytes)) == 0);
   unmap_memory(device, vertex_memory);

   /* A source range inside the carrier is a valid alias.  The recorder
    * snapshots all three records before copying the result, so the output
    * follows the original source bytes instead of reading a prefix that an
    * in-place gather already overwrote.
    */
   assert(map_memory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS &&
          map != NULL);
   memset(map, 0xa5, VERTEX_BYTES);
   uint8_t overlap_source[24];
   memcpy(overlap_source, map, sizeof(overlap_source));
   uint32_t overlap_expected[R300_TRIANGLE_VERTEX_DWORDS];
   const struct r300_vertex_stream overlap_stream = {
      .data = overlap_source,
      .stride = 8,
      .size_bytes = sizeof(overlap_source),
   };
   assert(r300_cpu_vertex_gather_baseline(
             R300_VERTEX_FORMAT_F32_2, &overlap_stream, 0, 3,
             overlap_expected, R300_TRIANGLE_VERTEX_DWORDS) == 0);
   cmd = fresh_cmd();
   assert(r3v_native_record_tcl_bypass_triangle_from_stream(
             cmd, vertex_memory, color_memory,
             &(struct r3v_native_vertex_stream_desc){
                .records = map,
                .size_bytes = 24,
                .stride = 8,
                .format_id = R300_VERTEX_FORMAT_F32_2,
             }) == VK_SUCCESS);
   assert(memcmp(map, overlap_expected, sizeof(overlap_expected)) == 0);
   unmap_memory(device, vertex_memory);

   /* A zero-stride source aliases the carrier in the same way.  Staging
    * reads the one source record before writing each repeated output vertex.
    */
   assert(map_memory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map) ==
             VK_SUCCESS &&
          map != NULL);
   memset(map, 0x5a, VERTEX_BYTES);
   uint8_t constant_source[16];
   memcpy(constant_source, map, sizeof(constant_source));
   uint32_t constant_expected[R300_TRIANGLE_VERTEX_DWORDS];
   const struct r300_vertex_stream constant_stream = {
      .data = constant_source,
      .stride = 0,
      .size_bytes = sizeof(constant_source),
   };
   assert(r300_cpu_vertex_gather_baseline(
             R300_VERTEX_FORMAT_F32_4, &constant_stream, 0, 3,
             constant_expected, R300_TRIANGLE_VERTEX_DWORDS) == 0);
   cmd = fresh_cmd();
   assert(r3v_native_record_tcl_bypass_triangle_from_stream(
             cmd, vertex_memory, color_memory,
             &(struct r3v_native_vertex_stream_desc){
                .records = map,
                .size_bytes = 16,
                .stride = 0,
                .format_id = R300_VERTEX_FORMAT_F32_4,
             }) == VK_SUCCESS);
   assert(memcmp(map, constant_expected, sizeof(constant_expected)) == 0);
   unmap_memory(device, vertex_memory);

   printf("r3v_native_vertex_carrier: every delivery shape reproduces the "
          "frozen carrier and cell\n");
   return 0;
}
