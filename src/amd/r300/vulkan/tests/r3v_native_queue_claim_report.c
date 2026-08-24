/*
 * SPDX-License-Identifier: MIT
 *
 * Queue-claim report: the queue family flags the ICD advertises in this
 * environment, the claim mode those flags rest on, and the compute verb
 * ledger digest that mode derives from.
 */

#undef NDEBUG

#include "amd/r300/common/r300_compute_verb.h"
#include "util/mesa-blake3.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

/* The report reaches the ICD through the Vulkan loader, so it probes
 * the manifest VK_DRIVER_FILES names -- the one the conformance run
 * pins -- and the claim predicates come from the verb ledger compiled
 * beside this report; the receipt binds the two through the source SHA
 * both were built from.
 */

/* The ledger digest covers every scalar field of every row, the FP24
 * tolerance by its bit pattern, so any ledger edit changes the digest.
 */
static void
ledger_digest(char out[BLAKE3_OUT_LEN * 2 + 1])
{
   uint32_t count = 0;
   const struct r300_compute_verb_row *rows = r300_compute_verb_rows(&count);
   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   for (uint32_t i = 0; i < count; i++) {
      const struct r300_compute_verb_row *r = &rows[i];
      uint32_t tolerance_bits;
      memcpy(&tolerance_bits, &r->tolerance, sizeof(tolerance_bits));
      char line[512];
      int n = snprintf(line, sizeof(line),
                       "%u\t%s\t%u\t%u\t%u\t%u\t%u\t%u\t%08x\t%u\t%u\t%u\t%u\t%s\n",
                       (unsigned)r->verb, r->name, (unsigned)r->operation_id,
                       (unsigned)r->implementation_id,
                       (unsigned)r->gpu_route_contract_id,
                       (unsigned)r->index_class, (unsigned)r->unit,
                       (unsigned)r->exactness, tolerance_bits,
                       (unsigned)r->cpu_route, (unsigned)r->gpu_route,
                       (unsigned)r->evidence, (unsigned)r->evidence_scope,
                       r->gpu_gate);
      assert(n > 0 && (size_t)n < sizeof(line));
      _mesa_blake3_update(&ctx, line, (size_t)n);
   }
   uint8_t digest[BLAKE3_OUT_LEN];
   _mesa_blake3_final(&ctx, digest);
   _mesa_blake3_format(out, digest);
}

int
main(void)
{
   if (getenv("VK_DRIVER_FILES") == NULL) {
      fprintf(stderr, "queue-claim-report: VK_DRIVER_FILES names no ICD\n");
      return 2;
   }
   PFN_vkVoidFunction (*gipa)(VkInstance, const char *) =
      vkGetInstanceProcAddr;
   PFN_vkCreateInstance create_instance =
      (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkInstance instance = VK_NULL_HANDLE;
   if (create_instance(&(VkInstanceCreateInfo){
                          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                          .pApplicationInfo =
                             &(VkApplicationInfo){
                                .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                .apiVersion = VK_API_VERSION_1_0,
                             },
                       },
                       NULL, &instance) != VK_SUCCESS) {
      fprintf(stderr, "queue-claim-report: no instance\n");
      return 2;
   }
   PFN_vkEnumeratePhysicalDevices enumerate =
      (PFN_vkEnumeratePhysicalDevices)gipa(instance,
                                           "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceQueueFamilyProperties families =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(
         instance, "vkGetPhysicalDeviceQueueFamilyProperties");
   PFN_vkDestroyInstance destroy_instance =
      (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
   uint32_t pdev_count = 1;
   VkPhysicalDevice pdev = VK_NULL_HANDLE;
   VkResult enumerated = enumerate(instance, &pdev_count, &pdev);
   if ((enumerated != VK_SUCCESS && enumerated != VK_INCOMPLETE) ||
       pdev_count != 1) {
      fprintf(stderr, "queue-claim-report: no physical device\n");
      destroy_instance(instance, NULL);
      return 2;
   }
   uint32_t family_count = 0;
   families(pdev, &family_count, NULL);
   if (family_count == 0) {
      fprintf(stderr, "queue-claim-report: no queue family\n");
      destroy_instance(instance, NULL);
      return 2;
   }
   VkQueueFamilyProperties *props = calloc(family_count, sizeof(*props));
   assert(props != NULL);
   families(pdev, &family_count, props);

   const char *gate = getenv(R300_COMPUTE_QUEUE_CLAIM_GATE);
   const bool gate_open =
      gate != NULL && strcmp(gate, R300_COMPUTE_QUEUE_CLAIM_GATE_VALUE) == 0;
   const bool conformant = r300_compute_verb_queue_conformant();
   const bool claimed = r300_compute_verb_queue_claim(gate_open);
   /* The mode names what the compute bit rests on: the whole ledger
    * executing, the exact framework gate over the delivered CPU route,
    * or the graphics-only default.
    */
   const char *mode = conformant ? "conformant"
                      : claimed  ? "experimental_compute_subset"
                                 : "default_graphics_only";
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   ledger_digest(digest);
   printf("queue_family_count\t%u\n", family_count);
   for (uint32_t i = 0; i < family_count; i++) {
      printf("queue_flags\t%u\t0x%x\tcount\t%u\ttimestamp_bits\t%u\n", i,
             props[i].queueFlags, props[i].queueCount,
             props[i].timestampValidBits);
   }
   printf("compute_bit\t%s\n",
          (props[0].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "1" : "0");
   printf("queue_claim_mode\t%s\n", mode);
   printf("queue_claim_gate\t%s\n", gate_open ? "1" : "0");
   printf("verb_table_blake3\t%s\n", digest);
   /* The advertised bit and the ledger's claim agree, and the gated
    * mode stands only under the open gate, or the report is itself the
    * finding.
    */
   const bool agrees =
      family_count == 1 &&
      ((props[0].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) == claimed &&
      (strcmp(mode, "experimental_compute_subset") != 0 || gate_open);
   printf("claim_consistent\t%s\n", agrees ? "1" : "0");
   free(props);
   destroy_instance(instance, NULL);
   return agrees ? 0 : 1;
}
