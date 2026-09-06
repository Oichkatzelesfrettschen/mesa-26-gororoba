/*
 * SPDX-License-Identifier: MIT
 *
 * Loader-only application for a declared measurement campaign: several
 * repetitions of several declared vkCmdFillBuffer cases over one
 * allocation, submitted through the installed Vulkan loader to whichever
 * ICD VK_DRIVER_FILES names.  The binary links libvulkan and libc alone
 * and defines no driver symbol, so the declaration load, the case
 * binding, the durable claim, and the consumption are all reached the way
 * an application reaches them.
 *
 * The oracle belongs to this application rather than to the driver.  The
 * driver closes the campaign on a submission or completion failure; the
 * bytes are checked here, after the fence, and a mismatch stops the
 * campaign immediately and publishes no further sample.  The drm-shim
 * performs no fill, so the oracle for a routed repetition is that every
 * byte still carries the image this application wrote.
 *
 * Observations are collected in preallocated storage during the run and
 * printed after it, so a measured repetition carries no allocation and no
 * write outside its own transport.
 *
 * Fixtures, each read from the environment:
 *   R3V_CAMPAIGN_CASES     off:bytes:value:count[,...], the cases this
 *                          run submits, in order
 *   R3V_CAMPAIGN_EXCESS    1 submits one repetition past the declared
 *                          budget and requires it refused
 *   R3V_CAMPAIGN_CONTINUE_AFTER_FAILURE
 *                          1 keeps submitting after a refusal, so a row
 *                          can read what the repetitions after the first
 *                          failure do rather than only the first
 *   R3V_CAMPAIGN_ALLOCATION_BYTES  the one allocation's size
 *   R3V_EXPECTED_ICD_DSO   the DSO the loader must have mapped
 *
 * --arm names the shape:
 *   repetitions   every case, its declared count of times
 *   undeclared    one fill outside every declared case
 *   mixed         two fills in one command buffer
 *   rebound       one declared repetition, then the same case over a
 *                 fresh allocation the buffer rebinds to
 *   oracle-stop   one declared repetition, then a mismatch this
 *                 application introduces, then no further submission
 *
 * Exit status: 0 the expected outcome, 1 a verdict failure, 2 a usage or
 * fixture failure, 3 an oracle failure.
 */

#include "amd/r300/common/r300_chip_identity.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CAMPAIGN_MAX_CASES 8u
#define CAMPAIGN_MAX_SAMPLES 4096u
#define IMAGE_BYTE 0x5au

enum arm {
   ARM_REPETITIONS,
   ARM_UNDECLARED,
   ARM_MIXED,
   ARM_REBOUND,
   ARM_ORACLE_STOP,
};

struct campaign_case {
   uint64_t offset;
   uint64_t bytes;
   uint32_t value;
   uint32_t count;
};

/* One repetition's compact observation, filled inside the run and
 * printed after it. */
struct sample {
   uint32_t case_index;
   uint32_t repetition;
   VkResult submitted;
   VkResult waited;
   uint64_t shim_cs_delta;
   bool oracle_passed;
};

static struct campaign_case cases[CAMPAIGN_MAX_CASES];
static uint32_t case_count;
static struct sample samples[CAMPAIGN_MAX_SAMPLES];
static uint32_t sample_count;
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
   case VK_ERROR_INITIALIZATION_FAILED:
      return "VK_ERROR_INITIALIZATION_FAILED";
   case VK_NOT_READY:
      return "VK_NOT_READY";
   default:
      return "(other)";
   }
}

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

/* Parses off:bytes:value:count[,...] into the case table. */
static bool
parse_cases(const char *text)
{
   while (*text != '\0' && case_count < CAMPAIGN_MAX_CASES) {
      char *end = NULL;
      errno = 0;
      const unsigned long long offset = strtoull(text, &end, 0);
      if (errno != 0 || end == text || *end != ':')
         return false;
      text = end + 1;
      const unsigned long long bytes = strtoull(text, &end, 0);
      if (errno != 0 || end == text || *end != ':')
         return false;
      text = end + 1;
      const unsigned long long value = strtoull(text, &end, 0);
      if (errno != 0 || end == text || *end != ':')
         return false;
      text = end + 1;
      const unsigned long long count = strtoull(text, &end, 0);
      if (errno != 0 || end == text || (*end != ',' && *end != '\0'))
         return false;
      if (value > UINT32_MAX || count == 0 || count > UINT32_MAX)
         return false;
      cases[case_count++] = (struct campaign_case){
         .offset = offset,
         .bytes = bytes,
         .value = (uint32_t)value,
         .count = (uint32_t)count,
      };
      text = *end == ',' ? end + 1 : end;
   }
   return case_count != 0 && *text == '\0';
}

struct run {
   VkDevice device;
   VkQueue queue;
   VkCommandPool pool;
   VkBuffer buffer;
   uint8_t *map;
   uint64_t allocation_bytes;
};

/* Records one command buffer of `fills` fills, submits it, waits, and
 * reads the shim's counter across the pair. */
static VkResult
submit_fills(struct run *run, const struct campaign_case *fills,
             uint32_t fills_count, VkResult *waited_out,
             uint64_t *cs_delta_out)
{
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   VkResult r = vkAllocateCommandBuffers(
      run->device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = run->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd);
   if (r != VK_SUCCESS)
      return r;
   r = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
           });
   if (r != VK_SUCCESS)
      return r;
   for (uint32_t i = 0; i < fills_count; i++) {
      vkCmdFillBuffer(cmd, run->buffer, fills[i].offset, fills[i].bytes,
                      fills[i].value);
   }
   r = vkEndCommandBuffer(cmd);
   if (r != VK_SUCCESS)
      return r;

   VkFence fence = VK_NULL_HANDLE;
   r = vkCreateFence(run->device,
                     &(VkFenceCreateInfo){
                        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                     },
                     NULL, &fence);
   if (r != VK_SUCCESS)
      return r;

   uint64_t before = 0, after = 0;
   read_shim_cs_count(&before);
   const VkResult submitted =
      vkQueueSubmit(run->queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd,
                    },
                    fence);
   VkResult waited = VK_NOT_READY;
   if (submitted == VK_SUCCESS) {
      waited = vkWaitForFences(run->device, 1, &fence, VK_TRUE,
                               (uint64_t)30 * 1000 * 1000 * 1000);
   }
   read_shim_cs_count(&after);
   *waited_out = waited;
   *cs_delta_out = after - before;
   vkDestroyFence(run->device, fence, NULL);
   vkFreeCommandBuffers(run->device, run->pool, 1, &cmd);
   return submitted;
}

/* The application's own oracle: the drm-shim writes nothing, so a routed
 * repetition leaves the image this run wrote. */
static bool
oracle_holds(const struct run *run)
{
   for (uint64_t i = 0; i < run->allocation_bytes; i++) {
      if (run->map[i] != IMAGE_BYTE)
         return false;
   }
   return true;
}

int
main(int argc, char **argv)
{
   enum arm arm = ARM_REPETITIONS;
   const char *arm_name = "repetitions";
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--arm") == 0 && i + 1 < argc) {
         arm_name = argv[++i];
      } else {
         fprintf(stderr, "usage: %s [--arm <name>]\n", argv[0]);
         return 2;
      }
   }
   if (strcmp(arm_name, "repetitions") == 0)
      arm = ARM_REPETITIONS;
   else if (strcmp(arm_name, "undeclared") == 0)
      arm = ARM_UNDECLARED;
   else if (strcmp(arm_name, "mixed") == 0)
      arm = ARM_MIXED;
   else if (strcmp(arm_name, "rebound") == 0)
      arm = ARM_REBOUND;
   else if (strcmp(arm_name, "oracle-stop") == 0)
      arm = ARM_ORACLE_STOP;
   else {
      fprintf(stderr, "no arm is named %s\n", arm_name);
      return 2;
   }

   const char *case_text = getenv("R3V_CAMPAIGN_CASES");
   if (case_text == NULL || !parse_cases(case_text)) {
      fprintf(stderr, "R3V_CAMPAIGN_CASES is absent or malformed\n");
      return 2;
   }
   const char *allocation_text = getenv("R3V_CAMPAIGN_ALLOCATION_BYTES");
   const uint64_t allocation_bytes =
      allocation_text != NULL ? strtoull(allocation_text, NULL, 0) : 65536ull;
   if (allocation_bytes == 0 || allocation_bytes > (1u << 24)) {
      fprintf(stderr, "R3V_CAMPAIGN_ALLOCATION_BYTES is outside the fixture "
                      "range\n");
      return 2;
   }
   const char *excess_text = getenv("R3V_CAMPAIGN_EXCESS");
   const bool excess = excess_text != NULL && strcmp(excess_text, "1") == 0;
   const char *continue_text =
      getenv("R3V_CAMPAIGN_CONTINUE_AFTER_FAILURE");
   const bool continue_after_failure =
      continue_text != NULL && strcmp(continue_text, "1") == 0;
   const char *expected_dso = getenv("R3V_EXPECTED_ICD_DSO");
   if (expected_dso == NULL || expected_dso[0] == '\0') {
      fprintf(stderr, "R3V_EXPECTED_ICD_DSO is unset\n");
      return 2;
   }
   uint64_t cs_at_start = 0;
   if (!read_shim_cs_count(&cs_at_start)) {
      fprintf(stderr, "the radeon drm-shim is not preloaded\n");
      return 2;
   }
   /* One fact per line: the check script reads each key anchored at the
    * start of a line. */
   printf("arm=%s\ncases=%u\nallocation_bytes=%llu\nexcess=%d\n", arm_name,
          case_count, (unsigned long long)allocation_bytes, excess ? 1 : 0);

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
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || pdev_count != 1) {
      fprintf(stderr, "vkEnumeratePhysicalDevices: %s\n", result_name(r));
      return 2;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
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
   printf("create_device=%s\n", result_name(r));
   if (r != VK_SUCCESS) {
      /* A declaration the driver refused is a verdict this application
       * reports rather than a fixture failure: the refusal legs read it. */
      printf("r3v-measurement-campaign-application: DEVICE_REFUSED\n");
      vkDestroyInstance(instance, NULL);
      return 1;
   }

   struct run run = { .device = device, .allocation_bytes = allocation_bytes };
   vkGetDeviceQueue(device, 0, 0, &run.queue);

   r = vkCreateBuffer(device,
                      &(VkBufferCreateInfo){
                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                         .size = allocation_bytes,
                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                      },
                      NULL, &run.buffer);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateBuffer: %s\n", result_name(r));
      return 2;
   }
   VkDeviceMemory memory = VK_NULL_HANDLE;
   r = vkAllocateMemory(device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = allocation_bytes,
                           .memoryTypeIndex = 0,
                        },
                        NULL, &memory);
   if (r != VK_SUCCESS || vkBindBufferMemory(device, run.buffer, memory, 0) !=
                             VK_SUCCESS) {
      fprintf(stderr, "the destination could not be allocated or bound\n");
      return 2;
   }
   r = vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&run.map);
   if (r != VK_SUCCESS || run.map == NULL) {
      fprintf(stderr, "vkMapMemory: %s\n", result_name(r));
      return 2;
   }
   memset(run.map, IMAGE_BYTE, allocation_bytes);

   r = vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
         .queueFamilyIndex = 0,
      },
      NULL, &run.pool);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateCommandPool: %s\n", result_name(r));
      return 2;
   }

   uint32_t stopped_at = UINT32_MAX;
   switch (arm) {
   case ARM_UNDECLARED: {
      /* One fill no declared case names.  Its offset sits a dword past
       * the first case, so every other fact matches and the undeclared
       * range is the single variable. */
      const struct campaign_case undeclared = {
         .offset = cases[0].offset + 4,
         .bytes = cases[0].bytes,
         .value = cases[0].value,
         .count = 1,
      };
      VkResult waited = VK_NOT_READY;
      uint64_t delta = 0;
      const VkResult submitted =
         submit_fills(&run, &undeclared, 1, &waited, &delta);
      samples[sample_count++] = (struct sample){
         .case_index = UINT32_MAX,
         .submitted = submitted,
         .waited = waited,
         .shim_cs_delta = delta,
         .oracle_passed = oracle_holds(&run),
      };
      REQUIRE(submitted != VK_SUCCESS, 1,
              "the undeclared fill was admitted");
      REQUIRE(delta == 0, 1, "the undeclared fill reached the shim");
      break;
   }
   case ARM_MIXED: {
      /* Two fills in one command buffer: the declared case and one more.
       * The route admits one fill per command buffer, and a declared
       * campaign refuses the wider shape rather than letting the host
       * perform it. */
      struct campaign_case pair[2] = { cases[0], cases[0] };
      pair[1].offset = cases[0].offset + cases[0].bytes;
      VkResult waited = VK_NOT_READY;
      uint64_t delta = 0;
      const VkResult submitted = submit_fills(&run, pair, 2, &waited, &delta);
      samples[sample_count++] = (struct sample){
         .case_index = UINT32_MAX,
         .submitted = submitted,
         .waited = waited,
         .shim_cs_delta = delta,
         .oracle_passed = oracle_holds(&run),
      };
      REQUIRE(submitted != VK_SUCCESS, 1, "the mixed recording was admitted");
      REQUIRE(delta == 0, 1, "the mixed recording reached the shim");
      break;
   }
   case ARM_REBOUND: {
      /* One declared repetition binds the case to this allocation.  The
       * buffer then binds to a second allocation and the same declared
       * fill is submitted again: every declared fact still matches and
       * the buffer object underneath is another one. */
      VkResult waited = VK_NOT_READY;
      uint64_t delta = 0;
      VkResult submitted = submit_fills(&run, &cases[0], 1, &waited, &delta);
      samples[sample_count++] = (struct sample){
         .submitted = submitted,
         .waited = waited,
         .shim_cs_delta = delta,
         .oracle_passed = oracle_holds(&run),
      };
      REQUIRE(submitted == VK_SUCCESS && delta == 1, 1,
              "the first declared repetition did not execute");

      vkUnmapMemory(device, memory);
      vkDestroyBuffer(device, run.buffer, NULL);
      vkFreeMemory(device, memory, NULL);
      memory = VK_NULL_HANDLE;
      r = vkCreateBuffer(device,
                         &(VkBufferCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = allocation_bytes,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                         },
                         NULL, &run.buffer);
      VkDeviceMemory replacement = VK_NULL_HANDLE;
      if (r != VK_SUCCESS ||
          vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = allocation_bytes,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &replacement) != VK_SUCCESS ||
          vkBindBufferMemory(device, run.buffer, replacement, 0) !=
             VK_SUCCESS ||
          vkMapMemory(device, replacement, 0, VK_WHOLE_SIZE, 0,
                      (void **)&run.map) != VK_SUCCESS) {
         fprintf(stderr, "the replacement destination could not be built\n");
         return 2;
      }
      memory = replacement;
      memset(run.map, IMAGE_BYTE, allocation_bytes);

      submitted = submit_fills(&run, &cases[0], 1, &waited, &delta);
      samples[sample_count++] = (struct sample){
         .repetition = 1,
         .submitted = submitted,
         .waited = waited,
         .shim_cs_delta = delta,
         .oracle_passed = oracle_holds(&run),
      };
      REQUIRE(submitted != VK_SUCCESS, 1,
              "the rebound destination was admitted");
      REQUIRE(delta == 0, 1, "the rebound destination reached the shim");
      break;
   }
   case ARM_REPETITIONS:
   case ARM_ORACLE_STOP: {
      for (uint32_t c = 0; c < case_count && stopped_at == UINT32_MAX; c++) {
         for (uint32_t n = 0; n < cases[c].count; n++) {
            VkResult waited = VK_NOT_READY;
            uint64_t delta = 0;
            const VkResult submitted =
               submit_fills(&run, &cases[c], 1, &waited, &delta);
            bool passed = oracle_holds(&run);
            if (arm == ARM_ORACLE_STOP && sample_count == 0) {
               /* The mismatch this application introduces after the
                * first completion.  The oracle is the application's, so
                * the campaign stops here without the driver refusing
                * anything. */
               run.map[0] ^= 0xffu;
               passed = oracle_holds(&run);
            }
            if (sample_count < CAMPAIGN_MAX_SAMPLES) {
               samples[sample_count++] = (struct sample){
                  .case_index = c,
                  .repetition = n,
                  .submitted = submitted,
                  .waited = waited,
                  .shim_cs_delta = delta,
                  .oracle_passed = passed,
               };
            }
            REQUIRE(submitted == VK_SUCCESS, 1,
                    "case %u repetition %u was refused: %s", c, n,
                    result_name(submitted));
            REQUIRE(delta == 1, 1,
                    "case %u repetition %u reached the shim %llu times", c, n,
                    (unsigned long long)delta);
            if ((!passed || submitted != VK_SUCCESS) &&
                !continue_after_failure) {
               stopped_at = sample_count;
               break;
            }
         }
      }
      if (arm == ARM_ORACLE_STOP) {
         /* The verdict is the stop itself, and it rests on the
          * submission having run: the mismatch is written
          * unconditionally, so a refused submission would fail the
          * oracle too and the arm would report the stop it never
          * demonstrated.  The first sample therefore has to carry a
          * successful submit and its one CS. */
         REQUIRE(sample_count == 1, 1,
                 "the campaign published %u samples after the mismatch",
                 sample_count);
         REQUIRE(sample_count == 1 && samples[0].submitted == VK_SUCCESS, 1,
                 "the repetition before the mismatch did not execute");
         REQUIRE(sample_count == 1 && samples[0].shim_cs_delta == 1, 1,
                 "the repetition before the mismatch reached no ioctl");
         REQUIRE(sample_count == 1 && !samples[0].oracle_passed, 1,
                 "the introduced mismatch did not fail the oracle");
      } else if (excess) {
         /* One repetition past the declared budget. */
         VkResult waited = VK_NOT_READY;
         uint64_t delta = 0;
         const VkResult submitted =
            submit_fills(&run, &cases[0], 1, &waited, &delta);
         printf("excess_result=%s\nexcess_shim_cs=%llu\n",
                result_name(submitted), (unsigned long long)delta);
         REQUIRE(submitted != VK_SUCCESS, 1,
                 "the excess request was admitted");
         REQUIRE(delta == 0, 1, "the excess request reached the shim");
      }
      break;
   }
   }

   uint64_t cs_at_end = 0;
   read_shim_cs_count(&cs_at_end);
   uint32_t oracle_passes = 0;
   for (uint32_t i = 0; i < sample_count; i++)
      oracle_passes += samples[i].oracle_passed ? 1u : 0u;
   printf("samples=%u\noracle_passes=%u\nshim_cs_total=%llu\n", sample_count,
          oracle_passes, (unsigned long long)(cs_at_end - cs_at_start));
   for (uint32_t i = 0; i < sample_count; i++) {
      printf("sample case=%d repetition=%u submit=%s fence=%s cs=%llu "
             "oracle=%d\n",
             samples[i].case_index == UINT32_MAX
                ? -1
                : (int)samples[i].case_index,
             samples[i].repetition, result_name(samples[i].submitted),
             result_name(samples[i].waited),
             (unsigned long long)samples[i].shim_cs_delta,
             samples[i].oracle_passed ? 1 : 0);
   }

   if (run.map != NULL)
      vkUnmapMemory(device, memory);
   vkDestroyCommandPool(device, run.pool, NULL);
   vkDestroyBuffer(device, run.buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("r3v-measurement-campaign-application: %s\n",
          status == 0 ? "PASS" : "FAIL");
   return status;
}
