/*
 * SPDX-License-Identifier: MIT
 *
 * Direct-table sweep of the native R3V public surface: the ICD's own
 * procaddr entrypoints, with no Vulkan loader in the process.
 */

#include "r3v_native_surface.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* The ICD entry the loader would call.  Linking the implementation archive
 * directly puts the driver's own dispatch construction under test: a NULL
 * this sweep reports is a table the driver built that way, with no loader
 * trampoline standing between the query and the answer.
 */
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

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

/* Resolve one command through the procaddr entrypoint its scope reaches.
 * Device-scope commands answer through vkGetDeviceProcAddr; every other scope
 * answers through vkGetInstanceProcAddr, which also answers device-scope
 * names for an application that never obtained a device.
 */
static PFN_vkVoidFunction
resolve(const struct r3v_surface_command *cmd, VkInstance instance,
        VkDevice device, PFN_vkGetDeviceProcAddr gdpa)
{
   if (cmd->scope == R3V_SCOPE_DEVICE)
      return gdpa(device, cmd->name);
   return vk_icdGetInstanceProcAddr(instance, cmd->name);
}

/* Calibration for the absent legs, spelled "<table label>:<command>" in
 * R3V_SURFACE_FIXTURE_LEAK.  Naming a command the surface really provides
 * appends it as a row of the named table, where it meets the same loop and the
 * same comparison as the generated rows.  A leg that stopped judging its rows
 * reports its ordinary pass rather than the leak, which is what makes the
 * fixture measure the loop under test instead of a check beside it.
 */
static const struct r3v_surface_command *
with_injected_row(const char *label, const struct r3v_surface_command *table,
                  uint32_t *count)
{
   const char *fixture = getenv("R3V_SURFACE_FIXTURE_LEAK");
   if (fixture == NULL)
      return table;

   const char *sep = strchr(fixture, ':');
   if (sep == NULL || (size_t)(sep - fixture) != strlen(label) ||
       strncmp(fixture, label, sep - fixture) != 0)
      return table;

   struct r3v_surface_command *injected =
      malloc((*count + 1) * sizeof(*injected));
   if (injected == NULL)
      return table;
   memcpy(injected, table, *count * sizeof(*injected));
   injected[*count] = (struct r3v_surface_command){ sep + 1,
                                                    R3V_SCOPE_DEVICE };
   (*count)++;
   printf("%s: fixture row %s injected\n", label, sep + 1);
   return injected;
}

/* Absence carries a verdict at device scope alone.  vk_instance_get_proc_addr
 * answers an instance-scope name straight from the instance dispatch table and
 * a physical-device-scope name from the trampoline table, both without
 * consulting the requested API version or the enabled extensions, because the
 * ICD interface makes that filtering the loader's job; the device-scope lookup
 * runs through vk_device_dispatch_table_get_if_supported and does apply it.
 * The loader black-box sweep carries the verdict for the two unfiltered
 * scopes, and this sweep records what the ICD answers there.
 */
static void
sweep(const char *label, const struct r3v_surface_command *table,
      uint32_t count, bool expect_present, VkInstance instance,
      VkDevice device, PFN_vkGetDeviceProcAddr gdpa)
{
   const struct r3v_surface_command *generated = table;
   table = with_injected_row(label, table, &count);

   unsigned judged = 0, judged_present = 0, observed_present = 0;
   unsigned observed = 0;
   for (uint32_t i = 0; i < count; i++) {
      const bool got = resolve(&table[i], instance, device, gdpa) != NULL;
      const bool judge =
         expect_present || table[i].scope == R3V_SCOPE_DEVICE;
      if (!judge) {
         observed++;
         observed_present += got;
         continue;
      }
      judged++;
      judged_present += got;
      CHECK(got == expect_present, "%s: %s resolves %s", label,
            table[i].name, got ? "non-NULL" : "NULL");
   }
   printf("%s: %u judged, %u present, %u expected", label, judged,
          judged_present, expect_present ? judged : 0);
   if (observed > 0)
      printf("; %u unfiltered instance/physical-device names observed, "
             "%u present", observed, observed_present);
   printf("\n");

   if (table != generated)
      free((void *)table);
}

/* A Vulkan 1.0 implementation returns VK_ERROR_INCOMPATIBLE_DRIVER when the
 * application requests a later version, and apiVersion 0 requests 1.0.  The
 * native ICD reports an instance version of 1.0, so vk_instance_init applies
 * that rule and the four calibrations below fix which versions it admits.
 */
static void
sweep_instance_versions(PFN_vkCreateInstance create_instance,
                        PFN_vkDestroyInstance destroy_instance)
{
   static const struct {
      uint32_t version;
      VkResult expected;
      const char *label;
   } cases[] = {
      { 0, VK_SUCCESS, "apiVersion 0 requests Vulkan 1.0" },
      { VK_API_VERSION_1_0, VK_SUCCESS, "Vulkan 1.0" },
      { VK_API_VERSION_1_1, VK_ERROR_INCOMPATIBLE_DRIVER, "Vulkan 1.1" },
      { VK_API_VERSION_1_3, VK_ERROR_INCOMPATIBLE_DRIVER, "Vulkan 1.3" },
   };

   for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
      VkInstance probe = VK_NULL_HANDLE;
      const VkApplicationInfo app = {
         .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
         .apiVersion = cases[i].version,
      };
      VkResult result = create_instance(
         &(VkInstanceCreateInfo){
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app,
         },
         NULL, &probe);
      CHECK(result == cases[i].expected, "%s: vkCreateInstance is %d, %d "
            "expected", cases[i].label, result, cases[i].expected);
      printf("instance version: %s -> %d\n", cases[i].label, result);
      if (result == VK_SUCCESS)
         destroy_instance(probe, NULL);
   }
}

/* The native product boundary is surface queries without presentation: the
 * instance may construct and query a surface, and the device advertises no
 * swapchain capability.  The device extension table is empty, so
 * vkEnumerateDeviceExtensionProperties reports nothing and VK_KHR_swapchain
 * is unreachable; the one queue family advertises no capability bit, so no
 * family can present.  The absence of the swapchain entrypoints themselves is
 * the closed-extension leg of the procaddr sweep.
 */
static void
sweep_wsi_contract(VkInstance instance, VkPhysicalDevice pdev)
{
   PFN_vkEnumerateDeviceExtensionProperties enumerate_device_ext =
      (PFN_vkEnumerateDeviceExtensionProperties)vk_icdGetInstanceProcAddr(
         instance, "vkEnumerateDeviceExtensionProperties");
   CHECK(enumerate_device_ext != NULL,
         "vkEnumerateDeviceExtensionProperties resolves");
   if (enumerate_device_ext == NULL)
      return;

   /* The advertised set is exactly the memory-requirements contract the
    * native image path executes: the *2 query and bind entry points and
    * the dedicated-allocation signal.  The check compares names, so the
    * count is a consequence of the identity rather than a proxy for it.
    */
   static const char *const expected_extensions[] = {
      "VK_KHR_get_memory_requirements2",
      "VK_KHR_bind_memory2",
      "VK_KHR_dedicated_allocation",
   };
   uint32_t device_ext_count = 0;
   VkResult result =
      enumerate_device_ext(pdev, NULL, &device_ext_count, NULL);
   CHECK(result == VK_SUCCESS && device_ext_count == 3,
         "the device advertises the three memory-contract extensions: "
         "%d count %u", result, device_ext_count);
   if (result == VK_SUCCESS && device_ext_count == 3) {
      VkExtensionProperties properties[3];
      result = enumerate_device_ext(pdev, NULL, &device_ext_count,
                                    properties);
      CHECK(result == VK_SUCCESS, "extension enumeration fills: %d",
            result);
      for (unsigned e = 0; e < 3; e++) {
         int found = 0;
         for (unsigned p = 0; p < 3; p++) {
            if (strcmp(properties[p].extensionName,
                       expected_extensions[e]) == 0)
               found = 1;
         }
         CHECK(found, "the device advertises %s", expected_extensions[e]);
      }
   }
   printf("device extensions: %u\n", device_ext_count);

   PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_families =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
         vk_icdGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceQueueFamilyProperties");
   CHECK(queue_families != NULL,
         "vkGetPhysicalDeviceQueueFamilyProperties resolves");
   if (queue_families == NULL)
      return;

   uint32_t family_count = 0;
   queue_families(pdev, &family_count, NULL);
   CHECK(family_count >= 1 && family_count <= 4,
         "the physical device advertises %u queue families", family_count);
   if (family_count == 0 || family_count > 4)
      return;

   VkQueueFamilyProperties families[4];
   queue_families(pdev, &family_count, families);
   for (uint32_t i = 0; i < family_count; i++) {
      /* The advertised capability set equals the recording surface: the
       * public render-pass/pipeline/draw subset records graphics work,
       * so GRAPHICS is claimed and every other bit waits for the
       * surface that executes it.
       */
      CHECK(families[i].queueFlags == VK_QUEUE_GRAPHICS_BIT,
            "queue family %u advertises flags 0x%x; the recording "
            "surface executes exactly VK_QUEUE_GRAPHICS_BIT", i,
            families[i].queueFlags);
      printf("queue family %u: flags 0x%x, %u queues\n", i,
             families[i].queueFlags, families[i].queueCount);
   }
}

/* Each scope answers through its own procaddr entrypoint, so a device table
 * holding an instance- or physical-device-scope command would let a program
 * dispatch it against a device that never owned it.  vkGetDeviceProcAddr
 * answers none of the 16 core 1.0 names outside device scope.
 */
static void
sweep_scope_separation(VkDevice device, PFN_vkGetDeviceProcAddr gdpa)
{
   unsigned judged = 0;
   for (uint32_t i = 0; i < r3v_surface_core10_count; i++) {
      if (r3v_surface_core10[i].scope == R3V_SCOPE_DEVICE)
         continue;
      judged++;
      CHECK(gdpa(device, r3v_surface_core10[i].name) == NULL,
            "scope separation: vkGetDeviceProcAddr answers the %s-scope "
            "command %s",
            r3v_surface_core10[i].scope == R3V_SCOPE_GLOBAL ? "global" :
            r3v_surface_core10[i].scope == R3V_SCOPE_INSTANCE ? "instance" :
            "physical-device",
            r3v_surface_core10[i].name);
   }
   printf("scope separation: %u non-device core 1.0 names, none answered "
          "by vkGetDeviceProcAddr\n", judged);
}

int
main(void)
{
   VkInstance instance = VK_NULL_HANDLE;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
                                                      "vkCreateInstance");
   CHECK(create_instance != NULL, "vkCreateInstance resolves without an "
                                  "instance");
   if (create_instance == NULL)
      return 1;

   /* Only the global commands answer a NULL instance; every other scope
    * requires one, so a pointer here would be the table answering a query the
    * specification leaves it no instance to resolve against.
    */
   CHECK(vk_icdGetInstanceProcAddr(NULL, "vkDestroyInstance") == NULL &&
            vk_icdGetInstanceProcAddr(NULL, "vkCreateDevice") == NULL &&
            vk_icdGetInstanceProcAddr(NULL, "vkAllocateMemory") == NULL,
         "a NULL instance resolves the global commands alone");


   /* apiVersion 0 requests Vulkan 1.0, so the version filter in
    * vk_device_dispatch_table_get_if_supported answers for the 1.0 set and
    * withholds every later core spelling.
    */
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo =
            &(VkApplicationInfo){
               .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
               .apiVersion = VK_API_VERSION_1_0,
            },
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS, "vkCreateInstance: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   /* vkDestroyInstance is instance-scope, so it resolves only once an
    * instance exists; the version calibration needs it to release each
    * admitted probe.
    */
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)vk_icdGetInstanceProcAddr(instance,
                                                       "vkDestroyInstance");
   CHECK(destroy_instance != NULL, "vkDestroyInstance resolves");
   if (destroy_instance == NULL)
      return 1;
   sweep_instance_versions(create_instance, destroy_instance);

   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)vk_icdGetInstanceProcAddr(
         instance, "vkEnumeratePhysicalDevices");
   PFN_vkCreateDevice create_device =
      (PFN_vkCreateDevice)vk_icdGetInstanceProcAddr(instance,
                                                    "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)vk_icdGetInstanceProcAddr(
         instance, "vkGetDeviceProcAddr");
   CHECK(enumerate && create_device && gdpa,
         "the three bootstrap entrypoints resolve");
   if (!enumerate || !create_device || !gdpa)
      return 1;

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = enumerate(instance, &pdev_count, &pdev);
   CHECK((result == VK_SUCCESS || result == VK_INCOMPLETE) &&
            pdev != VK_NULL_HANDLE,
         "one shim physical device enumerates: %d count %u", result,
         pdev_count);
   if (pdev == VK_NULL_HANDLE)
      return 1;

   sweep_wsi_contract(instance, pdev);

   const float queue_priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
   result = create_device(
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

   sweep_scope_separation(device, gdpa);

   sweep("core 1.0", r3v_surface_core10, r3v_surface_core10_count, true,
         instance, device, gdpa);
   sweep("higher core", r3v_surface_higher_core,
         r3v_surface_higher_core_count, false, instance, device, gdpa);
   sweep("promoted alias", r3v_surface_alias, r3v_surface_alias_count, false,
         instance, device, gdpa);
   sweep("closed extension", r3v_surface_closed_extension,
         r3v_surface_closed_extension_count, false, instance, device, gdpa);

   /* The device-scope members of the absent tables carry the discrimination
    * this sweep rests on: a table answering every name would report the same
    * complete core 1.0 result, and only a judged absence separates them.
    */
   unsigned device_scope_absent = 0;
   for (uint32_t i = 0; i < r3v_surface_higher_core_count; i++)
      device_scope_absent +=
         r3v_surface_higher_core[i].scope == R3V_SCOPE_DEVICE;
   for (uint32_t i = 0; i < r3v_surface_alias_count; i++)
      device_scope_absent += r3v_surface_alias[i].scope == R3V_SCOPE_DEVICE;
   for (uint32_t i = 0; i < r3v_surface_closed_extension_count; i++)
      device_scope_absent +=
         r3v_surface_closed_extension[i].scope == R3V_SCOPE_DEVICE;
   CHECK(device_scope_absent > 50,
         "the absent tables judge %u device-scope names, too few to "
         "discriminate a table that answers everything", device_scope_absent);

   PFN_vkDestroyDevice destroy_device =
      (PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice");
   destroy_device(device, NULL);
   destroy_instance(instance, NULL);

   printf("r3v-native-direct-table-sweep: %u failures\n", failures);
   return failures != 0;
}
