/*
 * SPDX-License-Identifier: MIT
 *
 * RB2D constant-fill crossover harness: measures the cost of one
 * vkCmdFillBuffer over a size sweep on the host route and on the
 * windowed RB2D GPU route, so an automatic-selection threshold rests on
 * measured execution rather than on byte size alone.
 *
 * The timed interval is the delivery, not the qualification machinery.
 * Both routes perform the fill inside vkQueueSubmit -- recording stores
 * a deferred copy and nothing else -- so the bracket opens immediately
 * before the submit and closes after the fence retires and the memory
 * contract's invalidate returns.  Route choice, legalization, carrier
 * choice, IB construction, relocation construction, the ioctl, hardware
 * execution, and completion all fall inside it; instance, device,
 * buffer, memory, mapping, command-pool and command-buffer creation,
 * the per-batch destination initialization, and the oracle pass all
 * fall outside.
 *
 * One process holds one device per arm because the route gates and the
 * execution policy are read once at vkCreateDevice: the environment is
 * set for each arm in turn and the device that arm submits through
 * carries that arm's gate state for its lifetime.  A single device
 * could not answer for two routes, and two processes could not
 * interleave.  Trials alternate arm order every repetition so a thermal
 * or load drift lands on both arms equally, and the fill value advances
 * per repetition so a destination that kept an earlier repetition's
 * bytes fails the batch oracle.
 */

#include "amd/r300/common/r300_rb2d_legalize.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

/* The destination is initialized to the sentinel before every batch and
 * carries a tail past the largest fill, so a write that runs long is a
 * byte whose value decides its origin. */
#define CROSSOVER_SENTINEL 0xa5u
#define CROSSOVER_TAIL_CANARY 0xc3u
#define CROSSOVER_TAIL_BYTES 64u
#define CROSSOVER_MAX_REPS 4096u
#define CROSSOVER_MAX_SIZES 32u

/* The declared sweep plus the two sizes bracketing the chooser's
 * carrier transition: the execution-floor chooser takes the 256-byte
 * carrier through 2096896 bytes, where one window still covers the
 * interval, and takes 16320 from 2096900 on. */
static const uint64_t default_sizes[] = {
   4u,       64u,      256u,     4096u,    65536u,   524288u,
   2096896u, 2096900u, 2097152u, 8388608u,
};

enum arm_id {
   ARM_HOST = 0,
   ARM_V2,
   ARM_V1,
   ARM_COUNT,
};

struct arm {
   const char *name;
   /* R3V_NATIVE_EXECUTION_POLICY for this arm's device. */
   const char *policy;
   /* The one route gate this arm opens, or NULL for the host arm.  Two
    * open fill gates are refused at device creation, so each arm names
    * at most one. */
   const char *gate;
   bool enabled;
   VkDevice device;
   VkQueue queue;
   VkBuffer buffer;
   VkDeviceMemory memory;
   uint8_t *map;
   bool host_coherent;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   uint64_t samples[CROSSOVER_MAX_REPS];
   uint32_t sample_count;
};

static struct arm arms[ARM_COUNT] = {
   [ARM_HOST] = { .name = "host", .policy = "cpu_reference", .gate = NULL },
   [ARM_V2] = { .name = "v2", .policy = "gpu_only",
                .gate = "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL" },
   [ARM_V1] = { .name = "v1", .policy = "gpu_only",
                .gate = "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL" },
};

static VkInstance instance;
static VkPhysicalDevice physical_device;
static VkPhysicalDeviceProperties device_properties;
static VkPhysicalDeviceMemoryProperties memory_properties;
static uint64_t allocation_bytes;
static uint64_t wait_bound_ns = (uint64_t)10 * 1000 * 1000 * 1000;

static int
refuse(const char *why)
{
   fprintf(stderr, "REFUSED: %s\n", why);
   return 2;
}

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int
compare_u64(const void *a, const void *b)
{
   const uint64_t x = *(const uint64_t *)a;
   const uint64_t y = *(const uint64_t *)b;
   return x < y ? -1 : (x > y ? 1 : 0);
}

/* The order statistic at a fraction of the sorted sample, nearest rank.
 * A sample of one reports that one value at every fraction. */
static uint64_t
quantile(const uint64_t *sorted, uint32_t n, double fraction)
{
   if (n == 0)
      return 0;
   double rank = fraction * (double)(n - 1);
   uint32_t index = (uint32_t)(rank + 0.5);
   if (index >= n)
      index = n - 1;
   return sorted[index];
}

/* Median absolute deviation about the median: the spread statistic that
 * a single stalled repetition cannot move, unlike a standard deviation. */
static uint64_t
median_absolute_deviation(const uint64_t *sorted, uint32_t n, uint64_t median)
{
   static uint64_t deviations[CROSSOVER_MAX_REPS];
   if (n == 0)
      return 0;
   for (uint32_t i = 0; i < n; i++)
      deviations[i] = sorted[i] > median ? sorted[i] - median
                                         : median - sorted[i];
   qsort(deviations, n, sizeof(deviations[0]), compare_u64);
   return quantile(deviations, n, 0.5);
}

/* The legalizer's prediction for one interval on the execution floor:
 * the carrier the chooser takes and the stream shape it produces.  It
 * reads no device, so the harness records the shape a timing row belongs
 * to without deriving it from a submission. */
static void
predict(uint64_t size, struct r300_rb2d_legalize_result *result)
{
   static struct r300_rb2d_window windows[R300_RB2D_LEGALIZE_MAX_WINDOWS];
   const struct r300_rb2d_legalize_request request = {
      .byte_offset = 0,
      .byte_size = size,
      .pattern = 0,
      .bo_size = allocation_bytes,
      .usage = R300_RB2D_USAGE_FILL_BUFFER,
      .contract = R300_RB2D_CONTRACT_CONST_FILL_V2,
      .minimum_evidence = R300_RB2D_PITCH_EVIDENCE_SILICON_RECEIPT,
      .minimum_contract_evidence = R300_RB2D_CONTRACT_EVIDENCE_SILICON_RECEIPT,
      .pinned_pitch_bytes = 0,
   };
   memset(result, 0, sizeof(*result));
   r300_rb2d_legalize_linear_span(&request, windows,
                                  R300_RB2D_LEGALIZE_MAX_WINDOWS, result);
}

static bool
select_memory_type(uint32_t supported, uint32_t *index_out, bool *coherent_out)
{
   /* Host-visible is the requirement: the batch oracle reads the
    * destination through the same mapping the host route writes. */
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
      if ((supported & (1u << i)) == 0)
         continue;
      const VkMemoryPropertyFlags flags =
         memory_properties.memoryTypes[i].propertyFlags;
      if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0)
         continue;
      *index_out = i;
      *coherent_out =
         (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      return true;
   }
   return false;
}

static bool
open_arm(struct arm *arm, char *why, size_t why_size)
{
   /* The gate state this arm needs, installed before vkCreateDevice
    * reads it and cleared for the next arm, so exactly one fill gate
    * stands at each creation. */
   for (uint32_t i = 0; i < ARM_COUNT; i++) {
      if (arms[i].gate != NULL)
         unsetenv(arms[i].gate);
   }
   if (arm->gate != NULL)
      setenv(arm->gate, "1", 1);
   setenv("R3V_NATIVE_EXECUTION_POLICY", arm->policy, 1);

   const float priority = 1.0f;
   VkResult r = vkCreateDevice(
      physical_device,
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
      NULL, &arm->device);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkCreateDevice returned %d",
               arm->name, (int)r);
      return false;
   }
   vkGetDeviceQueue(arm->device, 0, 0, &arm->queue);

   r = vkCreateBuffer(arm->device,
                      &(VkBufferCreateInfo){
                         .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                         .size = allocation_bytes,
                         .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                      },
                      NULL, &arm->buffer);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkCreateBuffer returned %d",
               arm->name, (int)r);
      return false;
   }
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(arm->device, arm->buffer, &requirements);
   uint32_t type_index;
   if (!select_memory_type(requirements.memoryTypeBits, &type_index,
                           &arm->host_coherent)) {
      snprintf(why, why_size, "arm %s: no host-visible memory type",
               arm->name);
      return false;
   }
   r = vkAllocateMemory(arm->device,
                        &(VkMemoryAllocateInfo){
                           .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                           .allocationSize = requirements.size,
                           .memoryTypeIndex = type_index,
                        },
                        NULL, &arm->memory);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkAllocateMemory returned %d",
               arm->name, (int)r);
      return false;
   }
   r = vkBindBufferMemory(arm->device, arm->buffer, arm->memory, 0);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkBindBufferMemory returned %d",
               arm->name, (int)r);
      return false;
   }
   void *map = NULL;
   r = vkMapMemory(arm->device, arm->memory, 0, VK_WHOLE_SIZE, 0, &map);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkMapMemory returned %d", arm->name,
               (int)r);
      return false;
   }
   arm->map = map;

   r = vkCreateCommandPool(arm->device,
                           &(VkCommandPoolCreateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                              .flags =
                                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                              .queueFamilyIndex = 0,
                           },
                           NULL, &arm->pool);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkCreateCommandPool returned %d",
               arm->name, (int)r);
      return false;
   }
   r = vkAllocateCommandBuffers(
      arm->device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = arm->pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &arm->cmd);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkAllocateCommandBuffers returned %d",
               arm->name, (int)r);
      return false;
   }
   r = vkCreateFence(arm->device,
                     &(VkFenceCreateInfo){
                        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                     },
                     NULL, &arm->fence);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkCreateFence returned %d", arm->name,
               (int)r);
      return false;
   }
   return true;
}

static void
close_arm(struct arm *arm)
{
   if (arm->device == VK_NULL_HANDLE)
      return;
   if (arm->fence != VK_NULL_HANDLE)
      vkDestroyFence(arm->device, arm->fence, NULL);
   if (arm->pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(arm->device, arm->pool, NULL);
   if (arm->map != NULL)
      vkUnmapMemory(arm->device, arm->memory);
   if (arm->buffer != VK_NULL_HANDLE)
      vkDestroyBuffer(arm->device, arm->buffer, NULL);
   if (arm->memory != VK_NULL_HANDLE)
      vkFreeMemory(arm->device, arm->memory, NULL);
   vkDestroyDevice(arm->device, NULL);
   arm->device = VK_NULL_HANDLE;
}

/* Restores the whole destination to the sentinel and publishes it, so
 * the batch that follows starts from bytes whose value names their
 * origin.  Runs between batches, never inside a timed interval. */
static bool
initialize_destination(struct arm *arm, char *why, size_t why_size)
{
   memset(arm->map, CROSSOVER_SENTINEL, (size_t)allocation_bytes);
   if (arm->host_coherent)
      return true;
   const VkResult r = vkFlushMappedMemoryRanges(
      arm->device, 1,
      &(VkMappedMemoryRange){
         .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
         .memory = arm->memory,
         .offset = 0,
         .size = VK_WHOLE_SIZE,
      });
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkFlushMappedMemoryRanges returned %d",
               arm->name, (int)r);
      return false;
   }
   return true;
}

/* Records one fill and delivers it, returning the elapsed delivery time.
 * Recording sits outside the bracket because both routes defer the fill
 * to the submission; the bracket therefore contains each arm's own work
 * and nothing else. */
static bool
run_one(struct arm *arm, uint64_t size, uint32_t value, uint64_t *elapsed_ns,
        char *why, size_t why_size)
{
   VkResult r = vkResetCommandBuffer(arm->cmd, 0);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkResetCommandBuffer returned %d",
               arm->name, (int)r);
      return false;
   }
   r = vkBeginCommandBuffer(
      arm->cmd, &(VkCommandBufferBeginInfo){
                   .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                });
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkBeginCommandBuffer returned %d",
               arm->name, (int)r);
      return false;
   }
   vkCmdFillBuffer(arm->cmd, arm->buffer, 0, size, value);
   r = vkEndCommandBuffer(arm->cmd);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkEndCommandBuffer returned %d",
               arm->name, (int)r);
      return false;
   }
   r = vkResetFences(arm->device, 1, &arm->fence);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkResetFences returned %d", arm->name,
               (int)r);
      return false;
   }

   const uint64_t start = now_ns();
   r = vkQueueSubmit(arm->queue, 1,
                     &(VkSubmitInfo){
                        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1,
                        .pCommandBuffers = &arm->cmd,
                     },
                     arm->fence);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkQueueSubmit returned %d", arm->name,
               (int)r);
      return false;
   }
   r = vkWaitForFences(arm->device, 1, &arm->fence, VK_TRUE, wait_bound_ns);
   if (r != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkWaitForFences returned %d",
               arm->name, (int)r);
      return false;
   }
   if (!arm->host_coherent) {
      r = vkInvalidateMappedMemoryRanges(
         arm->device, 1,
         &(VkMappedMemoryRange){
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = arm->memory,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
         });
      if (r != VK_SUCCESS) {
         snprintf(why, why_size,
                  "arm %s: vkInvalidateMappedMemoryRanges returned %d",
                  arm->name, (int)r);
         return false;
      }
   }
   *elapsed_ns = now_ns() - start;
   return true;
}

/* The batch oracle: every dword of the interval carries the last
 * repetition's value and every byte past it carries the sentinel.  A
 * route that wrote nothing, wrote short, wrote an earlier repetition's
 * value, or wrote past the interval fails here, so a timing row that
 * survives rests on delivered bytes. */
static bool
verify_batch(const struct arm *arm, uint64_t size, uint32_t value, char *why,
             size_t why_size)
{
   for (uint64_t i = 0; i < size; i += 4) {
      uint32_t observed;
      memcpy(&observed, arm->map + i, sizeof(observed));
      if (observed != value) {
         snprintf(why, why_size,
                  "arm %s size %" PRIu64 ": dword at %" PRIu64
                  " is 0x%08x, the batch wrote 0x%08x",
                  arm->name, size, i, observed, value);
         return false;
      }
   }
   for (uint64_t i = size; i < allocation_bytes; i++) {
      if (arm->map[i] != CROSSOVER_SENTINEL) {
         snprintf(why, why_size,
                  "arm %s size %" PRIu64 ": byte at %" PRIu64
                  " is 0x%02x past the interval",
                  arm->name, size, i, arm->map[i]);
         return false;
      }
   }
   return true;
}

static void
report_arm(FILE *json, const struct arm *arm, uint64_t size,
           const struct r300_rb2d_legalize_result *shape)
{
   static uint64_t sorted[CROSSOVER_MAX_REPS];
   const uint32_t n = arm->sample_count;
   memcpy(sorted, arm->samples, n * sizeof(sorted[0]));
   qsort(sorted, n, sizeof(sorted[0]), compare_u64);
   const uint64_t median = quantile(sorted, n, 0.5);
   const uint64_t mad = median_absolute_deviation(sorted, n, median);
   const uint64_t p10 = quantile(sorted, n, 0.10);
   const uint64_t p90 = quantile(sorted, n, 0.90);
   printf("size=%-9" PRIu64 " arm=%-5s n=%-4u median_ns=%-10" PRIu64
          " mad_ns=%-8" PRIu64 " p10_ns=%-10" PRIu64 " p90_ns=%-10" PRIu64
          " pitch=%u windows=%u rects=%u sites=%u ib_dwords=%u\n",
          size, arm->name, n, median, mad, p10, p90, shape->pitch_bytes,
          shape->window_count, shape->rect_count, shape->relocation_sites,
          shape->ib_dwords);
   if (json == NULL)
      return;
   fprintf(json,
           "{\"size_bytes\":%" PRIu64 ",\"arm\":\"%s\",\"samples\":%u,"
           "\"median_ns\":%" PRIu64 ",\"mad_ns\":%" PRIu64
           ",\"p10_ns\":%" PRIu64 ",\"p90_ns\":%" PRIu64
           ",\"chosen_pitch_bytes\":%u,\"window_count\":%u,"
           "\"rect_count\":%u,\"relocation_sites\":%u,\"ib_dwords\":%u,"
           "\"raw_ns\":[",
           size, arm->name, n, median, mad, p10, p90, shape->pitch_bytes,
           shape->window_count, shape->rect_count, shape->relocation_sites,
           shape->ib_dwords);
   for (uint32_t i = 0; i < n; i++)
      fprintf(json, "%s%" PRIu64, i == 0 ? "" : ",", arm->samples[i]);
   fprintf(json, "]}\n");
}

static bool
parse_u64(const char *text, uint64_t *out)
{
   char *end = NULL;
   const unsigned long long value = strtoull(text, &end, 0);
   if (text[0] == '\0' || end == NULL || *end != '\0')
      return false;
   *out = value;
   return true;
}

int
main(int argc, char **argv)
{
   uint64_t sizes[CROSSOVER_MAX_SIZES];
   uint32_t size_count = 0;
   uint32_t reps = 32;
   uint32_t warmup = 4;
   const char *json_path = NULL;
   bool run_v1 = true;

   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
         uint64_t value;
         if (!parse_u64(argv[++i], &value) || value == 0 ||
             value > CROSSOVER_MAX_REPS)
            return refuse("--reps is outside (0, 4096]");
         reps = (uint32_t)value;
      } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
         uint64_t value;
         if (!parse_u64(argv[++i], &value) || value > CROSSOVER_MAX_REPS)
            return refuse("--warmup is outside [0, 4096]");
         warmup = (uint32_t)value;
      } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
         json_path = argv[++i];
      } else if (strcmp(argv[i], "--no-v1") == 0) {
         run_v1 = false;
      } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
         uint64_t value;
         if (size_count == CROSSOVER_MAX_SIZES)
            return refuse("more than 32 sizes declared");
         if (!parse_u64(argv[++i], &value) || value == 0 || value % 4 != 0)
            return refuse("--size is not a positive dword multiple");
         sizes[size_count++] = value;
      } else if (strcmp(argv[i], "--wait-bound-ns") == 0 && i + 1 < argc) {
         uint64_t value;
         if (!parse_u64(argv[++i], &value) || value == 0 ||
             value > (uint64_t)120 * 1000 * 1000 * 1000)
            return refuse("--wait-bound-ns is outside (0, 120 s]");
         wait_bound_ns = value;
      } else {
         fprintf(stderr,
                 "usage: %s [--reps N] [--warmup N] [--size BYTES ...] "
                 "[--json PATH] [--no-v1] [--wait-bound-ns NS]\n",
                 argv[0]);
         return 2;
      }
   }
   if (size_count == 0) {
      size_count = (uint32_t)(sizeof(default_sizes) / sizeof(default_sizes[0]));
      memcpy(sizes, default_sizes, sizeof(default_sizes));
   }
   allocation_bytes = 0;
   for (uint32_t i = 0; i < size_count; i++) {
      if (sizes[i] > allocation_bytes)
         allocation_bytes = sizes[i];
   }
   allocation_bytes += CROSSOVER_TAIL_BYTES;

   arms[ARM_HOST].enabled = true;
   arms[ARM_V2].enabled = true;
   arms[ARM_V1].enabled = run_v1;

   /* The GPU arms submit, so the hazard consent stands for the whole
    * process; the host arm performs no submission and is unaffected. */
   setenv("R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED", "1", 1);

   VkResult r = vkCreateInstance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
         .pApplicationInfo =
            &(VkApplicationInfo){
               .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
               .apiVersion = VK_API_VERSION_1_0,
            },
      },
      NULL, &instance);
   if (r != VK_SUCCESS)
      return refuse("vkCreateInstance failed");
   uint32_t device_count = 1;
   r = vkEnumeratePhysicalDevices(instance, &device_count, &physical_device);
   if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || device_count == 0)
      return refuse("no physical device");
   vkGetPhysicalDeviceProperties(physical_device, &device_properties);
   vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
   printf("device=%04x:%04x name=%s allocation_bytes=%" PRIu64
          " reps=%u warmup=%u\n",
          device_properties.vendorID, device_properties.deviceID,
          device_properties.deviceName, allocation_bytes, reps, warmup);

   char why[512];
   int status = 0;
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (!arms[a].enabled)
         continue;
      if (!open_arm(&arms[a], why, sizeof(why))) {
         status = refuse(why);
         goto out;
      }
   }
   printf("arms=");
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (arms[a].enabled)
         printf("%s ", arms[a].name);
   }
   printf("\n");
   fflush(stdout);

   FILE *json = NULL;
   if (json_path != NULL) {
      json = fopen(json_path, "we");
      if (json == NULL) {
         status = refuse("the JSON path cannot be opened for writing");
         goto out;
      }
   }

   for (uint32_t s = 0; s < size_count; s++) {
      const uint64_t size = sizes[s];
      struct r300_rb2d_legalize_result shape;
      predict(size, &shape);

      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (!arms[a].enabled)
            continue;
         arms[a].sample_count = 0;
         if (!initialize_destination(&arms[a], why, sizeof(why))) {
            status = refuse(why);
            goto close_json;
         }
      }
      /* Warm both routes on this size before any sample: the first
       * submission on an interval pays page faults and allocator work
       * that no later one repeats. */
      for (uint32_t w = 0; w < warmup; w++) {
         for (uint32_t a = 0; a < ARM_COUNT; a++) {
            uint64_t discarded;
            if (!arms[a].enabled)
               continue;
            if (!run_one(&arms[a], size, 0x5a5a0000u + w, &discarded, why,
                         sizeof(why))) {
               status = refuse(why);
               goto close_json;
            }
         }
      }
      /* Alternating arm order per repetition, so a drift over the batch
       * falls on each arm in equal measure rather than on whichever ran
       * last. */
      uint32_t value = 0;
      for (uint32_t rep = 0; rep < reps; rep++) {
         for (uint32_t k = 0; k < ARM_COUNT; k++) {
            const uint32_t a = (rep % 2 == 0) ? k : ARM_COUNT - 1 - k;
            if (!arms[a].enabled)
               continue;
            uint64_t elapsed;
            value = 0x11223344u + rep;
            if (!run_one(&arms[a], size, value, &elapsed, why, sizeof(why))) {
               status = refuse(why);
               goto close_json;
            }
            arms[a].samples[arms[a].sample_count++] = elapsed;
         }
      }
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (!arms[a].enabled)
            continue;
         if (!verify_batch(&arms[a], size, value, why, sizeof(why))) {
            fprintf(stderr, "BATCH_MISMATCH: %s\n", why);
            status = 1;
            goto close_json;
         }
         report_arm(json, &arms[a], size, &shape);
      }
      fflush(stdout);
   }

close_json:
   if (json != NULL)
      fclose(json);
out:
   for (uint32_t a = 0; a < ARM_COUNT; a++)
      close_arm(&arms[a]);
   if (instance != VK_NULL_HANDLE)
      vkDestroyInstance(instance, NULL);
   return status;
}
