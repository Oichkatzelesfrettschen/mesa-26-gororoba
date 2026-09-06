/*
 * SPDX-License-Identifier: MIT
 *
 * RB2D constant-fill crossover harness: measures the delivery cost of one
 * vkCmdFillBuffer over a size sweep on the host route and on the two
 * windowed RB2D GPU routes, so an automatic-selection threshold rests on
 * measured execution rather than on byte size alone.
 *
 * Each arm holds its own device, and each GPU arm holds its own externally
 * supplied measurement declaration and its own evidence destination.  The
 * declaration governs: the harness compares the run it was asked to
 * perform against the cases, counts, route, destination role, and
 * completion bound the declaration states, and refuses before creating a
 * device when they disagree.  It never rewrites a declaration to fit its
 * arguments, so the budget a campaign spends is the one an operator
 * authorized.
 *
 * The measured interval is the public delivery: the caller's wait from
 * immediately before vkQueueSubmit through fence completion and the
 * invalidation a noncoherent mapping requires before the result is
 * readable.  Route selection, legalization, carrier choice, IB and
 * relocation construction, the live-resource identity checks, budget
 * consumption, the ioctl, hardware execution, and completion all fall
 * inside it, because they are what the route costs.  Instance, device,
 * buffer, memory, mapping and command-buffer creation, the campaign
 * claim, per-case artifact retention, destination conditioning, the byte
 * oracle, and every line of output fall outside it.  A nested transport
 * interval stops at fence completion and excludes the invalidation; it
 * carries host submission and waiting cost, so it measures the transport
 * rather than the GPU.  r3v_crossover_deliver holds that order, and
 * r3v_crossover_delivery_test holds r3v_crossover_deliver.
 *
 * --inject-delay-ns measures the timer's sensitivity to an interval the
 * bracket encloses: a known delay slept inside the timed region moves the
 * medians of a run that carries it.  It reports nothing about which work
 * the bracket encloses, since a bracket holding only the sleep would move
 * as well; the enclosure is read out of the delivery sequence.
 *
 * Every warmup and every measured repetition conditions the whole
 * destination to the sentinel, delivers, and runs the byte oracle, so a
 * repetition that wrote nothing, wrote short, or wrote past the interval
 * fails on its own rather than behind a later success.  The conditioning
 * class is whole-allocation host-initialized before each submission,
 * which is a controlled workload rather than a claim about which cache
 * lines are resident when timing starts.
 *
 * Arm order over the measured repetitions cycles the permutations of the
 * arms applicable to that size, so each arm holds each position equally
 * often.  An arm whose contract cannot legalize an interval -- V1 outside
 * its one-window envelope -- reports NOT_APPLICABLE, is omitted from its
 * declaration and its schedule, and the sweep continues.
 */

#include "amd/r300/common/r300_rb2d_legalize.h"
#include "r3v_crossover_delivery.h"
#include "r3v_measurement_session.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

/* The destination is initialized to the sentinel before every repetition
 * and carries a tail past the largest fill, so a write that runs long is
 * a byte whose value decides its origin.  The fill value is constant per
 * case and shares no byte with the sentinel, so a repetition that
 * delivered nothing leaves the sentinel standing and fails on its own --
 * which an oracle reading only a batch's final value cannot see. */
#define CROSSOVER_SENTINEL 0xa5u
#define CROSSOVER_FILL_VALUE 0x11223344u
#define CROSSOVER_TAIL_BYTES 64u
#define CROSSOVER_MAX_REPS 4096u
#define CROSSOVER_MAX_SIZES 32u
/* One arm's whole sample stream, preallocated before the first
 * repetition so no measured repetition allocates. */
#define CROSSOVER_MAX_SAMPLES 262144u
#define CROSSOVER_DIGEST_HEX 65u

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

/* What one case's counted enrollment observed, joined to every sample of
 * that case by case id.  The retained semantic manifest names the IB the
 * device actually built; the prediction is this harness's independent
 * legalizer result for the same interval. */
struct enrollment {
   bool enrolled;
   struct r300_rb2d_legalize_result predicted;
   uint32_t actual_ib_dwords;
   uint32_t actual_reloc_count;
   char actual_ib_blake3[CROSSOVER_DIGEST_HEX];
   char actual_relocs_blake3[CROSSOVER_DIGEST_HEX];
};

/* One repetition's observation.  Collected into preallocated storage
 * inside the campaign and published after it, so no measured repetition
 * formats a line. */
struct sample {
   uint32_t case_id;
   uint32_t repetition;
   bool measured;
   uint8_t arm;
   uint64_t fill_bytes;
   uint64_t allocation_bytes;
   uint64_t delivery_ns;
   uint64_t transport_ns;
   uint32_t allowance_consumed;
   int submit_result;
   int completion_result;
   bool oracle_passed;
};

struct arm {
   const char *name;
   /* R3V_NATIVE_EXECUTION_POLICY for this arm's device. */
   const char *policy;
   /* The one route gate this arm opens, or NULL for the host arm.  Two
    * open fill gates are refused at device creation, so each arm names
    * at most one. */
   const char *gate;
   /* The route name the declaration must state for this arm, or NULL
    * where the arm runs on the host and declares nothing. */
   const char *route_name;
   /* The contract whose envelope decides whether a size is this arm's to
    * measure at all.  V1 admits one window and at most three rectangles,
    * so a wider interval is NOT_APPLICABLE rather than a failed sample. */
   enum r300_rb2d_contract contract;
   /* The executor this arm's samples record.  A host row carries no GPU
    * stream shape, so its prediction is reported as not applicable
    * rather than as an observation. */
   const char *executor;
   bool enabled;
   /* Externally supplied and immutable: the declaration this arm's
    * device opens, and the directory its evidence retains into. */
   const char *declaration_path;
   const char *evidence_dir;
   struct r3v_measurement_manifest manifest;
   bool manifest_present;
   VkDevice device;
   VkQueue queue;
   VkBuffer buffer;
   VkDeviceMemory memory;
   VkDeviceSize allocated_bytes;
   uint8_t *map;
   bool host_coherent;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   struct enrollment enrollments[CROSSOVER_MAX_SIZES];
   struct sample *samples;
   uint32_t sample_count;
   uint32_t sample_capacity;
   uint32_t allowance_consumed;
};

static struct arm arms[ARM_COUNT] = {
   [ARM_HOST] = { .name = "host", .policy = "cpu_reference", .gate = NULL,
                  .route_name = NULL, .executor = "host",
                  .contract = R300_RB2D_CONTRACT_CONST_FILL_V2 },
   [ARM_V2] = { .name = "v2", .policy = "gpu_only",
                .gate = "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_V2_EXPERIMENTAL",
                .route_name = "rb2d_const_fill_v2", .executor = "gpu",
                .contract = R300_RB2D_CONTRACT_CONST_FILL_V2 },
   [ARM_V1] = { .name = "v1", .policy = "gpu_only",
                .gate = "R3V_NATIVE_ROUTE_RB2D_CONST_FILL_EXPERIMENTAL",
                .route_name = "rb2d_const_fill", .executor = "gpu",
                .contract = R300_RB2D_CONTRACT_CONST_FILL_V1 },
};

static VkInstance instance;
static VkPhysicalDevice physical_device;
static VkPhysicalDeviceProperties device_properties;
static VkPhysicalDeviceMemoryProperties memory_properties;
static uint64_t allocation_bytes;
static uint64_t wait_bound_ns = (uint64_t)10 * 1000 * 1000 * 1000;
/* A known interval slept inside the timed region, so a run that carries
 * it and a run that does not separate a timer measuring its bracket from
 * one that is not.  Zero for every measuring run. */
static uint64_t inject_delay_ns;
/* Set only where the transport under test absorbs submissions without
 * executing them, as the drm-shim does.  A GPU arm's oracle then records
 * its verdict and the campaign continues, so the mechanism around the
 * measurement -- declarations, enrollment, retention, budget,
 * publication -- is exercised where no fill reaches memory.  The host
 * arm executes on the host and stays held to its oracle.  Every sample
 * and every cell records that execution was not asserted, and a
 * measuring run leaves this closed. */
static bool absorbing_transport;

static int
refuse(const char *why)
{
   fprintf(stderr, "REFUSED: %s\n", why);
   return 2;
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
   if (n == 0 || n > CROSSOVER_MAX_REPS)
      return 0;
   for (uint32_t i = 0; i < n; i++)
      deviations[i] = sorted[i] > median ? sorted[i] - median
                                         : median - sorted[i];
   qsort(deviations, n, sizeof(deviations[0]), compare_u64);
   return quantile(deviations, n, 0.5);
}

/* The legalizer's prediction for one interval on the execution floor:
 * the carrier the chooser takes and the stream shape it produces.  It
 * reads no device, so it is a prediction the enrollment compares against
 * the device's own retained transport rather than an observation of it. */
static void
predict(uint64_t size, enum r300_rb2d_contract contract,
        struct r300_rb2d_legalize_result *result)
{
   static struct r300_rb2d_window windows[R300_RB2D_LEGALIZE_MAX_WINDOWS];
   const struct r300_rb2d_legalize_request request = {
      .byte_offset = 0,
      .byte_size = size,
      .pattern = 0,
      .bo_size = allocation_bytes,
      .usage = R300_RB2D_USAGE_FILL_BUFFER,
      .contract = contract,
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
   /* Host-visible is the requirement: the repetition oracle reads the
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

/* Reads a declaration through the same parser the driver opens it with,
 * so the harness and the device disagree about no field.  The text is
 * read whole; a declaration the parser refuses refuses the run here,
 * before any device exists to open it. */
static bool
load_declaration(struct arm *arm, char *why, size_t why_size)
{
   arm->manifest_present = false;
   FILE *file = fopen(arm->declaration_path, "re");
   if (file == NULL) {
      snprintf(why, why_size, "arm %s: the declaration %s cannot be read",
               arm->name, arm->declaration_path);
      return false;
   }
   static char text[65536];
   const size_t length = fread(text, 1, sizeof(text) - 1, file);
   const bool truncated = !feof(file);
   const bool failed = ferror(file) != 0;
   fclose(file);
   if (failed || truncated) {
      snprintf(why, why_size,
               "arm %s: the declaration %s is unreadable or longer than %zu "
               "bytes",
               arm->name, arm->declaration_path, sizeof(text) - 1);
      return false;
   }
   text[length] = '\0';
   const char *reason = NULL;
   const enum r3v_measurement_session_refusal r =
      r3v_measurement_manifest_parse(text, length, &arm->manifest, &reason);
   if (r != R3V_MEASUREMENT_SESSION_ADMITTED) {
      snprintf(why, why_size, "arm %s: the declaration %s is %s: %s",
               arm->name, arm->declaration_path,
               r3v_measurement_session_refusal_name(r),
               reason != NULL ? reason : "");
      return false;
   }
   arm->manifest_present = true;
   return true;
}

/* Holds the run against the declaration that authorizes it, over the
 * fields that need no device: the case set and its order, each case's
 * offset, size, value, warmup count and measured count, the route, the
 * destination's buffer size and binding offset, the summed budget, and
 * the completion bound.  The role's allocation size is checked against
 * the memory the device actually allocates, which no declaration can
 * predict, so that field is held after allocation and before the first
 * submission. */
static bool
declaration_agrees(const struct arm *arm, const uint64_t *sizes,
                   const bool *applicable, uint32_t size_count,
                   uint32_t warmup, uint32_t reps, char *why, size_t why_size)
{
   const struct r3v_measurement_manifest *m = &arm->manifest;
   uint32_t declared = 0;
   for (uint32_t s = 0; s < size_count; s++) {
      if (!applicable[s])
         continue;
      if (declared >= m->case_count) {
         snprintf(why, why_size,
                  "arm %s: the run measures size %" PRIu64
                  " that the declaration's %u cases do not reach",
                  arm->name, sizes[s], m->case_count);
         return false;
      }
      const struct r3v_measurement_case *c = &m->cases[declared];
      if (c->fill_offset != 0u || c->fill_bytes != sizes[s] ||
          c->fill_value != CROSSOVER_FILL_VALUE) {
         snprintf(why, why_size,
                  "arm %s case %u: the declaration states offset %" PRIu64
                  " size %" PRIu64 " value 0x%08x; the run performs offset 0 "
                  "size %" PRIu64 " value 0x%08x",
                  arm->name, declared, c->fill_offset, c->fill_bytes,
                  c->fill_value, sizes[s], CROSSOVER_FILL_VALUE);
         return false;
      }
      if (c->warmups != warmup || c->repetitions != reps) {
         snprintf(why, why_size,
                  "arm %s case %u: the declaration states %u warmups and %u "
                  "repetitions; the run performs %u and %u",
                  arm->name, declared, c->warmups, c->repetitions, warmup,
                  reps);
         return false;
      }
      declared++;
   }
   if (declared != m->case_count) {
      snprintf(why, why_size,
               "arm %s: the declaration states %u cases and the run measures "
               "%u sizes it admits",
               arm->name, m->case_count, declared);
      return false;
   }
   if (arm->route_name == NULL || strcmp(m->route, arm->route_name) != 0) {
      snprintf(why, why_size,
               "arm %s: the declaration measures route \"%s\"; this arm "
               "opens \"%s\"",
               arm->name, m->route,
               arm->route_name != NULL ? arm->route_name : "");
      return false;
   }
   if (m->role.buffer_bytes != allocation_bytes ||
       m->role.binding_offset != 0u) {
      snprintf(why, why_size,
               "arm %s: the declaration binds %" PRIu64 " bytes at offset %"
               PRIu64 "; the run binds %" PRIu64 " at offset 0",
               arm->name, m->role.buffer_bytes, m->role.binding_offset,
               allocation_bytes);
      return false;
   }
   /* The budget is the sum of every case's counted enrollment and
    * measured repetitions, so an excess submission has nothing to spend
    * rather than borrowing from a case that ran short. */
   uint64_t required = 0;
   for (uint32_t i = 0; i < m->case_count; i++)
      required += (uint64_t)m->cases[i].warmups + m->cases[i].repetitions;
   if (m->max_total_submissions != required) {
      snprintf(why, why_size,
               "arm %s: the declaration allows %u submissions and its cases "
               "account for %" PRIu64,
               arm->name, m->max_total_submissions, required);
      return false;
   }
   if (m->completion_timeout_ns != wait_bound_ns) {
      snprintf(why, why_size,
               "arm %s: the declaration bounds completion at %" PRIu64
               " ns; the run waits %" PRIu64,
               arm->name, m->completion_timeout_ns, wait_bound_ns);
      return false;
   }
   return true;
}

/* The environment each arm's device reads at creation.  Every name this
 * harness sets is cleared before the next construction, so one arm's
 * declaration, evidence destination, or route gate never reaches
 * another's device.  The device copies what it retains, so a name set
 * for a later arm does not move under an earlier arm's device. */
static void
apply_arm_environment(const struct arm *arm)
{
   for (uint32_t i = 0; i < ARM_COUNT; i++) {
      if (arms[i].gate != NULL)
         unsetenv(arms[i].gate);
   }
   unsetenv("R3V_NATIVE_MEASUREMENT_DECLARATION");
   unsetenv("R3V_NATIVE_MANIFEST_DIR");
   if (arm->gate != NULL)
      setenv(arm->gate, "1", 1);
   setenv("R3V_NATIVE_EXECUTION_POLICY", arm->policy, 1);
   if (arm->declaration_path != NULL)
      setenv("R3V_NATIVE_MEASUREMENT_DECLARATION", arm->declaration_path, 1);
   if (arm->evidence_dir != NULL)
      setenv("R3V_NATIVE_MANIFEST_DIR", arm->evidence_dir, 1);
}

static bool
open_arm(struct arm *arm, char *why, size_t why_size)
{
   apply_arm_environment(arm);

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
   /* The declared role names the allocation the device will hold, and
    * the size the implementation requires is the one it holds.  Held
    * here rather than before creation because no declaration can predict
    * an implementation's alignment, and held before any submission
    * because the device's own role check refuses at the first bind. */
   if (arm->manifest_present &&
       arm->manifest.role.allocation_bytes != requirements.size) {
      snprintf(why, why_size,
               "arm %s: the declaration states an allocation of %" PRIu64
               " bytes; the implementation requires %" PRIu64,
               arm->name, arm->manifest.role.allocation_bytes,
               (uint64_t)requirements.size);
      return false;
   }
   arm->allocated_bytes = requirements.size;
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
   free(arm->samples);
   arm->samples = NULL;
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

/* One repetition's live state, handed to the delivery sequence.  The
 * bound resources are resolved here on every repetition rather than
 * copied from a previous one, so the objects the sequence submits are
 * the objects it conditions and verifies. */
struct repetition {
   struct arm *arm;
   uint64_t fill_bytes;
   uint32_t fill_value;
   VkResult submit_result;
   VkResult completion_result;
   bool oracle_passed;
   char why[512];
};

static bool
op_read_clock(void *ctx, uint64_t *ns, char *why, size_t why_size)
{
   (void)ctx;
   struct timespec ts;
   /* A clock that refuses to answer refuses the repetition: an interval
    * computed from an unread timestamp names no duration. */
   if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
      snprintf(why, why_size, "clock_gettime(CLOCK_MONOTONIC_RAW) failed: %s",
               strerror(errno));
      return false;
   }
   *ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
   return true;
}

/* Restores the whole destination to the sentinel and publishes it under
 * the memory type's coherence contract, so the repetition that follows
 * starts from bytes whose value names their origin.  This is the
 * conditioning class the campaign measures against: whole-allocation
 * host-initialized before each submission. */
static bool
op_initialize(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   struct arm *arm = rep->arm;
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

static bool
op_record(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   struct arm *arm = rep->arm;
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
   vkCmdFillBuffer(arm->cmd, arm->buffer, 0, rep->fill_bytes,
                   rep->fill_value);
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
   return true;
}

/* Sleeps the whole requested interval inside the bracket.  A signal that
 * cuts the sleep short leaves the remainder to sleep rather than
 * reporting a partial interval as the requested one, because the
 * calibration compares medians against the interval it asked for. */
static bool
op_delay(void *ctx, char *why, size_t why_size)
{
   (void)ctx;
   if (inject_delay_ns == 0)
      return true;
   struct timespec remaining = {
      .tv_sec = (time_t)(inject_delay_ns / 1000000000u),
      .tv_nsec = (long)(inject_delay_ns % 1000000000u),
   };
   while (nanosleep(&remaining, &remaining) != 0) {
      if (errno != EINTR) {
         snprintf(why, why_size, "nanosleep failed: %s", strerror(errno));
         return false;
      }
   }
   return true;
}

static bool
op_submit(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   struct arm *arm = rep->arm;
   rep->submit_result =
      vkQueueSubmit(arm->queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &arm->cmd,
                    },
                    arm->fence);
   if (rep->submit_result != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkQueueSubmit returned %d", arm->name,
               (int)rep->submit_result);
      return false;
   }
   return true;
}

static bool
op_await_completion(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   struct arm *arm = rep->arm;
   rep->completion_result =
      vkWaitForFences(arm->device, 1, &arm->fence, VK_TRUE, wait_bound_ns);
   if (rep->completion_result != VK_SUCCESS) {
      snprintf(why, why_size, "arm %s: vkWaitForFences returned %d",
               arm->name, (int)rep->completion_result);
      return false;
   }
   return true;
}

/* The invalidation a noncoherent mapping requires before a host read of
 * the result.  Fence completion alone does not make a device write
 * visible to a host mapping under Vulkan's noncoherent rules, so this
 * operation is part of delivering the fill rather than part of reading
 * it, and it stays inside the delivery interval. */
static bool
op_make_visible(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   struct arm *arm = rep->arm;
   if (arm->host_coherent)
      return true;
   const VkResult r = vkInvalidateMappedMemoryRanges(
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
   return true;
}

/* The repetition oracle: every dword of the interval carries the fill
 * value and every byte past it carries the sentinel this repetition's
 * own conditioning wrote.  A route that wrote nothing, wrote short, or
 * wrote past the interval fails here, on its own repetition, so every
 * row rests on bytes that repetition delivered.  The oracle belongs to
 * the application: the driver reports submission and completion, and
 * this comparison happens after both. */
static bool
op_verify(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   struct arm *arm = rep->arm;
   for (uint64_t i = 0; i < rep->fill_bytes; i += 4) {
      uint32_t observed;
      memcpy(&observed, arm->map + i, sizeof(observed));
      if (observed != rep->fill_value) {
         snprintf(why, why_size,
                  "arm %s size %" PRIu64 ": dword at %" PRIu64
                  " is 0x%08x, the repetition wrote 0x%08x",
                  arm->name, rep->fill_bytes, i, observed, rep->fill_value);
         return false;
      }
   }
   for (uint64_t i = rep->fill_bytes; i < allocation_bytes; i++) {
      if (arm->map[i] != CROSSOVER_SENTINEL) {
         snprintf(why, why_size,
                  "arm %s size %" PRIu64 ": byte at %" PRIu64
                  " is 0x%02x past the interval",
                  arm->name, rep->fill_bytes, i, arm->map[i]);
         return false;
      }
   }
   rep->oracle_passed = true;
   return true;
}

/* The oracle the campaign runs.  It always compares and always records
 * its verdict; under an absorbing transport a GPU arm's failure does not
 * end the run, because no fill reached memory to compare against.  The
 * host arm executes on the host, so its verdict governs whatever the
 * transport does. */
static bool
op_verify_or_record(void *ctx, char *why, size_t why_size)
{
   struct repetition *rep = ctx;
   if (op_verify(ctx, why, why_size))
      return true;
   return absorbing_transport && rep->arm->route_name != NULL;
}

static const struct r3v_crossover_delivery_ops delivery_ops = {
   .read_clock = op_read_clock,
   .initialize = op_initialize,
   .record = op_record,
   .delay = op_delay,
   .submit = op_submit,
   .await_completion = op_await_completion,
   .make_visible = op_make_visible,
   .verify = op_verify_or_record,
};

/* Reads one field out of the retained semantic manifest.  The manifest
 * is the device's own record of the transport it built, so the values
 * here are observations rather than predictions. */
static bool
manifest_field(const char *text, const char *key, char *out, size_t out_size)
{
   char needle[64];
   if ((size_t)snprintf(needle, sizeof(needle), "\"%s\":", key) >=
       sizeof(needle))
      return false;
   const char *at = strstr(text, needle);
   if (at == NULL)
      return false;
   at += strlen(needle);
   while (*at == ' ' || *at == '"')
      at++;
   size_t length = 0;
   while (at[length] != '\0' && at[length] != '"' && at[length] != ',' &&
          at[length] != '\n' && at[length] != ' ')
      length++;
   if (length == 0 || length >= out_size)
      return false;
   memcpy(out, at, length);
   out[length] = '\0';
   return true;
}

/* Compares this harness's independent legalizer prediction against the
 * transport the device retained for the case's first repetition, and
 * refuses to measure a case whose stream the harness cannot account for.
 *
 * The retained semantic manifest states the IB dword count, the
 * relocation count, and the digests of both.  ib_dwords is the compared
 * field: it is a function of the carrier, the window count, and the
 * rectangle count the legalizer predicts, so a prediction that disagrees
 * with it disagrees about the stream.  The two digests are recorded as
 * the case's identity and carried by every sample of that case; the
 * window and rectangle counts stay predictions, since the manifest does
 * not publish them. */
static bool
enroll_case(struct arm *arm, uint32_t case_index,
            const struct r300_rb2d_legalize_result *predicted, char *why,
            size_t why_size)
{
   struct enrollment *e = &arm->enrollments[case_index];
   e->predicted = *predicted;
   if (arm->evidence_dir == NULL) {
      /* The host arm retains no transport, so it enrolls its prediction
       * and reports its executed GPU shape as not applicable. */
      e->enrolled = true;
      return true;
   }
   char path[1024];
   if ((size_t)snprintf(path, sizeof(path), "%s/case-%u/manifest.json",
                        arm->evidence_dir, case_index) >= sizeof(path)) {
      snprintf(why, why_size, "arm %s: the case %u manifest path is too long",
               arm->name, case_index);
      return false;
   }
   FILE *file = fopen(path, "re");
   if (file == NULL) {
      snprintf(why, why_size,
               "arm %s case %u: the device retained no transport at %s",
               arm->name, case_index, path);
      return false;
   }
   char text[4096];
   const size_t length = fread(text, 1, sizeof(text) - 1, file);
   const bool failed = ferror(file) != 0;
   fclose(file);
   if (failed) {
      snprintf(why, why_size, "arm %s case %u: %s is unreadable", arm->name,
               case_index, path);
      return false;
   }
   text[length] = '\0';
   char dwords[32];
   char relocs[32];
   if (!manifest_field(text, "ib_dwords", dwords, sizeof(dwords)) ||
       !manifest_field(text, "reloc_count", relocs, sizeof(relocs)) ||
       !manifest_field(text, "ib_blake3", e->actual_ib_blake3,
                       sizeof(e->actual_ib_blake3)) ||
       !manifest_field(text, "relocs_blake3", e->actual_relocs_blake3,
                       sizeof(e->actual_relocs_blake3))) {
      snprintf(why, why_size,
               "arm %s case %u: %s does not carry the retained transport's "
               "shape and identity",
               arm->name, case_index, path);
      return false;
   }
   e->actual_ib_dwords = (uint32_t)strtoul(dwords, NULL, 10);
   e->actual_reloc_count = (uint32_t)strtoul(relocs, NULL, 10);
   if (e->actual_ib_dwords != predicted->ib_dwords) {
      snprintf(why, why_size,
               "arm %s case %u: the legalizer predicts a %u-dword stream "
               "over %u window(s) and %u rectangle(s); the device retained "
               "%u dwords",
               arm->name, case_index, predicted->ib_dwords,
               predicted->window_count, predicted->rect_count,
               e->actual_ib_dwords);
      return false;
   }
   e->enrolled = true;
   return true;
}

/* The k-th permutation of n arm slots in lexicographic order.  With two
 * applicable arms the cycle is 2 and with three it is 6, so a
 * repetition count that completes the cycle gives every arm every
 * position equally often. */
static void
permutation(uint32_t k, const uint8_t *pool, uint32_t n, uint8_t *out)
{
   uint8_t remaining[ARM_COUNT];
   memcpy(remaining, pool, n);
   uint32_t count = n;
   for (uint32_t i = 0; i < n; i++) {
      uint32_t factorial = 1;
      for (uint32_t f = 2; f < count; f++)
         factorial *= f;
      const uint32_t pick = count > 1 ? k / factorial : 0;
      k = count > 1 ? k % factorial : 0;
      out[i] = remaining[pick];
      for (uint32_t m = pick; m + 1 < count; m++)
         remaining[m] = remaining[m + 1];
      count--;
   }
}

static uint32_t
balancing_cycle(uint32_t n)
{
   uint32_t factorial = 1;
   for (uint32_t f = 2; f <= n; f++)
      factorial *= f;
   return factorial;
}

/* FNV-1a over the published sample stream: the completeness record's
 * digest, so a truncated or reordered result file is detectable against
 * the run that produced it. */
static uint64_t
digest_update(uint64_t hash, const char *text)
{
   for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++)
      hash = (hash ^ *p) * 0x100000001b3ull;
   return hash;
}

/* The output file, with every write's result held.  A campaign that
 * spent its whole budget and then truncated its result file produced no
 * usable artifact, so a failed write or close refuses the run. */
struct output {
   FILE *file;
   bool failed;
   uint64_t digest;
   uint32_t rows;
};

static void
emit(struct output *out, const char *line)
{
   out->digest = digest_update(out->digest, line);
   out->rows++;
   if (out->file == NULL || out->failed)
      return;
   if (fputs(line, out->file) == EOF)
      out->failed = true;
}

static bool
output_close(struct output *out, char *why, size_t why_size)
{
   if (out->file == NULL)
      return !out->failed;
   /* A stream closed without its buffers reaching the file is a
    * truncated artifact, and fclose is where a deferred write error
    * surfaces. */
   const bool flushed = fflush(out->file) == 0;
   const bool closed = fclose(out->file) == 0;
   out->file = NULL;
   if (out->failed || !flushed || !closed) {
      snprintf(why, why_size,
               "the result file could not be written completely");
      return false;
   }
   return true;
}

static void
publish_sample(struct output *out, const struct arm *arm,
               const struct sample *s)
{
   const struct enrollment *e = &arm->enrollments[s->case_id];
   char line[1024];
   snprintf(
      line, sizeof(line),
      "{\"row\":\"sample\",\"arm\":\"%s\",\"executor\":\"%s\","
      "\"case_id\":%u,\"repetition\":%u,\"phase\":\"%s\","
      "\"fill_bytes\":%" PRIu64 ",\"allocation_bytes\":%" PRIu64
      ",\"delivery_ns\":%" PRIu64 ",\"transport_ns\":%" PRIu64
      ",\"allowance_consumed\":%u,\"submit_result\":%d,"
      "\"completion_result\":%d,\"oracle\":\"%s\","
      "\"destination_buffer\":\"0x%016" PRIx64 "\","
      "\"destination_memory\":\"0x%016" PRIx64 "\","
      "\"ib_blake3\":\"%s\",\"relocs_blake3\":\"%s\","
      "\"execution_asserted\":%s}\n",
      arm->name, arm->executor, s->case_id, s->repetition,
      s->measured ? "measured" : "warmup", s->fill_bytes,
      s->allocation_bytes, s->delivery_ns, s->transport_ns,
      s->allowance_consumed, s->submit_result, s->completion_result,
      s->oracle_passed ? "pass" : "fail", (uint64_t)arm->buffer,
      (uint64_t)arm->memory,
      e->actual_ib_blake3[0] != '\0' ? e->actual_ib_blake3 : "n/a",
      e->actual_relocs_blake3[0] != '\0' ? e->actual_relocs_blake3 : "n/a",
      absorbing_transport && arm->route_name != NULL ? "false" : "true");
   emit(out, line);
}

/* An arm whose contract cannot represent the interval.  The refusal is
 * the contract answering, so the cell reports NOT_APPLICABLE and the
 * campaign continues; counting it as a failed sample would end a
 * host-against-v2 sweep on a diagnostic arm. */
static void
publish_not_applicable(struct output *out, const struct arm *arm,
                       uint64_t size,
                       const struct r300_rb2d_legalize_result *shape)
{
   char line[512];
   snprintf(line, sizeof(line),
            "{\"row\":\"cell\",\"arm\":\"%s\",\"fill_bytes\":%" PRIu64
            ",\"disposition\":\"NOT_APPLICABLE\",\"refusal\":\"%s\"}\n",
            arm->name, size,
            r300_rb2d_legalize_refusal_name(shape->refusal));
   emit(out, line);
   printf("size=%-9" PRIu64 " arm=%-5s NOT_APPLICABLE refusal=%s\n", size,
          arm->name, r300_rb2d_legalize_refusal_name(shape->refusal));
}

/* One cell's statistics over its measured samples.  The GPU stream shape
 * is the enrolled prediction, which the enrollment already held against
 * the device's retained transport; the host arm reports it as not
 * applicable rather than as a shape it executed. */
static void
publish_cell(struct output *out, const struct arm *arm, uint32_t case_id,
             uint64_t size)
{
   static uint64_t sorted[CROSSOVER_MAX_REPS];
   uint32_t n = 0;
   for (uint32_t i = 0; i < arm->sample_count && n < CROSSOVER_MAX_REPS; i++) {
      if (arm->samples[i].case_id == case_id && arm->samples[i].measured)
         sorted[n++] = arm->samples[i].delivery_ns;
   }
   if (n == 0)
      return;
   qsort(sorted, n, sizeof(sorted[0]), compare_u64);
   const uint64_t median = quantile(sorted, n, 0.5);
   const uint64_t mad = median_absolute_deviation(sorted, n, median);
   const uint64_t p10 = quantile(sorted, n, 0.10);
   const uint64_t p90 = quantile(sorted, n, 0.90);
   const struct enrollment *e = &arm->enrollments[case_id];
   const bool gpu = arm->route_name != NULL;

   printf("size=%-9" PRIu64 " arm=%-5s n=%-4u median_ns=%-10" PRIu64
          " mad_ns=%-8" PRIu64 " p10_ns=%-10" PRIu64 " p90_ns=%-10" PRIu64,
          size, arm->name, n, median, mad, p10, p90);
   if (gpu)
      printf(" pitch=%u windows=%u rects=%u sites=%u ib_dwords=%u\n",
             e->predicted.pitch_bytes, e->predicted.window_count,
             e->predicted.rect_count, e->predicted.relocation_sites,
             e->actual_ib_dwords);
   else
      printf(" executed_gpu_shape=n/a\n");

   char line[1024];
   int length = snprintf(
      line, sizeof(line),
      "{\"row\":\"cell\",\"arm\":\"%s\",\"executor\":\"%s\",\"case_id\":%u,"
      "\"fill_bytes\":%" PRIu64 ",\"allocation_bytes\":%" PRIu64
      ",\"samples\":%u,\"median_delivery_ns\":%" PRIu64 ",\"mad_ns\":%" PRIu64
      ",\"p10_ns\":%" PRIu64 ",\"p90_ns\":%" PRIu64
      ",\"conditioning\":\"whole-allocation-host-initialized-per-submit\""
      ",\"execution_asserted\":%s",
      arm->name, arm->executor, case_id, size, allocation_bytes, n, median,
      mad, p10, p90,
      absorbing_transport && gpu ? "false" : "true");
   if (gpu && length > 0 && (size_t)length < sizeof(line))
      length += snprintf(
         line + length, sizeof(line) - (size_t)length,
         ",\"predicted_pitch_bytes\":%u,\"predicted_window_count\":%u,"
         "\"predicted_rect_count\":%u,\"predicted_relocation_sites\":%u,"
         "\"predicted_ib_dwords\":%u,\"retained_ib_dwords\":%u,"
         "\"retained_reloc_count\":%u",
         e->predicted.pitch_bytes, e->predicted.window_count,
         e->predicted.rect_count, e->predicted.relocation_sites,
         e->predicted.ib_dwords, e->actual_ib_dwords, e->actual_reloc_count);
   else if (length > 0 && (size_t)length < sizeof(line))
      length += snprintf(line + length, sizeof(line) - (size_t)length,
                         ",\"executed_gpu_shape\":\"not_applicable\"");
   if (length > 0 && (size_t)length < sizeof(line))
      snprintf(line + length, sizeof(line) - (size_t)length, "}\n");
   emit(out, line);
}

/* A bounded unsigned conversion.  strtoull maps a leading minus onto a
 * huge unsigned and reports ERANGE only for magnitudes past its own
 * type, so a sign is refused outright and the range is held against the
 * caller's declared limit. */
static bool
parse_u64(const char *text, uint64_t limit, uint64_t *out)
{
   if (text == NULL || text[0] == '\0' || text[0] == '-' || text[0] == '+')
      return false;
   errno = 0;
   char *end = NULL;
   const unsigned long long value = strtoull(text, &end, 0);
   if (errno == ERANGE || end == NULL || *end != '\0')
      return false;
   if ((uint64_t)value > limit)
      return false;
   *out = (uint64_t)value;
   return true;
}

int
main(int argc, char **argv)
{
   uint64_t sizes[CROSSOVER_MAX_SIZES];
   uint32_t size_count = 0;
   uint32_t reps = 30;
   uint32_t warmup = 4;
   const char *json_path = NULL;
   bool run_v1 = true;

   for (int i = 1; i < argc; i++) {
      uint64_t value = 0;
      if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
         if (!parse_u64(argv[++i], CROSSOVER_MAX_REPS, &value) || value == 0)
            return refuse("--reps is outside (0, 4096]");
         reps = (uint32_t)value;
      } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
         /* At least one counted, verified enrollment before any measured
          * sample: the case's transport retains during it, and the
          * enrollment compares that transport against the prediction. */
         if (!parse_u64(argv[++i], CROSSOVER_MAX_REPS, &value) || value == 0)
            return refuse("--warmup is outside (0, 4096]; a measured "
                          "campaign enrolls each case before measuring it");
         warmup = (uint32_t)value;
      } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
         json_path = argv[++i];
      } else if (strcmp(argv[i], "--no-v1") == 0) {
         run_v1 = false;
      } else if (strcmp(argv[i], "--absorbing-transport") == 0) {
         absorbing_transport = true;
      } else if (strcmp(argv[i], "--declaration-v2") == 0 && i + 1 < argc) {
         arms[ARM_V2].declaration_path = argv[++i];
      } else if (strcmp(argv[i], "--declaration-v1") == 0 && i + 1 < argc) {
         arms[ARM_V1].declaration_path = argv[++i];
      } else if (strcmp(argv[i], "--evidence-v2") == 0 && i + 1 < argc) {
         arms[ARM_V2].evidence_dir = argv[++i];
      } else if (strcmp(argv[i], "--evidence-v1") == 0 && i + 1 < argc) {
         arms[ARM_V1].evidence_dir = argv[++i];
      } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
         if (size_count == CROSSOVER_MAX_SIZES)
            return refuse("more than 32 sizes declared");
         if (!parse_u64(argv[++i], UINT64_MAX, &value) || value == 0 ||
             value % 4 != 0)
            return refuse("--size is not a positive dword multiple");
         for (uint32_t s = 0; s < size_count; s++) {
            if (sizes[s] == value)
               return refuse("--size names one interval twice");
         }
         sizes[size_count++] = value;
      } else if (strcmp(argv[i], "--inject-delay-ns") == 0 && i + 1 < argc) {
         if (!parse_u64(argv[++i], 1000000000u, &value))
            return refuse("--inject-delay-ns is above one second");
         inject_delay_ns = value;
      } else if (strcmp(argv[i], "--wait-bound-ns") == 0 && i + 1 < argc) {
         if (!parse_u64(argv[++i], (uint64_t)120 * 1000 * 1000 * 1000,
                        &value) ||
             value == 0)
            return refuse("--wait-bound-ns is outside (0, 120 s]");
         wait_bound_ns = value;
      } else {
         fprintf(stderr,
                 "usage: %s [--reps N] [--warmup N] [--size BYTES ...] "
                 "[--declaration-v2 PATH] [--declaration-v1 PATH] "
                 "[--evidence-v2 DIR] [--evidence-v1 DIR] "
                 "[--inject-delay-ns NS] [--json PATH] [--no-v1] "
                 "[--absorbing-transport] [--wait-bound-ns NS]\n",
                 argv[0]);
         return 2;
      }
   }
   if (size_count == 0) {
      size_count = (uint32_t)(sizeof(default_sizes) / sizeof(default_sizes[0]));
      memcpy(sizes, default_sizes, sizeof(default_sizes));
   }

   /* The allocation carries the largest interval plus the tail canary.
    * Both the addition and the host mapping it must fit are held, so a
    * size near the type's ceiling refuses rather than wrapping into a
    * small allocation the oracle would then read past. */
   uint64_t largest = 0;
   for (uint32_t i = 0; i < size_count; i++) {
      if (sizes[i] > largest)
         largest = sizes[i];
   }
   if (largest > UINT64_MAX - CROSSOVER_TAIL_BYTES)
      return refuse("the largest interval plus the tail canary overflows");
   allocation_bytes = largest + CROSSOVER_TAIL_BYTES;
   if (allocation_bytes > (uint64_t)SIZE_MAX)
      return refuse("the allocation is larger than the host can map");

   arms[ARM_HOST].enabled = true;
   arms[ARM_V2].enabled = true;
   arms[ARM_V1].enabled = run_v1;

   char why[512];
   /* Each GPU arm runs under its own declaration and its own evidence
    * destination.  A GPU arm without one would inherit whatever the
    * environment carried, which is the state a single declaration across
    * three arms produces. */
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (!arms[a].enabled || arms[a].route_name == NULL)
         continue;
      if (arms[a].declaration_path == NULL || arms[a].evidence_dir == NULL) {
         snprintf(why, sizeof(why),
                  "arm %s measures a route and needs --declaration-%s and "
                  "--evidence-%s",
                  arms[a].name, arms[a].name, arms[a].name);
         return refuse(why);
      }
      if (!load_declaration(&arms[a], why, sizeof(why)))
         return refuse(why);
   }

   /* One shape per arm per size, because the two GPU contracts legalize
    * the same interval differently and V1's refusal is what decides
    * applicability.  The host arm measures the store loop whatever a GPU
    * contract says of the interval. */
   static struct r300_rb2d_legalize_result shapes[ARM_COUNT]
                                                 [CROSSOVER_MAX_SIZES];
   static bool applicable[ARM_COUNT][CROSSOVER_MAX_SIZES];
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      for (uint32_t s = 0; s < size_count; s++) {
         predict(sizes[s], arms[a].contract, &shapes[a][s]);
         applicable[a][s] =
            a == ARM_HOST ||
            shapes[a][s].refusal == R300_RB2D_LEGALIZE_OK;
      }
   }

   /* The measured schedule cycles the permutations of the arms
    * applicable to a size, so a repetition count that completes the
    * cycle gives every arm every position equally often.  A size where
    * V1 refuses regenerates a two-arm schedule rather than skipping an
    * entry in a three-arm one. */
   for (uint32_t s = 0; s < size_count; s++) {
      uint32_t applicable_arms = 0;
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (arms[a].enabled && applicable[a][s])
            applicable_arms++;
      }
      const uint32_t cycle = balancing_cycle(applicable_arms);
      if (reps % cycle != 0) {
         snprintf(why, sizeof(why),
                  "size %" PRIu64 " admits %u arms, whose balancing cycle is "
                  "%u; --reps %u does not complete it",
                  sizes[s], applicable_arms, cycle, reps);
         return refuse(why);
      }
   }

   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (!arms[a].enabled || !arms[a].manifest_present)
         continue;
      if (!declaration_agrees(&arms[a], sizes, applicable[a], size_count,
                              warmup, reps, why, sizeof(why)))
         return refuse(why);
   }

   /* One arm's whole sample stream, allocated before the campaign so no
    * measured repetition allocates. */
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (!arms[a].enabled)
         continue;
      uint32_t rows = 0;
      for (uint32_t s = 0; s < size_count; s++) {
         if (applicable[a][s])
            rows += warmup + reps;
      }
      if (rows > CROSSOVER_MAX_SAMPLES)
         return refuse("the campaign asks for more samples than the "
                       "preallocated stream holds");
      arms[a].sample_capacity = rows;
      arms[a].samples = calloc(rows != 0 ? rows : 1, sizeof(struct sample));
      if (arms[a].samples == NULL)
         return refuse("the sample stream cannot be allocated");
   }

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

   int status = 0;
   struct output out = { .digest = 0xcbf29ce484222325ull };
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (!arms[a].enabled)
         continue;
      if (!open_arm(&arms[a], why, sizeof(why))) {
         status = refuse(why);
         goto out_close;
      }
   }
   if (absorbing_transport)
      printf("execution_asserted=false the transport absorbs submissions "
             "without executing them; no timing row measures a fill\n");
   printf("arms=");
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (arms[a].enabled)
         printf("%s ", arms[a].name);
   }
   printf("\n");
   fflush(stdout);

   if (json_path != NULL) {
      out.file = fopen(json_path, "we");
      if (out.file == NULL) {
         status = refuse("the result file cannot be opened for writing");
         goto out_close;
      }
   }

   /* What the run asked for, counted before it starts: a campaign that
    * stopped early then publishes fewer rows than it declared, and the
    * completeness record says so. */
   uint32_t declared_rows = 0;
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (arms[a].enabled)
         declared_rows += arms[a].sample_capacity;
   }

   for (uint32_t s = 0; s < size_count; s++) {
      const uint64_t size = sizes[s];
      /* A case index counts the cases an arm declares, so it matches the
       * index the device gave the case and the subdirectory it retained
       * into.  An arm that refuses a size does not advance its own
       * count. */
      uint32_t case_index[ARM_COUNT];
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         case_index[a] = 0;
         for (uint32_t p = 0; p < s; p++) {
            if (applicable[a][p])
               case_index[a]++;
         }
      }

      /* Counted enrollment: every warmup conditions, delivers, and runs
       * the same byte oracle a measured repetition runs.  A warmup that
       * delivered wrong bytes ends the campaign here rather than letting
       * measured samples rest on a route that was already wrong. */
      for (uint32_t w = 0; w < warmup; w++) {
         for (uint32_t a = 0; a < ARM_COUNT; a++) {
            if (!arms[a].enabled || !applicable[a][s])
               continue;
            struct repetition rep = { .arm = &arms[a],
                                      .fill_bytes = size,
                                      .fill_value = CROSSOVER_FILL_VALUE };
            struct r3v_crossover_delivery_result result;
            enum r3v_crossover_delivery_stage failed;
            const bool ok =
               r3v_crossover_deliver(&delivery_ops, &rep, &result, &failed,
                                     rep.why, sizeof(rep.why));
            arms[a].allowance_consumed++;
            struct sample *sample =
               &arms[a].samples[arms[a].sample_count++];
            *sample = (struct sample){
               .case_id = case_index[a],
               .repetition = w,
               .measured = false,
               .arm = (uint8_t)a,
               .fill_bytes = size,
               .allocation_bytes = arms[a].allocated_bytes,
               .delivery_ns = ok ? result.delivery_ns : 0,
               .transport_ns = ok ? result.transport_ns : 0,
               .allowance_consumed = arms[a].allowance_consumed,
               .submit_result = (int)rep.submit_result,
               .completion_result = (int)rep.completion_result,
               .oracle_passed = rep.oracle_passed,
            };
            if (!ok) {
               snprintf(why, sizeof(why), "warmup %u at %s: %s", w,
                        r3v_crossover_delivery_stage_name(failed), rep.why);
               status = failed == R3V_CROSSOVER_STAGE_VERIFY ? 1 : 2;
               fprintf(stderr, "%s: %s\n",
                       failed == R3V_CROSSOVER_STAGE_VERIFY
                          ? "REPETITION_MISMATCH"
                          : "REFUSED",
                       why);
               goto publish;
            }
         }
      }

      /* Every case enrolls against the transport the device retained
       * during its warmup before any measured sample of it is taken. */
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (!arms[a].enabled || !applicable[a][s])
            continue;
         if (!enroll_case(&arms[a], case_index[a], &shapes[a][s], why,
                          sizeof(why))) {
            status = refuse(why);
            goto publish;
         }
      }

      uint8_t pool[ARM_COUNT];
      uint32_t pool_count = 0;
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (arms[a].enabled && applicable[a][s])
            pool[pool_count++] = (uint8_t)a;
      }
      const uint32_t cycle = balancing_cycle(pool_count);

      for (uint32_t rep_index = 0; rep_index < reps; rep_index++) {
         uint8_t order[ARM_COUNT];
         permutation(rep_index % cycle, pool, pool_count, order);
         for (uint32_t k = 0; k < pool_count; k++) {
            const uint32_t a = order[k];
            struct repetition rep = { .arm = &arms[a],
                                      .fill_bytes = size,
                                      .fill_value = CROSSOVER_FILL_VALUE };
            struct r3v_crossover_delivery_result result;
            enum r3v_crossover_delivery_stage failed;
            const bool ok =
               r3v_crossover_deliver(&delivery_ops, &rep, &result, &failed,
                                     rep.why, sizeof(rep.why));
            arms[a].allowance_consumed++;
            struct sample *sample =
               &arms[a].samples[arms[a].sample_count++];
            *sample = (struct sample){
               .case_id = case_index[a],
               .repetition = rep_index,
               .measured = true,
               .arm = (uint8_t)a,
               .fill_bytes = size,
               .allocation_bytes = arms[a].allocated_bytes,
               .delivery_ns = ok ? result.delivery_ns : 0,
               .transport_ns = ok ? result.transport_ns : 0,
               .allowance_consumed = arms[a].allowance_consumed,
               .submit_result = (int)rep.submit_result,
               .completion_result = (int)rep.completion_result,
               .oracle_passed = rep.oracle_passed,
            };
            if (!ok) {
               snprintf(why, sizeof(why), "repetition %u at %s: %s",
                        rep_index,
                        r3v_crossover_delivery_stage_name(failed), rep.why);
               status = failed == R3V_CROSSOVER_STAGE_VERIFY ? 1 : 2;
               fprintf(stderr, "%s: %s\n",
                       failed == R3V_CROSSOVER_STAGE_VERIFY
                          ? "REPETITION_MISMATCH"
                          : "REFUSED",
                       why);
               goto publish;
            }
         }
      }
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (!arms[a].enabled)
            continue;
         if (!applicable[a][s])
            publish_not_applicable(&out, &arms[a], size, &shapes[a][s]);
      }
      fflush(stdout);
   }

publish:
   /* Published outside every timed interval, after the campaign has
    * stopped submitting. */
   for (uint32_t a = 0; a < ARM_COUNT; a++) {
      if (!arms[a].enabled)
         continue;
      for (uint32_t i = 0; i < arms[a].sample_count; i++)
         publish_sample(&out, &arms[a], &arms[a].samples[i]);
   }
   for (uint32_t s = 0; s < size_count; s++) {
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (!arms[a].enabled || !applicable[a][s])
            continue;
         uint32_t case_id = 0;
         for (uint32_t p = 0; p < s; p++) {
            if (applicable[a][p])
               case_id++;
         }
         publish_cell(&out, &arms[a], case_id, sizes[s]);
      }
   }
   {
      /* The completeness record: what the campaign was asked to produce,
       * what it produced, and a digest over the published stream, so a
       * truncated result file is detectable against the run. */
      uint32_t observed = 0;
      for (uint32_t a = 0; a < ARM_COUNT; a++) {
         if (arms[a].enabled)
            observed += arms[a].sample_count;
      }
      char line[512];
      snprintf(line, sizeof(line),
               "{\"row\":\"completeness\",\"declared_samples\":%u,"
               "\"observed_samples\":%u,\"published_rows\":%u,"
               "\"status\":%d,\"execution_asserted\":%s,"
               "\"stream_fnv1a64\":\"0x%016" PRIx64 "\"}\n",
               declared_rows, observed, out.rows, status,
               absorbing_transport ? "false" : "true", out.digest);
      /* The digest covers the rows published before it, so this row
       * carries the value it names rather than one including itself. */
      const uint64_t digest = out.digest;
      out.digest = digest;
      if (out.file != NULL && !out.failed &&
          fputs(line, out.file) == EOF)
         out.failed = true;
      printf("completeness declared=%u observed=%u rows=%u "
             "stream_fnv1a64=0x%016" PRIx64 "\n",
             declared_rows, observed, out.rows, digest);
   }
   if (!output_close(&out, why, sizeof(why)) && status == 0)
      status = refuse(why);
out_close:
   if (out.file != NULL)
      output_close(&out, why, sizeof(why));
   for (uint32_t a = 0; a < ARM_COUNT; a++)
      close_arm(&arms[a]);
   if (instance != VK_NULL_HANDLE)
      vkDestroyInstance(instance, NULL);
   return status;
}
