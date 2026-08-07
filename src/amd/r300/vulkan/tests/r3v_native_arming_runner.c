/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner: builds the exact cell an attended run
 * would submit, reports its digest and every arming factor, and stops at
 * the authorization boundary.  The runner performs no ioctl and creates
 * no Vulkan device, so running it is safe on the target host.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_fragment_binary.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/mesa-blake3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Builds the fixed cell and returns its IB digest, the content an
 * authorization declares through R3V_NATIVE_AUTHORIZED_IB_BLAKE3.  The
 * reference emission is the same construction the recorder installs and
 * the queue recomputes, so the armed digest names the submitted bytes.
 */
static int
cell_digest(char out[BLAKE3_OUT_LEN * 2 + 1], uint32_t *ib_dwords)
{
   struct r300_tcl_bypass_triangle_ib cell;
   if (r300_tcl_bypass_triangle_reference_emit(&cell) != 0)
      return 1;

   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, out);
   *ib_dwords = cell.ib_size_dwords;
   r300_tcl_bypass_triangle_release(&cell);
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
   /* The runner takes the evidence directory an attended run would use;
    * its freshness is itself an arming factor.
    */
   if (argc != 2) {
      fprintf(stderr, "usage: %s <evidence-directory>\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   char digest[BLAKE3_OUT_LEN * 2 + 1];
   uint32_t ib_dwords = 0;
   if (cell_digest(digest, &ib_dwords) != 0) {
      fprintf(stderr, "cell construction failed\n");
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
   r3v_native_arming_collect(&facts, vendor_id, device_id, digest,
                             evidence_dir, kernel, sizeof(kernel), module,
                             sizeof(module));

   printf("r3v native arming report\n");
   printf("  cell                   %u IB dwords, blake3 %s\n", ib_dwords,
          digest);
   printf("  %-22s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
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
   printf("no submission attempted: this runner stops at the "
          "authorization boundary\n");
   return verdict == R3V_NATIVE_ARMING_ARMED ? 0 : 1;
}
