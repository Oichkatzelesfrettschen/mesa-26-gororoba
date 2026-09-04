/*
 * SPDX-License-Identifier: MIT
 *
 * What R3V_NATIVE_EXECUTION_POLICY=gpu_only refuses.
 *
 * The policy is the submit's, and its whole content is that a caller who
 * asked for the device is told the device did not run rather than handed a
 * host result.  A command buffer carrying recorded work no GPU route
 * performs is exactly that case, so the submit refuses and the destination
 * is left as the application published it.
 *
 * Two shapes no route claims, each under both policies.  One command
 * buffer carrying two fills is refused because this route claims a buffer
 * whose whole content is one fill; two command buffers carrying one fill
 * each are refused because the route admits a submit of exactly one, and
 * that refusal must land ahead of the first buffer's execution rather than
 * after it.  Under AUTO the host store loop performs both shapes and the
 * destination reads the pattern; under gpu_only the submit refuses and
 * every byte still reads the sentinel.  The AUTO legs are the calibration,
 * so each refusal names the policy rather than a recording the driver would
 * have refused anyway.
 *
 * Each leg forks, because the policy is read once at device creation from
 * the environment the process started the device with.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#define BUFFER_BYTES 4096u
#define FILL_VALUE 0x11223344u
#define SENTINEL 0xa5a5a5a5u

static unsigned failures;

#define CHECK(condition, ...)                                                \
   do {                                                                      \
      if (!(condition)) {                                                     \
         failures++;                                                          \
         fprintf(stderr, "FAIL: ");                                           \
         fprintf(stderr, __VA_ARGS__);                                        \
         fprintf(stderr, "\n");                                               \
      }                                                                       \
   } while (0)

#define REQUIRE(condition, ...)                                              \
   do {                                                                      \
      if (!(condition)) {                                                     \
         failures++;                                                          \
         fprintf(stderr, "FAIL: ");                                           \
         fprintf(stderr, __VA_ARGS__);                                        \
         fprintf(stderr, "\n");                                               \
         return 1;                                                            \
      }                                                                       \
   } while (0)

/* Records one fill per command buffer over its own half of the
 * destination, and submits every buffer in one submit.  One buffer
 * carrying two fills and two buffers carrying one each are both shapes no
 * route claims, and the second is the shape whose refusal must land ahead
 * of the first buffer's execution.  Returns the submit's result and writes
 * whether the destination changed. */
static int
fill_submit(unsigned command_buffers, bool *filled_out,
            VkResult *submitted_out)
{
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   REQUIRE(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS,
           "instance creation");

   uint32_t count = 1;
   VkPhysicalDevice pdev;
   const VkResult enumerated =
      vkEnumeratePhysicalDevices(instance, &count, &pdev);
   REQUIRE((enumerated == VK_SUCCESS || enumerated == VK_INCOMPLETE) &&
              count == 1,
           "physical device enumeration");

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
   VkDevice device;
   REQUIRE(vkCreateDevice(pdev, &device_info, NULL, &device) == VK_SUCCESS,
           "device creation");

   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUFFER_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer buffer;
   REQUIRE(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS,
           "buffer creation");
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, buffer, &requirements);
   const VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      /* Type 0 is the one host-visible native type. */
      .memoryTypeIndex = 0,
   };
   VkDeviceMemory memory;
   REQUIRE(vkAllocateMemory(device, &allocate_info, NULL, &memory) ==
              VK_SUCCESS,
           "memory allocation");
   REQUIRE(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS,
           "buffer bind");
   void *map = NULL;
   REQUIRE(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &map) ==
              VK_SUCCESS,
           "memory map");
   uint32_t *words = map;
   for (uint32_t i = 0; i < BUFFER_BYTES / 4; i++)
      words[i] = SENTINEL;

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool;
   REQUIRE(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS,
           "command pool");
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd[2];
   REQUIRE(command_buffers <= 2, "the fixture records at most two buffers");
   VkCommandBufferAllocateInfo alloc = cmd_info;
   alloc.commandBufferCount = command_buffers;
   REQUIRE(vkAllocateCommandBuffers(device, &alloc, cmd) == VK_SUCCESS,
           "command buffer allocation");

   const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
   };
   for (unsigned i = 0; i < command_buffers; i++) {
      REQUIRE(vkBeginCommandBuffer(cmd[i], &begin_info) == VK_SUCCESS,
              "command buffer begin");
      const unsigned fills = command_buffers == 1 ? 2u : 1u;
      for (unsigned f = 0; f < fills; f++) {
         const VkDeviceSize half = BUFFER_BYTES / 2;
         const VkDeviceSize offset =
            (command_buffers == 1 ? f : i) * half;
         vkCmdFillBuffer(cmd[i], buffer, offset, half, FILL_VALUE);
      }
      REQUIRE(vkEndCommandBuffer(cmd[i]) == VK_SUCCESS,
              "command buffer end");
   }

   VkQueue queue;
   vkGetDeviceQueue(device, 0, 0, &queue);
   const VkSubmitInfo submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = command_buffers,
      .pCommandBuffers = cmd,
   };
   *submitted_out = vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);

   unsigned changed = 0;
   for (uint32_t i = 0; i < BUFFER_BYTES / 4; i++)
      changed += words[i] != SENTINEL;
   *filled_out = changed != 0;
   return 0;
}

static int
auto_one_buffer_leg(void)
{
   unsetenv("R3V_NATIVE_EXECUTION_POLICY");
   bool filled = false;
   VkResult submitted = VK_SUCCESS;
   if (fill_submit(1, &filled, &submitted) != 0)
      return 1;
   CHECK(submitted == VK_SUCCESS, "AUTO submit returns %d", submitted);
   CHECK(filled, "AUTO leaves the destination unwritten");
   return failures != 0 ? 1 : 0;
}

static int
auto_two_buffers_leg(void)
{
   unsetenv("R3V_NATIVE_EXECUTION_POLICY");
   bool filled = false;
   VkResult submitted = VK_SUCCESS;
   if (fill_submit(2, &filled, &submitted) != 0)
      return 1;
   CHECK(submitted == VK_SUCCESS,
         "AUTO two-buffer submit returns %d", submitted);
   CHECK(filled, "AUTO leaves the destination unwritten");
   return failures != 0 ? 1 : 0;
}

static int
gpu_only_one_buffer_leg(void)
{
   setenv("R3V_NATIVE_EXECUTION_POLICY", "gpu_only", 1);
   bool filled = false;
   VkResult submitted = VK_SUCCESS;
   if (fill_submit(1, &filled, &submitted) != 0)
      return 1;
   CHECK(submitted != VK_SUCCESS,
         "GPU_ONLY admits a submit no GPU route performs");
   CHECK(!filled, "GPU_ONLY wrote the destination on the host path");
   return failures != 0 ? 1 : 0;
}

/* The refusal lands ahead of the first command buffer's execution, so a
 * refused two-buffer submit leaves both halves of the destination at their
 * sentinel.  A refusal taken per command buffer would run the first half
 * and then report the second. */
static int
gpu_only_two_buffers_leg(void)
{
   setenv("R3V_NATIVE_EXECUTION_POLICY", "gpu_only", 1);
   bool filled = false;
   VkResult submitted = VK_SUCCESS;
   if (fill_submit(2, &filled, &submitted) != 0)
      return 1;
   CHECK(submitted != VK_SUCCESS,
         "GPU_ONLY admits a two-buffer submit no GPU route performs");
   CHECK(!filled,
         "GPU_ONLY executed a command buffer before refusing the submit");
   return failures != 0 ? 1 : 0;
}

static int
run_leg(const char *label, int (*body)(void))
{
   fflush(NULL);
   const pid_t pid = fork();
   if (pid == 0)
      _exit(body() == 0 ? 0 : 1);
   int status = 0;
   if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
       WEXITSTATUS(status) != 0) {
      fprintf(stderr, "FAIL: %s leg\n", label);
      return 1;
   }
   return 0;
}

int
main(void)
{
   unsigned legs = 0;
   legs += run_leg("auto-one-buffer", auto_one_buffer_leg);
   legs += run_leg("auto-two-buffers", auto_two_buffers_leg);
   legs += run_leg("gpu_only-one-buffer", gpu_only_one_buffer_leg);
   legs += run_leg("gpu_only-two-buffers", gpu_only_two_buffers_leg);
   if (legs != 0) {
      fprintf(stderr, "r3v-native-execution-policy: %u leg(s) failed\n", legs);
      return 1;
   }
   printf("r3v-native-execution-policy: all legs passed\n");
   return 0;
}
