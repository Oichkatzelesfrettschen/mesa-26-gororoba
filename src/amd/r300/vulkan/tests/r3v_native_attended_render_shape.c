/*
 * SPDX-License-Identifier: MIT
 *
 * Attended render-shape cell: submits the TCL-bypass triangle over a
 * declared extent, pitch, lane order, and fragment constant to RS482
 * silicon through the native ICD and reports the render-shape oracle's
 * verdict.  This program performs a live DRM_RADEON_CS and runs only
 * under the authorization and procedure in
 * docs/hardware/r3v-native-attended-render-shape-procedure.md; the
 * driver's arming conjunction admits it, and every stage prints and
 * flushes before it runs so a hang names the stage it hung in.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native_render_shape_args.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);
VkResult r3v_native_record_tcl_bypass_triangle_render_shape(
   VkCommandBuffer commandBuffer, VkDeviceMemory vertexMemory,
   VkDeviceMemory colorMemory,
   const struct r300_triangle_render_shape *shape);

#define R3V_ATTENDED_VERTEX_BYTES 4096

/* Names the stage about to run.  A hang leaves its stage as the last
 * line on the console and in the off-box log.
 */
static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

/* One directory reached by two spellings is still one directory, so the
 * comparison resolves both paths when they exist.
 */
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
write_target(const char *dir, const void *data, size_t size)
{
   char path[1024];
   snprintf(path, sizeof(path), "%s/color_target.bin", dir);
   FILE *f = fopen(path, "wb");
   if (f == NULL)
      return 1;
   size_t written = fwrite(data, 1, size, f);
   return fclose(f) != 0 || written != size;
}

int
main(int argc, char **argv)
{
   struct r300_triangle_render_shape shape;
   int argi = 2 + R3V_RENDER_SHAPE_ARGC;
   if (argc < 3 + R3V_RENDER_SHAPE_ARGC || strcmp(argv[1], "--shape") != 0 ||
       !r3v_render_shape_parse(&argv[2], &shape)) {
      fprintf(stderr,
              "usage: %s --shape <w> <h> <pitch> <bgra|rgba> <r> <g> <b> "
              "<a> [--offset <bytes>] <evidence-directory>\n",
              argv[0]);
      return 2;
   }
   /* The optional offset arm renders at a bind offset inside the color
    * allocation, the target shape a suballocated attachment carries.
    */
   if (argc >= argi + 2 && strcmp(argv[argi], "--offset") == 0) {
      if (!r3v_render_shape_parse_offset(argv[argi], argv[argi + 1], &shape))
         return 2;
      argi += 2;
   }
   if (argc != argi + 1) {
      fprintf(stderr, "usage: %s --shape <8 tokens> [--offset <bytes>] "
              "<evidence-directory>\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[argi];
   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&shape);
   const uint32_t predicted_dword =
      r300_tcl_bypass_triangle_render_shape_draw_dword(&shape);
   printf("[shape] ");
   r3v_render_shape_print(stdout, &shape);
   printf(" predicted interior 0x%08x color bytes %u\n", predicted_dword,
          color_bytes);
   fflush(stdout);

   /* The driver reads the evidence directory from the environment, and the
    * arming conjunction consumes the one-shot token in whichever directory
    * the environment names.  A caller-supplied value that disagrees with
    * the argument would arm one directory and read back another, breaking
    * the binding between the armed cell and the retained result, so the two
    * name one directory and a disagreement refuses.
    */
   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared != NULL && declared[0] != '\0' &&
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s; "
              "the armed directory and the readback directory are one "
              "directory\n",
              declared, evidence_dir);
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", evidence_dir, 1);

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
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       pdev_count != 1 || pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n",
          props.vendorID, props.deviceID, props.deviceName);
   fflush(stdout);
   /* The arming gate enforces this too; refusing here keeps the run off
    * a chip whose falsifiers were not written.
    */
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

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

   stage("memory");
   VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = R3V_ATTENDED_VERTEX_BYTES,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   if (vkAllocateMemory(device, &alloc, NULL, &vertex_memory) !=
       VK_SUCCESS) {
      fprintf(stderr, "vertex allocation failed\n");
      return 1;
   }
   alloc.allocationSize = color_bytes;
   if (vkAllocateMemory(device, &alloc, NULL, &color_memory) !=
       VK_SUCCESS) {
      fprintf(stderr, "color allocation failed\n");
      return 1;
   }

   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   if (vkCreateCommandPool(
          device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0,
          },
          NULL, &pool) != VK_SUCCESS) {
      fprintf(stderr, "command pool creation failed\n");
      return 1;
   }
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkAllocateCommandBuffers(
          device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1,
          },
          &cmd) != VK_SUCCESS) {
      fprintf(stderr, "command buffer allocation failed\n");
      return 1;
   }
   vkBeginCommandBuffer(cmd, &(VkCommandBufferBeginInfo){
                                .sType =
                                   VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                             });
   result = r3v_native_record_tcl_bypass_triangle_render_shape(
      cmd, vertex_memory, color_memory, &shape);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "cell recording failed: %d\n", result);
      return 1;
   }
   vkEndCommandBuffer(cmd);

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
   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *color_map = NULL;
   if (vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0, &color_map) !=
          VK_SUCCESS ||
       color_map == NULL) {
      fprintf(stderr, "color readback map failed\n");
      return 1;
   }
   /* The color target is the run's primary artifact and the submission is
    * one-shot, so the oracle's verdict reaches the console only once the
    * bytes behind it are durable.
    */
   if (write_target(evidence_dir, color_map, color_bytes) != 0) {
      fprintf(stderr, "color target retention failed\n");
      return 1;
   }

   struct r300_triangle_oracle_verdict verdict;
   r300_tcl_bypass_triangle_render_shape_oracle(&shape, color_map,
                                                color_bytes, &verdict);
   printf("[oracle] executed=%d interior=%d exterior=%d canary=%d "
          "interior_samples=%u exterior_samples=%u\n",
          verdict.executed, verdict.interior_pass, verdict.exterior_pass,
          verdict.canary_pass, verdict.interior_samples,
          verdict.exterior_samples);
   /* The centroid pixel, the corner, and the first canary-row pixel:
    * one sample per oracle region, so a deviation names its region.
    * (cx, cy) is the true centroid of the NDC reference triangle
    * (triangle_geometry_at) mapped through the viewport transform to
    * the shape's extent: width/2 horizontally, (height * 3) / 8
    * vertically.
    */
   /* Render row 0 sits at the shape's target offset, so the samples
    * index from there while the reported band below it stays the
    * untouched prefix the canary reads.
    */
   const uint32_t *pixels =
      (const uint32_t *)((const uint8_t *)color_map + shape.target_offset);
   const uint32_t cx = shape.width / 2, cy = (shape.height * 3) / 8;
   printf("[oracle] centroid (%u,%u)=0x%08x predicted 0x%08x exterior "
          "(0,0)=0x%08x canary row=0x%08x\n",
          cx, cy, pixels[cy * shape.pitch_pixels + cx], predicted_dword,
          pixels[0], pixels[shape.pitch_pixels * shape.height]);
   fflush(stdout);

   stage("teardown");
   vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   const bool passed = verdict.executed && verdict.interior_pass &&
                       verdict.exterior_pass && verdict.canary_pass;
   printf("[verdict] %s\n", passed ? "cell rendered as predicted"
                                   : "prediction deviated; the deviation "
                                     "is the finding");
   return passed ? 0 : 1;
}
