/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the composed render-then-sample recorder through the native ICD
 * on the radeon noop drm-shim and proves the record-time relocation
 * contract: the cell's five slots reach four buffer objects, the shared
 * first target carries both domains, each payload names its buffer's
 * merged index, and the winsys merging that array again returns the same
 * positions, so the indices the arming digest covers are the indices the
 * kernel reads.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_reloc.h"
#include "r3v_native.h"
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

/* The payload a relocation site carries is its buffer's index into the
 * relocation chunk, scaled by the four dwords each entry occupies.
 */
#define RELOC_PAYLOAD(index) ((index) * 4u)

int
main(int argc, char **argv)
{
   /* The unbound mode arms with the digest of the cell as emitted, the
    * form whose payloads name their slot numbers.  The recorder installs
    * the bound form, so the gate compares two different streams and
    * refuses: the calibration that keeps an offline report from
    * authorizing a stream the recorder never installs.
    */
   const bool unbound = argc == 2 && strcmp(argv[1], "unbound") == 0;
   /* Arm from the offline cell before the device reads its environment:
    * emit the composed cell, bind it with the merged slot map, and
    * declare that digest.  The recorder installs the same stream, so an
    * armed submission below proves the reported digest is the one the
    * gate compares.
    */
   struct r300_triangle_composed_render_sample armed_shape;
   r300_tcl_bypass_triangle_render_shape_reference(&armed_shape.render);
   r300_tcl_bypass_triangle_render_shape_reference(&armed_shape.sample);
   armed_shape.sample.target_offset = 0;
   struct r300_tcl_bypass_triangle_ib armed_cell;
   CHECK(r300_tcl_bypass_triangle_composed_render_sample_emit(
            &armed_shape, &armed_cell) == 0,
         "the offline cell emits");
   if (!unbound) {
      CHECK(r300_tcl_bypass_triangle_bind_reloc_indices(
               &armed_cell, r300_tcl_bypass_triangle_composed_slot_index,
               R300_TRIANGLE_SLOT_COUNT) == 0,
            "the offline cell binds");
   }
   char armed_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed_cell.ib, armed_cell.ib_size_dwords,
                               armed_digest);
   r300_tcl_bypass_triangle_release(&armed_cell);

   struct utsname host;
   CHECK(uname(&host) == 0, "uname");
   char manifest_template[] = "/tmp/r3v-composed-XXXXXX";
   const char *manifest_dir = mkdtemp(manifest_template);
   CHECK(manifest_dir != NULL, "mkdtemp");
   if (manifest_dir == NULL)
      return 1;
   setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
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

   r3v_native_device_from_handle(device)->arming_provider =
      &r3v_native_shim_arming_provider;

   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkDestroyDevice);

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

   struct r300_triangle_composed_render_sample composed;
   r300_tcl_bypass_triangle_render_shape_reference(&composed.render);
   r300_tcl_bypass_triangle_render_shape_reference(&composed.sample);
   composed.sample.target_offset = 0;

   /* A repeated handle merges entries whose roles carry different
    * domains, so the recorder refuses before emitting.
    */
   result = r3v_native_record_composed_render_sample(
      cmd, memory[0], memory[1], memory[0], memory[3], &composed);
   CHECK(result != VK_SUCCESS, "a repeated role handle refuses");

   result = r3v_native_record_composed_render_sample(
      cmd, memory[0], memory[1], memory[2], memory[3], &composed);
   CHECK(result == VK_SUCCESS, "composed recording: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(cmd);
   CHECK(native_cmd->cell_kind ==
            R3V_NATIVE_CELL_KIND_TRIANGLE_COMPOSED_RENDER_SAMPLE,
         "the composed cell kind installs");
   /* Five slots over four buffer objects: the render half's color slot
    * and the sample half's texture slot name the first target.
    */
   CHECK(native_cmd->reference_count == 4,
         "four references install, got %u", native_cmd->reference_count);

   /* The shared entry is read by the sample half's texture check and
    * written by the render half, so it carries both domains; a
    * write-only entry would leave the kernel's texture check reading a
    * buffer it has no read domain for.
    */
   struct r3v_native_memory *shared = r3v_native_memory_from_handle(memory[1]);
   const struct r3v_native_bo_reference *shared_reference = NULL;
   for (uint32_t i = 0; i < native_cmd->reference_count; i++) {
      if (native_cmd->references[i].handle == shared->bo.handle)
         shared_reference = &native_cmd->references[i];
   }
   CHECK(shared_reference != NULL, "the shared target has a reference");
   if (shared_reference != NULL) {
      CHECK((shared_reference->read_domains & RADEON_GEM_DOMAIN_GTT) != 0,
            "the shared target reads from GTT");
      CHECK((shared_reference->write_domain & RADEON_GEM_DOMAIN_GTT) != 0,
            "the shared target writes to GTT");
   }

   /* Merging the installed array again by the winsys rule returns each
    * entry's own position, so the queue's merge is idempotent and the
    * payloads the digest covers reach the kernel unchanged.
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

   /* Each payload in the installed stream names the index of the buffer
    * its slot fills, which the recorder bound from the array's own
    * positions.
    */
   const uint32_t *expected_index = r300_tcl_bypass_triangle_composed_slot_index;
   struct r300_tcl_bypass_triangle_ib reference_cell;
   CHECK(r300_tcl_bypass_triangle_composed_render_sample_emit(
            &composed, &reference_cell) == 0,
         "the reference cell emits");
   CHECK(reference_cell.ib_size_dwords == native_cmd->ib_size_dwords,
         "the installed stream keeps the emitted length");
   for (uint32_t i = 0; i < reference_cell.reloc_site_count; i++) {
      const uint32_t site = reference_cell.reloc_sites[i].ib_index;
      const uint32_t slot = reference_cell.reloc_sites[i].slot;
      CHECK(native_cmd->ib[site] == RELOC_PAYLOAD(expected_index[slot]),
            "slot %u payload names index %u, got %u", slot,
            expected_index[slot], native_cmd->ib[site] / 4u);
   }
   /* Only the payload dwords differ from the emitted form, so binding
    * touched the relocation sites alone.
    */
   uint32_t differing = 0;
   for (uint32_t i = 0; i < reference_cell.ib_size_dwords; i++) {
      if (reference_cell.ib[i] != native_cmd->ib[i])
         differing++;
   }
   CHECK(differing == 3, "three payloads move, got %u", differing);

   /* Binding the reference cell with the same map reproduces the
    * installed stream byte for byte, so the digest an offline emitter
    * reports under that map is the digest the arming gate compares
    * against the recorded cell.  A report from the unbound form would
    * name a stream the recorder never installs, and the armed run would
    * refuse on the mismatch.
    */
   CHECK(r300_tcl_bypass_triangle_bind_reloc_indices(
            &reference_cell, r300_tcl_bypass_triangle_composed_slot_index,
            R300_TRIANGLE_SLOT_COUNT) == 0,
         "the reference cell binds");
   CHECK(memcmp(reference_cell.ib, native_cmd->ib,
                (size_t)native_cmd->ib_size_dwords * sizeof(uint32_t)) == 0,
         "the bound reference stream equals the installed stream");
   r300_tcl_bypass_triangle_release(&reference_cell);
   radeon_drm_vk_reloc_list_finish(&relocs);

   /* The composed kind reaches the gate: the geometry predicate reads
    * the merged binding this recorder installs, and a kind with no
    * predicate reports its geometry unfrozen and refuses before the
    * ioctl.  The gate was armed ahead of device creation with the
    * offline cell's bound digest -- the same order the attended
    * procedure runs in -- so a submission proves that the digest an
    * offline emitter reports authorizes the recorded cell.
    */
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
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
   if (unbound) {
      CHECK(result != VK_SUCCESS,
            "the unbound digest refuses the recorded cell, got %d", result);
   } else {
      CHECK(result == VK_SUCCESS, "composed vkQueueSubmit: %d", result);
   }

   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 4; i++)
      vkFreeMemory(device, memory[i], NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures != 0) {
      fprintf(stderr, "r3v_native_composed_cell_harness: %u failure(s)\n",
              failures);
      return 1;
   }
   printf("r3v_native_composed_cell_harness: all checks passed\n");
   return 0;
}
