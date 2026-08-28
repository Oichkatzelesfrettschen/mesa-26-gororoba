/*
 * SPDX-License-Identifier: MIT
 *
 * Attended multisample resolve cell: records the resolve cell through
 * r3v_native_record_msaa_resolve and drives it to a live
 * DRM_RADEON_CS on RS482 silicon.  The render half draws the analytic
 * triangle into a sample-expanded surface with the subsample set live,
 * then the resolve half covers the extent under
 * RB3D_AARESOLVE_CTL.AARESOLVE_MODE_RESOLVE with a fragment constant no
 * multisample sample holds, so one submission classifies the resolve
 * semantics rather than confirming one reading of them.  Runs only
 * under the authorization and procedure in
 * docs/hardware/r3v-native-attended-msaa-resolve-procedure.md; every
 * stage prints and flushes before it runs.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"
#include "r3v_native_msaa_arms.h"
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

#define MSAA_ALLOCATION_BYTES 65536u

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
report(const char *label, const struct r300_triangle_sample_set_verdict *v)
{
   printf("[oracle] %s judged=%d interior_exact=%d interior=%u analytic=%u "
          "unjudged=%u\n",
          label, v->judged, v->interior_exact, v->interior_pixels,
          v->analytic_pixels, v->unjudged_pixels);
   if (!v->judged)
      printf("[oracle] %s verdict refused: the shape or the retained "
             "footprint left the producer's domain, so the zero counters "
             "carry no claim about the resolve\n",
             label);
   fflush(stdout);
}

int
main(int argc, char **argv)
{
   /* --record-only builds every object and records the command buffer,
    * then stops at the recording boundary.  The argument handling, the
    * seeding, the device-local surface allocation, and the merged
    * relocation binding are what this program can get wrong with no
    * device present, so the shim fixture calibrates them here and the
    * attended run inherits a proven sequence.
    */
   bool record_only = false;
   const char *waiver_path = NULL;
   uint32_t sample_count = 4;
   bool usage_error = argc < 2;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--record-only") == 0)
         record_only = true;
      else if (strcmp(argv[i], "--waiver") == 0 && i + 1 < argc)
         waiver_path = argv[++i];
      else if (strcmp(argv[i], "--samples") == 0 && i + 1 < argc)
         sample_count = (uint32_t)strtoul(argv[++i], NULL, 0);
      else
         usage_error = true;
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <evidence-directory> [--samples 2|4] "
              "[--record-only] [--waiver <path>]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   /* A silicon verdict binds to the real libc entry points, so a
    * preloaded interposer refuses before the first Vulkan call.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return 1;
   }

   struct r300_triangle_msaa_resolve msaa;
   r3v_native_msaa_reference(&msaa, sample_count);

   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&msaa.destination);
   const uint32_t downsample_dword = r3v_native_msaa_downsample_dword(&msaa);
   const uint32_t fragment_dword = r3v_native_msaa_fragment_dword(&msaa);

   struct r300_tcl_bypass_triangle_ib armed;
   if (r300_tcl_bypass_triangle_msaa_resolve_emit(&msaa, &armed) != 0) {
      fprintf(stderr, "the resolve cell refused to emit\n");
      return 1;
   }
   if (r300_tcl_bypass_triangle_bind_reloc_indices(
          &armed, r300_tcl_bypass_triangle_msaa_slot_index,
          R300_TRIANGLE_SLOT_COUNT) != 0) {
      fprintf(stderr, "the resolve cell refused to bind\n");
      r300_tcl_bypass_triangle_release(&armed);
      return 1;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed.ib, armed.ib_size_dwords, digest);
   const uint32_t ib_dwords = armed.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&armed);

   /* The judged footprint is the subsample-set one, a strict subset of
    * the pixel-center footprint: a resolved pixel whose samples straddle
    * an edge carries a blend no single dword names, so it stays
    * unjudged and counted.
    */
   printf("[shape] %ux%u pitch %u at %ux MSAA, destination %ux%u pitch %u, "
          "%u IB dwords, cell blake3 %.8s\n",
          msaa.render.width, msaa.render.height, msaa.render.pitch_pixels,
          sample_count, msaa.destination.width, msaa.destination.height,
          msaa.destination.pitch_pixels, ib_dwords, digest);
   printf("[predict] downsampled samples read 0x%08x, a fragment write "
          "reads 0x%08x, a mixture reads neither, and an unwritten "
          "destination reads the 0x%08x seed\n",
          downsample_dword, fragment_dword, R300_TRIANGLE_COLOR_SENTINEL);
   printf("[predict] the four passes share one denominator, so exactly "
          "one of them reads interior_exact=1 unless the destination "
          "carries an order other than the linear one\n");
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

   /* Three caller allocations reach the cell; the multisample surface
    * is the recorder's own device-local allocation, which the host
    * never maps, so it takes no memory type here.
    */
   stage("memory");
   enum { MEM_RENDER_VERTEX, MEM_COVER_VERTEX, MEM_DESTINATION, MEM_COUNT };
   VkDeviceMemory memory[MEM_COUNT] = { VK_NULL_HANDLE };
   for (unsigned i = 0; i < MEM_COUNT; i++) {
      CHECK(vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = MSAA_ALLOCATION_BYTES,
            .memoryTypeIndex = 0,
         },
         NULL, &memory[i]));
   }

   /* The render half fetches the analytic triangle; the resolve half
    * fetches the cover triangle whose interior holds every pixel center
    * in the extent, since a resolve emits only for the pixels a
    * fragment covers.
    */
   stage("seed");
   {
      float render_vertices[R300_TRIANGLE_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_render_shape_vertices(&msaa.render,
                                                     render_vertices);
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[MEM_RENDER_VERTEX], 0, VK_WHOLE_SIZE, 0,
                        &map));
      memcpy(map, render_vertices, sizeof(render_vertices));
      vkUnmapMemory(device, memory[MEM_RENDER_VERTEX]);
   }
   {
      float cover_vertices[R300_TRIANGLE_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_cover_vertices(&msaa.render, cover_vertices);
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[MEM_COVER_VERTEX], 0, VK_WHOLE_SIZE, 0,
                        &map));
      memcpy(map, cover_vertices, sizeof(cover_vertices));
      vkUnmapMemory(device, memory[MEM_COVER_VERTEX]);
   }
   {
      /* The destination carries the sentinel, so a resolve that wrote
       * nothing and one that wrote correctly are distinct results.  The
       * multisample surface stays uncleared, which is the arm's named
       * scope cut: the destination's exterior receives whatever the
       * surface held and stays unjudged.
       */
      void *map = NULL;
      CHECK(vkMapMemory(device, memory[MEM_DESTINATION], 0, VK_WHOLE_SIZE, 0,
                        &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < MSAA_ALLOCATION_BYTES / 4; p++)
         pixels[p] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, memory[MEM_DESTINATION]);
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
   CHECK(r3v_native_record_msaa_resolve(cmd, memory[MEM_RENDER_VERTEX],
                                        memory[MEM_COVER_VERTEX],
                                        memory[MEM_DESTINATION], &msaa));
   CHECK(vkEndCommandBuffer(cmd));

   /* The recorder allocated the multisample surface in VRAM with no
    * fallback domain, so reaching this line is the placement report.
    */
   printf("[placement] multisample surface %llu bytes, "
          "RADEON_GEM_DOMAIN_VRAM with no fallback, host mapping absent\n",
          (unsigned long long)r3v_native_cmd_buffer_from_handle(cmd)
             ->owned_multisample->bo.size);
   fflush(stdout);

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

   if (r3v_native_watchdog_guard_close(&guard, result) != 0)
      return 1;

   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *destination_map = NULL;
   CHECK(vkMapMemory(device, memory[MEM_DESTINATION], 0, VK_WHOLE_SIZE, 0,
                     &destination_map));
   if (write_target(evidence_dir, "resolve_destination.bin", destination_map,
                    color_bytes) != 0) {
      fprintf(stderr, "target retention failed\n");
      return 1;
   }

   /* Four passes over one denominator, each admitting a single dword:
    * the downsampled samples, the resolve half's fragment constant, the
    * pre-submission seed, and the union of the two writes.  A mixture
    * that both writers reached order-dependently reads exact under the
    * union alone.
    */
   const uint32_t either_dwords[2] = { downsample_dword, fragment_dword };
   struct r300_triangle_sample_set_verdict downsample, fragment, seed, either;
   r300_tcl_bypass_triangle_sample_set_oracle(
      &msaa.destination, sample_count, &downsample_dword, 1, destination_map,
      color_bytes, &downsample);
   r300_tcl_bypass_triangle_sample_set_oracle(
      &msaa.destination, sample_count, &fragment_dword, 1, destination_map,
      color_bytes, &fragment);
   const uint32_t sentinel = R300_TRIANGLE_COLOR_SENTINEL;
   r300_tcl_bypass_triangle_sample_set_oracle(&msaa.destination, sample_count,
                                              &sentinel, 1, destination_map,
                                              color_bytes, &seed);
   r300_tcl_bypass_triangle_sample_set_oracle(&msaa.destination, sample_count,
                                              either_dwords, 2,
                                              destination_map, color_bytes,
                                              &either);
   report("downsample", &downsample);
   report("fragment", &fragment);
   report("seed", &seed);
   report("either", &either);

   /* The byte-order falsifier is decidable without a tiling model: a
    * destination that holds the predicted dwords in the wrong places
    * carries the same multiset as one that holds them in the right
    * ones, so the footprint census separates a permuted order from an
    * absent write.
    */
   /* The census walks the render footprint from the target base, the
    * same window the oracle judges and a subset of the retained bytes,
    * so its counts describe what resolve_destination.bin holds.
    */
   const uint32_t *pixels =
      (const uint32_t *)destination_map + msaa.destination.target_offset / 4u;
   const uint32_t footprint_pixels =
      msaa.destination.pitch_pixels * msaa.destination.height;
   uint32_t census_downsample = 0, census_fragment = 0, census_seed = 0;
   for (uint32_t p = 0; p < footprint_pixels; p++) {
      if (pixels[p] == downsample_dword)
         census_downsample++;
      else if (pixels[p] == fragment_dword)
         census_fragment++;
      else if (pixels[p] == R300_TRIANGLE_COLOR_SENTINEL)
         census_seed++;
   }
   printf("[census] footprint %u pixels holds downsample %u fragment %u "
          "seed %u\n",
          footprint_pixels, census_downsample, census_fragment, census_seed);

   const uint32_t cx = msaa.destination.width / 2;
   const uint32_t cy = (msaa.destination.height * 3) / 8;
   printf("[oracle] destination centroid (%u,%u)=0x%08x corner (0,0)=0x%08x\n",
          cx, cy, pixels[cy * msaa.destination.pitch_pixels + cx], pixels[0]);
   fflush(stdout);

   /* The classification the run exists for.  Each reading of
    * AARESOLVE_MODE names one exact pass, so the verdict reports which
    * one the silicon took rather than whether a single expectation
    * held.
    */
   const char *semantics;
   if (downsample.interior_exact)
      semantics = "AARESOLVE_MODE_RESOLVE emits the downsampled samples "
                  "and the fragment supplies coverage alone";
   else if (fragment.interior_exact)
      semantics = "the resolve half's fragment write reaches the "
                  "destination";
   else if (seed.interior_exact)
      semantics = "the resolve wrote nothing; the destination holds the "
                  "pre-submission seed";
   else if (either.interior_exact)
      semantics = "both writes reach the destination order-dependently; "
                  "the mixture is the finding";
   else if (census_downsample != 0 || census_fragment != 0)
      semantics = "the predicted dwords are present outside their linear "
                  "positions; the destination byte order is the finding";
   else
      semantics = "the destination holds none of the predicted dwords; "
                  "the addressing or the coverage is the finding";
   printf("[classify] %s\n", semantics);

   const bool judged = downsample.judged && fragment.judged && seed.judged &&
                       either.judged;
   const bool classified =
      judged && (downsample.interior_exact || fragment.interior_exact ||
                 seed.interior_exact || either.interior_exact);

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < MEM_COUNT; i++)
      vkFreeMemory(device, memory[i], NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("[verdict] %s\n",
          classified ? "the resolve destination classifies under one "
                       "admitted dword over the subsample-set denominator"
                     : "no single reading classifies the destination; the "
                       "deviation is the finding");
   return classified ? 0 : 1;
}
