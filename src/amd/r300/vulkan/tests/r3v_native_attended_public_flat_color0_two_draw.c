/*
 * SPDX-License-Identifier: MIT
 *
 * Attended public direct GA Flat two-draw cell: records two render
 * passes with a draw each through the application-shaped Vulkan
 * surface under one pipeline whose vertex output and fragment input
 * carry the Flat decoration, and drives the concatenated stream to a
 * live DRM_RADEON_CS on RS485M silicon.  The pipeline selects the direct
 * hardware route (r3v_interpolation_lowering.h): the varying rides the
 * TCL-bypass color 0 vector, each draw's first-draw contract programs
 * GA_COLOR_CONTROL with RGB0 and ALPHA0 FLAT and PROVOKING_VERTEX_FIRST
 * and routes color 0 through RS_IP_0 / RS_INST_0 into US input 0
 * (r300_flat_color0_plan.h), and the CPU route keeps the three
 * vertices' distinct values in the carrier because every triangle's
 * clipping class is ACCEPT.  The vertex module writes distinct red,
 * green, and alpha per vertex, so the hardware's provoking selection is
 * judged on RGB and on alpha alone; each pass draws the triangle in its
 * own vertex order, so the provoking vertex differs between the passes.
 * The expected targets come from the CPU replication oracle
 * (r3v_post_vs_lower_triangles over the same records, then the analytic
 * coverage), retained before the submission; the receipt is byte
 * equality of each target with its expected image, since the direct and
 * the replication command streams necessarily differ.  Before the
 * submission the runner digests the recorded indirect buffer and
 * refuses unless it equals the offline emitter's, and refuses unless
 * the plan's registers hold their words ahead of both draws.  Runs only
 * under the authorization and procedure in
 * docs/hardware/r3v-native-attended-public-flat-color0-two-draw-procedure.md;
 * every stage prints and flushes before it runs.
 */

#include "r3v_native.h"
#include "r3v_interpolation_lowering.h"
#include "r3v_post_vs_lowering.h"
#include "r3v_shader_interface.h"
#include "r3v_native_arming.h"
#include "r3v_native_reference_spirv.h"
#include "r3v_native_watchdog_guard.h"

#include "amd/r300/common/r300_first_draw_state.h"
#include "amd/r300/common/r300_flat_color0_plan.h"
#include "amd/r300/common/r300_reg.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/radeon/drm_vk/radeon_drm_vk_bo.h"
#include "r3v_native_multi_pass_arms.h"

#include "util/mesa-blake3.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance,
                                             const char *pName);

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

static bool
gate_open(const char *name)
{
   const char *value = getenv(name);
   return value != NULL && strcmp(value, "1") == 0;
}

/* The color the RGBA Flat vertex module derives from a position:
 * fma(position, (2, 2, 0, 0), 0) for red and green, and the dot
 * dot(position, (0.5333333, 0, 0, 0.55)), w = 1 carrying the alpha
 * offset, placed in the alpha lane; then the color buffer's UNORM8
 * conversion, which the oracle's packer applies through unorm8_round.
 * Each predicted alpha sits a quarter step from the nearest UNORM8
 * boundary, so the job's dot rounding cannot move the byte.
 */
static void
flat_tint(const float *position, float tint[4])
{
   tint[0] = position[0] * 2.0f;
   tint[1] = position[1] * 2.0f;
   tint[2] = 0.0f;
   tint[3] = position[0] * 0.53333333f + position[3] * 0.55f;
}

static uint32_t
flat_tint_dword(const struct r300_triangle_render_shape *shape,
                const float *position)
{
   float tint[4];
   flat_tint(position, tint);
   return r300_tcl_bypass_triangle_pack_unorm8_dword(shape->lanes, tint);
}

/* The CPU replication oracle over one pass's three records in draw
 * order: the qualified post-vertex lowering replicates the provoking
 * vertex's tint across the triangle, and the packed result is the one
 * dword the pass's whole interior is expected to hold.  Refuses when
 * the lowering leaves the three records apart.
 */
static bool
replication_oracle_dword(const struct r300_triangle_render_shape *shape,
                         const float *ndc_triangle, const uint32_t order[3],
                         uint32_t *out)
{
   uint32_t records[3 * 8];
   for (unsigned v = 0; v < 3; v++) {
      float tint[4];
      flat_tint(&ndc_triangle[order[v] * 4], tint);
      memcpy(&records[v * 8], &ndc_triangle[order[v] * 4], 4 * sizeof(float));
      memcpy(&records[v * 8 + 4], tint, sizeof(tint));
   }
   const struct r3v_post_vs_lowering lowering = {
      .flat_mask = 1u,
      .provoking_vertex = R3V_POST_VS_PROVOKING_VERTEX_FIRST,
   };
   if (r3v_post_vs_lower_triangles(&lowering, records, 1u, 8u) != 0)
      return false;
   if (memcmp(&records[4], &records[12], 4 * sizeof(uint32_t)) != 0 ||
       memcmp(&records[4], &records[20], 4 * sizeof(uint32_t)) != 0)
      return false;
   float tint[4];
   memcpy(tint, &records[4], sizeof(tint));
   *out = r300_tcl_bypass_triangle_pack_unorm8_dword(shape->lanes, tint);
   return true;
}

static void
report(const char *label, const struct r300_triangle_coverage_verdict *v)
{
   printf("[oracle] %s judged=%d coverage_exact=%d canary=%d interior=%u "
          "analytic=%u exterior=%u ambiguous=%u mismatch=%u\n",
          label, v->judged, v->coverage_exact, v->canary_pass,
          v->interior_pixels, v->analytic_pixels, v->exterior_pixels,
          v->ambiguous_pixels, v->mismatch_pixels);
   if (!v->judged)
      printf("[oracle] %s verdict refused: the shape or the retained "
             "footprint left the producer's domain, so the zero counters "
             "carry no claim about the render\n",
             label);
   fflush(stdout);
}

/* One pass's public objects: the image, its memory, its view, and its
 * framebuffer over the shared render pass.
 */
struct pass_target {
   VkImage image;
   VkDeviceMemory memory;
   VkImageView view;
   VkFramebuffer framebuffer;
};

int
main(int argc, char **argv)
{
   /* --record-only builds every object and records the command buffer,
    * then stops at the recording boundary after comparing the recorded
    * stream's digest against the emitter's.  The public recording
    * contract -- image family, pass shape, pipeline admission, the
    * two-pass concatenation and its binding -- is what this program can
    * get wrong with no device present, so the shim fixture calibrates
    * it here and the attended run inherits a proven sequence.
    */
   bool record_only = false;
   const char *waiver_path = NULL;
   bool usage_error = argc < 2;
   for (int i = 2; i < argc && !usage_error; i++) {
      if (strcmp(argv[i], "--record-only") == 0)
         record_only = true;
      else if (strcmp(argv[i], "--waiver") == 0 && i + 1 < argc)
         waiver_path = argv[++i];
      else
         usage_error = true;
   }
   if (usage_error) {
      fprintf(stderr,
              "usage: %s <evidence-directory> [--record-only] "
              "[--waiver <path>]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const char *preload = getenv("LD_PRELOAD");
   if (!record_only && preload != NULL && preload[0] != '\0') {
      fprintf(stderr,
              "LD_PRELOAD is set (%s); a hardware run admits no "
              "interposer\n",
              preload);
      return 1;
   }

   /* The cell runs on the CPU delivery route, where the Flat lowering
    * lives; a producer gate naming another route refuses before the
    * one-shot token is spent on a stream the authorization does not
    * name.
    */
   if (gate_open("R3V_NATIVE_FLAT_REPLICATION_PINNED")) {
      fprintf(stderr, "R3V_NATIVE_FLAT_REPLICATION_PINNED=1 pins the "
              "replication route; the direct GA cell needs it unset\n");
      return 2;
   }
   if (!record_only &&
       (gate_open("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL") ||
        gate_open("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL") ||
        gate_open("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL"))) {
      fprintf(stderr,
              "the direct GA Flat two-draw cell runs on the CPU route; "
              "every "
              "R3V_NATIVE_R2VB_*_EXPERIMENTAL gate stays unset\n");
      return 2;
   }

   /* The stream the public route records: both passes the reference
    * shape carrying the varying through color 0 under the canonical
    * direct plan, bound at merged indices 2 and 3.  The vertex order,
    * and with it the provoking value, rides the vertex stream and
    * leaves the stream bytes unchanged.
    */
   struct r300_triangle_multi_pass mp;
   r3v_native_multi_pass_public_flat_color0_reference(&mp);
   struct r300_flat_color0_plan plan;
   r300_flat_color0_plan_direct_first(&plan);
   struct r300_first_draw_contract base_contract;
   if (r300_tcl_bypass_triangle_reference_contract(&base_contract) != 0) {
      fprintf(stderr, "the reference contract refused to resolve\n");
      return 1;
   }
   uint32_t ga_base = 0;
   bool ga_found = false;
   for (uint32_t i = 0; i < base_contract.count; i++) {
      if (base_contract.entries[i].reg == R300_GA_COLOR_CONTROL) {
         ga_base = base_contract.entries[i].value;
         ga_found = true;
      }
   }
   if (!ga_found) {
      fprintf(stderr, "the reference contract carries no GA_COLOR_CONTROL "
              "clause\n");
      return 1;
   }

   const uint32_t color_bytes =
      r300_tcl_bypass_triangle_render_shape_color_bytes(&mp.pass[0]);

   /* The reference triangle in NDC, A, B, C, and each pass's draw
    * order: the first pass draws B, C, A and the second C, A, B, so
    * the first pass's provoking vertex is B (red, alpha 0.95) and the
    * second's C (green, alpha 0.55), while A (black, alpha 0.15) is a
    * non-provoking input of both.  Every pass sees three distinct
    * inputs in RGB and in alpha, and each pass's provoking color is the
    * other pass's non-provoking one.
    */
   static const float ndc_triangle[R300_TRIANGLE_VERTEX_DWORDS] = {
      -0.75f, -0.75f, 0.0f, 1.0f,
       0.75f, -0.75f, 0.0f, 1.0f,
       0.00f,  0.75f, 0.0f, 1.0f,
   };
   static const uint32_t pass_order[2][3] = { { 1, 2, 0 }, { 2, 0, 1 } };
   uint32_t vertex_dword[3];
   for (unsigned v = 0; v < 3; v++)
      vertex_dword[v] = flat_tint_dword(&mp.pass[0], &ndc_triangle[v * 4]);
   uint32_t first_dword = 0, second_dword = 0;
   if (!replication_oracle_dword(&mp.pass[0], ndc_triangle, pass_order[0],
                                 &first_dword) ||
       !replication_oracle_dword(&mp.pass[1], ndc_triangle, pass_order[1],
                                 &second_dword) ||
       first_dword != vertex_dword[pass_order[0][0]] ||
       second_dword != vertex_dword[pass_order[1][0]]) {
      fprintf(stderr, "the CPU replication oracle and the first-vertex "
              "model disagree\n");
      return 1;
   }
   /* The alpha lane alone must separate every pair, so an alpha the
    * hardware interpolates while RGB stays flat is observable. */
   for (unsigned a = 0; a < 3; a++)
      for (unsigned b = a + 1; b < 3; b++)
         if ((vertex_dword[a] >> 24) == (vertex_dword[b] >> 24)) {
            fprintf(stderr, "vertex alphas are not pairwise distinct\n");
            return 1;
         }
   if (first_dword == second_dword || first_dword == vertex_dword[0] ||
       second_dword == vertex_dword[0] ||
       first_dword == R300_TRIANGLE_COLOR_SENTINEL ||
       second_dword == R300_TRIANGLE_COLOR_SENTINEL ||
       vertex_dword[0] == R300_TRIANGLE_COLOR_SENTINEL) {
      fprintf(stderr, "the three vertex colors and the sentinel are not "
              "pairwise distinct\n");
      return 1;
   }

   struct r300_tcl_bypass_triangle_ib armed;
   if (r300_tcl_bypass_triangle_clip_space_multi_pass_emit(&mp, &armed) !=
       0) {
      fprintf(stderr, "the two-pass cell refused to emit\n");
      return 1;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(armed.ib, armed.ib_size_dwords, digest);
   const uint32_t ib_dwords = armed.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&armed);

   printf("[shape] public direct GA Flat two-draw, two color-0 passes "
          "%ux%u pitch %u, binding (%u, %u), %u IB dwords, cell blake3 "
          "%.8s\n",
          mp.pass[0].width, mp.pass[0].height, mp.pass[0].pitch_pixels,
          mp.second_vertex_index, mp.second_color_index, ib_dwords, digest);
   printf("[predict] vertex colors A=0x%08x B=0x%08x C=0x%08x; first pass "
          "draws B, C, A and its interior reads 0x%08x; second pass draws "
          "C, A, B and its interior reads 0x%08x; exterior 0x%08x and "
          "canary clean on both; GA_COLOR_CONTROL 0x%08x, RS_COUNT 0x%08x, "
          "RS_IP_0 0x%08x, RS_INST_0 0x%08x ahead of each draw\n",
          vertex_dword[0], vertex_dword[1], vertex_dword[2], first_dword,
          second_dword, R300_TRIANGLE_COLOR_SENTINEL,
          r300_flat_color0_plan_ga_color_control(&plan, ga_base),
          r300_flat_color0_plan_rs_count(&plan),
          r300_flat_color0_plan_rs_ip_0(&plan),
          r300_flat_color0_plan_rs_inst_0(&plan));
   printf("[predict] falsifier: a recorded stream whose digest differs "
          "from the emitter's, or whose plan registers do not hold their "
          "words ahead of both draws, refuses before any ioctl; any "
          "interior pixel holding a non-provoking input color names "
          "another vertex selected; an interior pixel whose RGB is the "
          "provoking one under another alpha names alpha interpolated "
          "while RGB stays flat; any other interior dword names an "
          "interpolated varying or a color-lane conversion the oracle "
          "does not model; a target exact under the other pass's "
          "provoking color names state crossing the pass boundary; a "
          "second target holding the sentinel names a second cell the "
          "command processor never reached\n");
   fflush(stdout);

   /* The expected targets, generated through the replication oracle's
    * dwords over the analytic coverage and retained ahead of the
    * submission; the receipt is byte equality with these.
    */
   uint32_t *expected[2] = { calloc(1, color_bytes), calloc(1, color_bytes) };
   if (expected[0] == NULL || expected[1] == NULL ||
       r300_tcl_bypass_triangle_expected_target(
          &mp.pass[0], first_dword, R300_TRIANGLE_COLOR_SENTINEL,
          expected[0], color_bytes) != 0 ||
       r300_tcl_bypass_triangle_expected_target(
          &mp.pass[1], second_dword, R300_TRIANGLE_COLOR_SENTINEL,
          expected[1], color_bytes) != 0) {
      fprintf(stderr, "the expected targets refused to generate\n");
      return 1;
   }
   if (!record_only &&
       (r3v_native_evidence_write_file(evidence_dir, "first_expected.bin",
                                       expected[0], color_bytes) != 0 ||
        r3v_native_evidence_write_file(evidence_dir, "second_expected.bin",
                                       expected[1], color_bytes) != 0)) {
      fprintf(stderr, "expected target retention failed\n");
      return 1;
   }
   {
      char expected_digest[2][BLAKE3_OUT_LEN * 2 + 1];
      for (unsigned p = 0; p < 2; p++)
         r300_triangle_ib_digest_hex(expected[p], color_bytes / 4u,
                                     expected_digest[p]);
      printf("[predict] expected targets blake3 first %.8s second %.8s\n",
             expected_digest[0], expected_digest[1]);
      fflush(stdout);
   }

   const char *declared = getenv("R3V_NATIVE_MANIFEST_DIR");
   if (declared != NULL && declared[0] != '\0' &&
       !same_directory(declared, evidence_dir)) {
      fprintf(stderr,
              "R3V_NATIVE_MANIFEST_DIR names %s and the argument names %s\n",
              declared, evidence_dir);
      return 2;
   }
   setenv("R3V_NATIVE_MANIFEST_DIR", evidence_dir, 1);

   struct r3v_native_watchdog_guard guard = {0};
   if (!record_only) {
      stage("watchdog");
      if (r3v_native_watchdog_guard_open(&guard, waiver_path, evidence_dir,
                                         digest) != 0)
         return 2;
   }

   stage("instance");
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vk_icdGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = create_instance(
      &(VkInstanceCreateInfo){
         .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      },
      NULL, &instance);
   if (result != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance: %d\n", result);
      return 1;
   }

#define LOAD_INSTANCE(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
   LOAD_INSTANCE(vkEnumeratePhysicalDevices);
   LOAD_INSTANCE(vkGetPhysicalDeviceProperties);
   LOAD_INSTANCE(vkCreateDevice);
   LOAD_INSTANCE(vkGetDeviceProcAddr);
   LOAD_INSTANCE(vkDestroyInstance);

   stage("physical device");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || pdev_count != 1 ||
       pdev == VK_NULL_HANDLE) {
      fprintf(stderr, "no native physical device: %d count %u\n", result,
              pdev_count);
      return 1;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pdev, &props);
   printf("[identity] vendor 0x%04x device 0x%04x name %s\n", props.vendorID,
          props.deviceID, props.deviceName);
   fflush(stdout);
   if (props.vendorID != R3V_NATIVE_ARMING_PCI_VENDOR ||
       props.deviceID != R3V_NATIVE_ARMING_PCI_DEVICE) {
      fprintf(stderr, "enumerated chip is not the authorized RS485M\n");
      return 1;
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
      return 1;
   }

   r3v_native_watchdog_guard_install(&guard, device);

   PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
#define LOAD_DEVICE(name) PFN_##name name = (PFN_##name)gdpa(device, #name)
   LOAD_DEVICE(vkAllocateMemory);
   LOAD_DEVICE(vkFreeMemory);
   LOAD_DEVICE(vkMapMemory);
   LOAD_DEVICE(vkUnmapMemory);
   LOAD_DEVICE(vkCreateBuffer);
   LOAD_DEVICE(vkDestroyBuffer);
   LOAD_DEVICE(vkBindBufferMemory);
   LOAD_DEVICE(vkCreateImage);
   LOAD_DEVICE(vkDestroyImage);
   LOAD_DEVICE(vkGetImageMemoryRequirements);
   LOAD_DEVICE(vkBindImageMemory);
   LOAD_DEVICE(vkCreateImageView);
   LOAD_DEVICE(vkDestroyImageView);
   LOAD_DEVICE(vkCreateRenderPass);
   LOAD_DEVICE(vkDestroyRenderPass);
   LOAD_DEVICE(vkCreateFramebuffer);
   LOAD_DEVICE(vkDestroyFramebuffer);
   LOAD_DEVICE(vkCreateShaderModule);
   LOAD_DEVICE(vkDestroyShaderModule);
   LOAD_DEVICE(vkCreatePipelineLayout);
   LOAD_DEVICE(vkDestroyPipelineLayout);
   LOAD_DEVICE(vkCreateGraphicsPipelines);
   LOAD_DEVICE(vkDestroyPipeline);
   LOAD_DEVICE(vkCreateCommandPool);
   LOAD_DEVICE(vkDestroyCommandPool);
   LOAD_DEVICE(vkAllocateCommandBuffers);
   LOAD_DEVICE(vkBeginCommandBuffer);
   LOAD_DEVICE(vkEndCommandBuffer);
   LOAD_DEVICE(vkCmdBeginRenderPass);
   LOAD_DEVICE(vkCmdEndRenderPass);
   LOAD_DEVICE(vkCmdBindPipeline);
   LOAD_DEVICE(vkCmdBindVertexBuffers);
   LOAD_DEVICE(vkCmdDraw);
   LOAD_DEVICE(vkGetDeviceQueue);
   LOAD_DEVICE(vkQueueSubmit);
   LOAD_DEVICE(vkDestroyDevice);

#define CHECK(call)                                        \
   do {                                                    \
      VkResult check_result = (call);                      \
      if (check_result != VK_SUCCESS) {                    \
         fprintf(stderr, "%s: %d\n", #call, check_result); \
         return 1;                                         \
      }                                                    \
   } while (0)

   /* Two color targets, each the 64x64 B8G8R8A8 surface whose memory
    * requirement is the cell footprint with the canary row.  Both carry
    * the sentinel before the submission, so every dword either target
    * holds afterward names its writer.
    */
   stage("targets");
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = R3V_NATIVE_TARGET_FORMAT,
      .extent = { R3V_NATIVE_TARGET_WIDTH, R3V_NATIVE_TARGET_HEIGHT, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   struct pass_target target[2] = { { VK_NULL_HANDLE } };
   for (unsigned i = 0; i < 2; i++) {
      CHECK(vkCreateImage(device, &image_info, NULL, &target[i].image));
      VkMemoryRequirements reqs;
      vkGetImageMemoryRequirements(device, target[i].image, &reqs);
      if (reqs.size != R3V_NATIVE_TARGET_MEMORY_BYTES ||
          reqs.size < color_bytes) {
         fprintf(stderr, "target %u requirement %llu is not the cell "
                 "footprint %u\n",
                 i, (unsigned long long)reqs.size, color_bytes);
         return 1;
      }
      CHECK(vkAllocateMemory(
         device,
         &(VkMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = reqs.size,
            .memoryTypeIndex = 0,
         },
         NULL, &target[i].memory));
      CHECK(vkBindImageMemory(device, target[i].image, target[i].memory, 0));
      void *map = NULL;
      CHECK(vkMapMemory(device, target[i].memory, 0, VK_WHOLE_SIZE, 0, &map));
      uint32_t *pixels = map;
      for (size_t p = 0; p < reqs.size / 4; p++)
         pixels[p] = R300_TRIANGLE_COLOR_SENTINEL;
      vkUnmapMemory(device, target[i].memory);
   }

   /* The application's vertex records: the reference triangle in NDC
    * as F32_4, twice, each pass's three records in that pass's draw
    * order.  With the delivery gates closed the public route resolves
    * a clip-space carrier, so each pass's deferred draw admits the clip
    * volume alone (w == 1, x and y in [-1, 1]) and applies the Vulkan
    * viewport transform itself, (x + 1) * width / 2; every record
    * lands on its vertex's reference window position with no rounding.
    * The same transform runs here ahead of any device work and refuses
    * a payload that misses the reference.
    */
   stage("vertex stream");
   float stream[2 * R300_TRIANGLE_VERTEX_DWORDS];
   for (unsigned p = 0; p < 2; p++)
      for (unsigned v = 0; v < 3; v++)
         memcpy(&stream[(p * 3 + v) * 4], &ndc_triangle[pass_order[p][v] * 4],
                4 * sizeof(float));
   {
      float reference[R300_TRIANGLE_VERTEX_DWORDS];
      r300_tcl_bypass_triangle_render_shape_vertices(&mp.pass[0], reference);
      for (unsigned p = 0; p < 2; p++) {
         for (unsigned v = 0; v < 3; v++) {
            const float *pos = &stream[(p * 3 + v) * 4];
            const float window[4] = {
               (pos[0] + 1.0f) * ((float)mp.pass[0].width / 2.0f),
               (pos[1] + 1.0f) * ((float)mp.pass[0].height / 2.0f),
               pos[2], pos[3],
            };
            if (!(pos[3] == 1.0f) || !(pos[0] >= -1.0f && pos[0] <= 1.0f) ||
                !(pos[1] >= -1.0f && pos[1] <= 1.0f) ||
                memcmp(window, &reference[pass_order[p][v] * 4],
                       sizeof(window)) != 0) {
               fprintf(stderr,
                       "pass %u vertex %u: NDC (%g, %g, %g, %g) is outside "
                       "the clip volume or misses the reference window "
                       "position\n",
                       p, v, pos[0], pos[1], pos[2], pos[3]);
               return 1;
            }
         }
      }
   }
   VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
   VkBuffer vertex_buffer = VK_NULL_HANDLE;
   CHECK(vkAllocateMemory(device,
                          &(VkMemoryAllocateInfo){
                             .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = 4096,
                             .memoryTypeIndex = 0,
                          },
                          NULL, &vertex_memory));
   CHECK(vkCreateBuffer(device,
                        &(VkBufferCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                           .size = sizeof(stream),
                           .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                        },
                        NULL, &vertex_buffer));
   CHECK(vkBindBufferMemory(device, vertex_buffer, vertex_memory, 0));
   {
      void *map = NULL;
      CHECK(vkMapMemory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0, &map));
      memcpy(map, stream, sizeof(stream));
      vkUnmapMemory(device, vertex_memory);
   }

   stage("pipelines");
   VkRenderPass pass = VK_NULL_HANDLE;
   VkPipelineLayout layout = VK_NULL_HANDLE;
   CHECK(vkCreateRenderPass(
      device,
      &(VkRenderPassCreateInfo){
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments =
            &(VkAttachmentDescription){
               .format = R3V_NATIVE_TARGET_FORMAT,
               .samples = VK_SAMPLE_COUNT_1_BIT,
               .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
               .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
            },
         .subpassCount = 1,
         .pSubpasses =
            &(VkSubpassDescription){
               .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
               .colorAttachmentCount = 1,
               .pColorAttachments =
                  &(VkAttachmentReference){
                     .attachment = 0,
                     .layout = VK_IMAGE_LAYOUT_GENERAL,
                  },
            },
      },
      NULL, &pass));
   for (unsigned i = 0; i < 2; i++) {
      CHECK(vkCreateImageView(
         device,
         &(VkImageViewCreateInfo){
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = target[i].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = R3V_NATIVE_TARGET_FORMAT,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                  .levelCount = 1,
                                  .layerCount = 1 },
         },
         NULL, &target[i].view));
      CHECK(vkCreateFramebuffer(
         device,
         &(VkFramebufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = pass,
            .attachmentCount = 1,
            .pAttachments = &target[i].view,
            .width = R3V_NATIVE_TARGET_WIDTH,
            .height = R3V_NATIVE_TARGET_HEIGHT,
            .layers = 1,
         },
         NULL, &target[i].framebuffer));
   }
   CHECK(vkCreatePipelineLayout(
      device,
      &(VkPipelineLayoutCreateInfo){
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      },
      NULL, &layout));

   /* One pipeline over the Flat module pair for both passes: the RGBA
    * Flat vertex module writes its tint as a Flat location-0 output, and the Flat fragment module writes its Flat
    * location-0 input as the color, so the pipeline's shader-interface
    * record links one Flat varying.  Two pipelines over the same pair
    * would carry the same record; the passes differ in vertex order
    * alone.
    */
   VkShaderModule vs = VK_NULL_HANDLE;
   CHECK(vkCreateShaderModule(
      device,
      &(VkShaderModuleCreateInfo){
         .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
         .codeSize = sizeof(r3v_reference_vertex_flat_rgba_spirv),
         .pCode = r3v_reference_vertex_flat_rgba_spirv,
      },
      NULL, &vs));
   VkPipeline pipeline[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   for (unsigned i = 0; i < 1; i++) {
      VkShaderModule fs = VK_NULL_HANDLE;
      CHECK(vkCreateShaderModule(
         device,
         &(VkShaderModuleCreateInfo){
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = sizeof(r3v_reference_fragment_flat_spirv),
            .pCode = r3v_reference_fragment_flat_spirv,
         },
         NULL, &fs));
      const VkGraphicsPipelineCreateInfo pipeline_info = {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages =
            (VkPipelineShaderStageCreateInfo[]){
               { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                 .module = vs,
                 .pName = "main" },
               { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                 .module = fs,
                 .pName = "main" },
            },
         .pVertexInputState =
            &(VkPipelineVertexInputStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
               .vertexBindingDescriptionCount = 1,
               .pVertexBindingDescriptions =
                  &(VkVertexInputBindingDescription){
                     .binding = 0,
                     .stride = 16,
                     .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                  },
               .vertexAttributeDescriptionCount = 1,
               .pVertexAttributeDescriptions =
                  &(VkVertexInputAttributeDescription){
                     .location = 0,
                     .binding = 0,
                     .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                     .offset = 0,
                  },
            },
         .pInputAssemblyState =
            &(VkPipelineInputAssemblyStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
               .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            },
         .pViewportState =
            &(VkPipelineViewportStateCreateInfo){
               .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
               .viewportCount = 1,
               .pViewports =
                  &(VkViewport){
                     .width = (float)R3V_NATIVE_TARGET_WIDTH,
                     .height = (float)R3V_NATIVE_TARGET_HEIGHT,
                     .maxDepth = 1.0f,
                  },
               .scissorCount = 1,
               .pScissors =
                  &(VkRect2D){
                     .extent = { R3V_NATIVE_TARGET_WIDTH,
                                 R3V_NATIVE_TARGET_HEIGHT },
                  },
            },
         .pRasterizationState =
            &(VkPipelineRasterizationStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
               .polygonMode = VK_POLYGON_MODE_FILL,
               .cullMode = VK_CULL_MODE_NONE,
               .lineWidth = 1.0f,
            },
         .pMultisampleState =
            &(VkPipelineMultisampleStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            },
         .pColorBlendState =
            &(VkPipelineColorBlendStateCreateInfo){
               .sType =
                  VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
               .attachmentCount = 1,
               .pAttachments =
                  &(VkPipelineColorBlendAttachmentState){
                     .colorWriteMask =
                        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
                  },
            },
         .layout = layout,
         .renderPass = pass,
      };
      CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                      &pipeline_info, NULL, &pipeline[i]));
      vkDestroyShaderModule(device, fs, NULL);
   }
   vkDestroyShaderModule(device, vs, NULL);
   pipeline[1] = pipeline[0];

   /* The frozen interface: the pipeline's shader-interface record
    * serialized and digested, and its flat_mask, printed beside the
    * cell digest so the authorization can name them.  A record without
    * exactly the one Flat varying refuses ahead of any ioctl.
    */
   {
      VK_FROM_HANDLE(r3v_native_pipeline, native_pipeline, pipeline[0]);
      char text[4096];
      const size_t n = r3v_shader_interface_link_serialize(
         &native_pipeline->shader_interface, text, sizeof(text));
      if (n >= sizeof(text)) {
         fprintf(stderr, "the interface record overran its buffer\n");
         return 1;
      }
      blake3_hash hash;
      char hex[BLAKE3_OUT_LEN * 2 + 1];
      _mesa_blake3_compute(text, n, hash);
      _mesa_blake3_format(hex, hash);
      printf("[interface] blake3 %.8s varying_mask=0x%x flat_mask=0x%x "
             "noperspective_mask=0x%x post_vs.flat_mask=0x%x "
             "provoking=%u route=%s\n",
             hex, native_pipeline->shader_interface.varying_mask,
             native_pipeline->shader_interface.flat_mask,
             native_pipeline->shader_interface.noperspective_mask,
             native_pipeline->post_vs.flat_mask,
             native_pipeline->post_vs.provoking_vertex,
             native_pipeline->interpolation_route ==
                   R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0
                ? "direct-ga-color0"
                : "replicate");
      fflush(stdout);
      if (native_pipeline->shader_interface.varying_mask != 1u ||
          native_pipeline->shader_interface.flat_mask != 1u ||
          native_pipeline->post_vs.flat_mask != 1u ||
          native_pipeline->post_vs.provoking_vertex !=
             R3V_POST_VS_PROVOKING_VERTEX_FIRST ||
          native_pipeline->interpolation_route !=
             R3V_INTERPOLATION_ROUTE_DIRECT_GA_COLOR0) {
         fprintf(stderr, "the pipeline's interface is not one Flat varying "
                 "with the first provoking vertex on the direct GA "
                 "route\n");
         return 1;
      }
   }

   /* Two render passes with a draw each.  Each pass's load-op clear is
    * the sentinel the oracle reads as exterior and canary; the public
    * draw path realizes it as a host fill of the target footprint ahead
    * of the device draw, so the exterior prediction is the sentinel on
    * either realization.
    */
   stage("record");
   VkCommandPool pool = VK_NULL_HANDLE;
   CHECK(vkCreateCommandPool(
      device,
      &(VkCommandPoolCreateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = 0,
      },
      NULL, &pool));
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   CHECK(vkAllocateCommandBuffers(
      device,
      &(VkCommandBufferAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
         .commandPool = pool,
         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
         .commandBufferCount = 1,
      },
      &cmd));
   CHECK(vkBeginCommandBuffer(
      cmd, &(VkCommandBufferBeginInfo){
              .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
           }));
   const float sentinel = (float)0xa5 / 255.0f;
   const VkClearValue clear = { .color = { .float32 = { sentinel, sentinel,
                                                        sentinel,
                                                        sentinel } } };
   for (unsigned i = 0; i < 2; i++) {
      vkCmdBeginRenderPass(
         cmd,
         &(VkRenderPassBeginInfo){
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass,
            .framebuffer = target[i].framebuffer,
            .renderArea = { .extent = { R3V_NATIVE_TARGET_WIDTH,
                                        R3V_NATIVE_TARGET_HEIGHT } },
            .clearValueCount = 1,
            .pClearValues = &clear,
         },
         VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline[i]);
      vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &(VkDeviceSize){ 0 });
      vkCmdDraw(cmd, 3, 1, 3 * i, 0);
      vkCmdEndRenderPass(cmd);
   }
   CHECK(vkEndCommandBuffer(cmd));

   /* The recorded stream against the emitter's: the authorization names
    * the emitter's digest, so a recording that differs refuses here,
    * ahead of any ioctl, with the differing dwords named.
    */
   stage("recorded stream");
   VK_FROM_HANDLE(r3v_native_cmd_buffer, native, cmd);
   char recorded_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(native->ib, native->ib_size_dwords,
                               recorded_digest);
   printf("[record] kind=%d references=%u deferred_draws=%u ib_dwords=%u "
          "recorded blake3 %.8s emitted blake3 %.8s\n",
          (int)native->cell_kind, native->reference_count,
          native->deferred_draw_count, native->ib_size_dwords,
          recorded_digest, digest);
   fflush(stdout);
   const bool stream_agrees =
      native->cell_kind == R3V_NATIVE_CELL_KIND_TRIANGLE_MULTI_PASS &&
      native->reference_count == 4 && native->ib_size_dwords == ib_dwords &&
      strcmp(recorded_digest, digest) == 0;
   if (native->deferred_draw_count != 2 ||
       native->deferred_draws[0].post_vs.flat_mask != 1u ||
       native->deferred_draws[1].post_vs.flat_mask != 1u ||
       !native->deferred_draws[0].direct_flat ||
       !native->deferred_draws[1].direct_flat ||
       native->deferred_draws[0].first_vertex != 0 ||
       native->deferred_draws[1].first_vertex != 3) {
      fprintf(stderr, "the two deferred draws do not both carry the direct "
              "Flat route over their own vertex order\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }
   if (!stream_agrees) {
      struct r300_tcl_bypass_triangle_ib offline;
      if (r300_tcl_bypass_triangle_clip_space_multi_pass_emit(
             &mp, &offline) == 0) {
         uint32_t differing = 0;
         const uint32_t common = offline.ib_size_dwords < native->ib_size_dwords
                                    ? offline.ib_size_dwords
                                    : native->ib_size_dwords;
         for (uint32_t i = 0; i < common; i++) {
            if (offline.ib[i] != native->ib[i]) {
               if (differing < 8)
                  fprintf(stderr,
                          "dword %u: recorded 0x%08x emitted 0x%08x\n", i,
                          native->ib[i], offline.ib[i]);
               differing++;
            }
         }
         fprintf(stderr, "%u differing dwords over %u common\n", differing,
                 common);
         r300_tcl_bypass_triangle_release(&offline);
      }
      fprintf(stderr, "the public recording is not the authorized stream; "
              "refusing ahead of the ioctl\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }

   /* The plan's registers ahead of both draws, read from the recorded
    * bytes themselves: each draw's own contract establishes them.
    */
   const int state_draws = r300_flat_color0_plan_stream_check(
      &plan, ga_base, native->ib, native->ib_size_dwords);
   printf("[state] direct plan registers established ahead of %d draw(s)\n",
          state_draws);
   fflush(stdout);
   if (state_draws != 2) {
      fprintf(stderr, "the recorded stream does not establish the direct "
              "plan ahead of both draws; refusing ahead of the ioctl\n");
      if (!record_only)
         r3v_native_watchdog_guard_close(&guard, VK_ERROR_UNKNOWN);
      return 2;
   }

   if (record_only) {
      printf("record: ACCEPTED\n");
      free(expected[0]);
      free(expected[1]);
      fflush(stdout);
      vkDestroyCommandPool(device, pool, NULL);
      for (unsigned i = 0; i < 2; i++) {
         if (i == 0)
            vkDestroyPipeline(device, pipeline[i], NULL);
         vkDestroyFramebuffer(device, target[i].framebuffer, NULL);
         vkDestroyImageView(device, target[i].view, NULL);
         vkDestroyImage(device, target[i].image, NULL);
         vkFreeMemory(device, target[i].memory, NULL);
      }
      vkDestroyPipelineLayout(device, layout, NULL);
      vkDestroyRenderPass(device, pass, NULL);
      vkDestroyBuffer(device, vertex_buffer, NULL);
      vkFreeMemory(device, vertex_memory, NULL);
      vkDestroyDevice(device, NULL);
      vkDestroyInstance(instance, NULL);
      return 0;
   }

   VkQueue queue = VK_NULL_HANDLE;
   vkGetDeviceQueue(device, 0, 0, &queue);

   /* The hazard: the two load-op clears realize on the host, the two
    * carriers fill, and one live DRM_RADEON_CS reaches the command
    * processor here, with the bounded completion wait after it.
    */
   stage("submit");
   result = vkQueueSubmit(queue, 1,
                          &(VkSubmitInfo){
                             .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd,
                          },
                          VK_NULL_HANDLE);
   printf("[submit] vkQueueSubmit returned %d\n", result);
   fflush(stdout);

   if (r3v_native_watchdog_guard_close(&guard, result) != 0)
      return 1;

   if (result != VK_SUCCESS) {
      fprintf(stderr, "submission refused or failed: %d\n", result);
      return 1;
   }

   stage("readback");
   void *first_map = NULL;
   void *second_map = NULL;
   CHECK(vkMapMemory(device, target[0].memory, 0, VK_WHOLE_SIZE, 0,
                     &first_map));
   CHECK(vkMapMemory(device, target[1].memory, 0, VK_WHOLE_SIZE, 0,
                     &second_map));
   if (r3v_native_evidence_write_file(evidence_dir, "first_target.bin",
                                      first_map, color_bytes) != 0 ||
       r3v_native_evidence_write_file(evidence_dir, "second_target.bin",
                                      second_map, color_bytes) != 0) {
      fprintf(stderr, "target retention failed\n");
      return 1;
   }

   /* Each target under its own pass's provoking color, then under each
    * of that pass's two non-provoking input colors, one of which is the
    * other pass's provoking color.  A judged interior pixel holding a
    * non-provoking input names another vertex selected, an interpolated
    * varying, or state crossed from the other pass; the own-color
    * verdict's mismatch count catches every dword outside the three.
    */
   struct r300_triangle_coverage_verdict own[2];
   struct r300_triangle_coverage_verdict falsifier[2][2];
   const uint32_t *maps[2] = { first_map, second_map };
   uint32_t falsifier_interior[2] = { 0, 0 };
   for (unsigned p = 0; p < 2; p++) {
      const uint32_t provoking = vertex_dword[pass_order[p][0]];
      r300_tcl_bypass_triangle_coverage_oracle(
         &mp.pass[p], &provoking, 1, R300_TRIANGLE_COLOR_SENTINEL, maps[p],
         color_bytes, &own[p]);
      char label[64];
      snprintf(label, sizeof(label), "%s-under-own-provoking-0x%08x",
               p == 0 ? "first" : "second", provoking);
      report(label, &own[p]);
      for (unsigned f = 0; f < 2; f++) {
         const uint32_t other = vertex_dword[pass_order[p][1 + f]];
         r300_tcl_bypass_triangle_coverage_oracle(
            &mp.pass[p], &other, 1, R300_TRIANGLE_COLOR_SENTINEL, maps[p],
            color_bytes, &falsifier[p][f]);
         snprintf(label, sizeof(label), "%s-under-non-provoking-0x%08x",
                  p == 0 ? "first" : "second", other);
         report(label, &falsifier[p][f]);
         falsifier_interior[p] += falsifier[p][f].interior_pixels;
      }
   }

   /* The witness that the hardware selected: the carriers the device
    * fetched still hold three distinct records per pass with record 0
    * the provoking tint, so host replication did not run.  Direct
    * selection and replication produce byte-identical targets by
    * construction; this read is the one observation that separates
    * them.  The carrier is host memory the device only reads, so the
    * bytes are the host's own.
    */
   bool carrier_witness = true;
   {
      VK_FROM_HANDLE(r3v_native_device, native_device, device);
      for (unsigned p = 0; p < 2; p++) {
         struct r3v_native_memory *carrier = native->owned_carriers[p];
         void *carrier_map = NULL;
         if (carrier == NULL ||
             radeon_drm_vk_bo_map(&native_device->drm, &carrier->bo,
                                  &carrier_map) != 0) {
            printf("[witness] pass %u carrier unreadable\n", p);
            carrier_witness = false;
            continue;
         }
         const float *records = carrier_map;
         bool distinct = true;
         for (unsigned a = 0; a < 3; a++)
            for (unsigned b = a + 1; b < 3; b++)
               distinct &= memcmp(&records[a * 8 + 4], &records[b * 8 + 4],
                                  4 * sizeof(float)) != 0;
         float provoking_tint[4];
         flat_tint(&ndc_triangle[pass_order[p][0] * 4], provoking_tint);
         const bool first_is_provoking =
            memcmp(&records[4], provoking_tint, sizeof(provoking_tint)) == 0;
         printf("[witness] pass %u carrier records distinct=%d "
                "record0_is_provoking=%d\n",
                p, distinct, first_is_provoking);
         carrier_witness &= distinct && first_is_provoking;
         radeon_drm_vk_bo_unmap(&native_device->drm, &carrier->bo,
                                carrier_map);
      }
      fflush(stdout);
   }

   /* Byte equality with the retained expected targets, the receipt's
    * oracle; and the alpha discriminator: interior pixels whose RGB is
    * the provoking one under another alpha.
    */
   int differing[2] = { 0, 0 };
   uint32_t alpha_deviates[2] = { 0, 0 };
   for (unsigned p = 0; p < 2; p++) {
      bool judged = false;
      differing[p] = r300_tcl_bypass_triangle_target_compare(
         &mp.pass[p], expected[p], maps[p], color_bytes, &judged);
      const uint32_t provoking = vertex_dword[pass_order[p][0]];
      const uint32_t *rows = maps[p];
      for (uint32_t y = 0; y < mp.pass[p].height; y++)
         for (uint32_t x = 0; x < mp.pass[p].width; x++) {
            const uint32_t px = rows[y * mp.pass[p].pitch_pixels + x];
            if (expected[p][y * mp.pass[p].pitch_pixels + x] == provoking &&
                px != provoking &&
                (px & 0x00ffffffu) == (provoking & 0x00ffffffu))
               alpha_deviates[p]++;
         }
      printf("[oracle] %s-vs-expected judged=%d differing=%d "
             "alpha_deviates=%u\n",
             p == 0 ? "first" : "second", judged, differing[p],
             alpha_deviates[p]);
   }
   fflush(stdout);

   const uint32_t *second_pixels = second_map;
   const uint32_t cx = mp.pass[1].width / 2;
   const uint32_t cy = (mp.pass[1].height * 3) / 8;
   printf("[oracle] second centroid (%u,%u)=0x%08x predicted 0x%08x "
          "corner (0,0)=0x%08x\n",
          cx, cy, second_pixels[cy * mp.pass[1].pitch_pixels + cx],
          second_dword, second_pixels[0]);
   fflush(stdout);

   const bool second_unreached =
      own[1].judged && own[1].analytic_pixels != 0 &&
      own[1].interior_pixels == 0 &&
      own[1].exterior_pixels == mp.pass[1].width * mp.pass[1].height;
   bool falsifiers_judged = true;
   bool crossed = false;
   for (unsigned p = 0; p < 2; p++)
      for (unsigned f = 0; f < 2; f++) {
         falsifiers_judged &= falsifier[p][f].judged;
         crossed |= falsifier[p][f].judged && falsifier[p][f].coverage_exact;
      }
   const bool other_vertex_present =
      falsifier_interior[0] != 0 || falsifier_interior[1] != 0;
   const bool own_exact = own[0].judged && own[0].coverage_exact &&
                          own[0].canary_pass && own[1].judged &&
                          own[1].coverage_exact && own[1].canary_pass;
   const bool bytes_equal = differing[0] == 0 && differing[1] == 0;
   const bool alpha_interpolated =
      alpha_deviates[0] != 0 || alpha_deviates[1] != 0;
   printf("[classify] %s\n",
          !carrier_witness
             ? "the carriers do not hold three distinct records with the "
               "provoking first; the hardware route was not exercised"
          : crossed ? "a target is exact under a non-provoking input; the "
                    "GA selected another vertex or state crossed the "
                    "pass boundary"
          : other_vertex_present
             ? "a non-provoking input color appears inside a target; the "
               "GA selected another vertex or the varying interpolated"
          : alpha_interpolated
             ? "the provoking RGB appears under another alpha; alpha "
               "interpolated while RGB stayed flat"
          : second_unreached
             ? "the second target holds the sentinel; the command "
               "processor never reached the second cell"
          : own_exact && bytes_equal
             ? "each target is byte-equal to the replication oracle's "
               "expected image: its own pass's first-vertex RGBA over the "
               "analytic triangle and no other value"
             : "prediction deviated; the deviation is the finding");
   fflush(stdout);

   const bool pass_verdict = own_exact && bytes_equal && falsifiers_judged &&
                             carrier_witness && !crossed &&
                             !other_vertex_present && !alpha_interpolated;
   free(expected[0]);
   free(expected[1]);

   stage("teardown");
   vkUnmapMemory(device, target[0].memory);
   vkUnmapMemory(device, target[1].memory);
   vkDestroyCommandPool(device, pool, NULL);
   for (unsigned i = 0; i < 2; i++) {
      if (i == 0)
         vkDestroyPipeline(device, pipeline[i], NULL);
      vkDestroyFramebuffer(device, target[i].framebuffer, NULL);
      vkDestroyImageView(device, target[i].view, NULL);
      vkDestroyImage(device, target[i].image, NULL);
      vkFreeMemory(device, target[i].memory, NULL);
   }
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyRenderPass(device, pass, NULL);
   vkDestroyBuffer(device, vertex_buffer, NULL);
   vkFreeMemory(device, vertex_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);

   printf("[verdict] %s\n",
          pass_verdict
             ? "end-to-end Vulkan Flat (RGBA) delivered through RS485M GA "
               "provoking-vertex selection over color 0: each target is "
               "byte-equal to the CPU replication oracle's expected image"
          : crossed || other_vertex_present || alpha_interpolated
             ? "the GA selected, interpolated, or crossed a "
               "non-provoking value; the route is classified, not "
               "adjusted"
          : second_unreached
             ? "the second cell was not reached; the finding is the "
               "concatenation"
             : "prediction deviated; the deviation is the finding");
   return pass_verdict ? 0 : 1;
}
