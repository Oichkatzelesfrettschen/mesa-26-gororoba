/*
 * SPDX-License-Identifier: MIT
 *
 * Loader-only application for the public RB2D fill route: one
 * vkCmdFillBuffer over a named cell, submitted through the installed
 * Vulkan loader to whichever ICD VK_DRIVER_FILES names.  The binary links
 * libvulkan and libc alone and defines no driver symbol, so every command
 * below reaches the driver the way an application's does.
 *
 * The oracle is the destination allocation itself, mapped through the
 * public map: canary bytes before the interval, a sentinel inside it, and
 * canary bytes after it and in the allocation tail.  The mapping is
 * protected read-only before the submit, so a host store into the
 * destination faults instead of completing; a routed submission leaves
 * every byte as it was because the drm-shim performs no fill, and a
 * host-path submission over the same protection is the known-bad that
 * proves the protection judges stores.
 *
 * Every verdict the application can observe is printed as key=value, and
 * the exit status names its class: 0 the expected outcome, 1 a verdict
 * failure, 2 a usage or fixture failure, 3 an oracle failure.
 *
 * Fixtures, each read from the environment:
 *   R3V_LOADER_FILL_EXPECT     submitted | refused | host-filled
 *   R3V_LOADER_FILL_PROTECT    1 (default) protects the mapping read-only
 *   R3V_LOADER_FILL_VALUE      the fill dword (default 0x11223344)
 *   R3V_LOADER_FILL_OFFSET     the fill offset (default 12)
 *   R3V_LOADER_FILL_BYTES      the fill size (default 4992)
 *   R3V_EXPECTED_ICD_DSO       the DSO the loader must have mapped
 *
 * --cell <name> selects the cell; v1_public when absent.  The cell fixes
 * the allocation, the offset, the size, and the value, and the three
 * R3V_LOADER_FILL_ overrides then move the request inside that
 * allocation, so a mutation leg varies one fact against the cell it
 * names.
 */

#include "amd/r300/common/r300_chip_identity.h"
#include "r3v_public_rb2d_fill_oracle.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

/* The cell this run names, read once in main before any Vulkan call, so
 * every size below is that cell's rather than a compiled-in constant. */
static const struct r3v_public_rb2d_fill_cell *cell;

#define CELL_ALLOCATION_BYTES (cell->allocation_bytes)
#define CELL_TAIL_BYTES (cell->tail_bytes)

#define PREFIX_CANARY 0xc1u
#define INTERVAL_SENTINEL 0xa5u
#define SUFFIX_CANARY 0xc2u
#define TAIL_CANARY 0xc3u

enum expectation {
   EXPECT_SUBMITTED,
   EXPECT_REFUSED,
   EXPECT_HOST_FILLED,
};

static int status;

#define REQUIRE(condition, class, ...)                                        \
   do {                                                                       \
      if (!(condition)) {                                                     \
         fprintf(stderr, "FAIL: ");                                           \
         fprintf(stderr, __VA_ARGS__);                                        \
         fprintf(stderr, "\n");                                               \
         if (status == 0 || (class) < status)                                 \
            status = (class);                                                 \
      }                                                                       \
   } while (0)

static bool
env_flag_set(const char *name, bool default_value)
{
   const char *value = getenv(name);
   if (value == NULL || value[0] == '\0')
      return default_value;
   return strcmp(value, "1") == 0;
}

static bool
env_number(const char *name, uint64_t default_value, uint64_t *out)
{
   const char *value = getenv(name);
   if (value == NULL || value[0] == '\0') {
      *out = default_value;
      return true;
   }
   char *end = NULL;
   errno = 0;
   unsigned long long parsed = strtoull(value, &end, 0);
   if (errno != 0 || end == value || *end != '\0')
      return false;
   *out = parsed;
   return true;
}

/* The loader resolves the ICD through its manifest, so the proof that
 * this run exercised the intended driver is the mapped DSO itself. */
static bool
icd_dso_mapped(const char *expected)
{
   FILE *maps = fopen("/proc/self/maps", "r");
   if (maps == NULL)
      return false;
   char line[4096];
   bool found = false;
   while (fgets(line, sizeof(line), maps) != NULL) {
      if (strstr(line, expected) != NULL) {
         found = true;
         break;
      }
   }
   fclose(maps);
   return found;
}

/* The preloaded shim exports the count with default visibility; an
 * absent symbol is a fixture failure rather than a zero count. */
static bool
read_shim_cs_count(uint64_t *out)
{
   uint64_t (*counter)(void) = (uint64_t (*)(void))dlsym(
      RTLD_DEFAULT, "drm_shim_test_radeon_cs_ioctls");
   if (counter == NULL)
      return false;
   *out = counter();
   return true;
}

static const char *
result_name(VkResult r)
{
   switch (r) {
   case VK_SUCCESS:
      return "VK_SUCCESS";
   case VK_ERROR_DEVICE_LOST:
      return "VK_ERROR_DEVICE_LOST";
   case VK_ERROR_UNKNOWN:
      return "VK_ERROR_UNKNOWN";
   case VK_ERROR_OUT_OF_HOST_MEMORY:
      return "VK_ERROR_OUT_OF_HOST_MEMORY";
   case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
   case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_TIMEOUT:
      return "VK_TIMEOUT";
   default:
      return "(other)";
   }
}

static uint8_t
expected_byte(uint64_t i, uint64_t fill_offset, uint64_t fill_bytes)
{
   if (i >= (uint64_t)CELL_ALLOCATION_BYTES - CELL_TAIL_BYTES)
      return TAIL_CANARY;
   if (i < fill_offset)
      return PREFIX_CANARY;
   if (i < fill_offset + fill_bytes)
      return INTERVAL_SENTINEL;
   return SUFFIX_CANARY;
}

int
main(int argc, char **argv)
{
   const char *cell_name = "v1_public";
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--cell") == 0 && i + 1 < argc) {
         cell_name = argv[++i];
      } else {
         fprintf(stderr, "usage: %s [--cell <name>] (the rest of the "
                         "fixtures come from the environment)\n",
                 argv[0]);
         return 2;
      }
   }
   cell = r3v_public_rb2d_fill_cell_by_name(cell_name);
   if (cell == NULL) {
      fprintf(stderr, "no cell is named %s\n", cell_name);
      return 2;
   }

   enum expectation expect = EXPECT_SUBMITTED;
   const char *expect_env = getenv("R3V_LOADER_FILL_EXPECT");
   if (expect_env == NULL || strcmp(expect_env, "submitted") == 0)
      expect = EXPECT_SUBMITTED;
   else if (strcmp(expect_env, "refused") == 0)
      expect = EXPECT_REFUSED;
   else if (strcmp(expect_env, "host-filled") == 0)
      expect = EXPECT_HOST_FILLED;
   else {
      fprintf(stderr, "R3V_LOADER_FILL_EXPECT names no expectation: %s\n",
              expect_env);
      return 2;
   }
   const bool protect = env_flag_set("R3V_LOADER_FILL_PROTECT", true);
   uint64_t fill_value, fill_offset, fill_bytes;
   if (!env_number("R3V_LOADER_FILL_VALUE", cell->fill_value, &fill_value) ||
       !env_number("R3V_LOADER_FILL_OFFSET", cell->fill_offset,
                   &fill_offset) ||
       !env_number("R3V_LOADER_FILL_BYTES", cell->fill_bytes, &fill_bytes) ||
       fill_value > UINT32_MAX || fill_offset > CELL_ALLOCATION_BYTES ||
       fill_bytes == 0 || fill_bytes > CELL_ALLOCATION_BYTES - fill_offset) {
      fprintf(stderr, "fill fixture is malformed or outside the cell\n");
      return 2;
   }
   const char *expected_dso = getenv("R3V_EXPECTED_ICD_DSO");
   if (expected_dso == NULL || expected_dso[0] == '\0') {
      fprintf(stderr, "R3V_EXPECTED_ICD_DSO is unset\n");
      return 2;
   }
   uint64_t cs_before = 0;
   if (!read_shim_cs_count(&cs_before)) {
      fprintf(stderr, "drm_shim_test_radeon_cs_ioctls is not resolvable: "
                      "the radeon drm-shim is not preloaded\n");
      return 2;
   }

   /* One fact per line: the check script reads each key anchored at the
    * start of a line. */
   printf("cell=%s\n", cell->name);
   printf("allocation_bytes=%u\n", cell->allocation_bytes);
   printf("expected_pitch=%u\n", cell->expected_pitch_bytes);
   printf("expected_windows=%u\n", cell->expected_window_count);
   printf("expected_relocation_sites=%u\n",
          cell->expected_relocation_sites);
   printf("mode=%s protect=%d fill_offset=%llu fill_bytes=%llu "
          "fill_value=0x%08llx\n",
          expect_env != NULL ? expect_env : "submitted", protect ? 1 : 0,
          (unsigned long long)fill_offset, (unsigned long long)fill_bytes,
          (unsigned long long)fill_value);

   VkInstance instance = VK_NULL_HANDLE;
   VkResult r = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %s\n", result_name(r));
      return 2;
   }

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   r = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || pdev_count != 1 ||
       pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "vkEnumeratePhysicalDevices: %s, %u devices\n",
              result_name(r), pdev_count);
      return 2;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("device=%04x:%04x driver=%s\n", props.vendorID, props.deviceID,
          props.deviceName);
   if (props.vendorID != R300_PCI_VENDOR_ATI ||
       props.deviceID != R300_PCI_DEVICE_RS48X_5974) {
      fprintf(stderr, "the enumerated device is not the RS48x 1002:5974\n");
      return 2;
   }

   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   r = vkCreateDevice(
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
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %s\n", result_name(r));
      return 2;
   }
   if (!icd_dso_mapped(expected_dso)) {
      fprintf(stderr, "the loader did not map %s\n", expected_dso);
      return 2;
   }
   printf("icd_dso=%s\n", expected_dso);

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);
   if (queue == VK_NULL_HANDLE) {
      fprintf(stderr, "vkGetDeviceQueue returned no queue\n");
      return 2;
   }

   VkBuffer buffer = VK_NULL_HANDLE;
   r = vkCreateBuffer(device,
                      &(VkBufferCreateInfo){
                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                         .size = CELL_ALLOCATION_BYTES,
                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                      },
                      NULL, &buffer);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateBuffer: %s\n", result_name(r));
      return 2;
   }
   VkMemoryRequirements reqs;
   vkGetBufferMemoryRequirements(device, buffer, &reqs);
   if (reqs.size != CELL_ALLOCATION_BYTES || (reqs.memoryTypeBits & 1) == 0) {
      fprintf(stderr, "buffer requirements: size %llu, types 0x%x\n",
              (unsigned long long)reqs.size, reqs.memoryTypeBits);
      return 2;
   }
   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(pdev, &memory_properties);
   const VkMemoryPropertyFlags host_visible =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   if (memory_properties.memoryTypeCount < 1 ||
       (memory_properties.memoryTypes[0].propertyFlags & host_visible) !=
          host_visible) {
      fprintf(stderr, "memory type 0 is not host-visible and coherent\n");
      return 2;
   }

   VkDeviceMemory memory = VK_NULL_HANDLE;
   r = vkAllocateMemory(device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = CELL_ALLOCATION_BYTES,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &memory);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkAllocateMemory: %s\n", result_name(r));
      return 2;
   }
   r = vkBindBufferMemory(device, buffer, memory, 0);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkBindBufferMemory: %s\n", result_name(r));
      return 2;
   }

   /* The oracle's initial state, written through the public map in a
    * phase of its own and left mapped so the sweep after the submit reads
    * the same bytes the device would have written. */
   uint8_t *map = NULL;
   r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&map);
   if (r != VK_SUCCESS || map == NULL) {
      fprintf(stderr, "vkMapMemory: %s\n", result_name(r));
      return 2;
   }
   for (uint64_t i = 0; i < CELL_ALLOCATION_BYTES; i++)
      map[i] = expected_byte(i, fill_offset, fill_bytes);

   if (protect) {
      const long page = sysconf(_SC_PAGESIZE);
      if (page <= 0 || ((uintptr_t)map % (uintptr_t)page) != 0) {
         fprintf(stderr, "the mapping is not page-aligned; protection "
                         "would cover the wrong bytes\n");
         return 2;
      }
      if (mprotect(map, CELL_ALLOCATION_BYTES, PROT_READ) != 0) {
         fprintf(stderr, "mprotect: %s\n", strerror(errno));
         return 2;
      }
   }
   printf("destination_protected=%d\n", protect ? 1 : 0);

   VkCommandPool pool = VK_NULL_HANDLE;
   r = vkCreateCommandPool(device,
                           &(VkCommandPoolCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                              .queueFamilyIndex = 0,
                           },
                           NULL, &pool);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateCommandPool: %s\n", result_name(r));
      return 2;
   }
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   r = vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkAllocateCommandBuffers: %s\n", result_name(r));
      return 2;
   }
   r = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
           });
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkBeginCommandBuffer: %s\n", result_name(r));
      return 2;
   }
   vkCmdFillBuffer(cmd, buffer, fill_offset, fill_bytes, (uint32_t)fill_value);
   r = vkEndCommandBuffer(cmd);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkEndCommandBuffer: %s\n", result_name(r));
      return 2;
   }

   VkFence fence = VK_NULL_HANDLE;
   r = vkCreateFence(device,
                     &(VkFenceCreateInfo){
                        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                     },
                     NULL, &fence);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateFence: %s\n", result_name(r));
      return 2;
   }

   /* The one submission.  Every gate the route asks stands ahead of any
    * effect, so a refusal returns here with the shim's count unmoved and
    * the destination as the phase above left it. */
   const VkResult submitted =
      vkQueueSubmit(queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd,
                    },
                    fence);
   printf("submit_result=%s\n", result_name(submitted));
   VkResult waited = VK_NOT_READY;
   if (submitted == VK_SUCCESS) {
      waited = vkWaitForFences(device, 1, &fence, VK_TRUE,
                               (uint64_t)30 * 1000 * 1000 * 1000);
      printf("fence_result=%s\n", result_name(waited));
   }
   uint64_t cs_after = 0;
   if (!read_shim_cs_count(&cs_after)) {
      fprintf(stderr, "the shim counter vanished after the submit\n");
      return 2;
   }
   const uint64_t shim_cs = cs_after - cs_before;
   printf("shim_cs_ioctls=%llu\n", (unsigned long long)shim_cs);

   /* The sweep: every byte against the phase-one image, and the interval
    * against the fill value, counted apart so a partial fill names where
    * it stopped. */
   uint64_t deviating = 0;
   uint64_t interval_filled = 0;
   uint64_t first_deviation = UINT64_MAX;
   for (uint64_t i = 0; i < CELL_ALLOCATION_BYTES; i++) {
      if (map[i] != expected_byte(i, fill_offset, fill_bytes)) {
         deviating++;
         if (first_deviation == UINT64_MAX)
            first_deviation = i;
      }
   }
   for (uint64_t i = fill_offset; i + 4 <= fill_offset + fill_bytes; i += 4) {
      uint32_t word;
      memcpy(&word, map + i, sizeof(word));
      interval_filled += word == (uint32_t)fill_value;
   }
   printf("bytes_deviating=%llu first_deviation=%lld interval_filled_dwords=%llu "
          "of %llu\n",
          (unsigned long long)deviating,
          first_deviation == UINT64_MAX ? -1LL : (long long)first_deviation,
          (unsigned long long)interval_filled,
          (unsigned long long)(fill_bytes / 4));

   switch (expect) {
   case EXPECT_SUBMITTED:
      REQUIRE(submitted == VK_SUCCESS, 1, "the submit did not succeed");
      REQUIRE(waited == VK_SUCCESS, 1, "the fence did not signal");
      REQUIRE(shim_cs == 1, 1, "the shim observed %llu DRM_RADEON_CS, not 1",
              (unsigned long long)shim_cs);
      REQUIRE(deviating == 0, 3,
              "the routed submission changed %llu bytes on the host",
              (unsigned long long)deviating);
      break;
   case EXPECT_REFUSED:
      REQUIRE(submitted != VK_SUCCESS, 1, "the submit was not refused");
      REQUIRE(shim_cs == 0, 1,
              "the refused submit still reached the shim %llu times",
              (unsigned long long)shim_cs);
      REQUIRE(deviating == 0, 3, "the refused submit changed %llu bytes",
              (unsigned long long)deviating);
      break;
   case EXPECT_HOST_FILLED:
      REQUIRE(submitted == VK_SUCCESS, 1, "the host submit did not succeed");
      REQUIRE(shim_cs == 0, 1, "the host path reached the shim %llu times",
              (unsigned long long)shim_cs);
      REQUIRE(interval_filled == fill_bytes / 4, 3,
              "the host filled %llu of %llu dwords",
              (unsigned long long)interval_filled,
              (unsigned long long)(fill_bytes / 4));
      REQUIRE(deviating == fill_bytes, 3,
              "the host changed %llu bytes, not the %llu of the interval",
              (unsigned long long)deviating, (unsigned long long)fill_bytes);
      break;
   }

   if (protect)
      mprotect(map, CELL_ALLOCATION_BYTES, PROT_READ | PROT_WRITE);
   vkUnmapMemory(device, memory);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("r3v-native-loader-fill-application: %s\n",
          status == 0 ? "PASS" : "FAIL");
   return status;
}
