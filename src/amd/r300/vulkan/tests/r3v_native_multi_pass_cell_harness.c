/*
 * SPDX-License-Identifier: MIT
 *
 * Two-pass recorder contract on the drm-shim fixture: the first pass
 * installs and the second appends through the same primitive the
 * public two-draw command buffer takes, so the recorded stream is the
 * offline emitter's dword for dword, the merged binding is idempotent
 * under the winsys rule, and the armed gate admits the recording under
 * the emitter's digest.  The mutation modes arm the gate with a digest
 * of a stream the recorder never installs -- the first cell's flush
 * altered, or the second pass at another constant -- and the alias
 * mode records a second pass whose vertex page is the first pass's
 * target; each refuses where the contract says it does.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"
#include "r3v_native.h"
#include "tests/r3v_native_multi_pass_arms.h"
#include "tests/r3v_native_shim_arming.h"

#include "util/mesa-blake3.h"

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

static unsigned failures;

#define CHECK(condition, ...)                   \
   do {                                         \
      if (!(condition)) {                       \
         fprintf(stderr, "FAIL: " __VA_ARGS__); \
         fprintf(stderr, "\n");                 \
         failures++;                            \
      }                                         \
   } while (0)

typedef PFN_vkVoidFunction (*icd_gipa_fn)(VkInstance, const char *);

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name)
#define LOAD_DEVICE(name) \
   PFN_##name name = (PFN_##name)gdpa(device, #name)

enum mode { MODE_BOUND, MODE_SHARED, MODE_ALIAS, MODE_MUTATED_FLUSH,
            MODE_MUTATED_SECOND_STATE };

/* Returns the dword index of the first PACKET0 write to reg. */
static int
find_reg_write(const uint32_t *ib, uint32_t dwords, uint32_t reg)
{
   for (uint32_t i = 0; i < dwords;) {
      const uint32_t header = ib[i];
      if ((header >> 30) != 0) {
         i += 2 + ((header >> 16) & 0x3fff);
         continue;
      }
      const uint32_t count = ((header >> 16) & 0x3fff) + 1;
      const uint32_t base = (header & 0x1fff) * 4;
      for (uint32_t k = 0; k < count; k++)
         if (base + 4 * k == reg)
            return (int)(i + 1 + k);
      i += 1 + count;
   }
   return -1;
}

int
main(int argc, char **argv)
{
   enum mode mode = MODE_BOUND;
   if (argc == 2) {
      if (strcmp(argv[1], "shared") == 0)
         mode = MODE_SHARED;
      else if (strcmp(argv[1], "alias") == 0)
         mode = MODE_ALIAS;
      else if (strcmp(argv[1], "mutated-flush") == 0)
         mode = MODE_MUTATED_FLUSH;
      else if (strcmp(argv[1], "mutated-second-state") == 0)
         mode = MODE_MUTATED_SECOND_STATE;
      else if (strcmp(argv[1], "bound") != 0) {
         fprintf(stderr, "unknown mode %s\n", argv[1]);
         return 2;
      }
   }

   /* The shared binding reuses the first pass's page and target, so
    * the merge holds two entries and the second cell binds at (0, 1).
    */
   struct r300_triangle_multi_pass mp;
   r3v_native_multi_pass_reference(&mp);
   if (mode == MODE_SHARED) {
      mp.second_vertex_index = 0;
      mp.second_color_index = 1;
   }

   /* The armed digest: the emitter's stream, or a mutation of it the
    * recorder never installs.
    */
   struct r300_tcl_bypass_triangle_ib armed_cell;
   CHECK(r300_tcl_bypass_triangle_multi_pass_emit(&mp, &armed_cell) == 0,
         "the offline two-pass cell emits");
   if (mode == MODE_MUTATED_FLUSH) {
      const int flush = find_reg_write(armed_cell.ib, armed_cell.ib_size_dwords,
                                       R300_RB3D_DSTCACHE_CTLSTAT);
      CHECK(flush >= 0, "the first cell carries the destination-cache flush");
      if (flush >= 0)
         armed_cell.ib[flush] = 0;
   } else if (mode == MODE_MUTATED_SECOND_STATE) {
      struct r300_triangle_multi_pass other = mp;
      const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
      for (unsigned i = 0; i < 4; i++)
         memcpy(&other.pass[1].color_bits[i], &red[i], sizeof(float));
      r300_tcl_bypass_triangle_release(&armed_cell);
      CHECK(r300_tcl_bypass_triangle_multi_pass_emit(&other, &armed_cell) ==
               0,
            "the second-state mutation emits");
   }
   char armed_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed_cell.ib, armed_cell.ib_size_dwords,
                               armed_digest);
   const uint32_t armed_dwords = armed_cell.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&armed_cell);

   struct utsname host;
   CHECK(uname(&host) == 0, "uname");
   char manifest_template[] = "/tmp/r3v-multi-pass-XXXXXX";
   const char *manifest_dir = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (manifest_dir == NULL || manifest_dir[0] == '\0') {
      manifest_dir = mkdtemp(manifest_template);
      CHECK(manifest_dir != NULL, "mkdtemp");
      if (manifest_dir == NULL)
         return 1;
      setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
   }
   setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", armed_digest, 1);
   setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
   setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
          R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);

   icd_gipa_fn gipa = vk_icdGetInstanceProcAddr;

   VkInstance instance = VK_NULL_HANDLE;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS, "vkCreateInstance: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);
   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) && pdev_count == 1,
         "one shim physical device enumerates: %d count %u", result,
         pdev_count);
   if (pdev == VK_NULL_HANDLE)
      return 1;

   const float queue_priority = 1.0f;
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
               .pQueuePriorities = &queue_priority,
            },
      },
      NULL, &device);
   CHECK(result == VK_SUCCESS, "vkCreateDevice: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   r3v_native_install_shim_arming(r3v_native_device_from_handle(device));

   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   /* Four caller allocations: each pass's vertex page and target. */
   VkDeviceMemory memory[4] = { VK_NULL_HANDLE };
   for (unsigned i = 0; i < 4; i++) {
      result = vkAllocateMemory(device,
                                &(VkMemoryAllocateInfo){
                                   .sType =
                                      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = 65536,
                                   .memoryTypeIndex = 0,
                                },
                                NULL, &memory[i]);
      CHECK(result == VK_SUCCESS, "vkAllocateMemory %u: %d", i, result);
      if (result != VK_SUCCESS)
         return 1;
   }

   VkCommandPool pool = VK_NULL_HANDLE;
   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS, "vkCreateCommandPool: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   VkCommandBuffer cmd = VK_NULL_HANDLE;
   result = vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd);
   CHECK(result == VK_SUCCESS, "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS)
      return 1;
   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);

   if (mode == MODE_ALIAS) {
      /* A second vertex page that is the first pass's target crosses
       * two roles over one GEM object, which no binding admits.
       */
      result = r3v_native_record_multi_pass(cmd, memory[0], memory[1],
                                            memory[1], memory[3], &mp);
      CHECK(result != VK_SUCCESS, "an aliased second vertex page refuses");
      /* The declared binding must be the one the handles produce. */
      result = r3v_native_record_multi_pass(cmd, memory[0], memory[1],
                                            memory[0], memory[1], &mp);
      CHECK(result != VK_SUCCESS,
            "a binding disagreeing with the handles refuses");
      vkDestroyCommandPool(device, pool, NULL);
      for (unsigned i = 0; i < 4; i++)
         vkFreeMemory(device, memory[i], NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      if (failures != 0) {
         fprintf(stderr, "r3v_native_multi_pass_cell_harness: %u failure(s)\n",
                 failures);
         return 1;
      }
      printf("r3v_native_multi_pass_cell_harness: all checks passed\n");
      return 0;
   }

   result = mode == MODE_SHARED
               ? r3v_native_record_multi_pass(cmd, memory[0], memory[1],
                                              memory[0], memory[1], &mp)
               : r3v_native_record_multi_pass(cmd, memory[0], memory[1],
                                              memory[2], memory[3], &mp);
   CHECK(result == VK_SUCCESS, "two-pass recording: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(cmd);
   CHECK(native_cmd->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS,
         "the two-pass kind installs");
   const uint32_t expected_references =
      r300_tcl_bypass_triangle_multi_pass_reference_count(&mp);
   CHECK(native_cmd->reference_count == expected_references,
         "%u references install, got %u", expected_references,
         native_cmd->reference_count);

   /* Merging the installed array again by the winsys rule returns each
    * entry's own position, so the queue's merge is idempotent.
    */
   struct radeon_drm_vk_reloc_list relocs;
   radeon_drm_vk_reloc_list_init(&relocs);
   for (uint32_t i = 0; i < native_cmd->reference_count; i++) {
      uint32_t index = UINT32_MAX;
      CHECK(radeon_drm_vk_reloc_list_add(
               &relocs, native_cmd->references[i].handle,
               native_cmd->references[i].read_domains,
               native_cmd->references[i].write_domain, 0, &index) == 0,
            "reloc add %u", i);
      CHECK(index == i, "reference %u keeps its index, got %u", i, index);
   }
   CHECK(relocs.count == native_cmd->reference_count,
         "the merge adds no entry, got %u", relocs.count);
   radeon_drm_vk_reloc_list_finish(&relocs);

   /* The offline emitter's stream is the installed stream. */
   struct r300_tcl_bypass_triangle_ib reference_cell;
   CHECK(r300_tcl_bypass_triangle_multi_pass_emit(&mp, &reference_cell) == 0,
         "the reference two-pass cell emits");
   CHECK(reference_cell.ib_size_dwords == native_cmd->ib_size_dwords,
         "the installed stream keeps the emitted length: %u vs %u",
         reference_cell.ib_size_dwords, native_cmd->ib_size_dwords);
   CHECK(memcmp(reference_cell.ib, native_cmd->ib,
                (size_t)native_cmd->ib_size_dwords * sizeof(uint32_t)) == 0,
         "the emitted stream equals the installed stream");
   r300_tcl_bypass_triangle_release(&reference_cell);
   CHECK(armed_dwords == native_cmd->ib_size_dwords,
         "the armed stream has the installed length");

   /* The two-pass kind reaches the gate frozen: the geometry predicate
    * reads the merged binding, and the digest decides.  A mutated
    * authorization names a stream the recorder never installed.
    */
   result = vkEndCommandBuffer(cmd);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);
   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   if (mode == MODE_MUTATED_FLUSH || mode == MODE_MUTATED_SECOND_STATE) {
      CHECK(result != VK_SUCCESS,
            "the mutated digest refuses the recorded cell, got %d", result);
   } else {
      CHECK(result == VK_SUCCESS, "two-pass vkQueueSubmit: %d", result);
   }

   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 4; i++)
      vkFreeMemory(device, memory[i], NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures != 0) {
      fprintf(stderr, "r3v_native_multi_pass_cell_harness: %u failure(s)\n",
              failures);
      return 1;
   }
   printf("r3v_native_multi_pass_cell_harness: all checks passed\n");
   return 0;
}
