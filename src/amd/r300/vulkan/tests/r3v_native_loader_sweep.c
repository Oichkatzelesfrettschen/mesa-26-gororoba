/*
 * SPDX-License-Identifier: MIT
 *
 * Loader black-box sweep of the native R3V public surface: the real Vulkan
 * loader resolves and calls the ICD, and each invocation runs in a child
 * process so a null call reports as a signal rather than killing the sweep.
 * Each invocation body reaches its command through a loader-linked symbol,
 * and the procaddr legs that follow them query the tables instead of
 * calling.
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

/* A query pool and a descriptor set layout take no object handle, so their
 * creation is invocable with fully valid input and its refusal is observed
 * rather than argued from the class map.
 */
static int
call_create_query_pool(void)
{
   VkQueryPool pool = (VkQueryPool)(uintptr_t)0x1;
   VkResult result = vkCreateQueryPool(
      device,
      &(VkQueryPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
         .queryType = VK_QUERY_TYPE_OCCLUSION,
         .queryCount = 1,
      },
      NULL, &pool);
   return (result != VK_SUCCESS && pool == VK_NULL_HANDLE) ? 0 : 1;
}

static int
call_create_descriptor_set_layout(void)
{
   VkDescriptorSetLayout layout = (VkDescriptorSetLayout)(uintptr_t)0x1;
   VkResult result = vkCreateDescriptorSetLayout(
      device,
      &(VkDescriptorSetLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      },
      NULL, &layout);
   return (result != VK_SUCCESS && layout == VK_NULL_HANDLE) ? 0 : 1;
}

/* vkCreateBuffer executes natively, so a real VkBuffer is available and the
 * buffer-view creation that consumes it runs on valid input.
 */
static int
call_create_buffer_view(void)
{
   VkBuffer buffer = VK_NULL_HANDLE;
   VkResult result = vkCreateBuffer(
      device,
      &(VkBufferCreateInfo){
         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
         .size = 1024,
         .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      },
      NULL, &buffer);
   if (result != VK_SUCCESS || buffer == VK_NULL_HANDLE)
      return 1;

   VkBufferView view = (VkBufferView)(uintptr_t)0x1;
   result = vkCreateBufferView(
      device,
      &(VkBufferViewCreateInfo){
         .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
         .buffer = buffer,
         .format = VK_FORMAT_R8G8B8A8_UNORM,
         .offset = 0,
         .range = VK_WHOLE_SIZE,
      },
      NULL, &view);
   const int rc = (result != VK_SUCCESS && view == VK_NULL_HANDLE) ? 0 : 1;
   vkDestroyBuffer(device, buffer, NULL);
   return rc;
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

/* The one live allocation the mapped-range legs validate against, mapped for
 * the whole sweep because vkFlushMappedMemoryRanges and
 * vkInvalidateMappedMemoryRanges name a currently mapped allocation.  The
 * parent allocates and maps it, and each child inherits the handle: the
 * drm-shim keeps its buffer-object state per process, so a GEM create in a
 * forked child refuses.  That inheritance is a property of this harness and
 * the shim under it, and the range checks under test read the allocation size
 * rather than the kernel.  A child that creates its own instance and device
 * through exec is what a harness measuring object lifetime across processes
 * would need.
 */
static VkDeviceMemory live_memory = VK_NULL_HANDLE;
static VkDeviceSize live_size;
static VkDeviceSize atom_size = 1;

/* VkDeviceMemory is real, so the mapped-range commands run their validation
 * against a live mapped allocation.  Each leg sets one bit, so a single exit
 * status names which range the validator judged wrongly.
 */
static int
call_memory_range_validation(void)
{
   const VkMappedMemoryRange base = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = live_memory,
   };
   /* An offset and a size a flush names are multiples of nonCoherentAtomSize,
    * so each range below is built from that granularity.  The first three are
    * ranges an application forms; the last four lie outside the allocation,
    * which the specification forbids and the validator answers by refusing
    * rather than by reading past the bound it was given.
    */
   const struct {
      VkDeviceSize offset, size;
      bool succeeds;
      const char *what;
   } cases[] = {
      { 0, VK_WHOLE_SIZE, true, "the whole allocation" },
      { atom_size, VK_WHOLE_SIZE, true,
        "an interior offset expanded to the remaining bytes" },
      { 0, live_size, true, "an explicit size covering the allocation" },
      { live_size, atom_size, false, "an offset at the end" },
      { live_size + atom_size, atom_size, false, "an offset past the end" },
      { live_size - atom_size, atom_size * 2, false,
        "a size past the remaining bytes" },
      { atom_size * 2, UINT64_MAX - atom_size, false,
        "a size that wraps its offset" },
   };

   int rc = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      VkMappedMemoryRange range = base;
      range.offset = cases[i].offset;
      range.size = cases[i].size;
      const bool flushed =
         vkFlushMappedMemoryRanges(device, 1, &range) == VK_SUCCESS;
      const bool invalidated =
         vkInvalidateMappedMemoryRanges(device, 1, &range) == VK_SUCCESS;
      if (flushed != cases[i].succeeds || invalidated != cases[i].succeeds) {
         fprintf(stderr, "FAIL: %s: flush %d invalidate %d, %d expected\n",
                 cases[i].what, flushed, invalidated, cases[i].succeeds);
         rc |= 1 << i;
      }
   }

   /* Zero ranges name no allocation and succeed. */
   if (vkFlushMappedMemoryRanges(device, 0, NULL) != VK_SUCCESS ||
       vkInvalidateMappedMemoryRanges(device, 0, NULL) != VK_SUCCESS)
      rc |= 1 << ARRAY_SIZE(cases);

   /* The commitment query describes memory carrying
    * VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, and the native device
    * advertises no such memory type, so the query reports zero.
    */
   VkDeviceSize committed = 1;
   vkGetDeviceMemoryCommitment(device, live_memory, &committed);
   if (committed != 0)
      rc |= 1 << (ARRAY_SIZE(cases) + 1);

   return rc;
}

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

/* A Vulkan 1.0 application reaches the image-format query through the
 * VK_KHR_get_physical_device_properties2 command alias.  Resolve that alias
 * through the loader, then run the usage boundary over the same accepted and
 * refused families as the direct public-surface harness.
 */
static int
check_image_query_alias(VkInstance alias_instance)
{
   PFN_vkGetPhysicalDeviceImageFormatProperties2KHR query =
      (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR)vkGetInstanceProcAddr(
         alias_instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
   if (query == NULL)
      return 1;

   uint32_t pdev_count = 1;
   VkPhysicalDevice physical_device = VK_NULL_HANDLE;
   VkResult result = vkEnumeratePhysicalDevices(alias_instance, &pdev_count,
                                                &physical_device);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       pdev_count != 1 || physical_device == VK_NULL_HANDLE)
      return 1;

   const VkImageUsageFlags transfer_usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   const struct {
      VkImageUsageFlags usage;
      VkResult expected;
   } cases[] = {
      { 0, VK_ERROR_FORMAT_NOT_SUPPORTED },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SUCCESS },
      { VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SUCCESS },
      { VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SUCCESS },
      { transfer_usage, VK_SUCCESS },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_ERROR_FORMAT_NOT_SUPPORTED },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_ERROR_FORMAT_NOT_SUPPORTED },
      { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_ERROR_FORMAT_NOT_SUPPORTED },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
      const VkPhysicalDeviceImageFormatInfo2 query_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
         .format = VK_FORMAT_B8G8R8A8_UNORM,
         .type = VK_IMAGE_TYPE_2D,
         .tiling = VK_IMAGE_TILING_LINEAR,
         .usage = cases[i].usage,
      };
      VkImageFormatProperties2 properties = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      };
      if (query(physical_device, &query_info, &properties) !=
          cases[i].expected)
         return 1;
   }

   return 0;
}

int
main(void)
{
   /* The submission gate stays closed for the whole sweep, so no invocation
    * below can reach DRM_RADEON_CS even if one recorded a command buffer.
    */
   unsetenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED");

   const char *const instance_extensions[] = {
      VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
   };
   VkInstance alias_instance = VK_NULL_HANDLE;
   VkResult result = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo =
            &(VkApplicationInfo){
               .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
               .apiVersion = VK_API_VERSION_1_0,
            },
         .enabledExtensionCount = 1,
         .ppEnabledExtensionNames = instance_extensions,
      },
      NULL, &alias_instance);
   CHECK(result == VK_SUCCESS,
         "Vulkan 1.0 enables VK_KHR_get_physical_device_properties2: %d",
         result);
   if (result == VK_SUCCESS) {
      CHECK(check_image_query_alias(alias_instance) == 0,
            "the loader resolves and executes the KHR image-query alias");
      vkDestroyInstance(alias_instance, NULL);
   }

   result = vkCreateInstance(
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
   unsigned alias_gipa = 0;
   sweep("promoted alias", r3v_surface_alias, r3v_surface_alias_count, false,
         &alias_gipa, &higher_gdpa, &higher_judged);
   sweep("closed extension", r3v_surface_closed_extension,
         r3v_surface_closed_extension_count, false, &ext_gipa, &ext_gdpa,
         &ext_judged);

   printf("core 1.0: %u device names, GetDeviceProcAddr answers %u\n",
          core_judged, core_gdpa);
   printf("higher core and promoted aliases: %u device names, "
          "GetDeviceProcAddr answers %u\n", higher_judged, higher_gdpa);
   printf("closed extension: %u device names, GetDeviceProcAddr answers "
          "%u\n", ext_judged, ext_gdpa);
   printf("GetInstanceProcAddr answers %u/%u core 1.0, %u/%u higher core, "
          "%u/%u promoted alias, %u/%u closed extension\n", core_gipa,
          r3v_surface_core10_count, higher_gipa,
          r3v_surface_higher_core_count, alias_gipa, r3v_surface_alias_count,
          ext_gipa, r3v_surface_closed_extension_count);

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
   /* The higher-core leg is the one that isolates the version rule: every
    * one of those names belongs to a core version this instance did not
    * request, so answering all of them is the loader answering from its own
    * table rather than the driver reporting what it implements.
    */
   printf("vkGetInstanceProcAddr applies the requested version: %s\n",
          higher_gipa < r3v_surface_higher_core_count ? "yes" :
          "no, it answers every higher-core name and carries no driver "
          "verdict");

   /* nonCoherentAtomSize is the granularity a flush range is built from, so
    * the allocation is sized to a whole number of atoms and every offset the
    * mapped-range legs name is a multiple of one.
    */
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   atom_size = props.limits.nonCoherentAtomSize;
   CHECK(atom_size >= 1 && (atom_size & (atom_size - 1)) == 0,
         "nonCoherentAtomSize is %llu, not a power of two",
         (unsigned long long)atom_size);
   if (atom_size == 0)
      atom_size = 1;
   live_size = atom_size * 4;

   result = vkAllocateMemory(
      device, &(VkMemoryAllocateInfo){
                 .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                 .allocationSize = live_size,
                 .memoryTypeIndex = 0,
              },
      NULL, &live_memory);
   CHECK(result == VK_SUCCESS, "vkAllocateMemory through the loader: %d",
         result);

   /* Both mapped-range commands name a currently mapped allocation, so the
    * map is what makes those legs valid use rather than a call the
    * specification leaves undefined.
    */
   void *live_map = NULL;
   if (result == VK_SUCCESS) {
      result = vkMapMemory(device, live_memory, 0, VK_WHOLE_SIZE, 0,
                           &live_map);
      CHECK(result == VK_SUCCESS && live_map != NULL,
            "vkMapMemory over the live allocation: %d", result);
   }

   in_child("vkCreateImage refuses", call_create_image);
   in_child("vkCreateEvent refuses", call_create_event);
   in_child("vkCreateSampler refuses", call_create_sampler);
   in_child("vkCreateDescriptorPool refuses", call_create_descriptor_pool);
   in_child("vkCreateQueryPool refuses", call_create_query_pool);
   in_child("vkCreateDescriptorSetLayout refuses",
            call_create_descriptor_set_layout);
   in_child("vkCreateBufferView refuses over a live buffer",
            call_create_buffer_view);
   in_child("vkDestroy* over the null handle", call_destroy_image_null);
   in_child("vkUpdateDescriptorSets over zero writes",
            call_update_descriptor_sets_empty);
   in_child("mapped-range commands over zero ranges", call_flush_empty);
   in_child("mapped-range validation over a live allocation",
            call_memory_range_validation);

   if (live_map != NULL)
      vkUnmapMemory(device, live_memory);
   vkFreeMemory(device, live_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("r3v-native-loader-sweep: %u failures\n", failures);
   return failures != 0;
}
