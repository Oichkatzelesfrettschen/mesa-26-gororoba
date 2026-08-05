/*
 * SPDX-License-Identifier: MIT
 *
 * Drives the fixed TCL-bypass triangle through the native ICD on the
 * radeon noop drm-shim: instance, device, GEM-backed memories, command
 * recording through the exported cell recorder, and the gated queue
 * submission.  The closed-gate mode proves the fail-closed verdict and
 * byte-identity of the retained IB against the direct emitter; the
 * open-gate mode proves the transport path end to end with the shim
 * absorbing DRM_RADEON_CS.
 */

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

#include "amd/r300/common/r300_fragment_binary.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "r3v_native.h"

/* The harness links the native implementation directly (the ICD version
 * script keeps the shared library's export surface at the three vk_icd*
 * symbols), so the loader entry and the cell recorder resolve at link
 * time.
 */
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);
VkResult r3v_native_record_tcl_bypass_triangle(VkCommandBuffer commandBuffer,
                                               VkDeviceMemory vertexMemory,
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

/* A VK_SUCCESS open-gate run only proves the shim absorbed DRM_RADEON_CS
 * when the interposed entry points actually resolve into the preloaded
 * shim DSO; the same result code would come back from a live kernel
 * accept.  The attestation resolves each interposed symbol with dlsym,
 * maps it to its providing object with dladdr, and compares that object
 * to DRM_SHIM_EXPECTED_DSO by device and inode.  It runs before the
 * hazard gate opens and before any Vulkan call, so a wrong or missing
 * provider refuses the run ahead of device enumeration and GEM
 * allocation.
 */
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

typedef PFN_vkVoidFunction (*icd_gipa_fn)(VkInstance, const char *);
typedef VkResult (*record_cell_fn)(VkCommandBuffer, VkDeviceMemory,
                                   VkDeviceMemory);

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

/* The reference IB built directly from the shared emitter; the retained
 * ib.bin must equal it byte for byte.
 */
static int
build_reference_ib(struct r300_tcl_bypass_triangle_ib *cell)
{
   struct r300_fragment_binary fs;
   if (r300_tcl_bypass_triangle_reference_fs(&fs) != 0)
      return 1;
   struct r300_tcl_bypass_triangle_params params = {
      .vertex_offset = 0,
      .color_pitch_format = 64,
      .fragment_binary = &fs,
   };
   int rc = r300_tcl_bypass_triangle_emit(&params, cell);
   r300_fragment_binary_finish(&fs);
   return rc != 0;
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

   if (attest_shim_provider() != 0)
      return 3;

   char manifest_dir[] = "/tmp/r3v-native-cell-XXXXXX";
   if (open_gate) {
      setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);
   } else {
      if (mkdtemp(manifest_dir) == NULL) {
         fprintf(stderr, "mkdtemp failed\n");
         return 2;
      }
      setenv("R3V_NATIVE_MANIFEST_DIR", manifest_dir, 1);
      unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");
   }

   icd_gipa_fn gipa = vk_icdGetInstanceProcAddr;
   record_cell_fn record_cell = r3v_native_record_tcl_bypass_triangle;

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

   VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = 4096,
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   result = vkAllocateMemory(device, &alloc_info, NULL, &vertex_memory);
   CHECK(result == VK_SUCCESS, "vertex vkAllocateMemory: %d", result);

   alloc_info.allocationSize = 64 * 64 * 4;
   VkDeviceMemory color_memory = VK_NULL_HANDLE;
   result = vkAllocateMemory(device, &alloc_info, NULL, &color_memory);
   CHECK(result == VK_SUCCESS, "color vkAllocateMemory: %d", result);

   VkCommandPool pool = VK_NULL_HANDLE;
   result = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool);
   CHECK(result == VK_SUCCESS, "vkCreateCommandPool: %d", result);

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

   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   CHECK(result == VK_SUCCESS, "vkBeginCommandBuffer: %d", result);

   /* Known-bad calibration: an undersized color target must be refused
    * before any IB installs.
    */
   result = record_cell(cmd, vertex_memory, vertex_memory);
   CHECK(result == VK_ERROR_INITIALIZATION_FAILED,
         "undersized color memory refuses recording: %d", result);

   result = record_cell(cmd, vertex_memory, color_memory);
   CHECK(result == VK_SUCCESS, "cell recording: %d", result);

   result = vkEndCommandBuffer(cmd);
   CHECK(result == VK_SUCCESS, "vkEndCommandBuffer: %d", result);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The recorder published the vertex write while its mapping was live;
    * exactly one cache sync has run before any submission.
    */
   struct r3v_native_device *native_device =
      r3v_native_device_from_handle(device);
   CHECK(native_device->drm.cache_sync_count == 1,
         "recorder publication ran once: %" PRIu64,
         native_device->drm.cache_sync_count);

   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   if (open_gate) {
      CHECK(result == VK_SUCCESS,
            "open-gate submission through the shim: %d", result);

      /* Mapping the color target after completion invalidates its stale
       * cache lines through the map-establishment sync.
       */
      LOAD_DEVICE(vkMapMemory);
      void *color_map = NULL;
      CHECK(vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0,
                        &color_map) == VK_SUCCESS &&
               color_map != NULL,
            "color memory maps after completion");
      CHECK(native_device->drm.cache_sync_count == 2,
            "map establishment invalidated the color target: %" PRIu64,
            native_device->drm.cache_sync_count);
   } else {
      CHECK(result == VK_ERROR_DEVICE_LOST,
            "closed gate fails closed: %d", result);

      /* The retained IB equals the direct emitter's stream byte for
       * byte, and the relocation chunk carries exactly the two cell
       * references.
       */
      struct r300_tcl_bypass_triangle_ib reference;
      CHECK(build_reference_ib(&reference) == 0, "reference IB builds");
      void *ib_data = NULL;
      size_t ib_size = 0;
      CHECK(read_whole_file(manifest_dir, "ib.bin", &ib_data, &ib_size) ==
               0,
            "manifest ib.bin is retained");
      if (ib_data != NULL) {
         CHECK(ib_size ==
                  reference.ib_size_dwords * sizeof(uint32_t),
               "ib.bin length %zu matches the emitter's %u dwords",
               ib_size, reference.ib_size_dwords);
         CHECK(memcmp(ib_data, reference.ib, ib_size) == 0,
               "ib.bin is byte-identical to the direct emitter stream");
         free(ib_data);
      }
      r300_tcl_bypass_triangle_release(&reference);

      void *reloc_data = NULL;
      size_t reloc_size = 0;
      CHECK(read_whole_file(manifest_dir, "relocs.bin", &reloc_data,
                            &reloc_size) == 0,
            "manifest relocs.bin is retained");
      if (reloc_data != NULL) {
         CHECK(reloc_size ==
                  R300_TRIANGLE_SLOT_COUNT *
                     sizeof(struct drm_radeon_cs_reloc),
               "relocation chunk carries the two cell references "
               "(%zu bytes)",
               reloc_size);
         free(reloc_data);
      }
   }

   vkDestroyCommandPool(device, pool, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkFreeMemory(device, color_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   if (failures == 0) {
      printf("r3v_native_triangle_cell_harness(%s): all checks passed\n",
             argv[1]);
      return 0;
   }
   return 1;
}
