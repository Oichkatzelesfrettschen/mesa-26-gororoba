/*
 * SPDX-License-Identifier: MIT
 *
 * Loader black-box sweep of the native R3V public surface: the real Vulkan
 * loader resolves and calls the ICD, and each invocation runs in a child
 * process so a null call reports as a signal rather than killing the sweep.
 */

#include "r3v_native_surface.h"

#include "util/macros.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

static unsigned failures;

#define CHECK(cond, ...)                                                     \
   do {                                                                      \
      if (!(cond)) {                                                         \
         failures++;                                                         \
         fprintf(stderr, "FAIL: ");                                          \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                              \
      }                                                                      \
   } while (0)

static VkInstance instance = VK_NULL_HANDLE;
static VkDevice device = VK_NULL_HANDLE;
static PFN_vkGetDeviceProcAddr gdpa;

/* Run one invocation in a child so a null call, an abort, or a sanitizer
 * report lands as an exit status the parent reads instead of ending the
 * sweep.  The child's verdict is its exit code, and the parent adds the
 * signal that killed it.
 */
static void
in_child(const char *label, int (*body)(void))
{
   fflush(NULL);
   pid_t pid = fork();
   if (pid == 0)
      _exit(body());
   if (pid < 0) {
      failures++;
      fprintf(stderr, "FAIL: %s: fork failed\n", label);
      return;
   }

   int status = 0;
   waitpid(pid, &status, 0);
   if (WIFSIGNALED(status)) {
      failures++;
      fprintf(stderr, "FAIL: %s: killed by signal %d\n", label,
              WTERMSIG(status));
      return;
   }
   CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "%s: exit status %d", label, WEXITSTATUS(status));
}

/* --- The invocation bodies, each reached through a loader-linked symbol --- */

/* Creation of an unsupported type refuses and hands back no handle. */
static int
call_create_image(void)
{
   VkImage image = (VkImage)(uintptr_t)0x1;
   VkResult result = vkCreateImage(
      device,
      &(VkImageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .extent = { 4, 4, 1 },
         .mipLevels = 1,
         .arrayLayers = 1,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      },
      NULL, &image);
   return (result != VK_SUCCESS && image == VK_NULL_HANDLE) ? 0 : 1;
}

static int
call_create_event(void)
{
   VkEvent event = (VkEvent)(uintptr_t)0x1;
   VkResult result = vkCreateEvent(
      device, &(VkEventCreateInfo){
                 .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
              },
      NULL, &event);
   return (result != VK_SUCCESS && event == VK_NULL_HANDLE) ? 0 : 1;
}

static int
call_create_sampler(void)
{
   VkSampler sampler = (VkSampler)(uintptr_t)0x1;
   VkResult result = vkCreateSampler(
      device, &(VkSamplerCreateInfo){
                 .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
              },
      NULL, &sampler);
   return (result != VK_SUCCESS && sampler == VK_NULL_HANDLE) ? 0 : 1;
}

static int
call_create_descriptor_pool(void)
{
   VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)0x1;
   VkResult result = vkCreateDescriptorPool(
      device,
      &(VkDescriptorPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
         .maxSets = 1,
         .poolSizeCount = 1,
         .pPoolSizes =
            &(VkDescriptorPoolSize){
               .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
               .descriptorCount = 1,
            },
      },
      NULL, &pool);
   return (result != VK_SUCCESS && pool == VK_NULL_HANDLE) ? 0 : 1;
}

/* Destroying the null handle is a specified no-op. */
static int
call_destroy_image_null(void)
{
   vkDestroyImage(device, VK_NULL_HANDLE, NULL);
   vkDestroyImageView(device, VK_NULL_HANDLE, NULL);
   vkDestroySampler(device, VK_NULL_HANDLE, NULL);
   vkDestroyEvent(device, VK_NULL_HANDLE, NULL);
   vkDestroyQueryPool(device, VK_NULL_HANDLE, NULL);
   vkDestroyDescriptorPool(device, VK_NULL_HANDLE, NULL);
   vkDestroyBufferView(device, VK_NULL_HANDLE, NULL);
   return 0;
}

/* Writing and copying zero descriptors asks nothing and returns. */
static int
call_update_descriptor_sets_empty(void)
{
   vkUpdateDescriptorSets(device, 0, NULL, 0, NULL);
   return 0;
}

/* Flushing zero ranges asks nothing and succeeds. */
static int
call_flush_empty(void)
{
   if (vkFlushMappedMemoryRanges(device, 0, NULL) != VK_SUCCESS)
      return 1;
   return vkInvalidateMappedMemoryRanges(device, 0, NULL) != VK_SUCCESS;
}

/* The one live allocation the mapped-range legs validate against.  It is
 * allocated in the parent: the drm-shim keeps its buffer-object state per
 * process, so a GEM create in a forked child refuses, and the range checks
 * under test read the allocation size rather than the kernel.
 */
static VkDeviceMemory live_memory = VK_NULL_HANDLE;

/* VkDeviceMemory is real, so the mapped-range commands run their validation
 * against a live allocation: the whole-size range covers it and succeeds, an
 * offset past its end refuses, a size that would wrap its offset refuses, and
 * the commitment of a non-lazy allocation reports zero.
 */
static int
call_memory_range_validation(void)
{
   VkDeviceMemory memory = live_memory;
   int rc = 0;
   VkMappedMemoryRange whole = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = memory,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   if (vkFlushMappedMemoryRanges(device, 1, &whole) != VK_SUCCESS)
      rc |= 1;
   if (vkInvalidateMappedMemoryRanges(device, 1, &whole) != VK_SUCCESS)
      rc |= 2;

   VkMappedMemoryRange past_end = whole;
   past_end.offset = 1 << 20;
   past_end.size = 64;
   if (vkFlushMappedMemoryRanges(device, 1, &past_end) == VK_SUCCESS)
      rc |= 4;

   /* A size that would wrap the offset must not pass the bound check. */
   VkMappedMemoryRange wrapping = whole;
   wrapping.offset = 2048;
   wrapping.size = UINT64_MAX - 1024;
   if (vkFlushMappedMemoryRanges(device, 1, &wrapping) == VK_SUCCESS)
      rc |= 8;

   VkDeviceSize committed = 1;
   vkGetDeviceMemoryCommitment(device, memory, &committed);
   if (committed != 0)
      rc |= 16;

   return rc;
}

/* --- The procaddr legs --- */

/* Device-scope names the loader answers from its own table for a Vulkan 1.0
 * instance even though this ICD leaves them NULL.  The direct-table sweep
 * measures the ICD and reports each of these absent, so a name here is the
 * loader adding a pointer rather than the driver exposing one; listing them
 * keeps a second such name from arriving unnoticed.
 */
static const char *const LOADER_ADDED_DEVICE_NAMES[] = {
   "vkGetDeviceQueue2",
};

static bool
loader_added(const char *name)
{
   for (unsigned i = 0; i < ARRAY_SIZE(LOADER_ADDED_DEVICE_NAMES); i++) {
      if (strcmp(name, LOADER_ADDED_DEVICE_NAMES[i]) == 0)
         return true;
   }
   return false;
}

static void
sweep(const char *label, const struct r3v_surface_command *table,
      uint32_t count, bool expect_present, unsigned *gipa_present,
      unsigned *gdpa_present, unsigned *judged)
{
   for (uint32_t i = 0; i < count; i++) {
      const bool via_gipa =
         vkGetInstanceProcAddr(instance, table[i].name) != NULL;
      const bool via_gdpa =
         table[i].scope == R3V_SCOPE_DEVICE &&
         gdpa(device, table[i].name) != NULL;

      *gipa_present += via_gipa;
      if (table[i].scope != R3V_SCOPE_DEVICE)
         continue;

      (*judged)++;
      *gdpa_present += via_gdpa;
      if (!expect_present && via_gdpa && loader_added(table[i].name)) {
         printf("%s: %s answered by the loader, absent from the ICD\n",
                label, table[i].name);
         continue;
      }
      CHECK(via_gdpa == expect_present, "%s: vkGetDeviceProcAddr(%s) is %s",
            label, table[i].name, via_gdpa ? "non-NULL" : "NULL");
   }
}

int
main(void)
{
   /* The submission gate stays closed for the whole sweep, so no invocation
    * below can reach DRM_RADEON_CS even if one recorded a command buffer.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");

   VkResult result = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo =
            &(VkApplicationInfo){
               .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
               .apiVersion = VK_API_VERSION_1_0,
            },
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS, "vkCreateInstance through the loader: %d",
         result);
   if (result != VK_SUCCESS)
      return 1;

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
            pdev != VK_NULL_HANDLE,
         "the native ICD enumerates one physical device: %d count %u",
         result, pdev_count);
   if (pdev == VK_NULL_HANDLE)
      return 1;

   const float queue_priority = 1.0f;
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
   CHECK(result == VK_SUCCESS, "vkCreateDevice through the loader: %d",
         result);
   if (result != VK_SUCCESS)
      return 1;

   gdpa = (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(
      instance, "vkGetDeviceProcAddr");
   CHECK(gdpa != NULL, "the loader resolves vkGetDeviceProcAddr");
   if (gdpa == NULL)
      return 1;

   unsigned core_gipa = 0, core_gdpa = 0, core_judged = 0;
   unsigned higher_gipa = 0, higher_gdpa = 0, higher_judged = 0;
   unsigned ext_gipa = 0, ext_gdpa = 0, ext_judged = 0;

   sweep("core 1.0", r3v_surface_core10, r3v_surface_core10_count, true,
         &core_gipa, &core_gdpa, &core_judged);
   sweep("higher core", r3v_surface_higher_core,
         r3v_surface_higher_core_count, false, &higher_gipa, &higher_gdpa,
         &higher_judged);
   sweep("closed extension", r3v_surface_closed_extension,
         r3v_surface_closed_extension_count, false, &ext_gipa, &ext_gdpa,
         &ext_judged);

   printf("core 1.0: %u device names, GetDeviceProcAddr answers %u\n",
          core_judged, core_gdpa);
   printf("higher core: %u device names, GetDeviceProcAddr answers %u\n",
          higher_judged, higher_gdpa);
   printf("closed extension: %u device names, GetDeviceProcAddr answers "
          "%u\n", ext_judged, ext_gdpa);
   printf("GetInstanceProcAddr answers %u core 1.0, %u higher core, %u "
          "closed extension of %u/%u/%u names\n", core_gipa, higher_gipa,
          ext_gipa, r3v_surface_core10_count, r3v_surface_higher_core_count,
          r3v_surface_closed_extension_count);

   /* Calibration before the completeness result carries weight: the loader
    * answers a device-level vkGetInstanceProcAddr query from its own
    * trampoline table, so that leg reports a pointer whether or not the ICD
    * implements anything.  vkGetDeviceProcAddr resolves through the device
    * dispatch table the loader filled by asking this ICD, so it discriminates.
    * The absent tables prove which of the two is measuring the driver: a leg
    * that answers every name is measuring the loader and holds no verdict.
    */
   const bool gdpa_discriminates =
      higher_gdpa <= ARRAY_SIZE(LOADER_ADDED_DEVICE_NAMES) &&
      ext_gdpa == 0 && core_gdpa == core_judged;
   CHECK(gdpa_discriminates,
         "vkGetDeviceProcAddr separates the implemented surface from the "
         "absent one: %u/%u core, %u higher, %u extension", core_gdpa,
         core_judged, higher_gdpa, ext_gdpa);
   printf("vkGetInstanceProcAddr discriminates: %s\n",
          higher_gipa < r3v_surface_higher_core_count ? "yes" : "no, it "
          "answers every name and carries no driver verdict");

   result = vkAllocateMemory(
      device, &(VkMemoryAllocateInfo){
                 .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                 .allocationSize = 4096,
                 .memoryTypeIndex = 0,
              },
      NULL, &live_memory);
   CHECK(result == VK_SUCCESS, "vkAllocateMemory through the loader: %d",
         result);

   in_child("vkCreateImage refuses", call_create_image);
   in_child("vkCreateEvent refuses", call_create_event);
   in_child("vkCreateSampler refuses", call_create_sampler);
   in_child("vkCreateDescriptorPool refuses", call_create_descriptor_pool);
   in_child("vkDestroy* over the null handle", call_destroy_image_null);
   in_child("vkUpdateDescriptorSets over zero writes",
            call_update_descriptor_sets_empty);
   in_child("mapped-range commands over zero ranges", call_flush_empty);
   in_child("mapped-range validation over a live allocation",
            call_memory_range_validation);

   vkFreeMemory(device, live_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("r3v-native-loader-sweep: %u failures\n", failures);
   return failures != 0;
}
