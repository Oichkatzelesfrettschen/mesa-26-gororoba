/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the fetched GPU-producer route:
 * composes the exact stream an attended run submits -- the fetched
 * producer over the slot and source arrays ahead of the consumer cell,
 * bound through the role composer -- reports its digest and every
 * arming factor, and stops at the authorization boundary.  The runner
 * performs no ioctl and creates no Vulkan device, so running it is safe
 * on the target host.  Each cell's digest authorizes only its own
 * stream, and each source width is its own cell.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_fetched_producer.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_vertex_format.h"

#include "util/mesa-blake3.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Composes the route stream and returns its IB digest, the content an
 * authorization declares through R3V_NATIVE_AUTHORIZED_IB_BLAKE3.  The
 * composition is the reference fetched route for the width -- one-page
 * source at offset zero with the width's record size as stride, one-page
 * slot BO, the reference consumer -- which is the geometry the attended
 * runner's vertex buffer binds for that width, so the driver's
 * submit-time composition is these bytes.
 */
static int
route_digest(int format_id, char out[BLAKE3_OUT_LEN * 2 + 1],
             uint32_t *ib_dwords, uint32_t *consumer_start_dwords)
{
   struct r300_r2vb_fetched_route_ib route;
   if (r300_r2vb_fetched_route_reference_compose(format_id, &route) != 0)
      return 1;
   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, out);
   *ib_dwords = route.ib_size_dwords;
   *consumer_start_dwords = route.consumer_start_dwords;
   r300_r2vb_fetched_route_release(&route);
   return 0;
}

static void
report(const char *factor, const char *declared, const char *observed)
{
   const char *state =
      declared == NULL || declared[0] == '\0' ? "UNDECLARED"
      : observed != NULL && strcmp(declared, observed) == 0 ? "match"
                                                            : "MISMATCH";
   printf("  %-22s declared=%-34s observed=%-34s %s\n", factor,
          declared != NULL && declared[0] != '\0' ? declared : "(unset)",
          observed != NULL && observed[0] != '\0' ? observed : "(none)",
          state);
}

int
main(int argc, char **argv)
{
   /* The runner takes the evidence directory an attended run would use
    * -- its freshness is itself an arming factor -- and the source width
    * of the cell, F32_4 when unnamed.
    */
   if (argc != 2 && argc != 3) {
      fprintf(stderr, "usage: %s <evidence-directory> [f32_4|f32_3|f32_2]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];
   int format_id = R300_VERTEX_FORMAT_F32_4;
   const char *format_name = "F32_4";
   if (argc == 3) {
      if (strcmp(argv[2], "f32_4") == 0) {
         format_id = R300_VERTEX_FORMAT_F32_4;
      } else if (strcmp(argv[2], "f32_3") == 0) {
         format_id = R300_VERTEX_FORMAT_F32_3;
         format_name = "F32_3";
      } else if (strcmp(argv[2], "f32_2") == 0) {
         format_id = R300_VERTEX_FORMAT_F32_2;
         format_name = "F32_2";
      } else {
         fprintf(stderr, "source width %s is outside f32_4, f32_3, f32_2\n",
                 argv[2]);
         return 2;
      }
   }

   char digest[BLAKE3_OUT_LEN * 2 + 1];
   uint32_t ib_dwords = 0;
   uint32_t consumer_start_dwords = 0;
   if (route_digest(format_id, digest, &ib_dwords, &consumer_start_dwords) !=
       0) {
      fprintf(stderr, "route composition failed\n");
      return 2;
   }

   /* The chip identity an attended run would enumerate is supplied
    * rather than probed, so the runner opens no device node.
    */
   const char *vendor_env = getenv("R3V_NATIVE_RUNNER_PCI_VENDOR");
   const char *device_env = getenv("R3V_NATIVE_RUNNER_PCI_DEVICE");
   uint32_t vendor_id = vendor_env != NULL
                           ? (uint32_t)strtoul(vendor_env, NULL, 0)
                           : R3V_NATIVE_ARMING_PCI_VENDOR;
   uint32_t device_id = device_env != NULL
                           ? (uint32_t)strtoul(device_env, NULL, 0)
                           : R3V_NATIVE_ARMING_PCI_DEVICE;

   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   r3v_native_arming_collect(
      &facts, vendor_id, device_id,
      R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_FETCHED, digest, evidence_dir,
      kernel, sizeof(kernel), module, sizeof(module));

   printf("r3v native r2vb-gpu-producer-fetched arming report\n");
   printf("cell_kind=r2vb-gpu-producer-fetched\n");
   printf("source_format=%s\n", format_name);
   printf("ib_dwords=%u\n", ib_dwords);
   printf("consumer_start_dwords=%u\n", consumer_start_dwords);
   printf("ib_blake3=%s\n", digest);
   /* The route resolves only under all three delivery gates, so the
    * report carries them beside the hazard gate: a run armed on
    * everything else takes the CPU route with a producer gate unset and
    * the immediate route with the fetched gate unset.
    */
   const char *delivery_gate = getenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
   const char *gpu_gate = getenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
   const char *fetched_gate =
      getenv("R3V_NATIVE_R2VB_FETCHED_PRODUCER_EXPERIMENTAL");
   printf("  %-22s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
   report("r2vb delivery gate", delivery_gate, "1");
   report("r2vb gpu gate", gpu_gate, "1");
   report("r2vb fetched gate", fetched_gate, "1");
   report("bundle digest", facts.authorized_ib_blake3,
          facts.actual_ib_blake3);
   printf("  %-22s declared=0x%04x:0x%04x%-22s observed=0x%04x:0x%04x%-20s "
          "%s\n",
          "chip identity", R3V_NATIVE_ARMING_PCI_VENDOR,
          R3V_NATIVE_ARMING_PCI_DEVICE, "", vendor_id, device_id, "",
          vendor_id == R3V_NATIVE_ARMING_PCI_VENDOR &&
                device_id == R3V_NATIVE_ARMING_PCI_DEVICE
             ? "match"
             : "MISMATCH");
   report("kernel release", facts.authorized_kernel_release,
          facts.running_kernel_release);
   report("module srcversion", facts.authorized_module_srcversion,
          facts.running_module_srcversion);
   printf("  %-22s %s\n", "evidence directory",
          facts.evidence_dir_present ? "present" : "ABSENT");
   printf("  %-22s %s\n", "one-shot token",
          facts.attempt_token_present ? "PRESENT (already attempted)"
                                      : "absent");

   enum r3v_native_arming_verdict verdict =
      r3v_native_arming_evaluate(&facts);
   printf("verdict: %s\n", r3v_native_arming_verdict_name(verdict));
   /* The delivery gates sit outside the arming conjunction: the gate
    * decides whether the submission happens and the gates decide which
    * stream it carries.  A run armed on every factor with a gate unset
    * submits the consumer alone under an authorization naming the
    * composed stream, so the route stands beside the verdict and the
    * runner withholds the zero exit.
    */
   const bool producer_gates_open =
      delivery_gate != NULL && strcmp(delivery_gate, "1") == 0 &&
      gpu_gate != NULL && strcmp(gpu_gate, "1") == 0;
   const bool gates_open = producer_gates_open && fetched_gate != NULL &&
                           strcmp(fetched_gate, "1") == 0;
   printf("route: %s\n",
          gates_open ? "gpu-producer-fetched (all three delivery gates open)"
          : producer_gates_open
             ? "gpu-producer (the fetched gate is unset; this digest "
               "names the fetched composition)"
             : "cpu (a delivery gate is unset; this digest names the "
               "fetched composition)");
   printf("no submission attempted: this runner stops at the "
          "authorization boundary\n");
   return verdict == R3V_NATIVE_ARMING_ARMED && gates_open ? 0 : 1;
}
