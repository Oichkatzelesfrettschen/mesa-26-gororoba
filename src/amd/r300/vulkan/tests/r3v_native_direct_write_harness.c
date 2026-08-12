/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the fixed 2D solid-fill direct-write control cell through the
 * native ICD on the radeon noop drm-shim: instance, device, one
 * GEM-backed color memory, command recording through the exported
 * direct-write recorder, and the gated queue submission.  The
 * closed-gate mode proves the fail-closed verdict and byte-identity of
 * the retained IB against the direct emitter; the open-gate mode proves
 * the transport path end to end with the shim absorbing DRM_RADEON_CS,
 * armed by the control cell's own digest.
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

#include "amd/r300/common/r300_direct_write.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
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
VkResult r3v_native_record_direct_write(VkCommandBuffer commandBuffer,
                                        VkDeviceMemory colorMemory);

static unsigned failures;

#define CHECK(condition, ...)                   \
   do {                                         \
      if (!(condition)) {                       \
         fprintf(stderr, "FAIL: " __VA_ARGS__); \
         fprintf(stderr, "\n");                 \
         failures++;                            \
      }                                         \
   } while (0)

static void
check_cell_emitter_error_mapping(void)
{
   /* Host calibration covers the allocator failure returned by both fixed
    * cell emitters and a structural emitter refusal.
    */
   CHECK(r3v_native_cell_vk_result_from_errno(-ENOMEM) ==
            VK_ERROR_OUT_OF_HOST_MEMORY,
         "-ENOMEM maps to VK_ERROR_OUT_OF_HOST_MEMORY");
   CHECK(r3v_native_cell_vk_result_from_errno(-EINVAL) ==
            VK_ERROR_INITIALIZATION_FAILED,
         "structural emitter errors map to initialization failure");

   /* Calibrate the status classifier with one accepted completion and the
    * two negative transport boundaries.  The sideband status preserves this
    * split when runtime result normalization maps each failure to device loss.
    */
   CHECK(r3v_native_queue_status_from_transport(true, true) ==
            R3V_NATIVE_QUEUE_STATUS_COMPLETED,
         "accepted ioctl and retired wait classify as completed");
   CHECK(r3v_native_queue_status_from_transport(false, false) ==
            R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
         "rejected ioctl classifies as submission refusal");
   CHECK(r3v_native_queue_status_from_transport(true, false) ==
            R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE,
         "accepted ioctl and failed wait classify as completion failure");
}

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

/* A VK_SUCCESS open-gate run only proves the shim absorbed DRM_RADEON_CS
 * when the interposed entry points actually resolve into the preloaded
 * shim DSO; the attestation maps each interposed symbol to its providing
 * object and compares it to DRM_SHIM_EXPECTED_DSO by device and inode,
 * before the hazard gate opens and before any Vulkan call.
 */
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

int
main(int argc, char **argv)
{
   if (argc != 2 ||
       (strcmp(argv[1], "closed") != 0 && strcmp(argv[1], "open") != 0 &&
        strcmp(argv[1], "reject") != 0 &&
        strcmp(argv[1], "completion-failure") != 0 &&
        strcmp(argv[1], "mixed") != 0)) {
      fprintf(stderr,
              "usage: %s closed|open|reject|completion-failure|mixed\n",
              argv[0]);
      return 2;
   }

   check_cell_emitter_error_mapping();

   const bool transport_reject = strcmp(argv[1], "reject") == 0;
   const bool completion_failure = strcmp(argv[1], "completion-failure") == 0;
   const bool mixed_submit = strcmp(argv[1], "mixed") == 0;
   const bool open_gate = strcmp(argv[1], "closed") != 0;

   unsetenv("R3V_NATIVE_SHIM_CS_REFUSE");
   unsetenv("R3V_NATIVE_SHIM_COMPLETION_FAIL");
   if (transport_reject)
      setenv("R3V_NATIVE_SHIM_CS_REFUSE", "1", 1);
   if (completion_failure)
      setenv("R3V_NATIVE_SHIM_COMPLETION_FAIL", "1", 1);

   if (attest_shim_provider() != 0)
      return 3;

   char manifest_template[] = "/tmp/r3v-direct-write-XXXXXX";
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
      /* The arming gate admits this shim run only under the control
       * cell's own IB digest; neither the triangle cell's digest nor any
       * other stream's digest authorizes this stream.
       */
      struct r300_direct_write_ib authorized;
      if (r300_direct_write_emit(&authorized) != 0) {
         fprintf(stderr, "reference direct-write emission failed\n");
         return 2;
      }
      char digest[BLAKE3_OUT_LEN * 2 + 1];
      r300_triangle_ib_digest_hex(authorized.ib, authorized.ib_size_dwords,
                                  digest);
      r300_direct_write_release(&authorized);
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
   native_device->arming_provider = &r3v_native_shim_arming_provider;

   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   /* One 65536-byte GTT allocation: 64 target rows, the canary row, and
    * an oracle-uncovered tail (the manifest's bo_table names the same
    * size).
    */
   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = 65536,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   result = vkAllocateMemory(device, &alloc_info, NULL, &color_memory);
   CHECK(result == VK_SUCCESS, "color vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || color_memory == VK_NULL_HANDLE)
      return 1;

   /* An allocation below target-plus-canary must refuse recording; the
    * undersized memory exists only for that calibration.
    */
   alloc_info.allocationSize = 4096;
   VkDeviceMemory undersized_memory = VK_NULL_HANDLE;
   result = vkAllocateMemory(device, &alloc_info, NULL, &undersized_memory);
   CHECK(result == VK_SUCCESS, "undersized vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || undersized_memory == VK_NULL_HANDLE)
      return 1;

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

   VkCommandBuffer commands[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   result = vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = mixed_submit ? 2 : 1,
      },
      commands);
   CHECK(result == VK_SUCCESS, "vkAllocateCommandBuffers: %d", result);
   if (result != VK_SUCCESS || commands[0] == VK_NULL_HANDLE ||
       (mixed_submit && commands[1] == VK_NULL_HANDLE))
      return 1;

   VkCommandBuffer cmd = commands[0];

   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   result = r3v_native_record_direct_write(cmd, undersized_memory);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "undersized color memory refuses recording: %d", result);

   result = r3v_native_record_direct_write(cmd, color_memory);
   CHECK(result == VK_SUCCESS, "direct-write recording: %d", result);

   result = vkEndCommandBuffer(cmd);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);

   if (mixed_submit) {
      result = vkBeginCommandBuffer(
         commands[1], &(VkCommandBufferBeginInfo){
                         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                      });
      CHECK(result == VK_SUCCESS, "empty vkBeginCommandBuffer: %d", result);
      if (result != VK_SUCCESS)
         return 1;

      result = vkEndCommandBuffer(commands[1]);
      CHECK(result == VK_SUCCESS, "empty vkEndCommandBuffer: %d", result);
      if (result != VK_SUCCESS)
         return 1;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The recorder published exactly the sentinel fill before any
    * submission.
    */
   CHECK(native_device->drm.cache_sync_count == 1,
         "recorder published the sentinel fill: %" PRIu64,
         native_device->drm.cache_sync_count);

   VkCommandBuffer submit_commands[2] = {cmd, commands[1]};
   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = mixed_submit ? 2 : 1,
                             .pCommandBuffers = submit_commands,
                          },
                          VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   if (transport_reject) {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "shim CS refusal reports device loss: %d", result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "shim CS refusal preserves submission refusal: %s",
            r3v_native_queue_status_name(queue_status));
   } else if (completion_failure) {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "shim completion failure reports device loss: %d", result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE,
            "shim completion failure preserves completion status: %s",
            r3v_native_queue_status_name(queue_status));
   } else if (open_gate) {
      CHECK(result == VK_SUCCESS,
            "open-gate submission through the shim: %d", result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "open-gate shim submission retires: %s",
            r3v_native_queue_status_name(queue_status));

      /* The retained submit object folds the completion reference in as
       * a second relocation entry beside the color reference.
       */
      void *submit_relocs = NULL;
      size_t submit_relocs_size = 0;
      CHECK(read_whole_file(manifest_dir, "submit_relocs.bin",
                            &submit_relocs, &submit_relocs_size) == 0,
            "submit_relocs.bin is retained");
      CHECK(submit_relocs_size ==
               (R300_DIRECT_WRITE_SLOT_COUNT + 1) *
                  sizeof(struct drm_radeon_cs_reloc),
            "submit object carries the completion reloc (%zu bytes)",
            submit_relocs_size);
      free(submit_relocs);

      void *submit_manifest = NULL;
      size_t submit_manifest_size = 0;
      CHECK(read_whole_file(manifest_dir, "submit_manifest.json",
                            &submit_manifest, &submit_manifest_size) == 0 &&
               submit_manifest_size > 0 &&
               memmem(submit_manifest, submit_manifest_size,
                      "\"submit-object\"", 15) != NULL,
            "submit_manifest.json names the submit object");
      free(submit_manifest);

      /* The retained ib.bin identifies itself against the in-tree
       * emitter, byte for byte in the canonical little-endian encoding,
       * so file identity holds without an external hasher: a retention
       * path that wrote different parser-valid bytes fails here.
       */
      struct r300_direct_write_ib reference = {0};
      int emit_result = r300_direct_write_emit(&reference);
      CHECK(emit_result == 0, "reference direct-write emission: %d",
            emit_result);
      if (emit_result != 0)
         return 1;
      void *ib_data = NULL;
      size_t ib_size = 0;
      CHECK(read_whole_file(manifest_dir, "ib.bin", &ib_data, &ib_size) ==
               0,
            "retained ib.bin reads back");
      if (ib_data != NULL) {
         CHECK(ib_size == reference.ib_size_dwords * sizeof(uint32_t),
               "retained ib.bin length %zu matches the emitter's %u "
               "dwords", ib_size, reference.ib_size_dwords);
         uint8_t *reference_bytes =
            malloc(reference.ib_size_dwords * sizeof(uint32_t));
         CHECK(reference_bytes != NULL, "reference serialization buffer");
         if (reference_bytes != NULL &&
             ib_size == reference.ib_size_dwords * sizeof(uint32_t)) {
            r300_triangle_ib_serialize(reference.ib,
                                       reference.ib_size_dwords,
                                       reference_bytes);
            CHECK(memcmp(ib_data, reference_bytes, ib_size) == 0,
                  "retained ib.bin is byte-identical to the emitter "
                  "stream");
         }
         free(reference_bytes);
         free(ib_data);
      }
      r300_direct_write_release(&reference);

      /* Mapping the color target after completion invalidates its stale
       * cache lines through the map-establishment sync.
       */
      LOAD_DEVICE(vkMapMemory);
      void *color_map = NULL;
      VkResult map_result = vkMapMemory(device, color_memory, 0,
                                        VK_WHOLE_SIZE, 0, &color_map);
      CHECK(map_result == VK_SUCCESS && color_map != NULL,
            "color memory maps after completion: %d", map_result);
      if (map_result != VK_SUCCESS || color_map == NULL)
         return 1;
      CHECK(native_device->drm.cache_sync_count == 2,
            "map establishment invalidated the color target: %" PRIu64,
            native_device->drm.cache_sync_count);

      /* The shim absorbs the submission without executing it, so the
       * honest oracle verdict is a sentinel-intact target: no execution
       * evidence, sentinel and canary untouched.
       */
      struct r300_direct_write_verdict oracle;
      r300_direct_write_oracle(color_map, 65536, &oracle);
      CHECK(!oracle.executed && !oracle.value_a_pass &&
               !oracle.value_b_pass && oracle.sentinel_pass &&
               oracle.canary_pass,
            "shim run leaves the sentinel intact (executed %d a %d b %d "
            "sentinel %d canary %d)",
            oracle.executed, oracle.value_a_pass, oracle.value_b_pass,
            oracle.sentinel_pass, oracle.canary_pass);

      /* The tail check detects writes outside the oracle's contract:
       * the oracle stops at the target-plus-canary extent, the recorder
       * sentinel-fills the whole 65536-byte allocation, and on the shim
       * no device write is legitimate, so any disturbed tail pixel is a
       * write no oracle predicate reports.
       */
      const uint32_t *all_pixels = color_map;
      uint32_t disturbed_tail = 0;
      for (uint32_t i = 65 * 64; i < 65536 / 4; i++) {
         if (all_pixels[i] != R300_TRIANGLE_COLOR_SENTINEL)
            disturbed_tail++;
      }
      CHECK(disturbed_tail == 0,
            "allocation tail past the canary row holds sentinel "
            "(%u pixels disturbed)", disturbed_tail);
   } else {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "closed gate fails closed: %d", result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "closed gate reports refusal before the ioctl: %s",
            r3v_native_queue_status_name(queue_status));

      /* The retained IB equals the direct emitter's stream byte for
       * byte, and the relocation chunk carries exactly the one color
       * reference.
       */
      struct r300_direct_write_ib reference = {0};
      int emit_result = r300_direct_write_emit(&reference);
      CHECK(emit_result == 0, "reference direct-write emission: %d",
            emit_result);
      if (emit_result != 0)
         return 1;
      void *ib_data = NULL;
      size_t ib_size = 0;
      CHECK(read_whole_file(manifest_dir, "ib.bin", &ib_data, &ib_size) ==
               0,
            "manifest ib.bin is retained");
      if (ib_data != NULL) {
         CHECK(ib_size == reference.ib_size_dwords * sizeof(uint32_t),
               "ib.bin length %zu matches the emitter's %u dwords",
               ib_size, reference.ib_size_dwords);
         uint8_t *reference_bytes =
            malloc(reference.ib_size_dwords * sizeof(uint32_t));
         CHECK(reference_bytes != NULL, "reference serialization buffer");
         if (reference_bytes != NULL &&
             ib_size == reference.ib_size_dwords * sizeof(uint32_t)) {
            r300_triangle_ib_serialize(reference.ib,
                                       reference.ib_size_dwords,
                                       reference_bytes);
            CHECK(memcmp(ib_data, reference_bytes, ib_size) == 0,
                  "ib.bin is byte-identical to the canonical serialization "
                  "of the direct emitter stream");
            free(reference_bytes);
         }
         free(ib_data);
      }
      r300_direct_write_release(&reference);

      void *reloc_data = NULL;
      size_t reloc_size = 0;
      CHECK(read_whole_file(manifest_dir, "relocs.bin", &reloc_data,
                            &reloc_size) == 0,
            "manifest relocs.bin is retained");
      if (reloc_data != NULL) {
         CHECK(reloc_size ==
                  R300_DIRECT_WRITE_SLOT_COUNT *
                     sizeof(struct drm_radeon_cs_reloc),
               "relocation chunk carries the one color reference "
               "(%zu bytes)",
               reloc_size);
         free(reloc_data);
      }
   }

   vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, undersized_memory, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_direct_write_harness(%s): all checks passed\n",
             argv[1]);
      return 0;
   }
   return 1;
}
