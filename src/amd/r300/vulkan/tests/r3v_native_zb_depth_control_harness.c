/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the two-triangle depth control cell through the native ICD on
 * the radeon noop drm-shim: instance, device, three GEM-backed memories,
 * command recording through the exported depth-control recorder, and the
 * gated queue submission.  The closed-gate mode proves the fail-closed
 * verdict and byte-identity of the retained IB against the reference
 * emitter; the open-gate mode proves the transport path end to end with
 * the shim absorbing DRM_RADEON_CS, armed by the cell's own digest.
 */

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_control_cell.h"
#include "r3v_native.h"
#include "tests/r3v_native_shim_arming.h"

#include "util/mesa-blake3.h"

/* The harness links the native implementation directly (the ICD version
 * script keeps the shared library's export surface at the three vk_icd*
 * symbols), so the loader entry and the cell recorder resolve at link
 * time.
 */
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

static bool
same_file(const char *left, const char *right)
{
   struct stat left_status;
   struct stat right_status;
   return stat(left, &left_status) == 0 &&
          stat(right, &right_status) == 0 &&
          left_status.st_dev == right_status.st_dev &&
          left_status.st_ino == right_status.st_ino;
}

static int
attest_shim_provider(void)
{
   const char *expected = getenv("DRM_SHIM_EXPECTED_DSO");
   if (expected == NULL || expected[0] == '\0') {
      fprintf(stderr,
              "REFUSE: DRM_SHIM_EXPECTED_DSO is unset; the harness "
              "cannot attest the interposition provider\n");
      return 1;
   }

   static const char *const interposed[] = { "open", "ioctl" };
   for (unsigned i = 0; i < 2; i++) {
      dlerror();
      void *symbol = dlsym(RTLD_DEFAULT, interposed[i]);
      const char *error = dlerror();
      if (symbol == NULL || error != NULL) {
         fprintf(stderr, "REFUSE: symbol %s is unavailable: %s\n",
                 interposed[i], error != NULL ? error : "unknown");
         return 1;
      }
      Dl_info info;
      memset(&info, 0, sizeof(info));
      if (dladdr(symbol, &info) == 0 || info.dli_fname == NULL) {
         fprintf(stderr, "REFUSE: symbol %s has no provider object\n",
                 interposed[i]);
         return 1;
      }
      if (!same_file(info.dli_fname, expected)) {
         fprintf(stderr,
                 "REFUSE: symbol %s provider %s differs from expected "
                 "shim %s\n",
                 interposed[i], info.dli_fname, expected);
         return 1;
      }
   }
   return 0;
}

typedef PFN_vkVoidFunction (*icd_gipa_fn)(VkInstance, const char *);

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name)
#define LOAD_DEVICE(name) \
   PFN_##name name = (PFN_##name)gdpa(device, #name)

static int
read_whole_file(const char *dir, const char *name, void **data_out,
                size_t *size_out)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return 1;
   long size = -1;
   if (fseek(f, 0, SEEK_END) == 0)
      size = ftell(f);
   if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
      fclose(f);
      return 1;
   }
   void *data = malloc(size > 0 ? (size_t)size : 1);
   if (data == NULL) {
      fclose(f);
      return 1;
   }
   size_t got = fread(data, 1, (size_t)size, f);
   fclose(f);
   if ((long)got != size) {
      free(data);
      return 1;
   }
   *data_out = data;
   *size_out = (size_t)size;
   return 0;
}

/* The retained ib.bin against the in-tree reference emitter, byte for
 * byte in the canonical little-endian encoding, so file identity holds
 * without an external hasher.
 */
static void
check_retained_ib(const char *manifest_dir)
{
   struct r300_zb_depth_control_ib reference = {0};
   int emit_result = r300_zb_depth_control_reference_emit(&reference);
   CHECK(emit_result == 0, "reference depth-control emission: %d",
         emit_result);
   if (emit_result != 0)
      return;
   void *ib_data = NULL;
   size_t ib_size = 0;
   CHECK(read_whole_file(manifest_dir, "ib.bin", &ib_data, &ib_size) == 0,
         "retained ib.bin reads back");
   if (ib_data != NULL) {
      CHECK(ib_size == reference.ib_size_dwords * sizeof(uint32_t),
            "retained ib.bin length %zu matches the emitter's %u dwords",
            ib_size, reference.ib_size_dwords);
      uint8_t *reference_bytes =
         malloc(reference.ib_size_dwords * sizeof(uint32_t));
      CHECK(reference_bytes != NULL, "reference serialization buffer");
      if (reference_bytes != NULL &&
          ib_size == reference.ib_size_dwords * sizeof(uint32_t)) {
         r300_triangle_ib_serialize(reference.ib, reference.ib_size_dwords,
                                    reference_bytes);
         CHECK(memcmp(ib_data, reference_bytes, ib_size) == 0,
               "retained ib.bin is byte-identical to the emitter stream");
      }
      free(reference_bytes);
      free(ib_data);
   }
   r300_zb_depth_control_release(&reference);
}

int
main(int argc, char **argv)
{
   if (argc != 2 ||
       (strcmp(argv[1], "closed") != 0 && strcmp(argv[1], "open") != 0)) {
      fprintf(stderr, "usage: %s closed|open\n", argv[0]);
      return 2;
   }
   const bool open_gate = strcmp(argv[1], "open") == 0;

   unsetenv("R3V_NATIVE_SHIM_CS_REFUSE");
   unsetenv("R3V_NATIVE_SHIM_COMPLETION_FAIL");

   if (attest_shim_provider() != 0)
      return 3;

   char manifest_template[] = "/tmp/r3v-zb-depth-control-XXXXXX";
   const char *manifest_dir = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (manifest_dir == NULL || manifest_dir[0] == '\0') {
      if (mkdtemp(manifest_template) == NULL) {
         fprintf(stderr, "mkdtemp failed\n");
         return 2;
      }
      manifest_dir = manifest_template;
      setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
   }
   if (open_gate) {
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
      /* The arming gate admits this shim run only under the cell's own
       * IB digest; no other stream's digest authorizes it.
       */
      struct r300_zb_depth_control_ib authorized;
      if (r300_zb_depth_control_reference_emit(&authorized) != 0) {
         fprintf(stderr, "reference depth-control emission failed\n");
         return 2;
      }
      char digest[BLAKE3_OUT_LEN * 2 + 1];
      r300_triangle_ib_digest_hex(authorized.ib, authorized.ib_size_dwords,
                                  digest);
      r300_zb_depth_control_release(&authorized);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", digest, 1);

      struct utsname host;
      if (uname(&host) != 0) {
         fprintf(stderr, "uname failed\n");
         return 2;
      }
      setenv("R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE", host.release, 1);
      setenv("R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION",
             R3V_NATIVE_SHIM_MODULE_SRCVERSION, 1);
   } else {
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   }

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
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
            pdev_count == 1,
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
   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   r3v_native_install_shim_arming(native_device);

   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkDestroyDevice);

   /* The three role allocations at the cell's exact footprints, plus a
    * page-sized fourth that exists only to calibrate the exact-size
    * refusal on the color role.
    */
   struct { VkDeviceSize size; VkDeviceMemory memory; } allocations[] = {
      { R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION, VK_NULL_HANDLE },
      { R300_ZB_DEPTH_CONTROL_COLOR_BYTES, VK_NULL_HANDLE },
      { R300_ZB_DEPTH_CONTROL_DEPTH_BYTES, VK_NULL_HANDLE },
      { 4096, VK_NULL_HANDLE },
   };
   for (unsigned i = 0; i < 4; i++) {
      result = vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = allocations[i].size,
            .memoryTypeIndex = 0,
         },
         NULL, &allocations[i].memory);
      CHECK(result == VK_SUCCESS, "vkAllocateMemory[%u]: %d", i, result);
      if (result != VK_SUCCESS)
         return 1;
   }
   VkDeviceMemory vertex_memory = allocations[0].memory;
   VkDeviceMemory color_memory = allocations[1].memory;
   VkDeviceMemory depth_memory = allocations[2].memory;
   VkDeviceMemory wrong_size_memory = allocations[3].memory;

   VkCommandPool pool = VK_NULL_HANDLE;
   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS, "vkCreateCommandPool: %d", result);
   if (result != VK_SUCCESS || pool == VK_NULL_HANDLE)
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
   CHECK(result == VK_SUCCESS && cmd != VK_NULL_HANDLE,
         "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS || cmd == VK_NULL_HANDLE)
      return 1;

   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   /* Refusal calibration before the good recording: a wrong-size color
    * role and an aliased depth role each refuse without touching the
    * command buffer.
    */
   result = r3v_native_record_zb_depth_control(cmd, vertex_memory,
                                               wrong_size_memory,
                                               depth_memory);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "wrong-size color memory refuses recording: %d", result);
   result = r3v_native_record_zb_depth_control(cmd, vertex_memory,
                                               color_memory, color_memory);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "aliased color and depth roles refuse recording: %d", result);

   result = r3v_native_record_zb_depth_control(cmd, vertex_memory,
                                               color_memory, depth_memory);
   CHECK(result == VK_SUCCESS, "depth-control recording: %d", result);

   result = vkEndCommandBuffer(cmd);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);

   /* The recorder published the vertex payload and both sentinel fills
    * before any submission.
    */
   CHECK(native_device->drm.cache_sync_count == 3,
         "recorder published vertex, color, and depth: %" PRIu64,
         native_device->drm.cache_sync_count);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   if (open_gate) {
      CHECK(result == VK_SUCCESS,
            "open-gate submission through the shim: %d", result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "open-gate shim submission retires: %s",
            r3v_native_queue_status_name(queue_status));

      /* The retained submit object folds the completion reference in as
       * a fourth relocation entry beside the three role references.
       */
      void *submit_relocs = NULL;
      size_t submit_relocs_size = 0;
      CHECK(read_whole_file(manifest_dir, "submit_relocs.bin",
                            &submit_relocs, &submit_relocs_size) == 0,
            "submit_relocs.bin is retained");
      CHECK(submit_relocs_size ==
               (R300_ZB_DEPTH_CONTROL_SLOT_COUNT + 1) *
                  sizeof(struct drm_radeon_cs_reloc),
            "submit object carries three roles plus the completion reloc "
            "(%zu bytes)", submit_relocs_size);
      free(submit_relocs);

      check_retained_ib(manifest_dir);

      /* The shim absorbs the submission without executing it, so the
       * honest verdict of each oracle is its sentinel intact: no
       * execution evidence on the color target and no write on the
       * depth surface.
       */
      void *color_map = NULL;
      result = vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                           &color_map);
      CHECK(result == VK_SUCCESS && color_map != NULL,
            "color memory maps after completion: %d", result);
      if (color_map != NULL) {
         struct r300_zb_depth_control_color_verdict color_verdict;
         r300_zb_depth_control_color_oracle(
            color_map, R300_ZB_DEPTH_CONTROL_COLOR_BYTES, &color_verdict);
         CHECK(!color_verdict.executed && !color_verdict.near_pass &&
                  color_verdict.far_pass && color_verdict.exterior_pass &&
                  color_verdict.canary_pass,
               "shim run leaves the color sentinel intact (executed %d "
               "near %d far %d exterior %d canary %d)",
               color_verdict.executed, color_verdict.near_pass,
               color_verdict.far_pass, color_verdict.exterior_pass,
               color_verdict.canary_pass);
      }
      void *depth_map = NULL;
      result = vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0,
                           &depth_map);
      CHECK(result == VK_SUCCESS && depth_map != NULL,
            "depth memory maps after completion: %d", result);
      if (depth_map != NULL) {
         struct r300_zb_depth_control_depth_verdict depth_verdict;
         r300_zb_depth_control_depth_oracle(
            depth_map, R300_ZB_DEPTH_CONTROL_DEPTH_BYTES, &depth_verdict);
         CHECK(!depth_verdict.written && !depth_verdict.near_pass &&
                  depth_verdict.far_pass && depth_verdict.exterior_pass &&
                  depth_verdict.canary_pass,
               "shim run leaves the depth sentinel intact (written %d "
               "near %d far %d exterior %d canary %d)",
               depth_verdict.written, depth_verdict.near_pass,
               depth_verdict.far_pass, depth_verdict.exterior_pass,
               depth_verdict.canary_pass);
      }
   } else {
      CHECK(result == VK_ERROR_DEVICE_LOST, "closed gate fails closed: %d",
            result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "closed gate reports refusal before the ioctl: %s",
            r3v_native_queue_status_name(queue_status));

      check_retained_ib(manifest_dir);

      /* The relocation chunk carries exactly the three role references
       * in slot order.
       */
      void *reloc_data = NULL;
      size_t reloc_size = 0;
      CHECK(read_whole_file(manifest_dir, "relocs.bin", &reloc_data,
                            &reloc_size) == 0,
            "manifest relocs.bin is retained");
      if (reloc_data != NULL) {
         CHECK(reloc_size == R300_ZB_DEPTH_CONTROL_SLOT_COUNT *
                                sizeof(struct drm_radeon_cs_reloc),
               "relocation chunk carries the three role references "
               "(%zu bytes)", reloc_size);
         free(reloc_data);
      }
   }

   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 4; i++)
      vkFreeMemory(device, allocations[i].memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_zb_depth_control_harness(%s): all checks passed\n",
             argv[1]);
      return 0;
   }
   return 1;
}
