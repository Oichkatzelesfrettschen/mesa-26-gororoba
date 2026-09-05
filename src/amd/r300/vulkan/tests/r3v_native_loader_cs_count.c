/*
 * SPDX-License-Identifier: MIT
 *
 * How many command submissions an application makes by loading the driver.
 *
 * The radeon drm-shim answers DRM_RADEON_CS in place of the kernel, and it
 * is the one place a submission can arrive, so the count its handler keeps
 * is the number of submissions the process made.  An application that
 * creates an instance, enumerates physical devices, creates a device, takes
 * its queue, and destroys all three submits nothing: the driver's route
 * runs only under its gate, the gate is closed, and no recorded work
 * exists.  The
 * loader leg reads that count back as zero.
 *
 * A zero on its own proves nothing, so the calibration leg issues one
 * DRM_RADEON_CS through the same interposed ioctl path and reads the count
 * back as one.  Both legs run in the same process shape under the same
 * preload, so the loader leg's zero is the absence of a submission rather
 * than the absence of a counter.
 *
 * The claim stops at the shim: it absorbs the ioctl, so nothing here
 * reaches a kernel.  What the legs establish is that the driver's own
 * loading and device creation make no command submission.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <radeon_drm.h>
#include <vulkan/vulkan.h>

static unsigned failures;

#define CHECK(condition, ...)                                                \
   do {                                                                      \
      if (!(condition)) {                                                    \
         failures++;                                                         \
         fprintf(stderr, "FAIL: ");                                          \
         fprintf(stderr, __VA_ARGS__);                                       \
         fprintf(stderr, "\n");                                              \
      }                                                                      \
   } while (0)

/* The preloaded shim exports the count with default visibility, so the
 * symbol resolves through the global scope the preload put it in.  An
 * absent symbol is a fixture failure rather than a zero count: a run
 * without the shim would otherwise report the strongest possible result
 * for the weakest possible reason. */
static bool
read_cs_count(uint64_t *out)
{
   uint64_t (*counter)(void) =
      (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "drm_shim_test_radeon_cs_ioctls");
   if (counter == NULL)
      return false;
   *out = counter();
   return true;
}

/* The whole application: load the driver, reach a device and its queue,
 * and give them back. */
static void
loader_only(void)
{
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   if (vkCreateInstance(&instance_info, NULL, &instance) != VK_SUCCESS) {
      CHECK(false, "the loader does not create an instance");
      return;
   }

   uint32_t count = 0;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS ||
       count == 0) {
      CHECK(false, "the loader enumerates no physical device");
      vkDestroyInstance(instance, NULL);
      return;
   }
   count = 1;
   vkEnumeratePhysicalDevices(instance, &count, &pdev);

   VkPhysicalDeviceProperties properties;
   vkGetPhysicalDeviceProperties(pdev, &properties);

   uint32_t families = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pdev, &families, NULL);
   CHECK(families != 0, "the physical device advertises no queue family");

   const float priority = 1.0f;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
   };
   VkDevice device = VK_NULL_HANDLE;
   if (vkCreateDevice(pdev, &device_info, NULL, &device) == VK_SUCCESS) {
      VkQueue queue = VK_NULL_HANDLE;
      vkGetDeviceQueue(device, 0, 0, &queue);
      CHECK(queue != VK_NULL_HANDLE, "the device yields no queue");
      vkDestroyDevice(device, NULL);
   } else {
      CHECK(false, "the loader does not create a device");
   }
   vkDestroyInstance(instance, NULL);
}

/* One submission through the same interposed ioctl path the driver's own
 * transport takes, so the loader leg's zero is a measured absence. */
static bool
issue_one_submission(void)
{
   const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      CHECK(false, "the render node does not open: %s", strerror(errno));
      return false;
   }
   struct drm_radeon_cs request;
   memset(&request, 0, sizeof(request));
   const int result = ioctl(fd, DRM_IOCTL_RADEON_CS, &request);
   close(fd);
   CHECK(result == 0, "the shim refuses a submission with %d", result);
   return result == 0;
}

int
main(int argc, char **argv)
{
   const bool calibrate = argc > 1 && strcmp(argv[1], "--calibrate") == 0;

   uint64_t before = 0;
   if (!read_cs_count(&before)) {
      fprintf(stderr,
              "FAIL: the shim's submission counter is not reachable; the "
              "run carries no fixture\n");
      return 1;
   }
   CHECK(before == 0, "the process starts with %llu submissions",
         (unsigned long long)before);

   if (calibrate) {
      if (issue_one_submission()) {
         uint64_t after = 0;
         CHECK(read_cs_count(&after), "the counter became unreachable");
         CHECK(after == before + 1,
               "one submission moved the count from %llu to %llu",
               (unsigned long long)before, (unsigned long long)after);
      }
   } else {
      loader_only();
      uint64_t after = 0;
      CHECK(read_cs_count(&after), "the counter became unreachable");
      CHECK(after == 0,
            "loading the driver made %llu kernel-entering DRM_RADEON_CS "
            "submissions", (unsigned long long)after);
   }

   if (failures != 0) {
      fprintf(stderr, "%u check(s) failed\n", failures);
      return 1;
   }
   printf("r3v-native-loader-cs-count%s: passed\n",
          calibrate ? " (calibration)" : "");
   return 0;
}
