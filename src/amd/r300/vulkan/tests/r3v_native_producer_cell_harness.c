/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the R2VB producer-only cell through the native ICD on the radeon
 * noop drm-shim: instance, device, the one GEM-backed carrier, recording
 * through the exported producer recorder, and the gated queue submission.
 * The shim absorbs DRM_RADEON_CS and executes nothing, so an accepted
 * submission leaves the recorder's poison prefill standing across the whole
 * carrier; asserting transport acceptance and the unwritten-carrier verdict
 * side by side is what separates a delivered submission from a delivered
 * carrier.  The closed-gate mode proves the fail-closed verdict and the
 * byte-identity of the retained IB against the reference pass, and the
 * geometry mode binds a wrong-size carrier so the producer kind's frozen
 * geometry is the only arming factor that fails.
 */

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_r2vb_producer_pass.h"
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
VkResult r3v_native_record_r2vb_producer(VkCommandBuffer commandBuffer,
                                         VkDeviceMemory carrierMemory);

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

/* A VK_SUCCESS open-gate run only proves the shim absorbed DRM_RADEON_CS
 * when the interposed entry points resolve into the preloaded shim DSO; a
 * live kernel accept returns the same result code.  The attestation maps
 * each interposed symbol to its providing object and compares that object
 * to DRM_SHIM_EXPECTED_DSO by device and inode, before the hazard gate
 * opens and before any Vulkan call.
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

   /* The shim interposes open (open64 aliases it) and ioctl; the render
    * node open and DRM_RADEON_CS both travel through them.
    */
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

#define LOAD_INSTANCE(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name)
#define LOAD_DEVICE(name) \
   PFN_##name name = (PFN_##name)gdpa(device, #name)

static int
read_whole_file(const char *dir, const char *name, void **data_out,
                size_t *size_out)
{
   char path[1024];
   int path_length = snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (path_length < 0 || (size_t)path_length >= sizeof(path))
      return 1;
   FILE *f = fopen(path, "rb");
   if (f == NULL)
      return 1;
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fseek(f, 0, SEEK_SET);
   void *data = malloc(size > 0 ? (size_t)size : 1);
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

static bool
evidence_file_present(const char *dir, const char *name)
{
   char path[1024];
   int length = snprintf(path, sizeof(path), "%s/%s", dir, name);
   if (length < 0 || (size_t)length >= sizeof(path))
      return false;
   struct stat status;
   return stat(path, &status) == 0;
}

static bool
manifest_path_fits_writer(int template_length)
{
   const char *manifest_name = "submit_manifest.json";
   const char *temporary_suffix = ".XXXXXX";
   size_t path_length =
      template_length < 0 ? SIZE_MAX : (size_t)template_length;
   if (path_length > SIZE_MAX - strlen("/.") ||
       path_length + strlen("/.") > SIZE_MAX - strlen(manifest_name))
      return false;
   path_length += strlen("/.") + strlen(manifest_name);
   if (path_length > SIZE_MAX - strlen(temporary_suffix))
      return false;
   path_length += strlen(temporary_suffix);
   return path_length < 1024;
}

int
main(int argc, char **argv)
{
   if (argc != 2 ||
       (strcmp(argv[1], "closed") != 0 && strcmp(argv[1], "open") != 0 &&
        strcmp(argv[1], "geometry") != 0)) {
      fprintf(stderr, "usage: %s closed|open|geometry\n", argv[0]);
      return 2;
   }

   const bool geometry_mode = strcmp(argv[1], "geometry") == 0;
   const bool open_gate = strcmp(argv[1], "open") == 0 || geometry_mode;

   /* The producer harness has no fault-injection mode.  Clearing inherited
    * shim controls keeps a caller's refusal or completion fixture from
    * changing the normal transport verdicts.
    */
   unsetenv("R3V_NATIVE_SHIM_CS_REFUSE");
   unsetenv("R3V_NATIVE_SHIM_COMPLETION_FAIL");

   if (attest_shim_provider() != 0)
      return 3;

   char manifest_template[PATH_MAX];
   const char *manifest_dir = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (manifest_dir == NULL || manifest_dir[0] == '\0') {
      const char *tmp_dir = getenv("TMPDIR");
      if (tmp_dir == NULL || tmp_dir[0] == '\0')
         tmp_dir = getenv("MESON_BUILD_ROOT");
      if (tmp_dir == NULL || tmp_dir[0] == '\0')
         tmp_dir = ".";

      size_t tmp_dir_length = strlen(tmp_dir);
      int template_length = snprintf(
         manifest_template, sizeof(manifest_template), "%s%s%s", tmp_dir,
         tmp_dir[tmp_dir_length - 1] == '/' ? "" : "/",
         "r3v-native-producer-XXXXXX");
      if (template_length < 0 ||
          (size_t)template_length >= sizeof(manifest_template) ||
          !manifest_path_fits_writer(template_length)) {
         fprintf(stderr, "manifest template path is too long\n");
         return 2;
      }
      if (mkdtemp(manifest_template) == NULL) {
         fprintf(stderr, "mkdtemp failed\n");
         return 2;
      }
      manifest_dir = manifest_template;
      setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
   }

   /* The reference pass supplies both the carrier's expected delivery and
    * the authorized digest, so the open and geometry legs declare their
    * arming facts from one emission of the cell the recorder installs.
    */
   struct r300_r2vb_producer_ib reference;
   CHECK(r300_r2vb_producer_reference_emit(&reference) == 0,
         "reference producer pass emits");
   if (failures != 0)
      return 1;
   char reference_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(reference.ib, reference.ib_size_dwords,
                               reference_digest);

   uint32_t carrier_bytes = 0;
   CHECK(r3v_native_producer_carrier_bytes(&carrier_bytes) == 0,
         "producer carrier geometry resolves");
   uint32_t expected[R300_R2VB_PRODUCER_REFERENCE_COUNT * 4];
   const uint32_t expected_dwords =
      (uint32_t)(sizeof(expected) / sizeof(expected[0]));
   CHECK(r300_r2vb_producer_reference_expected(expected, expected_dwords) ==
            0,
         "carrier identity delivery resolves");

   if (open_gate) {
      /* The geometry leg derives its arming facts here, beside the open
       * leg's, so the carrier's size is the only input separating the two:
       * the open leg arms and the geometry leg refuses.
       */
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
      setenv("R3V_NATIVE_AUTHORIZED_IB_BLAKE3", reference_digest, 1);

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

   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;

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
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   /* The pass writes the carrier through the color backend and no other BO
    * rides the stream, so the carrier is the run's one allocation.  The
    * geometry leg adds a second, oversized allocation that carries the same
    * stream with a carrier the frozen layout does not describe.
    */
   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = carrier_bytes,
      .memoryTypeIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   VkDeviceMemory carrier_memory = VK_NULL_HANDLE;
   VkDeviceMemory oversized_memory = VK_NULL_HANDLE;
   void *carrier_map = NULL;
   result = vkAllocateMemory(device, &alloc_info, NULL, &carrier_memory);
   CHECK(result == VK_SUCCESS, "carrier vkAllocateMemory: %d", result);
   if (result != VK_SUCCESS || carrier_memory == VK_NULL_HANDLE)
      goto done;

   if (geometry_mode) {
      alloc_info.allocationSize = carrier_bytes + R3V_NATIVE_MEMORY_ALIGNMENT;
      result = vkAllocateMemory(device, &alloc_info, NULL, &oversized_memory);
      CHECK(result == VK_SUCCESS, "oversized vkAllocateMemory: %d", result);
      if (result != VK_SUCCESS || oversized_memory == VK_NULL_HANDLE)
         goto done;
   }

   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS, "vkCreateCommandPool: %d", result);

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

   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);

   struct r3v_native_cmd_buffer *native_cmd =
      r3v_native_cmd_buffer_from_handle(cmd);

   if (geometry_mode) {
      /* The recorder holds the carrier to the frozen layout's footprint, so
       * a wrong-size carrier reaches the arming gate through the installer
       * the recorder itself calls: the stream and the cell kind are the
       * reference pass's, and only the bound allocation's size differs.
       */
      result = r3v_native_record_r2vb_producer(cmd, oversized_memory);
      CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
            "oversized carrier refuses recording: %d", result);

      struct r3v_native_memory *native_oversized =
         r3v_native_memory_from_handle(oversized_memory);
      CHECK(r3v_native_producer_cell_install(native_cmd, native_oversized) ==
               0,
            "producer cell installs over the oversized carrier");
   } else {
      /* Known-bad calibration for the recorder's own carrier contract: the
       * cell renders into the carrier, so a carrier that is not the frozen
       * layout's row refuses before any stream installs.
       */
      VkDeviceMemory undersized_memory = VK_NULL_HANDLE;
      alloc_info.allocationSize = carrier_bytes / 2;
      result = vkAllocateMemory(device, &alloc_info, NULL,
                                &undersized_memory);
      CHECK(result == VK_SUCCESS, "undersized vkAllocateMemory: %d", result);
      if (undersized_memory != VK_NULL_HANDLE) {
         result = r3v_native_record_r2vb_producer(cmd, undersized_memory);
         CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
               "undersized carrier refuses recording: %d", result);
         CHECK(native_cmd->ib == NULL && native_cmd->references == NULL,
               "a refused carrier leaves the command buffer empty");
         vkFreeMemory(device, undersized_memory, NULL);
      }

      result = r3v_native_record_r2vb_producer(cmd, carrier_memory);
      CHECK(result == VK_SUCCESS, "producer cell recording: %d", result);
   }

   result = vkEndCommandBuffer(cmd);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);

   /* The installed stream is the reference pass and the cell kind names the
    * producer geometry contract, in every mode.
    */
   CHECK(native_cmd->cell_kind == R3V_NATIVE_CELL_KIND_R2VB_PRODUCER,
         "the recorded cell declares the producer kind");
   CHECK(native_cmd->ib != NULL &&
            native_cmd->ib_size_dwords == reference.ib_size_dwords &&
            memcmp(native_cmd->ib, reference.ib,
                   reference.ib_size_dwords * sizeof(uint32_t)) == 0,
         "the installed stream is the reference producer pass");
   CHECK(native_cmd->reference_count == R300_R2VB_PRODUCER_SLOT_COUNT,
         "the cell carries its one carrier reference: %u",
         native_cmd->reference_count);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The carrier's mapping stays live across the submission: the queue's
    * cache maintenance publishes and invalidates the lines of every
    * referenced memory that carries a map, which is the window a device
    * write would have to land in.
    */
   if (!geometry_mode) {
      result = vkMapMemory(device, carrier_memory, 0, VK_WHOLE_SIZE, 0,
                           &carrier_map);
      CHECK(result == VK_SUCCESS && carrier_map != NULL,
            "carrier maps before submission: %d", result);
      if (result != VK_SUCCESS || carrier_map == NULL)
         goto done;
   }

   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);

   if (geometry_mode) {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "a carrier outside the frozen geometry refuses submission: %d",
            result);
      /* The exact ioctl payload retains ahead of the arming evaluation, so
       * a refused run leaves the submit object describing the CS the
       * kernel never saw.  The disarm follows the evaluation, so the
       * one-shot token stays unwritten and the directory arms again.
       */
      CHECK(evidence_file_present(manifest_dir, "submit_manifest.json"),
            "the refused payload retains before the arming evaluation");
      CHECK(!evidence_file_present(manifest_dir, "attempt.token"),
            "the geometry refusal precedes the one-shot disarm");
      goto done;
   }

   if (open_gate) {
      /* Transport acceptance and carrier production are separate results.
       * The shim absorbs DRM_RADEON_CS and runs no command processor, so
       * the accepted submission below stands beside a carrier that still
       * holds the recorder's poison across its whole extent.  The pair is
       * the proof that the carrier verdict measures production rather than
       * transport.
       */
      CHECK(result == VK_SUCCESS,
            "open-gate submission through the shim: %d", result);
      CHECK(r3v_native_queue_submission_status(device) ==
               R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "the shim submission reports a completed transport: %s",
            r3v_native_queue_status_name(
               r3v_native_queue_submission_status(device)));

      struct r300_r2vb_producer_carrier_verdict verdict;
      CHECK(r300_r2vb_producer_carrier_check(
               expected, expected_dwords, R300_R2VB_PRODUCER_POISON_DWORD,
               carrier_map, carrier_bytes, &verdict) == 0,
            "the carrier check accepts its inputs");
      CHECK(!verdict.expected_pass &&
               verdict.mismatched_dwords == expected_dwords,
            "an absorbed submission delivers no dword (mismatched %u of %u)",
            verdict.mismatched_dwords, expected_dwords);
      CHECK(verdict.tail_poison_pass && verdict.disturbed_tail_dwords == 0,
            "the poison past the delivered extent stays intact (disturbed "
            "%u)",
            verdict.disturbed_tail_dwords);

      /* The retained submit object folds the completion reference in beside
       * the cell's own carrier reference.
       */
      void *submit_relocs = NULL;
      size_t submit_relocs_size = 0;
      CHECK(read_whole_file(manifest_dir, "submit_relocs.bin",
                            &submit_relocs, &submit_relocs_size) == 0,
            "submit_relocs.bin is retained");
      CHECK(submit_relocs_size ==
               (R300_R2VB_PRODUCER_SLOT_COUNT + 1) *
                  sizeof(struct drm_radeon_cs_reloc),
            "the submit object carries the completion reloc (%zu bytes)",
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

      /* The evidence directory is one-shot: the accepted submission wrote
       * the attempt token, and the token refuses the next arming in the
       * same directory ahead of any further retention.  This refusal loses
       * the queue, so it stands last.
       */
      CHECK(evidence_file_present(manifest_dir, "attempt.token"),
            "the accepted submission consumed the one-shot token");
      result = vkQueueSubmit(queue, 1,
                             &(VkSubmitInfo){
                                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &cmd,
                             },
                             VK_NULL_HANDLE);
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "a consumed token refuses the second submission: %d", result);
   } else {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "closed gate fails closed: %d", result);

      /* The semantic cell retains before the gate, so the closed leg reads
       * the same artifacts an armed run would: ib.bin equals the canonical
       * serialization of the reference pass, and relocs.bin carries the one
       * carrier reference.  The ioctl payload and the one-shot token belong
       * to the ioctl the closed gate never reaches.
       */
      void *ib_data = NULL;
      size_t ib_size = 0;
      CHECK(read_whole_file(manifest_dir, "ib.bin", &ib_data, &ib_size) == 0,
            "manifest ib.bin is retained");
      if (ib_data != NULL) {
         CHECK(ib_size == reference.ib_size_dwords * sizeof(uint32_t),
               "ib.bin length %zu matches the reference pass's %u dwords",
               ib_size, reference.ib_size_dwords);
         uint8_t *reference_bytes =
            malloc(reference.ib_size_dwords * sizeof(uint32_t));
         CHECK(reference_bytes != NULL, "reference serialization buffer");
         if (reference_bytes != NULL) {
            r300_triangle_ib_serialize(reference.ib,
                                       reference.ib_size_dwords,
                                       reference_bytes);
            if (ib_size == reference.ib_size_dwords * sizeof(uint32_t)) {
               CHECK(memcmp(ib_data, reference_bytes, ib_size) == 0,
                     "ib.bin is byte-identical to the canonical "
                     "serialization of the reference producer pass");
            }
            free(reference_bytes);
         }
         free(ib_data);
      }

      void *reloc_data = NULL;
      size_t reloc_size = 0;
      CHECK(read_whole_file(manifest_dir, "relocs.bin", &reloc_data,
                            &reloc_size) == 0,
            "manifest relocs.bin is retained");
      if (reloc_data != NULL) {
         CHECK(reloc_size == R300_R2VB_PRODUCER_SLOT_COUNT *
                                sizeof(struct drm_radeon_cs_reloc),
               "the relocation chunk carries the one carrier reference "
               "(%zu bytes)",
               reloc_size);
         free(reloc_data);
      }

      CHECK(!evidence_file_present(manifest_dir, "submit_manifest.json"),
            "the closed gate retains no submit object");
      CHECK(!evidence_file_present(manifest_dir, "attempt.token"),
            "the closed gate consumes no one-shot token");
   }

done:
   r300_r2vb_producer_pass_release(&reference);
   if (pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(device, pool, NULL);
   if (carrier_map != NULL)
      vkUnmapMemory(device, carrier_memory);
   if (carrier_memory != VK_NULL_HANDLE)
      vkFreeMemory(device, carrier_memory, NULL);
   if (oversized_memory != VK_NULL_HANDLE)
      vkFreeMemory(device, oversized_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_producer_cell_harness(%s): all checks passed\n",
             argv[1]);
      return 0;
   }
   return 1;
}
