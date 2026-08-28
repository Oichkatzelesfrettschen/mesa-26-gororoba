/*
 * SPDX-License-Identifier: MIT
 *
 * Attended two-pass cell: records two render-shape cells through
 * r3v_native_record_multi_pass -- the first installed, the second
 * appended through the primitive the public two-draw command buffer
 * takes -- and drives the concatenation to a live DRM_RADEON_CS on
 * RS482 silicon.  Each pass draws the reference triangle into its own
 * target with its own fragment constant, so each target names the pass
 * that wrote it and the offline emitter's digest is the stream's.  Runs
 * only under the authorization and procedure in
 * docs/hardware/r3v-native-attended-multi-pass-procedure.md; every
 * stage prints and flushes before it runs.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_watchdog_guard.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native_multi_pass_arms.h"

#include "util/mesa-blake3.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* Both targets carry the sentinel before the submission, so every dword
 * either target holds afterward names its writer: the first pass's
 * constant, the second's, or neither.
 */
#define MULTI_PASS_ALLOCATION_BYTES 65536u

static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

static bool
same_directory(const char *a, const char *b)
{
   if (strcmp(a, b) == 0)
      return true;
   char resolved_a[PATH_MAX];
   char resolved_b[PATH_MAX];
   return realpath(a, resolved_a) != NULL && realpath(b, resolved_b) != NULL &&
          strcmp(resolved_a, resolved_b) == 0;
}

static int
write_target(const char *dir, const char *name, const void *data, size_t size)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "wb");
   if (f == NULL)
      return 1;
   size_t written = fwrite(data, 1, size, f);
   return fclose(f) != 0 || written != size;
}

static void
report(const char *label, const struct r300_triangle_coverage_verdict *v)
{
   printf("[oracle] %s judged=%d coverage_exact=%d canary=%d interior=%u "
          "analytic=%u exterior=%u ambiguous=%u mismatch=%u\n",
          label, v->judged, v->coverage_exact, v->canary_pass,
          v->interior_pixels, v->analytic_pixels, v->exterior_pixels,
          v->ambiguous_pixels, v->mismatch_pixels);
   if (!v->judged)
      printf("[oracle] %s verdict refused: the shape or the retained "
             "footprint left the producer's domain, so the zero counters "
             "carry no claim about the render\n",
             label);
   fflush(stdout);
}

int
main(int argc, char **argv)
{
   /* --record-only builds every object and records the command buffer,
    * then stops at the recording boundary.  The argument handling, the
    * seeding, and the recording contract -- allocation size, both
    * vertex layouts, the merged relocation binding -- are what this
    * program can get wrong with no device present, so the shim fixture
    * calibrates them here and the attended run inherits a proven
    * sequence.
    */
   bool record_only = false;
   const char *waiver_path = NULL;
   bool usage_error = argc < 2;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--record-only") == 0)
         record_only = true;
      else if (strcmp(argv[i], "--waiver") == 0 && i + 1 < argc)
         waiver_path = argv[++i];
      else
         usage_error = true;
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <evidence-directory> [--record-only] "
              "[--waiver <path>]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   /* A silicon verdict binds to the real libc entry points, so a
    * preloaded interposer refuses before the first Vulkan call.  The
    * recording mode reaches no ioctl and reports no verdict, so it runs
    * on the fixture.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return 1;
   }

   /* Both passes take the reference shape; the second brings its own
    * vertex page, target, and fragment constant, bound at merged
    * indices 2 and 3.
    */
   struct r300_triangle_multi_pass mp;
   r3v_native_multi_pass_reference(&mp);

   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&mp.pass[0]);
   const uint32_t first_dword =
      r300_tcl_bypass_triangle_render_shape_draw_dword(&mp.pass[0]);
   const uint32_t second_dword =
      r300_tcl_bypass_triangle_render_shape_draw_dword(&mp.pass[1]);

   /* The emitter's output is the bound form the recorder installs, so
    * its digest is the arming report's and the operator compares it
    * against the authorization before the submission runs.
    */
   struct r300_tcl_bypass_triangle_ib armed;
   if (r300_tcl_bypass_triangle_multi_pass_emit(&mp, &armed) != 0) {
      fprintf(stderr, "the two-pass cell refused to emit\n");
      return 1;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed.ib, armed.ib_size_dwords, digest);
   const uint32_t ib_dwords = armed.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&armed);

   printf("[shape] two passes %ux%u pitch %u, binding (%u, %u), "
          "%u IB dwords, cell blake3 %.8s\n",
          mp.pass[0].width, mp.pass[0].height, mp.pass[0].pitch_pixels,
          mp.second_vertex_index, mp.second_color_index, ib_dwords, digest);
   printf("[predict] first target: interior 0x%08x over the analytic "
          "triangle, exterior 0x%08x, canary clean; second target: "
          "interior 0x%08x, exterior 0x%08x, canary clean\n",
          first_dword, R300_TRIANGLE_COLOR_SENTINEL, second_dword,
          R300_TRIANGLE_COLOR_SENTINEL);
   printf("[predict] falsifier: a target holding the other pass's constant "
          "names state crossing the pass boundary; a second target holding "
          "the sentinel names a second cell the command processor never "
          "reached\n");
   fflush(stdout);

   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared != NULL && declared[0] != '\0' &&
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s\n",
              declared, evidence_dir);
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", evidence_dir, 1);

   struct r3v_native_watchdog_guard guard = {0};
   if (!record_only) {
      stage("watchdog");
      if (r3v_native_watchdog_guard_open(&guard, waiver_path, evidence_dir,
                                         digest) != 0)
         return 2;
   }

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", result);
      return 1;
   }

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || pdev_count != 1 ||
       pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS482\n");
      return 1;
   }

   stage("device");
   const float priority = 1.0f;
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
               .pQueuePriorities = &priority,
            },
      },
      NULL, &device);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %d\n", result);
      return 1;
   }

   r3v_native_watchdog_guard_install(&guard, device);

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

#define CHECK(call)                                        \
   do {                                                    \
      VkResult check_result = (call);                      \
      if (check_result != VK_SUCCESS) {                    \
         fprintf(stderr, "%s: %d\n", #call, check_result); \
         return 1;                                         \
      }                                                    \
   } while (0)

   /* The four buffer objects the two passes reach: each pass's vertex
    * page and color target.
    */
   stage("memory");
   enum { MEM_FIRST_VERTEX, MEM_FIRST_TARGET, MEM_SECOND_VERTEX,
          MEM_SECOND_TARGET, MEM_COUNT };
   VkDeviceMemory memory[MEM_COUNT] = { VK_NULL_HANDLE };
   for (unsigned i = 0; i < MEM_COUNT; i++) {
      CHECK(vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = MULTI_PASS_ALLOCATION_BYTES,
            .memoryTypeIndex = 0,
         },
         NULL, &memory[i]));
   }

   /* Both passes fetch four-dword position records of the reference
    * triangle from their own page.
    */
   stage("seed");
   for (unsigned i = 0; i < 2; i++) {
      const unsigned page = i == 0 ? MEM_FIRST_VERTEX : (unsigned)MEM_SECOND_VERTEX;
      float vertices[R300_TRIANGLE_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_render_shape_vertices(&mp.pass[i], vertices);
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[page], 0, VK_WHOLE_SIZE, 0, &map));
      memcpy(map, vertices, sizeof(vertices));
      vkUnmapMemory(device, memory[page]);
   }
   for (unsigned i = 0; i < 2; i++) {
      const unsigned target =
         i == 0 ? MEM_FIRST_TARGET : (unsigned)MEM_SECOND_TARGET;
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[target], 0, VK_WHOLE_SIZE, 0, &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < MULTI_PASS_ALLOCATION_BYTES / 4; p++)
         pixels[p] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, memory[target]);
   }

   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   CHECK(vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool));
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   CHECK(vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd));
   CHECK(vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           }));
   CHECK(r3v_native_record_multi_pass(
      cmd, memory[MEM_FIRST_VERTEX], memory[MEM_FIRST_TARGET],
      memory[MEM_SECOND_VERTEX], memory[MEM_SECOND_TARGET], &mp));
   CHECK(vkEndCommandBuffer(cmd));

   if (record_only) {
      printf("record: ACCEPTED\n");
      fflush(stdout);
      vkDestroyCommandPool(device, pool, NULL);
      for (unsigned i = 0; i < MEM_COUNT; i++)
         vkFreeMemory(device, memory[i], NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 0;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: a live DRM_RADEON_CS reaches the command processor
    * here, and the bounded completion wait follows it.
    */
   stage("submit");
   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   printf("[submit] vkQueueSubmit returned %d\n", result);
   fflush(stdout);

   /* The counter comes to rest before the first target read, because a
    * fire after a good submission destroys the result and spends the
    * attempt.
    */
   if (r3v_native_watchdog_guard_close(&guard, result) != 0)
      return 1;

   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *first_map = NULL;
   void *second_map = NULL;
   CHECK(vkMapMemory(device, memory[MEM_FIRST_TARGET], 0, VK_WHOLE_SIZE, 0,
                     &first_map));
   CHECK(vkMapMemory(device, memory[MEM_SECOND_TARGET], 0, VK_WHOLE_SIZE, 0,
                     &second_map));
   if (write_target(evidence_dir, "first_target.bin", first_map,
                    color_bytes) != 0 ||
       write_target(evidence_dir, "second_target.bin", second_map,
                    color_bytes) != 0) {
      fprintf(stderr, "target retention failed\n");
      return 1;
   }

   /* Each target under its own pass's constant, then under the other
    * pass's: a target exact under the other constant names state that
    * crossed the pass boundary.
    */
   struct r300_triangle_coverage_verdict first_verdict, second_verdict;
   struct r300_triangle_coverage_verdict first_crossed, second_crossed;
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[0], &first_dword, 1, R300_TRIANGLE_COLOR_SENTINEL, first_map,
      color_bytes, &first_verdict);
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[1], &second_dword, 1, R300_TRIANGLE_COLOR_SENTINEL,
      second_map, color_bytes, &second_verdict);
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[0], &second_dword, 1, R300_TRIANGLE_COLOR_SENTINEL, first_map,
      color_bytes, &first_crossed);
   r300_tcl_bypass_triangle_coverage_oracle(
      &mp.pass[1], &first_dword, 1, R300_TRIANGLE_COLOR_SENTINEL, second_map,
      color_bytes, &second_crossed);
   report("first", &first_verdict);
   report("second", &second_verdict);
   report("first-under-second-constant", &first_crossed);
   report("second-under-first-constant", &second_crossed);

   const uint32_t *second_pixels = second_map;
   const uint32_t cx = mp.pass[1].width / 2;
   const uint32_t cy = (mp.pass[1].height * 3) / 8;
   printf("[oracle] second centroid (%u,%u)=0x%08x predicted 0x%08x "
          "corner (0,0)=0x%08x\n",
          cx, cy, second_pixels[cy * mp.pass[1].pitch_pixels + cx],
          second_dword, second_pixels[0]);
   fflush(stdout);

   const bool second_unreached =
      second_verdict.judged && second_verdict.analytic_pixels != 0 &&
      second_verdict.interior_pixels == 0 &&
      second_verdict.exterior_pixels ==
         mp.pass[1].width * mp.pass[1].height;
   const bool crossed = first_crossed.coverage_exact ||
                        second_crossed.coverage_exact;
   printf("[classify] %s\n",
          crossed ? "a target carries the other pass's constant; state "
                    "crossed the pass boundary"
          : second_unreached
             ? "the second target holds the sentinel; the command "
               "processor never reached the second cell"
             : "each target carries its own pass's constant over the "
               "analytic triangle");
   fflush(stdout);

   const bool pass = first_verdict.judged && first_verdict.coverage_exact &&
                     first_verdict.canary_pass && second_verdict.judged &&
                     second_verdict.coverage_exact &&
                     second_verdict.canary_pass && !crossed;

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < MEM_COUNT; i++)
      vkFreeMemory(device, memory[i], NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("[verdict] %s\n",
          pass ? "two passes executed through one indirect buffer, each "
                 "target holding its own pass's constant"
          : crossed ? "state crossed the pass boundary; the finding is the "
                      "concatenation's contract"
          : second_unreached
             ? "the second cell was not reached; the finding is the "
               "concatenation"
             : "prediction deviated; the deviation is the finding");
   return pass ? 0 : 1;
}
