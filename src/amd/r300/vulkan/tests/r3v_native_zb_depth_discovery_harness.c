/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the depth address-discovery cell through the native ICD on the
 * radeon noop drm-shim: instance, device, three GEM-backed memories,
 * command recording through the exported discovery recorder, and the
 * gated queue submission.  The closed-gate mode proves the fail-closed
 * verdict and byte-identity of the retained IB against the reference
 * emitter; the open-gate mode proves the transport path end to end with
 * the shim absorbing DRM_RADEON_CS, armed by the cell's own digest.
 *
 * Each refusal mode alters one recorded fact after recording and leaves
 * everything else as the open mode built it, so the open mode is the
 * positive control for all of them: same device, same recording, same
 * arming, one difference, opposite outcome.  Every mode builds its own
 * device and submits exactly once, because vk_QueueSubmit returns
 * VK_ERROR_DEVICE_LOST without reaching the driver once the device is
 * lost and a refused submission loses it.
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

#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_discovery_cell.h"
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

/* The retained ib.bin against the in-tree reference emitter bound to the
 * scenario and arm this run recorded, byte for byte in the canonical
 * little-endian encoding, so file identity holds without an external
 * hasher.
 */
static void
check_retained_ib(const char *manifest_dir,
                  const struct r300_zb_depth_discovery_scenario *scenario,
                  uint32_t depth_function, bool depth_write)
{
   struct r300_zb_depth_discovery_ib reference = {0};
   int emit_result = r300_zb_depth_discovery_reference_emit(
      scenario, depth_function, depth_write, &reference);
   CHECK(emit_result == 0, "reference discovery emission: %d", emit_result);
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
   r300_zb_depth_discovery_release(&reference);
}

int
main(int argc, char **argv)
{
   /* One submission per process.  vk_QueueSubmit returns
    * VK_ERROR_DEVICE_LOST without reaching the driver once the device is
    * lost, and a refused submission loses it, so a second submission in
    * the same process observes the runtime's short circuit rather than
    * the gate under test.  Each refusal mode therefore builds its own
    * device and submits exactly once.
    */
   const char *const modes[] = {
      "closed", "open", "refuse-unnamed-scenario", "refuse-unnamed-arm",
      "refuse-arm-disagreement", "refuse-compression-enabled",
      "refuse-altered-stream",
   };
   /* The argument count is established before any argument is read, so
    * an invocation with none reports the usage rather than reading past
    * the vector. */
   bool mode_known = false;
   if (argc >= 2) {
      for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
         mode_known = mode_known || strcmp(argv[1], modes[m]) == 0;
   }
   if (argc < 2 || argc > 3 || !mode_known ||
       (argc == 3 && strcmp(argv[2], "z24_macrotiled") != 0)) {
      fprintf(stderr,
              "usage: %s closed|open|refuse-unnamed-scenario|"
              "refuse-unnamed-arm|refuse-arm-disagreement|"
              "refuse-compression-enabled|refuse-altered-stream "
              "[z24_macrotiled]\n",
              argv[0]);
      return 2;
   }
   const bool refuse_unnamed_scenario =
      strcmp(argv[1], "refuse-unnamed-scenario") == 0;
   const bool refuse_unnamed_arm = strcmp(argv[1], "refuse-unnamed-arm") == 0;
   const bool refuse_arm = strcmp(argv[1], "refuse-arm-disagreement") == 0;
   const bool refuse_compression =
      strcmp(argv[1], "refuse-compression-enabled") == 0;
   const bool refuse_stream = strcmp(argv[1], "refuse-altered-stream") == 0;
   const bool refusal_mode = refuse_unnamed_scenario || refuse_unnamed_arm ||
                             refuse_arm || refuse_compression ||
                             refuse_stream;
   const bool open_gate = strcmp(argv[1], "open") == 0 || refusal_mode;

   /* The scenario this run records.  The macrotiled arm is the one the
    * capability gate on the ordinary depth control would refuse, so it
    * proves the discovery path admits a surface no logical address
    * transform exists for, and its envelope fills the constant
    * allocation exactly while the linear one leaves 3840 unclaimed
    * bytes. */
   const enum r3v_native_zb_discovery_scenario scenario_selection =
      argc == 3 ? R3V_NATIVE_ZB_DISCOVERY_SCENARIO_Z24_MACROTILED
                : R3V_NATIVE_ZB_DISCOVERY_SCENARIO_Z24_LINEAR;
   const struct r300_zb_depth_discovery_scenario *scenario =
      r3v_native_zb_discovery_scenario_descriptor(scenario_selection);
   CHECK(scenario != NULL, "scenario selector %d names a scenario",
         (int)scenario_selection);
   if (scenario == NULL)
      return 1;
   struct r300_zb_depth_layout layout;
   CHECK(r300_zb_depth_discovery_layout(scenario, &layout) == 0,
         "scenario %s resolves a layout", scenario->name);
   const VkDeviceSize depth_allocation_bytes = scenario->allocation_bytes;

   /* The arm the recording declares, and the state it names.  The
    * disagreement mode records this arm and then rewrites the recorded
    * declaration to another, leaving the stream untouched, so the
    * comparison between the two records is the one failed condition. */
   const enum r3v_native_zb_discovery_arm arm_selection =
      R3V_NATIVE_ZB_DISCOVERY_ARM_MEASURE;
   uint32_t depth_function = 0;
   bool depth_write = false;
   CHECK(r3v_native_zb_discovery_arm_state(arm_selection, &depth_function,
                                           &depth_write),
         "arm %d names a state", (int)arm_selection);

   unsetenv("R3V_NATIVE_SHIM_CS_REFUSE");
   unsetenv("R3V_NATIVE_SHIM_COMPLETION_FAIL");

   if (attest_shim_provider() != 0)
      return 3;

   char manifest_template[] = "/tmp/r3v-zb-depth-discovery-XXXXXX";
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
       * IB digest.  The scenario moves the DEPTHPITCH tile bits and the
       * arm moves the comparison and the write enable, so each
       * combination authorizes the stream it actually records.
       */
      struct r300_zb_depth_discovery_ib authorized;
      if (r300_zb_depth_discovery_reference_emit(scenario, depth_function,
                                                 depth_write,
                                                 &authorized) != 0) {
         fprintf(stderr, "reference discovery emission failed\n");
         return 2;
      }
      char digest[BLAKE3_OUT_LEN * 2 + 1];
      r300_triangle_ib_digest_hex(authorized.ib, authorized.ib_size_dwords,
                                  digest);
      r300_zb_depth_discovery_release(&authorized);
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
    * refusal on the color role.  The depth footprint is the campaign
    * constant, so it does not move between scenarios.
    */
   struct { VkDeviceSize size; VkDeviceMemory memory; } allocations[] = {
      { R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION, VK_NULL_HANDLE },
      { R300_ZB_DISCOVERY_COLOR_BYTES, VK_NULL_HANDLE },
      { depth_allocation_bytes, VK_NULL_HANDLE },
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

   /* Refusal calibration before the good recording.  Each arm refuses
    * at the recorder, which is where a footprint or declaration error
    * carries a reason; the recorder leaves the command buffer alone, so
    * the good recording that follows is the positive control for all of
    * them.
    */
   result = r3v_native_record_zb_depth_discovery(
      cmd, vertex_memory, wrong_size_memory, depth_memory,
      scenario_selection, arm_selection);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "wrong-size color memory refuses recording: %d", result);

   /* The depth allocation carries guards and is not the surface's
    * parser footprint, so a color-sized depth role refuses. */
   result = r3v_native_record_zb_depth_discovery(
      cmd, vertex_memory, color_memory, wrong_size_memory,
      scenario_selection, arm_selection);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "wrong-size depth memory refuses recording: %d", result);

   result = r3v_native_record_zb_depth_discovery(
      cmd, vertex_memory, color_memory, color_memory, scenario_selection,
      arm_selection);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "aliased color and depth roles refuse recording: %d", result);

   result = r3v_native_record_zb_depth_discovery(
      cmd, vertex_memory, color_memory, depth_memory,
      (enum r3v_native_zb_discovery_scenario)97, arm_selection);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "an unnamed scenario selector refuses recording: %d", result);

   result = r3v_native_record_zb_depth_discovery(
      cmd, vertex_memory, color_memory, depth_memory, scenario_selection,
      (enum r3v_native_zb_discovery_arm)97);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "an unnamed arm selector refuses recording: %d", result);

   result = r3v_native_record_zb_depth_discovery(
      cmd, vertex_memory, color_memory, depth_memory, scenario_selection,
      arm_selection);
   CHECK(result == VK_SUCCESS, "discovery recording on %s: %d",
         scenario->name, result);

   result = vkEndCommandBuffer(cmd);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);

   /* The recorder published the vertex payload, the color fill, and the
    * depth initialization before any submission.  The count is read here,
    * ahead of the readback below: this harness maps the depth allocation
    * itself to check the initial image, and a map of its own would move
    * the counter the check reads.
    */
   CHECK(native_device->drm.cache_sync_count == 3,
         "recorder published vertex, color, and depth: %" PRIu64,
         native_device->drm.cache_sync_count);

   /* The recorded stream carries the state the recorded declaration
    * names, checked here through the same predicate the queue uses. */
   {
      struct r3v_native_cmd_buffer *recorded =
         r3v_native_cmd_buffer_from_handle(cmd);
      const struct r300_zb_depth_discovery_params declared = {
         .scenario = scenario,
         .depth_function = depth_function,
         .depth_write = depth_write,
      };
      CHECK(r300_zb_depth_discovery_check_state(&declared, recorded->ib,
                                                recorded->ib_size_dwords) == 0,
            "the recorded stream carries the declared discovery state");
   }

   /* The initial depth image is the declared one: every byte outside the
    * storage envelope at the guard fill, every slot at the packed
    * initial word.  A discovery observation compares two images and says
    * nothing about whether the first was the declared state, so the fill
    * is checked on its own.
    */
   {
      void *initial_map = NULL;
      result = vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0,
                           &initial_map);
      CHECK(result == VK_SUCCESS && initial_map != NULL,
            "depth memory maps after recording: %d", result);
      uint8_t *declared_image = malloc((size_t)depth_allocation_bytes);
      CHECK(declared_image != NULL, "declared image buffer");
      CHECK(declared_image == NULL ||
               r300_zb_depth_discovery_fill_initial(scenario, &layout,
                                                    declared_image) == 0,
            "scenario %s builds its declared image", scenario->name);
      if (initial_map != NULL && declared_image != NULL) {
         uint64_t wrong_guard = 0;
         uint64_t wrong_slot = 0;
         const uint8_t *bytes = initial_map;
         for (uint64_t off = 0; off < depth_allocation_bytes; off++) {
            if (bytes[off] == declared_image[off])
               continue;
            if (r300_zb_depth_discovery_region_of(
                   &layout, depth_allocation_bytes, off) ==
                R300_ZB_DISCOVERY_REGION_STORAGE)
               wrong_slot++;
            else
               wrong_guard++;
         }
         CHECK(wrong_guard == 0 && wrong_slot == 0,
               "the recorder wrote the declared initial image (%llu guard "
               "bytes, %llu storage bytes differ)",
               (unsigned long long)wrong_guard,
               (unsigned long long)wrong_slot);
      }
      free(declared_image);
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* Each refusal mode alters one recorded fact and leaves everything
    * else as the open mode built it.
    */
   if (refusal_mode) {
      struct r3v_native_cmd_buffer *recorded =
         r3v_native_cmd_buffer_from_handle(cmd);
      if (refuse_unnamed_scenario) {
         /* A selector cast in from outside the catalogue names no
          * scenario.  The recorder refuses one, so only an alteration
          * after recording presents it. */
         recorded->zb_discovery_scenario =
            (enum r3v_native_zb_discovery_scenario)97;
      } else if (refuse_unnamed_arm) {
         recorded->zb_discovery_arm = (enum r3v_native_zb_discovery_arm)97;
      } else if (refuse_arm) {
         /* The declaration and the stream disagreeing about the arm,
          * with the stream unaltered so its digest still matches.  The
          * comparison between the two records is then the one remaining
          * failed condition. */
         recorded->zb_discovery_arm = R3V_NATIVE_ZB_DISCOVERY_ARM_NEVER;
      } else if (refuse_compression) {
         /* ZB_CB_CLEAR set in the emitted ZB_BW_CNTL.  Its set state
          * selects cache-line-granular write-only operation, which
          * leaves the untouched portion of a partially written microtile
          * unknown, so a stream carrying it cannot produce a readable
          * one-pixel address observation. */
         for (uint32_t i = 0; i + 1u < recorded->ib_size_dwords; i++) {
            if (recorded->ib[i] ==
                ((0u << 30) | ((R300_ZB_BW_CNTL >> 2) & 0x1fffu))) {
               recorded->ib[i + 1u] |=
                  R300_ZB_CB_CLEAR_CACHE_LINE_WRITE_ONLY;
               break;
            }
         }
      } else {
         /* A second ZB_FORMAT write appended after the stream the
          * recorder installed.  The one-write grammar is what makes the
          * effective format unambiguous, and the state check refuses a
          * stream carrying two rather than reading its first or its
          * last. */
         uint32_t *extended =
            realloc(recorded->ib,
                    (recorded->ib_size_dwords + 2u) * sizeof(uint32_t));
         CHECK(extended != NULL, "extended stream allocation");
         if (extended != NULL) {
            extended[recorded->ib_size_dwords] =
               (0u << 30) | ((R300_ZB_FORMAT >> 2) & 0x1fffu);
            extended[recorded->ib_size_dwords + 1u] =
               scenario->surface->depth_format;
            recorded->ib = extended;
            recorded->ib_size_dwords += 2u;
         }
      }
   }

   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   if (refusal_mode) {
      CHECK(result == VK_ERROR_DEVICE_LOST, "%s refuses submission: %d",
            argv[1], result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "%s refuses before the ioctl: %s", argv[1],
            r3v_native_queue_status_name(queue_status));
      /* radeon_drm_vk_cs_submit snapshots this immediately before
       * DRM_RADEON_CS, so a zero here says the refusal landed ahead of
       * the ioctl rather than after it. */
      CHECK(native_device->drm.submit_boundary_sync_count == 0,
            "%s reaches no command submission: %" PRIu64, argv[1],
            (uint64_t)native_device->drm.submit_boundary_sync_count);
   } else if (open_gate) {
      CHECK(result == VK_SUCCESS,
            "open-gate submission through the shim: %d", result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED,
            "open-gate shim submission retires: %s",
            r3v_native_queue_status_name(queue_status));

      void *submit_relocs = NULL;
      size_t submit_relocs_size = 0;
      CHECK(read_whole_file(manifest_dir, "submit_relocs.bin",
                            &submit_relocs, &submit_relocs_size) == 0,
            "submit_relocs.bin is retained");
      CHECK(submit_relocs_size ==
               (R300_ZB_DISCOVERY_SLOT_COUNT + 1) *
                  sizeof(struct drm_radeon_cs_reloc),
            "submit object carries three roles plus the completion reloc "
            "(%zu bytes)", submit_relocs_size);
      free(submit_relocs);

      check_retained_ib(manifest_dir, scenario, depth_function, depth_write);

      /* The shim absorbs the submission without executing it, so the
       * honest observation is that nothing moved: the depth image the
       * recorder wrote is the image the readback finds, in every class.
       */
      void *depth_map = NULL;
      result = vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0,
                           &depth_map);
      CHECK(result == VK_SUCCESS && depth_map != NULL,
            "depth memory maps after completion: %d", result);
      uint8_t *reference_image = malloc((size_t)depth_allocation_bytes);
      CHECK(reference_image != NULL, "reference image buffer");
      if (depth_map != NULL && reference_image != NULL) {
         CHECK(r300_zb_depth_discovery_fill_initial(
                  scenario, &layout, reference_image) == 0,
               "the declared image builds for the shim comparison");

         struct r300_zb_discovery_observation observation;
         r300_zb_depth_discovery_observe(scenario, &layout, reference_image,
                                         depth_map, depth_allocation_bytes,
                                         &observation);
         CHECK(observation.judged, "the shim readback is judged");
         CHECK(observation.depth_locations == 0 &&
                  observation.change_count == 0 &&
                  observation.guard_bytes_changed == 0 &&
                  observation.unclaimed_bytes_changed == 0,
               "shim run moves no byte (depth %u changes %u guard %llu "
               "unclaimed %llu)", observation.depth_locations,
               observation.change_count,
               (unsigned long long)observation.guard_bytes_changed,
               (unsigned long long)observation.unclaimed_bytes_changed);
         /* The three classes partition the allocation exactly, which is
          * the relation the constant allocation over a moving envelope
          * exists to keep honest. */
         CHECK(observation.guard_bytes_inspected +
                  observation.unclaimed_bytes_inspected +
                  (uint64_t)observation.slots_inspected * 4u ==
                  depth_allocation_bytes,
               "guard, unclaimed, and storage partition the allocation");
         CHECK(observation.unclaimed_bytes_inspected ==
                  depth_allocation_bytes - layout.total_bytes,
               "unclaimed bytes are the slack past the layout: %llu",
               (unsigned long long)observation.unclaimed_bytes_inspected);
      }
      free(reference_image);

      /* The color target is untouched, so the coordinate oracle reports
       * the declared pixel uncolored rather than exact. */
      void *color_map = NULL;
      result = vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                           &color_map);
      CHECK(result == VK_SUCCESS && color_map != NULL,
            "color memory maps after completion: %d", result);
      if (color_map != NULL) {
         struct r300_zb_discovery_color_verdict color_verdict;
         r300_zb_depth_discovery_color_observe(
            color_map, R300_ZB_DISCOVERY_COLOR_BYTES,
            R300_ZB_DISCOVERY_PITCH_PIXELS, R300_ZB_DISCOVERY_TARGET_WIDTH,
            R300_ZB_DISCOVERY_TARGET_HEIGHT, scenario->pixel_x,
            scenario->pixel_y, 1u, 1u, R300_TRIANGLE_COLOR_SENTINEL,
            R300_TRIANGLE_DRAW_COLOR_B8G8R8A8, &color_verdict);
         CHECK(color_verdict.judged && !color_verdict.exact &&
                  color_verdict.inside_colored == 0 &&
                  color_verdict.outside_colored == 0 &&
                  color_verdict.beyond_changed == 0,
               "shim run leaves the color sentinel intact (inside %u "
               "outside %u beyond %u)", color_verdict.inside_colored,
               color_verdict.outside_colored, color_verdict.beyond_changed);
      }
   } else {
      CHECK(result == VK_ERROR_DEVICE_LOST, "closed gate fails closed: %d",
            result);
      CHECK(queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED,
            "closed gate reports refusal before the ioctl: %s",
            r3v_native_queue_status_name(queue_status));

      check_retained_ib(manifest_dir, scenario, depth_function, depth_write);

      void *reloc_data = NULL;
      size_t reloc_size = 0;
      CHECK(read_whole_file(manifest_dir, "relocs.bin", &reloc_data,
                            &reloc_size) == 0,
            "manifest relocs.bin is retained");
      if (reloc_data != NULL) {
         CHECK(reloc_size == R300_ZB_DISCOVERY_SLOT_COUNT *
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
      printf("r3v_native_zb_depth_discovery_harness(%s): all checks passed\n",
             argv[1]);
      return 0;
   }
   return 1;
}
