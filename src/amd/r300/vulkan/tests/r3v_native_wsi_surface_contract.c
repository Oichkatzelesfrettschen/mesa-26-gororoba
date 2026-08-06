/*
 * SPDX-License-Identifier: MIT
 *
 * The native R3V WSI boundary: surface construction and surface queries at
 * instance scope, with no presentation capability at device scope.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xcb/xcb.h>

/* The build defines VK_USE_PLATFORM_XCB_KHR for the WSI-enabled tree, which
 * is what brings vulkan_xcb.h in with the header below.
 */
#include <vulkan/vulkan.h>

/* The ICD entry the loader would call: this contract measures the driver, so
 * the implementation archive links in directly.
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

#define LOAD(instance, name) \
   PFN_##name name = (PFN_##name)vk_icdGetInstanceProcAddr(instance, #name)

int
main(void)
{
   int screen_index = 0;
   xcb_connection_t *connection = xcb_connect(NULL, &screen_index);
   if (connection == NULL || xcb_connection_has_error(connection)) {
      /* The X server is the fixture, so its absence is a skip rather than a
       * verdict; meson reads 77 as skipped.
       */
      fprintf(stderr, "SKIP: no X connection for the surface contract\n");
      return 77;
   }

   const xcb_setup_t *setup = xcb_get_setup(connection);
   xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
   xcb_window_t window = xcb_generate_id(connection);
   xcb_create_window(connection, XCB_COPY_FROM_PARENT, window, screen->root,
                     0, 0, 64, 64, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                     screen->root_visual, 0, NULL);
   xcb_flush(connection);

   VkInstance instance = VK_NULL_HANDLE;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)vk_icdGetInstanceProcAddr(NULL,
                                                      "vkCreateInstance");
   static const char *const instance_extensions[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      VK_KHR_XCB_SURFACE_EXTENSION_NAME,
   };
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo =
            &(VkApplicationInfo){
               .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
               .apiVersion = VK_API_VERSION_1_0,
            },
         .enabledExtensionCount = 2,
         .ppEnabledExtensionNames = instance_extensions,
      },
      NULL, &instance);
   CHECK(result == VK_SUCCESS,
         "the surface extensions enable at instance creation: %d", result);
   if (result != VK_SUCCESS)
      return 1;

   LOAD(instance, vkCreateXcbSurfaceKHR);
   LOAD(instance, vkDestroySurfaceKHR);
   LOAD(instance, vkGetPhysicalDeviceSurfaceSupportKHR);
   LOAD(instance, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
   LOAD(instance, vkGetPhysicalDeviceSurfaceFormatsKHR);
   LOAD(instance, vkGetPhysicalDeviceSurfacePresentModesKHR);
   LOAD(instance, vkEnumeratePhysicalDevices);
   LOAD(instance, vkGetPhysicalDeviceQueueFamilyProperties);
   LOAD(instance, vkDestroyInstance);
   CHECK(vkCreateXcbSurfaceKHR && vkDestroySurfaceKHR &&
            vkGetPhysicalDeviceSurfaceSupportKHR,
         "the enabled surface extension resolves its entrypoints");
   if (!vkCreateXcbSurfaceKHR || !vkDestroySurfaceKHR ||
       !vkGetPhysicalDeviceSurfaceSupportKHR)
      return 1;

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   CHECK(pdev != VK_NULL_HANDLE, "one physical device enumerates: %d",
         result);
   if (pdev == VK_NULL_HANDLE)
      return 1;

   VkSurfaceKHR surface = VK_NULL_HANDLE;
   result = vkCreateXcbSurfaceKHR(
      instance,
      &(VkXcbSurfaceCreateInfoKHR){
         .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
         .connection = connection,
         .window = window,
      },
      NULL, &surface);
   CHECK(result == VK_SUCCESS && surface != VK_NULL_HANDLE,
         "vkCreateXcbSurfaceKHR: %d", result);
   if (surface == VK_NULL_HANDLE)
      return 1;

   /* The surface exists and answers queries; presentation is what the native
    * device withholds.  Every queue family reports no present support, so an
    * application selecting a present-capable family finds none and never
    * reaches the swapchain entrypoints the device leaves absent.
    */
   uint32_t family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &family_count, NULL);
   for (uint32_t i = 0; i < family_count; i++) {
      VkBool32 supported = VK_TRUE;
      result = vkGetPhysicalDeviceSurfaceSupportKHR(pdev, i, surface,
                                                    &supported);
      CHECK(result == VK_SUCCESS && supported == VK_FALSE,
            "queue family %u reports present support %u (query %d)", i,
            supported, result);
      printf("queue family %u present support: %u\n", i, supported);
   }

   /* The capability queries answer without reaching a swapchain, so an
    * application probing the surface completes its survey.
    */
   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps);
   printf("surface capabilities query: %d, images %u..%u\n", result,
          caps.minImageCount, caps.maxImageCount);

   uint32_t format_count = 0;
   result = vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface,
                                                 &format_count, NULL);
   printf("surface formats query: %d, %u formats\n", result, format_count);

   uint32_t mode_count = 0;
   result = vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface,
                                                      &mode_count, NULL);
   printf("surface present modes query: %d, %u modes\n", result, mode_count);

   vkDestroySurfaceKHR(instance, surface, NULL);
   vkDestroyInstance(instance, NULL);
   xcb_destroy_window(connection, window);
   xcb_disconnect(connection);

   printf("r3v-native-wsi-surface-contract: %u failures\n", failures);
   return failures != 0;
}
