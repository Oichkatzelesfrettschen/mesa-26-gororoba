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
 * sweep.  The child verdict is normalized to zero or one before _exit(), so
 * every nonzero body result remains visible in the low exit-status byte.
 */
static int
child_exit_status(int body_status)
{
   return body_status == 0 ? 0 : 1;
}

static int
calibration_child_success(void)
{
   return 0;
}

static int
calibration_child_high_bit_failure(void)
{
   return 1 << 8;
}

/* Run the complete child verdict boundary shared by calibration and sweep
 * legs: fork, normalize the body result before _exit(), wait for the child,
 * and decode WEXITSTATUS in the parent.  A signaled child has no exit byte,
 * so the caller receives false and keeps that distinction in its verdict.
 */
static bool
run_child(const char *label, int (*body)(void), int *exit_status)
{
   fflush(NULL);
   pid_t pid = fork();
   if (pid == 0) {
      const int body_status = body();
      _exit(child_exit_status(body_status));
   }
   if (pid < 0) {
      fprintf(stderr, "FAIL: %s: fork failed\n", label);
      return false;
   }

   int status = 0;
   if (waitpid(pid, &status, 0) != pid) {
      fprintf(stderr, "FAIL: %s: waitpid failed\n", label);
      return false;
   }
   if (WIFSIGNALED(status)) {
      fprintf(stderr, "FAIL: %s: killed by signal %d\n", label,
              WTERMSIG(status));
      return false;
   }
   if (!WIFEXITED(status)) {
      fprintf(stderr, "FAIL: %s: child did not exit\n", label);
      return false;
   }

   *exit_status = WEXITSTATUS(status);
   return true;
}

static bool
mapped_range_setup_ready(VkResult allocation_result, VkResult map_result,
                         const void *live_map)
{
   return allocation_result == VK_SUCCESS && map_result == VK_SUCCESS &&
          live_map != NULL;
}

/* Host calibration keeps the setup gate and child status boundary explicit:
 * a successful allocation and mapping is the known-good shape, a failed
 * setup is the known-bad shape, and the high-bit body runs through the same
 * fork, _exit(), waitpid(), and WEXITSTATUS path as every sweep leg.
 */
static void
check_loader_sweep_calibration(void)
{
   static const char setup_marker;

   CHECK(mapped_range_setup_ready(VK_SUCCESS, VK_SUCCESS, &setup_marker),
         "successful allocation and mapping enable range validation");
   CHECK(!mapped_range_setup_ready(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS,
                                   &setup_marker),
         "allocation failure disables range validation");
   CHECK(!mapped_range_setup_ready(VK_SUCCESS, VK_ERROR_MEMORY_MAP_FAILED,
                                   &setup_marker),
         "mapping failure disables range validation");
   CHECK(!mapped_range_setup_ready(VK_SUCCESS, VK_SUCCESS, NULL),
         "a NULL mapping disables range validation");

   int exit_status = -1;
   CHECK(run_child("known-good child verdict", calibration_child_success,
                   &exit_status) &&
            exit_status == 0,
         "known-good child result is %d, expected 0", exit_status);
   exit_status = -1;
   CHECK(run_child("known-bad high-bit child verdict",
                   calibration_child_high_bit_failure, &exit_status) &&
            exit_status == 1,
         "known-bad high-bit child result is %d, expected 1", exit_status);
}

static void
in_child(const char *label, int (*body)(void))
{
   int exit_status = -1;
   if (!run_child(label, body, &exit_status)) {
      failures++;
      return;
   }
   CHECK(exit_status == 0, "%s: exit status %d", label, exit_status);
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
   VkSampler sampler = VK_NULL_HANDLE;
   VkResult result = vkCreateSampler(
      device, &(VkSamplerCreateInfo){
                 .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
              },
      NULL, &sampler);
   if (result != VK_SUCCESS || sampler == VK_NULL_HANDLE)
      return 1;
   vkDestroySampler(device, sampler, NULL);
   return 0;
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
   VkQueryPool pool = VK_NULL_HANDLE;
   VkResult result = vkCreateQueryPool(
      device,
      &(VkQueryPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
         .queryType = VK_QUERY_TYPE_OCCLUSION,
         .queryCount = 4,
      },
      NULL, &pool);
   if (result != VK_SUCCESS || pool == VK_NULL_HANDLE)
      return 1;
   vkDestroyQueryPool(device, pool, NULL);
   return 0;
}

/* The descriptor-set-layout surface is live for the compute route:
 * the storage-buffer contract admits (the empty layout among it), and
 * a binding type outside it refuses with the handle cleared.
 */
static int
call_create_descriptor_set_layout(void)
{
   VkDescriptorSetLayout layout = VK_NULL_HANDLE;
   VkResult result = vkCreateDescriptorSetLayout(
      device,
      &(VkDescriptorSetLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      },
      NULL, &layout);
   if (result != VK_SUCCESS || layout == VK_NULL_HANDLE)
      return 1;
   vkDestroyDescriptorSetLayout(device, layout, NULL);

   layout = (VkDescriptorSetLayout)(uintptr_t)0x1;
   result = vkCreateDescriptorSetLayout(
      device,
      &(VkDescriptorSetLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 1,
         .pBindings =
            &(VkDescriptorSetLayoutBinding){
               .binding = 0,
               .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
               .descriptorCount = 1,
               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            },
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

/* The parent allocates and maps this VkDeviceMemory before in_child() forks,
 * so every range child inherits a live handle and mapping and issues no GEM
 * create of its own.  The shim's test_fork_child_close_preserves_parent_bo
 * calibration preserves a parent BO across child activity, while
 * drm_shim_atfork_child resets child synchronization
 * (rg --fixed-strings test_fork_child_close_preserves_parent_bo
 * src/amd/drm-shim/radeon_noop_drm_shim.c; rg --fixed-strings
 * drm_shim_atfork_child src/drm-shim/drm_shim.c).  The range verdict therefore
 * comes from r3v_FlushMappedMemoryRanges and
 * r3v_InvalidateMappedMemoryRanges, not a child allocation path
 * (rg --fixed-strings r3v_FlushMappedMemoryRanges src/amd/r300/vulkan/).
 */
static VkDeviceMemory live_memory = VK_NULL_HANDLE;
static VkDeviceSize live_size;
static VkDeviceSize atom_size = 1;

/* VkMappedMemoryRange.memory must be currently host mapped under
 * VUID-VkMappedMemoryRange-memory-00684 in the Vulkan 1.4 Device Memory
 * chapter.  The native flush and invalidate entrypoints dispatch to
 * r3v_native_validate_mapped_ranges, and each child leg reports one aggregate
 * nonzero verdict when that validation disagrees
 * (rg --fixed-strings r3v_native_validate_mapped_ranges
 * src/amd/r300/vulkan/).
 */
static int
call_memory_range_validation(void)
{
   const VkMappedMemoryRange base = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = live_memory,
   };
   /* VUID-VkMappedMemoryRange-size-00685 and
    * VUID-VkMappedMemoryRange-size-00686 keep each range inside the mapped
    * allocation.  The native bound implementation is
    * r3v_native_validate_mapped_ranges (rg --fixed-strings
    * r3v_native_validate_mapped_ranges src/amd/r300/vulkan/), so the first
    * three cases exercise valid ranges and the remaining cases exercise its
    * refusal boundaries.  The native memory table marks memoryTypeIndex 0
    * VK_MEMORY_PROPERTY_HOST_COHERENT_BIT through
    * r3v_GetPhysicalDeviceMemoryProperties2 (rg --fixed-strings
    * r3v_GetPhysicalDeviceMemoryProperties2 src/amd/r300/vulkan/), so the
    * conditional VUID-VkMappedMemoryRange-offset-00687 alignment rule is not
    * a verdict for this allocation.  The ordinary offsets and sizes use
    * nonCoherentAtomSize multiples as test-table geometry; VK_WHOLE_SIZE and
    * the overflowing size remain explicit whole-range and bounds cases.
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

   /* The native validator loops over zero entries and returns VK_SUCCESS for
    * this implementation edge (rg --fixed-strings
    * r3v_native_validate_mapped_ranges src/amd/r300/vulkan/).  The Vulkan
    * memoryRangeCount > 0 rule in
    * VUID-vkFlushMappedMemoryRanges-memoryRangeCount-arraylength and
    * VUID-vkInvalidateMappedMemoryRanges-memoryRangeCount-arraylength remains
    * a separate contract from this loader-sweep boundary.
    */
   if (vkFlushMappedMemoryRanges(device, 0, NULL) != VK_SUCCESS ||
       vkInvalidateMappedMemoryRanges(device, 0, NULL) != VK_SUCCESS)
      rc |= 1 << ARRAY_SIZE(cases);

   /* vkGetDeviceMemoryCommitment reports commitment for a memory type with
    * VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT (Vulkan 1.4 Device Memory,
    * vkGetDeviceMemoryCommitment reference page).  The native memory table
    * advertises host-visible and device-local types without that flag, and
    * r3v_GetDeviceMemoryCommitment writes zero
    * (rg --fixed-strings r3v_GetPhysicalDeviceMemoryProperties2
    * src/amd/r300/vulkan/; rg --fixed-strings
    * r3v_GetDeviceMemoryCommitment src/amd/r300/vulkan/).
    */
   VkDeviceSize committed = 1;
   vkGetDeviceMemoryCommitment(device, live_memory, &committed);
   if (committed != 0)
      rc |= 1 << (ARRAY_SIZE(cases) + 1);

   return rc != 0;
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
   check_loader_sweep_calibration();

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

   /* The allocation uses four nonCoherentAtomSize multiples as deterministic
    * test-table geometry for the mapped-range bounds cases.  The native
    * memory table marks memoryTypeIndex 0 HOST_COHERENT, so this allocation
    * does not claim the conditional offset-alignment VUID
    * (rg --fixed-strings r3v_GetPhysicalDeviceMemoryProperties2
    * src/amd/r300/vulkan/; rg --fixed-strings
    * r3v_native_validate_mapped_ranges src/amd/r300/vulkan/).
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

   const VkResult allocation_result = vkAllocateMemory(
      device, &(VkMemoryAllocateInfo){
                 .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                 .allocationSize = live_size,
                 .memoryTypeIndex = 0,
              },
      NULL, &live_memory);
   CHECK(allocation_result == VK_SUCCESS,
         "vkAllocateMemory through the loader: %d", allocation_result);

   /* Vulkan 1.4 Device Memory requires VkMappedMemoryRange.memory to be
    * currently host mapped (VUID-VkMappedMemoryRange-memory-00684), so map
    * success and a non-NULL pointer gate call_memory_range_validation.  The
    * native implementation's r3v_MapMemory and
    * r3v_native_validate_mapped_ranges symbols carry this setup contract
    * (rg --fixed-strings r3v_MapMemory src/amd/r300/vulkan/; rg
    * --fixed-strings r3v_native_validate_mapped_ranges src/amd/r300/vulkan/).
    */
   void *live_map = NULL;
   VkResult map_result = VK_ERROR_INITIALIZATION_FAILED;
   if (allocation_result == VK_SUCCESS) {
      map_result = vkMapMemory(device, live_memory, 0, VK_WHOLE_SIZE, 0,
                               &live_map);
      CHECK(map_result == VK_SUCCESS && live_map != NULL,
            "vkMapMemory over the live allocation: %d", map_result);
   }

   const bool live_memory_ready =
      mapped_range_setup_ready(allocation_result, map_result, live_map);

   in_child("vkCreateImage refuses", call_create_image);
   in_child("vkCreateEvent refuses", call_create_event);
   in_child("vkCreateSampler records state and destroys",
            call_create_sampler);
   in_child("vkCreateDescriptorPool refuses", call_create_descriptor_pool);
   in_child("vkCreateQueryPool constructs the occlusion pool",
            call_create_query_pool);
   in_child("vkCreateDescriptorSetLayout admits the storage contract "
            "and refuses outside it",
            call_create_descriptor_set_layout);
   in_child("vkCreateBufferView refuses over a live buffer",
            call_create_buffer_view);
   in_child("vkDestroy* over the null handle", call_destroy_image_null);
   in_child("vkUpdateDescriptorSets over zero writes",
            call_update_descriptor_sets_empty);
   in_child("mapped-range commands over zero ranges", call_flush_empty);
   if (live_memory_ready) {
      in_child("mapped-range validation over a live allocation",
               call_memory_range_validation);
   } else if (allocation_result != VK_SUCCESS) {
      fprintf(stderr,
              "NOT RUN: mapped-range validation: allocation returned %d\n",
              allocation_result);
   } else if (map_result != VK_SUCCESS) {
      fprintf(stderr,
              "NOT RUN: mapped-range validation: map returned %d\n",
              map_result);
   } else {
      fprintf(stderr,
              "NOT RUN: mapped-range validation: map returned NULL\n");
   }

   if (live_map != NULL)
      vkUnmapMemory(device, live_memory);
   if (live_memory != VK_NULL_HANDLE)
      vkFreeMemory(device, live_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("r3v-native-loader-sweep: %u failures\n", failures);
   return failures != 0;
}
