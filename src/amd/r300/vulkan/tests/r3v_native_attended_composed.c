/*
 * SPDX-License-Identifier: MIT
 *
 * Attended composed render-then-sample cell: records the composed cell
 * through r3v_native_record_composed_render_sample and drives it to a
 * live DRM_RADEON_CS on RS482 silicon.  The render half draws into the
 * first target and the sample half samples that target as its texture,
 * so one indirect buffer carries the destination-cache flush ahead of
 * the texture-tag invalidate and the second target holds what the first
 * one received.  Runs only under the authorization and procedure in
 * docs/hardware/r3v-native-attended-composed-render-sample-procedure.md;
 * every stage prints and flushes before it runs.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_watchdog_guard.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

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
 * either target holds afterward names its writer.  The sample half's
 * texture fetch reads the first target, so an interior that reads the
 * sentinel says the fetch ran before the render half's writes were
 * visible to the TX block: the named alternative to the predicted
 * result, and an RCA on the cell's coherency edge rather than a
 * prediction to revise.
 */
#define COMPOSED_ALLOCATION_BYTES 65536u

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
   printf("[oracle] %s coverage_exact=%d canary=%d interior=%u analytic=%u "
          "exterior=%u ambiguous=%u mismatch=%u\n",
          label, v->coverage_exact, v->canary_pass, v->interior_pixels,
          v->analytic_pixels, v->exterior_pixels, v->ambiguous_pixels,
          v->mismatch_pixels);
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

   /* Both halves take the reference shape, so the sample half's texture
    * geometry is the render half's target at the same extent, pitch, and
    * lane order.  TEX0 reads (x / 64, y / 64) at each of the three
    * vertices (r300_tcl_bypass_triangle_varying_vertices against the
    * window positions), so a nearest fetch at pixel center (x + 0.5,
    * y + 0.5) lands on texel (x, y) a half texel from either boundary:
    * the sample half reproduces the render half's coverage pixel for
    * pixel, and both targets take one predicted interior dword.
    */
   struct r300_triangle_composed_render_sample composed;
   r300_tcl_bypass_triangle_render_shape_reference(&composed.render);
   r300_tcl_bypass_triangle_render_shape_reference(&composed.sample);
   composed.sample.target_offset = 0;

   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&composed.render);
   const uint32_t draw_dword =
      r300_tcl_bypass_triangle_render_shape_draw_dword(&composed.render);

   /* The digest of the cell in the form the recorder installs: emitted,
    * then bound to the merged relocation indices.  The arming report
    * carries the same value, and printing it here lets the operator
    * compare the gate's authorization against the cell this binary
    * records before the submission runs.
    */
   struct r300_tcl_bypass_triangle_ib armed;
   if (r300_tcl_bypass_triangle_composed_render_sample_emit(&composed,
                                                            &armed) != 0) {
      fprintf(stderr, "the composed cell refused to emit\n");
      return 1;
   }
   if (r300_tcl_bypass_triangle_bind_reloc_indices(
          &armed, r300_tcl_bypass_triangle_composed_slot_index,
          R300_TRIANGLE_SLOT_COUNT) != 0) {
      fprintf(stderr, "the composed cell refused to bind\n");
      r300_tcl_bypass_triangle_release(&armed);
      return 1;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed.ib, armed.ib_size_dwords, digest);
   const uint32_t ib_dwords = armed.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&armed);

   printf("[shape] render %ux%u pitch %u, sample %ux%u pitch %u, "
          "%u IB dwords, cell blake3 %.8s\n",
          composed.render.width, composed.render.height,
          composed.render.pitch_pixels, composed.sample.width,
          composed.sample.height, composed.sample.pitch_pixels, ib_dwords,
          digest);
   printf("[predict] both targets: interior 0x%08x over the analytic "
          "triangle, exterior 0x%08x, canary clean\n",
          draw_dword, R300_TRIANGLE_COLOR_SENTINEL);
   printf("[predict] falsifier: a sample-target interior reading 0x%08x "
          "names a texture fetch ahead of the render half's publication\n",
          R300_TRIANGLE_COLOR_SENTINEL);
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

   /* The four buffer objects the cell's five slots reach: the render
    * half's vertex array and target, then the sample half's vertex
    * array and target, with the render target filling the sample half's
    * texture slot as well.
    */
   stage("memory");
   enum { MEM_RENDER_VERTEX, MEM_RENDER_TARGET, MEM_SAMPLE_VERTEX,
          MEM_SAMPLE_TARGET, MEM_COUNT };
   VkDeviceMemory memory[MEM_COUNT] = { VK_NULL_HANDLE };
   for (unsigned i = 0; i < MEM_COUNT; i++) {
      CHECK(vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = COMPOSED_ALLOCATION_BYTES,
            .memoryTypeIndex = 0,
         },
         NULL, &memory[i]));
   }

   /* Each half fetches its own record layout: four-dword positions for
    * the render half, eight-dword position-plus-TEX0 records for the
    * sample half.
    */
   stage("seed");
   {
      float render_vertices[R300_TRIANGLE_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_render_shape_vertices(&composed.render,
                                                     render_vertices);
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[MEM_RENDER_VERTEX], 0, VK_WHOLE_SIZE, 0,
                        &map));
      memcpy(map, render_vertices, sizeof(render_vertices));
      vkUnmapMemory(device, memory[MEM_RENDER_VERTEX]);
   }
   {
      float sample_vertices[R300_TRIANGLE_VARYING_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_varying_shape_vertices(&composed.sample,
                                                      sample_vertices);
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[MEM_SAMPLE_VERTEX], 0, VK_WHOLE_SIZE, 0,
                        &map));
      memcpy(map, sample_vertices, sizeof(sample_vertices));
      vkUnmapMemory(device, memory[MEM_SAMPLE_VERTEX]);
   }
   for (unsigned i = 0; i < 2; i++) {
      const unsigned target =
         i == 0 ? MEM_RENDER_TARGET : (unsigned)MEM_SAMPLE_TARGET;
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[target], 0, VK_WHOLE_SIZE, 0, &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < COMPOSED_ALLOCATION_BYTES / 4; p++)
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
   CHECK(r3v_native_record_composed_render_sample(
      cmd, memory[MEM_RENDER_VERTEX], memory[MEM_RENDER_TARGET],
      memory[MEM_SAMPLE_VERTEX], memory[MEM_SAMPLE_TARGET], &composed));
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
   void *render_map = NULL;
   void *sample_map = NULL;
   CHECK(vkMapMemory(device, memory[MEM_RENDER_TARGET], 0, VK_WHOLE_SIZE, 0,
                     &render_map));
   CHECK(vkMapMemory(device, memory[MEM_SAMPLE_TARGET], 0, VK_WHOLE_SIZE, 0,
                     &sample_map));
   if (write_target(evidence_dir, "render_target.bin", render_map,
                    color_bytes) != 0 ||
       write_target(evidence_dir, "sample_target.bin", sample_map,
                    color_bytes) != 0) {
      fprintf(stderr, "target retention failed\n");
      return 1;
   }

   struct r300_triangle_coverage_verdict render_verdict, sample_verdict;
   r300_tcl_bypass_triangle_coverage_oracle(
      &composed.render, &draw_dword, 1, R300_TRIANGLE_COLOR_SENTINEL,
      render_map, color_bytes, &render_verdict);
   r300_tcl_bypass_triangle_coverage_oracle(
      &composed.sample, &draw_dword, 1, R300_TRIANGLE_COLOR_SENTINEL,
      sample_map, color_bytes, &sample_verdict);
   report("render", &render_verdict);
   report("sample", &sample_verdict);

   const uint32_t *sample_pixels = sample_map;
   const uint32_t cx = composed.sample.width / 2;
   const uint32_t cy = (composed.sample.height * 3) / 8;
   printf("[oracle] sample centroid (%u,%u)=0x%08x predicted 0x%08x "
          "corner (0,0)=0x%08x\n",
          cx, cy, sample_pixels[cy * composed.sample.pitch_pixels + cx],
          draw_dword, sample_pixels[0]);
   fflush(stdout);

   /* The two failure modes the sample target separates: an interior
    * that reads the sentinel over the whole analytic triangle names a
    * texture fetch ahead of the render half's publication, while an
    * interior that reads anything else names the fetch's addressing or
    * the coverage.  The counters carry the distinction, so the verdict
    * classifies the run instead of reporting one aggregate flag.
    */
   const uint32_t analytic = sample_verdict.analytic_pixels;
   const bool sample_unpublished = analytic != 0 &&
                                   sample_verdict.interior_pixels == 0 &&
                                   sample_verdict.exterior_pixels ==
                                      composed.sample.width *
                                         composed.sample.height;
   printf("[classify] sample interior %u of %u analytic, exterior %u of "
          "%u classified%s\n",
          sample_verdict.interior_pixels, analytic,
          sample_verdict.exterior_pixels,
          composed.sample.width * composed.sample.height,
          sample_unpublished
             ? "; the render half's writes reached no texture fetch"
             : "");
   fflush(stdout);

   const bool pass = render_verdict.coverage_exact &&
                     render_verdict.canary_pass &&
                     sample_verdict.coverage_exact &&
                     sample_verdict.canary_pass;

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < MEM_COUNT; i++)
      vkFreeMemory(device, memory[i], NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("[verdict] %s\n",
          pass ? "the sample target reproduces the render target's coverage "
                 "through one indirect buffer"
               : sample_unpublished
                    ? "the texture fetch read the pre-render sentinel; the "
                      "cell's coherency edge is the finding"
                    : "prediction deviated; the deviation is the finding");
   return pass ? 0 : 1;
}
