/*
 * SPDX-License-Identifier: MIT
 *
 * The native R3V WSI boundary: surface construction and surface queries at
 * instance scope, with no presentation capability at device scope.
 */

#include <stdbool.h>
#include <stdint.h>
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

/* Calibration for the query verdicts: R3V_WSI_FIXTURE_QUERY_ERROR names one
 * query and replaces only that loaded pointer.  The capabilities value targets
 * the minimum-count verdict, formats-full/formats-short and
 * modes-full/modes-short target one array verdict each, and the -error
 * suffixes retain direct query-error fixtures.  Each substitution lands after
 * entrypoint loading and before the first call, so the fixture reaches its
 * target branch independently.
 */
static bool
fixture_selects(const char *query)
{
   const char *selected = getenv("R3V_WSI_FIXTURE_QUERY_ERROR");
   return selected != NULL && strcmp(selected, query) == 0;
}

static bool
allocation_size_overflows(uint32_t count, size_t element_size)
{
   size_t count_size = count;
   return element_size != 0 && count_size > SIZE_MAX / element_size;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_capabilities_error(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                           VkSurfaceCapabilitiesKHR *pCaps)
{
   (void)pdev;
   (void)surface;
   (void)pCaps;
   return VK_ERROR_SURFACE_LOST_KHR;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_capabilities_min_count(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                               VkSurfaceCapabilitiesKHR *pCaps)
{
   (void)pdev;
   (void)surface;
   *pCaps = (VkSurfaceCapabilitiesKHR){
      .minImageCount = 0,
   };
   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_formats_error(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                      uint32_t *pCount, VkSurfaceFormatKHR *pFormats)
{
   (void)pdev;
   (void)surface;
   (void)pFormats;
   *pCount = 0;
   return VK_ERROR_SURFACE_LOST_KHR;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_formats_full_array(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                            uint32_t *pCount, VkSurfaceFormatKHR *pFormats)
{
   (void)pdev;
   (void)surface;

   /* The count call must succeed.  The full-array call reports too few
    * elements, while the short-array call follows the Vulkan contract.
    */
   if (pFormats == NULL) {
      *pCount = 2;
      return VK_SUCCESS;
   }

   if (*pCount > 1) {
      *pCount = 1;
      return VK_INCOMPLETE;
   }

   return VK_INCOMPLETE;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_formats_short_array(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                             uint32_t *pCount, VkSurfaceFormatKHR *pFormats)
{
   (void)pdev;
   (void)surface;

   /* The count and full-array calls follow the Vulkan contract.  The
    * short-array call returns success, which is the deliberate defect.
    */
   if (pFormats == NULL) {
      *pCount = 2;
      return VK_SUCCESS;
   }

   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_modes_full_array(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                         uint32_t *pCount, VkPresentModeKHR *pModes)
{
   (void)pdev;
   (void)surface;

   /* The count call must succeed.  The full-array call reports too few
    * elements, while the short-array call follows the Vulkan contract.
    */
   if (pModes == NULL) {
      *pCount = 2;
      return VK_SUCCESS;
   }

   if (*pCount > 1) {
      *pCount = 1;
      return VK_INCOMPLETE;
   }

   return VK_INCOMPLETE;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_modes_short_array(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                          uint32_t *pCount, VkPresentModeKHR *pModes)
{
   (void)pdev;
   (void)surface;

   /* The count and full-array calls follow the Vulkan contract.  The
    * short-array call returns success, which is the deliberate defect.
    */
   if (pModes == NULL) {
      *pCount = 2;
      return VK_SUCCESS;
   }

   return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
fixture_modes_error(VkPhysicalDevice pdev, VkSurfaceKHR surface,
                    uint32_t *pCount, VkPresentModeKHR *pModes)
{
   (void)pdev;
   (void)surface;
   (void)pModes;
   *pCount = 0;
   return VK_ERROR_SURFACE_LOST_KHR;
}

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
   CHECK(create_instance != NULL, "vkCreateInstance resolves");
   if (create_instance == NULL)
      goto out_window;

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
      goto out_window;

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
            vkGetPhysicalDeviceSurfaceSupportKHR &&
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR &&
            vkGetPhysicalDeviceSurfaceFormatsKHR &&
            vkGetPhysicalDeviceSurfacePresentModesKHR &&
            vkEnumeratePhysicalDevices &&
            vkGetPhysicalDeviceQueueFamilyProperties && vkDestroyInstance,
         "the WSI and instance entrypoints resolve");
   if (!vkCreateXcbSurfaceKHR || !vkDestroySurfaceKHR ||
       !vkGetPhysicalDeviceSurfaceSupportKHR ||
       !vkGetPhysicalDeviceSurfaceCapabilitiesKHR ||
       !vkGetPhysicalDeviceSurfaceFormatsKHR ||
       !vkGetPhysicalDeviceSurfacePresentModesKHR ||
       !vkEnumeratePhysicalDevices ||
       !vkGetPhysicalDeviceQueueFamilyProperties || !vkDestroyInstance)
      goto out_instance;

   if (fixture_selects("capabilities"))
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR =
         fixture_capabilities_min_count;
   else if (fixture_selects("capabilities-error"))
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR = fixture_capabilities_error;
   if (fixture_selects("formats-full"))
      vkGetPhysicalDeviceSurfaceFormatsKHR = fixture_formats_full_array;
   else if (fixture_selects("formats-short"))
      vkGetPhysicalDeviceSurfaceFormatsKHR = fixture_formats_short_array;
   else if (fixture_selects("formats-error"))
      vkGetPhysicalDeviceSurfaceFormatsKHR = fixture_formats_error;
   if (fixture_selects("modes-full"))
      vkGetPhysicalDeviceSurfacePresentModesKHR = fixture_modes_full_array;
   else if (fixture_selects("modes-short"))
      vkGetPhysicalDeviceSurfacePresentModesKHR = fixture_modes_short_array;
   else if (fixture_selects("modes-error"))
      vkGetPhysicalDeviceSurfacePresentModesKHR = fixture_modes_error;

   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   CHECK(pdev != VK_NULL_HANDLE, "one physical device enumerates: %d",
         result);
   if (pdev == VK_NULL_HANDLE)
      goto out_instance;

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
   if (result != VK_SUCCESS || surface == VK_NULL_HANDLE)
      goto out_instance;

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

   /* The queries answer without reaching a swapchain, and the survey an
    * application runs over the surface is what they have to complete: a
    * capability set it can size an image list against, and an enumeration of
    * at least one format and one present mode to choose between.  A query
    * that reports an error leaves that survey unfinished, so each result
    * carries the verdict.
    */
   VkSurfaceCapabilitiesKHR caps;
   memset(&caps, 0, sizeof(caps));
   result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pdev, surface, &caps);
   CHECK(result == VK_SUCCESS,
         "vkGetPhysicalDeviceSurfaceCapabilitiesKHR: %d", result);
   /* Per the Vulkan VkSurfaceCapabilitiesKHR refpage
    * (https://docs.vulkan.org/refpages/latest/refpages/source/VkSurfaceCapabilitiesKHR.html),
    * maxImageCount is zero when the surface sets no upper bound, so
    * minImageCount is the floor that fixes a usable image count.
    */
   CHECK(result != VK_SUCCESS || caps.minImageCount >= 1,
         "the surface reports minImageCount %u", caps.minImageCount);
   printf("surface capabilities query: %d, images %u..%u\n", result,
          caps.minImageCount, caps.maxImageCount);

   uint32_t format_count = 0;
   result = vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface,
                                                 &format_count, NULL);
   CHECK(result == VK_SUCCESS,
         "vkGetPhysicalDeviceSurfaceFormatsKHR count query: %d", result);
   CHECK(result != VK_SUCCESS || format_count > 0,
         "the surface enumerates %u formats", format_count);
   printf("surface formats query: %d, %u formats\n", result, format_count);

   if (result == VK_SUCCESS && format_count > 0) {
      if (allocation_size_overflows(format_count,
                                    sizeof(VkSurfaceFormatKHR))) {
         CHECK(false, "the format count fits the allocation size");
         goto out_surface;
      }

      VkSurfaceFormatKHR *formats =
         malloc(format_count * sizeof(*formats));
      CHECK(formats != NULL, "the format array allocation succeeds");
      if (formats == NULL)
         goto out_surface;

      uint32_t written = format_count;
      result = vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface, &written,
                                                    formats);
      CHECK(result == VK_SUCCESS && written == format_count,
            "the format array query is %d and writes %u of %u", result,
            written, format_count);

      /* The Vulkan refpage for
       * vkGetPhysicalDeviceSurfaceFormatsKHR
       * (https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceSurfaceFormatsKHR.html)
       * requires VK_INCOMPLETE when the offered array is shorter than the
       * available list.  The two-call idiom therefore proves that the
       * enumeration honors its count parameter rather than writing past it.
       */
      if (format_count > 1) {
         written = format_count - 1;
         result = vkGetPhysicalDeviceSurfaceFormatsKHR(pdev, surface,
                                                       &written, formats);
         CHECK(result == VK_INCOMPLETE && written == format_count - 1,
               "the short format array query is %d and writes %u of the %u "
               "offered", result, written, format_count - 1);
      }
      free(formats);
   }

   uint32_t mode_count = 0;
   result = vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface,
                                                      &mode_count, NULL);
   CHECK(result == VK_SUCCESS,
         "vkGetPhysicalDeviceSurfacePresentModesKHR count query: %d", result);
   CHECK(result != VK_SUCCESS || mode_count > 0,
         "the surface enumerates %u present modes", mode_count);
   printf("surface present modes query: %d, %u modes\n", result, mode_count);

   if (result == VK_SUCCESS && mode_count > 0) {
      if (allocation_size_overflows(mode_count, sizeof(VkPresentModeKHR))) {
         CHECK(false, "the present-mode count fits the allocation size");
         goto out_surface;
      }

      VkPresentModeKHR *modes = malloc(mode_count * sizeof(*modes));
      CHECK(modes != NULL, "the present-mode array allocation succeeds");
      if (modes == NULL)
         goto out_surface;

      uint32_t written = mode_count;
      result = vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface,
                                                         &written, modes);
      CHECK(result == VK_SUCCESS && written == mode_count,
            "the present-mode array query is %d and writes %u of %u", result,
            written, mode_count);

      /* The Vulkan refpage for
       * vkGetPhysicalDeviceSurfacePresentModesKHR
       * (https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceSurfacePresentModesKHR.html)
       * gives the same VK_INCOMPLETE contract for a short present-mode array.
       */
      if (mode_count > 1) {
         written = mode_count - 1;
         result = vkGetPhysicalDeviceSurfacePresentModesKHR(pdev, surface,
                                                            &written, modes);
         CHECK(result == VK_INCOMPLETE && written == mode_count - 1,
               "the short present-mode array query is %d and writes %u of "
               "the %u offered", result, written, mode_count - 1);
      }
      free(modes);
   }

out_surface:
   vkDestroySurfaceKHR(instance, surface, NULL);
out_instance:
   if (vkDestroyInstance != NULL)
      vkDestroyInstance(instance, NULL);
out_window:
   xcb_destroy_window(connection, window);
   xcb_disconnect(connection);

   printf("r3v-native-wsi-surface-contract: %u failures\n", failures);
   return failures != 0;
}
