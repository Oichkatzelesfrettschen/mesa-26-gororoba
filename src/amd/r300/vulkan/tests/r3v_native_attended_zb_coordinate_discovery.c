/*
 * SPDX-License-Identifier: MIT
 *
 * Coordinate-selectable depth address discovery: submits a bounded tiled
 * single-pixel cell to RS485M through the native ICD, retains the depth
 * allocation before and after the draw, and reports which physical byte
 * moved.  Coordinate, pitch, and surface base are bound into the declared
 * scenario and checked again by the queue against the emitted stream.
 *
 * The run retains both images rather than reconstructing the initial one
 * from the scenario.  A reconstructed before image would report the fill
 * that was declared instead of the fill that reached the device, so the
 * observation would be against an intention; the run therefore reads the
 * allocation back before the submission, checks it against the
 * declaration, and keeps the bytes.
 *
 * The scenario digest covers coordinate and geometry even when a uniform
 * initial image is byte-identical at two coordinates.  The initial-image
 * and IB digests independently bind the host fill and submitted stream.
 */

#include "r3v_native.h"
#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zb_depth_discovery_cell.h"
#include "util/mesa-blake3.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

/* The run's outcome classes.  Every class except CONTROL_PASS exits
 * nonzero, and the run never retries: a failed control is
 * INCONCLUSIVE / CONTROL FAILED, so no address hypothesis follows from
 * it.
 */
enum outcome {
   OUTCOME_CONTROL_PASS,
   OUTCOME_CONTROL_FAILED_INCONCLUSIVE,
   OUTCOME_CONTAINMENT_FAILURE,
   OUTCOME_SUBMISSION_REFUSED,
   OUTCOME_COMPLETION_FAILURE,
   OUTCOME_RETENTION_FAILURE,
};

static const char *const outcome_names[] = {
   [OUTCOME_CONTROL_PASS] = "CONTROL_PASS",
   [OUTCOME_CONTROL_FAILED_INCONCLUSIVE] = "CONTROL_FAILED_INCONCLUSIVE",
   [OUTCOME_CONTAINMENT_FAILURE] = "CONTAINMENT_FAILURE",
   [OUTCOME_SUBMISSION_REFUSED] = "SUBMISSION_REFUSED",
   [OUTCOME_COMPLETION_FAILURE] = "COMPLETION_FAILURE",
   [OUTCOME_RETENTION_FAILURE] = "RETENTION_FAILURE",
};

static int
finish(enum outcome outcome)
{
   printf("verdict: %s\n", outcome_names[outcome]);
   fflush(stdout);
   return outcome == OUTCOME_CONTROL_PASS ? 0 : 1;
}

static void
stage(const char *name)
{
   printf("[stage] %s\n", name);
   fflush(stdout);
}

static bool
same_directory(const char *a, const char *b)
{
   if (strcmp(a, b) == 0)
      return true;
   char resolved_a[PATH_MAX];
   char resolved_b[PATH_MAX];
   return realpath(a, resolved_a) != NULL && realpath(b, resolved_b) != NULL &&
          strcmp(resolved_a, resolved_b) == 0;
}

static void
blake3_hex(const void *data, size_t size, char out[BLAKE3_OUT_LEN * 2 + 1])
{
   struct mesa_blake3 ctx;
   blake3_hash digest;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, data, size);
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(out, digest);
}

struct layout_option {
   const char *name;
   enum r300_zb_coordinate_discovery_layout layout;
};

static const struct layout_option layout_options[] = {
   { "microtiled", R300_ZB_COORDINATE_DISCOVERY_MICROTILED },
   { "macrotiled", R300_ZB_COORDINATE_DISCOVERY_MACROTILED },
};

struct arm_option {
   const char *name;
   enum r3v_native_zb_discovery_arm arm;
};

static const struct arm_option arm_options[] = {
   { "measure", R3V_NATIVE_ZB_DISCOVERY_ARM_MEASURE },
   { "writes_disabled", R3V_NATIVE_ZB_DISCOVERY_ARM_WRITES_DISABLED },
   { "never", R3V_NATIVE_ZB_DISCOVERY_ARM_NEVER },
};

static bool
parse_u32(const char *text, uint32_t *out)
{
   char *end = NULL;
   if (text == NULL || text[0] == '\0' ||
       (text[0] == '0' && text[1] != '\0'))
      return false;
   for (const char *cursor = text; *cursor != '\0'; cursor++)
      if (*cursor < '0' || *cursor > '9')
         return false;
   unsigned long value = strtoul(text, &end, 10);
   if (end == text || *end != '\0' || value > UINT32_MAX)
      return false;
   *out = (uint32_t)value;
   return true;
}

/* Holds the image the recorder left to the initialization the scenario
 * declares: every storage slot at the packed initial word, every byte
 * outside the envelope at the guard fill.  The observation compares two
 * images and reports what moved between them, which says nothing about
 * whether the first one was the declared state, so the fill gets its own
 * verdict.
 */
static bool
initialization_is_declared(
   const struct r300_zb_depth_discovery_scenario *scenario,
   const struct r300_zb_depth_layout *layout, const uint8_t *bytes)
{
   uint8_t *declared = malloc((size_t)scenario->allocation_bytes);
   if (declared == NULL)
      return false;
   /* The same constructor the recorder filled the device allocation
    * with and the arming runner hashed, so this compares the image that
    * reached the device against the image the operator armed on. */
   if (r300_zb_depth_discovery_fill_initial(scenario, layout, declared) !=
       0) {
      free(declared);
      return false;
   }
   const bool equal =
      memcmp(declared, bytes, (size_t)scenario->allocation_bytes) == 0;
   free(declared);
   return equal;
}

int
main(int argc, char **argv)
{
   if (argc != 8) {
      fprintf(stderr,
              "usage: %s <evidence-directory> <layout> <x> <y> "
              "<pitch-pixels> <base-bytes> <arm>\n"
              "  layout: microtiled | macrotiled\n"
              "  x,y: 0..63; pitch-pixels: 64 | 96; "
              "base-bytes: 2048 | 4096\n"
              "  arm: measure | writes_disabled | never\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const struct layout_option *chosen_layout = NULL;
   for (size_t i = 0; i < sizeof(layout_options) / sizeof(*layout_options);
        i++)
      if (strcmp(argv[2], layout_options[i].name) == 0)
         chosen_layout = &layout_options[i];
   const struct arm_option *chosen_arm = NULL;
   for (size_t i = 0; i < sizeof(arm_options) / sizeof(*arm_options); i++)
      if (strcmp(argv[7], arm_options[i].name) == 0)
         chosen_arm = &arm_options[i];
   uint32_t pixel_x, pixel_y, pitch_pixels, base_bytes;
   if (chosen_layout == NULL || chosen_arm == NULL ||
       !parse_u32(argv[3], &pixel_x) || !parse_u32(argv[4], &pixel_y) ||
       !parse_u32(argv[5], &pitch_pixels) ||
       !parse_u32(argv[6], &base_bytes)) {
      fprintf(stderr, "invalid coordinate-discovery declaration\n");
      return 2;
   }

   struct r300_zb_coordinate_discovery configured;
   if (r300_zb_coordinate_discovery_init(
          chosen_layout->layout, pixel_x, pixel_y, pitch_pixels, base_bytes,
          &configured) != 0) {
      fprintf(stderr, "coordinate-discovery declaration refused\n");
      return 2;
   }
   const struct r300_zb_depth_discovery_scenario *scenario =
      &configured.scenario;
   struct r300_zb_depth_layout layout;
   if (r300_zb_depth_discovery_layout(scenario, &layout) != 0) {
      fprintf(stderr, "scenario resolves no layout\n");
      return 2;
   }
   const uint64_t depth_bytes = scenario->allocation_bytes;

   char declaration[512];
   const int declaration_bytes = r300_zb_coordinate_discovery_declaration(
      scenario, declaration, sizeof(declaration));
   if (declaration_bytes < 0) {
      fprintf(stderr, "scenario declaration does not serialize\n");
      return 2;
   }
   char scenario_blake3[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(declaration, (size_t)declaration_bytes, scenario_blake3);

   uint32_t depth_function;
   bool depth_write;
   struct r300_zb_depth_discovery_ib reference;
   if (!r3v_native_zb_discovery_arm_state(chosen_arm->arm, &depth_function,
                                           &depth_write) ||
       r300_zb_depth_discovery_reference_emit(
          scenario, depth_function, depth_write, &reference) != 0 ||
       r300_zb_depth_discovery_validate_reloc_sites(&reference) != 0) {
      fprintf(stderr, "coordinate-discovery cell does not construct\n");
      return 2;
   }
   char ib_blake3[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(reference.ib,
              (size_t)reference.ib_size_dwords * sizeof(uint32_t), ib_blake3);
   const uint32_t ib_dwords = reference.ib_size_dwords;
   r300_zb_depth_discovery_release(&reference);

   printf("[scenario] %s arm=%s pixel=(%u,%u) initial=0x%06x/0x%02x "
          "marker=0x%06x\n",
          scenario->name, chosen_arm->name, scenario->pixel_x,
          scenario->pixel_y, scenario->initial_depth_code,
          scenario->initial_stencil, scenario->marker_depth_code);
   printf("[layout] allocation=%llu guards=[0,%llu)+[%llu,%llu) "
          "envelope=[%llu,%llu) unclaimed=%llu\n",
          (unsigned long long)depth_bytes,
          (unsigned long long)layout.prefix_guard_bytes,
          (unsigned long long)layout.suffix_guard_offset_bytes,
          (unsigned long long)layout.total_bytes,
          (unsigned long long)layout.base_offset_bytes,
          (unsigned long long)(layout.base_offset_bytes +
                               layout.storage_bytes),
          (unsigned long long)(depth_bytes - layout.total_bytes));
   printf("layout=%s\n", chosen_layout->name);
   printf("pixel_x=%u\n", scenario->pixel_x);
   printf("pixel_y=%u\n", scenario->pixel_y);
   printf("pitch_pixels=%u\n", scenario->surface->pitch_pixels);
   printf("base_bytes=%u\n", scenario->guard_bytes);
   printf("arm=%s\n", chosen_arm->name);
   printf("allocation_bytes=%llu\n",
          (unsigned long long)scenario->allocation_bytes);
   printf("storage_bytes=%llu\n",
          (unsigned long long)layout.storage_bytes);
   printf("scenario_blake3=%s\n", scenario_blake3);
   printf("ib_dwords=%u\n", ib_dwords);
   printf("ib_blake3=%s\n", ib_blake3);
   fflush(stdout);

   /* A silicon result binds to the real libc entry points.  A preloaded
    * interposer would let the run report a silicon verdict it never
    * earned, so any LD_PRELOAD refuses before the first Vulkan call.
    */
   const char *preload = getenv("LD_PRELOAD");
   if (preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware control run admits no "
              "interposer\n",
              preload);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared == NULL || declared[0] == '\0' ||
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s; "
              "the armed directory and the readback directory are one "
              "directory\n",
              declared != NULL ? declared : "(unset)", evidence_dir);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (create_instance == NULL) {
      fprintf(stderr, "native ICD provides no vkCreateInstance\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);
   if (vkEnumeratePhysicalDevices == NULL ||
       vkGetPhysicalDeviceProperties == NULL || vkCreateDevice == NULL ||
       vkGetDeviceProcAddr == NULL || vkDestroyInstance == NULL) {
      fprintf(stderr, "native ICD omits a required instance entry point\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) ||
       pdev_count != 1 || pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS485M\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("device");
   const float priority = 1.0f;
   VkDevice device = VK_NULL_HANDLE;
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
               .pQueuePriorities = &priority,
            },
      },
      NULL, &device);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateDevice: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);
   if (vkAllocateMemory == NULL || vkFreeMemory == NULL ||
       vkMapMemory == NULL || vkGetDeviceQueue == NULL ||
       vkCreateCommandPool == NULL || vkDestroyCommandPool == NULL ||
       vkAllocateCommandBuffers == NULL || vkBeginCommandBuffer == NULL ||
       vkEndCommandBuffer == NULL || vkQueueSubmit == NULL ||
       vkDestroyDevice == NULL) {
      fprintf(stderr, "native ICD omits a required device entry point\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   stage("memory");
   struct { VkDeviceSize size; VkDeviceMemory memory; } allocations[] = {
      { R3V_ZB_DEPTH_CONTROL_VERTEX_ALLOCATION, VK_NULL_HANDLE },
      { R300_ZB_DISCOVERY_COLOR_BYTES, VK_NULL_HANDLE },
      { depth_bytes, VK_NULL_HANDLE },
   };
   for (unsigned i = 0; i < 3; i++) {
      if (vkAllocateMemory(device,
                           &(VkMemoryAllocateInfo){
                              .sType =
                                 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = allocations[i].size,
                              .memoryTypeIndex = 0,
                           },
                           NULL, &allocations[i].memory) != VK_SUCCESS) {
         fprintf(stderr, "allocation %u failed\n", i);
         return finish(OUTCOME_SUBMISSION_REFUSED);
      }
   }
   VkDeviceMemory vertex_memory = allocations[0].memory;
   VkDeviceMemory color_memory = allocations[1].memory;
   VkDeviceMemory depth_memory = allocations[2].memory;

   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   if (vkCreateCommandPool(
          device,
          &(VkCommandPoolCreateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
             .queueFamilyIndex = 0,
          },
          NULL, &pool) != VK_SUCCESS) {
      fprintf(stderr, "command pool creation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkAllocateCommandBuffers(
          device,
          &(VkCommandBufferAllocateInfo){
             .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
             .commandPool = pool,
             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
             .commandBufferCount = 1,
          },
          &cmd) != VK_SUCCESS) {
      fprintf(stderr, "command buffer allocation failed\n");
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           });
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkBeginCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = r3v_native_record_zb_coordinate_discovery(
      cmd, vertex_memory, color_memory, depth_memory,
      chosen_layout->layout, pixel_x, pixel_y, pitch_pixels, base_bytes,
      chosen_arm->arm);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "cell recording failed: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }
   result = vkEndCommandBuffer(cmd);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkEndCommandBuffer: %d\n", result);
      return finish(OUTCOME_SUBMISSION_REFUSED);
   }

   /* The initial image, read back from the allocation the recorder
    * filled and kept as bytes.  The observation compares this against
    * the post-draw image, so the run reports what the device changed in
    * the state that actually reached it rather than in the state the
    * scenario declared.
    */
   stage("initial image");
   void *depth_map = NULL;
   if (vkMapMemory(device, depth_memory, 0, VK_WHOLE_SIZE, 0, &depth_map) !=
          VK_SUCCESS ||
       depth_map == NULL) {
      fprintf(stderr, "depth map before submission failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   uint8_t *before = malloc((size_t)depth_bytes);
   if (before == NULL) {
      fprintf(stderr, "initial image allocation failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   memcpy(before, depth_map, (size_t)depth_bytes);
   const bool initialization_exact =
      initialization_is_declared(scenario, &layout, before);
   char initial_blake3[BLAKE3_OUT_LEN * 2 + 1];
   blake3_hex(before, (size_t)depth_bytes, initial_blake3);
   printf("[initial] declared=%d blake3=%s\n", initialization_exact,
          initial_blake3);
   printf("initial_image_blake3=%s\n", initial_blake3);
   fflush(stdout);
   if (r3v_native_evidence_write_file(evidence_dir, "depth_before.bin", before,
                                      (size_t)depth_bytes) != 0) {
      fprintf(stderr, "initial image retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: a live DRM_RADEON_CS reaches the command processor
    * here, and the bounded completion wait follows it inside the queue.
    * The submission is one-shot; whatever it returns, no resubmission
    * follows.
    */
   stage("submit");
   VkResult submit_result =
      vkQueueSubmit(queue, 1,
                    &(VkSubmitInfo){
                       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cmd,
                    },
                    VK_NULL_HANDLE);
   enum r3v_native_queue_status queue_status =
      r3v_native_queue_submission_status(device);
   printf("[submit] vkQueueSubmit returned %d status=%s\n", submit_result,
          r3v_native_queue_status_name(queue_status));
   fflush(stdout);

   /* Readback and retention run for every submit result: a refused or
    * incomplete submission still leaves both surfaces' states as
    * evidence.
    */
   stage("readback");
   void *color_map = NULL;
   if (vkMapMemory(device, color_memory, 0, VK_WHOLE_SIZE, 0, &color_map) !=
          VK_SUCCESS ||
       color_map == NULL) {
      fprintf(stderr, "color readback map failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "color_target.bin",
                                      color_map,
                                      R300_ZB_DISCOVERY_COLOR_BYTES) != 0) {
      fprintf(stderr, "color target retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }
   if (r3v_native_evidence_write_file(evidence_dir, "depth_after.bin",
                                      depth_map, (size_t)depth_bytes) != 0) {
      fprintf(stderr, "depth image retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("oracle");
   struct r300_zb_discovery_observation observation;
   r300_zb_depth_discovery_observe(scenario, &layout, before, depth_map,
                                   depth_bytes, &observation);
   struct r300_zb_discovery_color_verdict color_verdict;
   r300_zb_depth_discovery_color_observe(
      color_map, R300_ZB_DISCOVERY_COLOR_BYTES,
      R300_ZB_DISCOVERY_PITCH_PIXELS, R300_ZB_DISCOVERY_TARGET_WIDTH,
      R300_ZB_DISCOVERY_TARGET_HEIGHT, scenario->pixel_x, scenario->pixel_y,
      1u, 1u, R300_TRIANGLE_COLOR_SENTINEL, R300_TRIANGLE_DRAW_COLOR_B8G8R8A8,
      &color_verdict);

   printf("[oracle] judged=%d slots=%u unchanged=%u depth_only=%u "
          "stencil_only=%u both=%u depth_locations=%u\n",
          observation.judged, observation.slots_inspected,
          observation.slots_unchanged, observation.slots_depth_only,
          observation.slots_stencil_only, observation.slots_both,
          observation.depth_locations);
   printf("[oracle] guard=%llu/%llu unclaimed=%llu/%llu overflow=%d\n",
          (unsigned long long)observation.guard_bytes_changed,
          (unsigned long long)observation.guard_bytes_inspected,
          (unsigned long long)observation.unclaimed_bytes_changed,
          (unsigned long long)observation.unclaimed_bytes_inspected,
          observation.change_overflow);
   printf("[oracle] color judged=%d exact=%d inside=%u/%u outside=%u/%u\n",
          color_verdict.judged, color_verdict.exact,
          color_verdict.inside_colored, color_verdict.inside_samples,
          color_verdict.outside_colored, color_verdict.outside_samples);
   for (uint32_t i = 0; i < observation.change_count; i++) {
      const struct r300_zb_discovery_slot_change *c = &observation.changes[i];
      printf("[change] offset=%llu before=0x%08x after=0x%08x depth=%d "
             "stencil=%d depth_before=0x%06x depth_after=0x%06x "
             "stencil_before=0x%02x stencil_after=0x%02x\n",
             (unsigned long long)c->byte_offset, c->before_word, c->after_word,
             c->depth_changed, c->stencil_changed, c->before_depth,
             c->after_depth, c->before_stencil, c->after_stencil);
   }
   fflush(stdout);

   /* The changed slots as a JSON array.  The buffer holds the retention
    * capacity's worth of records and the composition refuses rather than
    * truncating, so a retained artifact never carries a partial list
    * that reads as a complete one.
    */
   char changes_json[8192];
   size_t changes_used = 0;
   changes_json[0] = '\0';
   for (uint32_t i = 0; i < observation.change_count; i++) {
      const struct r300_zb_discovery_slot_change *c = &observation.changes[i];
      const int written = snprintf(
         changes_json + changes_used, sizeof(changes_json) - changes_used,
         "%s{\"offset\": %llu, \"before\": \"0x%08x\", \"after\": "
         "\"0x%08x\", \"depth_changed\": %s, \"stencil_changed\": %s, "
         "\"depth_before\": \"0x%06x\", \"depth_after\": \"0x%06x\", "
         "\"stencil_before\": \"0x%02x\", \"stencil_after\": \"0x%02x\"}",
         i == 0 ? "" : ", ", (unsigned long long)c->byte_offset,
         c->before_word, c->after_word, c->depth_changed ? "true" : "false",
         c->stencil_changed ? "true" : "false", c->before_depth,
         c->after_depth, c->before_stencil, c->after_stencil);
      if (written <= 0 ||
          (size_t)written >= sizeof(changes_json) - changes_used) {
         fprintf(stderr, "change list does not compose\n");
         return finish(OUTCOME_RETENTION_FAILURE);
      }
      changes_used += (size_t)written;
   }

   /* What each arm requires.  The measurement arm locates exactly one
    * depth slot; the writes-disabled arm must move the color and no
    * depth code, which separates a scissor that reached the pixel from a
    * write that reached memory; the NEVER arm must move nothing at all.
    * A stencil-only change is retained in every arm and fails none of
    * them: whether a depth write preserves the packed stencil byte is
    * what the seed pair measures, not a condition on the address.
    */
   bool arm_expectation = false;
   switch (chosen_arm->arm) {
   case R3V_NATIVE_ZB_DISCOVERY_ARM_MEASURE:
      arm_expectation =
         observation.depth_locations == 1u && color_verdict.exact;
      break;
   case R3V_NATIVE_ZB_DISCOVERY_ARM_WRITES_DISABLED:
      arm_expectation =
         observation.depth_locations == 0u && color_verdict.exact;
      break;
   case R3V_NATIVE_ZB_DISCOVERY_ARM_NEVER:
      arm_expectation = observation.depth_locations == 0u &&
                        observation.change_count == 0u &&
                        color_verdict.inside_colored == 0u &&
                        color_verdict.outside_colored == 0u;
      break;
   }

   /* Classification order: a write into a guard range or into the slack
    * past the envelope stops the sequence whatever else passed, because
    * neither is a byte the experiment declared writable; then the
    * transport's own failures; then the arm's own expectation over an
    * initialization the run proved was the declared one.
    */
   enum outcome outcome;
   if (observation.judged && (observation.guard_bytes_changed != 0u ||
                              observation.unclaimed_bytes_changed != 0u))
      outcome = OUTCOME_CONTAINMENT_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE)
      outcome = OUTCOME_COMPLETION_FAILURE;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED ||
            submit_result != VK_SUCCESS)
      outcome = OUTCOME_SUBMISSION_REFUSED;
   else if (queue_status == R3V_NATIVE_QUEUE_STATUS_COMPLETED &&
            observation.judged && initialization_exact &&
            color_verdict.judged && arm_expectation)
      outcome = OUTCOME_CONTROL_PASS;
   else
      outcome = OUTCOME_CONTROL_FAILED_INCONCLUSIVE;

   char outcome_json[12288];
   int length = snprintf(
      outcome_json, sizeof(outcome_json),
      "{\n"
      "  \"schema\": \"r3v-native-zb-depth-discovery-outcome/1\",\n"
      "  \"verdict\": \"%s\",\n"
      "  \"scenario\": \"%s\",\n"
      "  \"layout\": \"%s\",\n"
      "  \"arm\": \"%s\",\n"
      "  \"pixel_x\": %u,\n"
      "  \"pixel_y\": %u,\n"
      "  \"initial_depth_code\": \"0x%06x\",\n"
      "  \"initial_stencil\": \"0x%02x\",\n"
      "  \"marker_depth_code\": \"0x%06x\",\n"
      "  \"pitch_pixels\": %u,\n"
      "  \"base_bytes\": %u,\n"
      "  \"scenario_blake3\": \"%s\",\n"
      "  \"ib_blake3\": \"%s\",\n"
      "  \"ib_dwords\": %u,\n"
      "  \"initial_image_blake3\": \"%s\",\n"
      "  \"initialization_declared\": %s,\n"
      "  \"allocation_bytes\": %llu,\n"
      "  \"envelope_offset\": %llu,\n"
      "  \"envelope_bytes\": %llu,\n"
      "  \"submit_result\": %d,\n"
      "  \"queue_status\": \"%s\",\n"
      "  \"judged\": %s,\n"
      "  \"slots_inspected\": %u,\n"
      "  \"slots_unchanged\": %u,\n"
      "  \"slots_depth_only\": %u,\n"
      "  \"slots_stencil_only\": %u,\n"
      "  \"slots_both\": %u,\n"
      "  \"depth_locations\": %u,\n"
      "  \"guard_bytes_inspected\": %llu,\n"
      "  \"guard_bytes_changed\": %llu,\n"
      "  \"unclaimed_bytes_inspected\": %llu,\n"
      "  \"unclaimed_bytes_changed\": %llu,\n"
      "  \"change_overflow\": %s,\n"
      "  \"changes\": [%s],\n"
      "  \"color_judged\": %s,\n"
      "  \"color_exact\": %s,\n"
      "  \"color_inside_colored\": %u,\n"
      "  \"color_inside_samples\": %u,\n"
      "  \"color_outside_colored\": %u,\n"
      "  \"color_outside_samples\": %u\n"
      "}\n",
      outcome_names[outcome], scenario->name, chosen_layout->name,
      chosen_arm->name,
      scenario->pixel_x, scenario->pixel_y, scenario->initial_depth_code,
      scenario->initial_stencil, scenario->marker_depth_code,
      scenario->surface->pitch_pixels, scenario->guard_bytes,
      scenario_blake3, ib_blake3, ib_dwords, initial_blake3,
      initialization_exact ? "true" : "false",
      (unsigned long long)depth_bytes,
      (unsigned long long)layout.base_offset_bytes,
      (unsigned long long)layout.storage_bytes, submit_result,
      r3v_native_queue_status_name(queue_status),
      observation.judged ? "true" : "false", observation.slots_inspected,
      observation.slots_unchanged, observation.slots_depth_only,
      observation.slots_stencil_only, observation.slots_both,
      observation.depth_locations,
      (unsigned long long)observation.guard_bytes_inspected,
      (unsigned long long)observation.guard_bytes_changed,
      (unsigned long long)observation.unclaimed_bytes_inspected,
      (unsigned long long)observation.unclaimed_bytes_changed,
      observation.change_overflow ? "true" : "false", changes_json,
      color_verdict.judged ? "true" : "false",
      color_verdict.exact ? "true" : "false", color_verdict.inside_colored,
      color_verdict.inside_samples, color_verdict.outside_colored,
      color_verdict.outside_samples);
   if (length <= 0 || (size_t)length >= sizeof(outcome_json) ||
       r3v_native_evidence_write_file(evidence_dir,
                                      "zb_depth_discovery_outcome.json",
                                      outcome_json, (size_t)length) != 0) {
      fprintf(stderr, "outcome retention failed\n");
      return finish(OUTCOME_RETENTION_FAILURE);
   }

   stage("teardown");
   free(before);
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 3; i++)
      vkFreeMemory(device, allocations[i].memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   return finish(outcome);
}
