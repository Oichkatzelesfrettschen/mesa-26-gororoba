/*
 * SPDX-License-Identifier: MIT
 *
 * Attended native cell: submits the fixed TCL-bypass triangle to RS482
 * silicon through the native ICD and reports the output oracle's
 * verdict.  This program performs a live DRM_RADEON_CS and runs only
 * under the authorization and procedure in
 * docs/hardware/r3v-native-attended-cell-procedure.md; the driver's
 * arming conjunction admits it, and every stage prints and flushes
 * before it runs so a hang names the stage it hung in.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);
VkResult r3v_native_record_tcl_bypass_triangle(VkCommandBuffer commandBuffer,
                                               VkDeviceMemory vertexMemory,
                                               VkDeviceMemory colorMemory);

#define R3V_ATTENDED_COLOR_BYTES (64 * 65 * 4)
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
   if (argc != 2) {
      fprintf(stderr, "usage: %s <evidence-directory>\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   /* The driver reads the evidence directory from the environment; the
    * argument and the environment must name the same directory so the
    * retained artifacts and the readback land together.
    */
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
   alloc.allocationSize = R3V_ATTENDED_COLOR_BYTES;
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
   result = r3v_native_record_tcl_bypass_triangle(cmd, vertex_memory,
                                                  color_memory);
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
   if (write_target(evidence_dir, color_map, R3V_ATTENDED_COLOR_BYTES) != 0)
      fprintf(stderr, "color target retention failed\n");

   struct r300_triangle_oracle_verdict verdict;
   r300_tcl_bypass_triangle_oracle(color_map, R3V_ATTENDED_COLOR_BYTES,
                                   &verdict);
   printf("[oracle] executed=%d interior=%d exterior=%d canary=%d\n",
          verdict.executed, verdict.interior_pass, verdict.exterior_pass,
          verdict.canary_pass);
   const uint32_t *pixels = color_map;
   printf("[oracle] interior sample (32,20)=0x%08x exterior (0,0)=0x%08x "
          "canary=0x%08x\n",
          pixels[20 * 64 + 32], pixels[0], pixels[64 * 64]);
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
