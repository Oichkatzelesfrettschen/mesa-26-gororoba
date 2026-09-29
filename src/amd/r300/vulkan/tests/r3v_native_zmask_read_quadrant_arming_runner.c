/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the ZMASK read-group discovery cell:
 * builds the exact stream an attended run submits, writes it to
 * <evidence-directory>/zmask_read_quadrant.ib for offline replay through
 * the kernel CS parser, reports its digest and every arming factor, and
 * stops at the authorization boundary.  It performs no ioctl and creates
 * no Vulkan device, so running it is safe on the target host.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_tcl_bypass_triangle.h"
#include "amd/r300/common/r300_zmask_read_quadrant_cell.h"
#include "util/mesa-blake3.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int
write_stream(const char *dir, const uint32_t *ib, uint32_t dwords)
{
   char path[4096];
   const int n = snprintf(path, sizeof(path), "%s/zmask_read_quadrant.ib",
                          dir);
   if (n <= 0 || (size_t)n >= sizeof(path))
      return -ENAMETOOLONG;
   FILE *f = fopen(path, "wb");
   if (f == NULL)
      return -errno;
   const size_t written = fwrite(ib, sizeof(*ib), dwords, f);
   const int closed = fclose(f);
   return written == dwords && closed == 0 ? 0 : -EIO;
}

int
main(int argc, char **argv)
{
   if (argc != 2) {
      fprintf(stderr, "usage: %s <evidence-directory>\n", argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];

   const struct r300_zb_depth_surface *surface;
   struct r300_zb_depth_layout layout;
   struct r300_zmask_layout zmask;
   if (r300_zmask_read_quadrant_surface(&surface, &layout, &zmask) != 0) {
      fprintf(stderr, "surface resolves no layout\n");
      return 2;
   }
   struct r300_zmask_read_quadrant_ib cell;
   if (r300_zmask_read_quadrant_reference_emit(&cell) != 0 ||
       r300_zmask_read_quadrant_validate_reloc_sites(&cell) != 0) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }
   const struct r300_zmask_read_quadrant_params declared = {
      .surface = surface,
      .zmask_layout = &zmask,
      .depth_offset_bytes = (uint32_t)layout.base_offset_bytes,
   };
   /* No digest for a stream its own state check refuses. */
   if (r300_zmask_read_quadrant_check_state(&declared, cell.ib,
                                            cell.ib_size_dwords) != 0) {
      fprintf(stderr, "cell refuses its own state check\n");
      r300_zmask_read_quadrant_release(&cell);
      return 2;
   }
   char digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, digest);
   const uint32_t ib_dwords = cell.ib_size_dwords;
   const int stream_rc = write_stream(evidence_dir, cell.ib, ib_dwords);
   r300_zmask_read_quadrant_release(&cell);
   if (stream_rc != 0) {
      fprintf(stderr, "stream retention failed: %s\n", strerror(-stream_rc));
      return 2;
   }

   const char *vendor_env = getenv("R3V_NATIVE_RUNNER_PCI_VENDOR");
   const char *device_env = getenv("R3V_NATIVE_RUNNER_PCI_DEVICE");
   const uint32_t vendor_id = vendor_env != NULL
                                 ? (uint32_t)strtoul(vendor_env, NULL, 0)
                                 : R3V_NATIVE_ARMING_PCI_VENDOR;
   const uint32_t device_id = device_env != NULL
                                 ? (uint32_t)strtoul(device_env, NULL, 0)
                                 : R3V_NATIVE_ARMING_PCI_DEVICE;
   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   r3v_native_arming_collect(&facts, R3V_NATIVE_ARMING_PLATFORM, vendor_id,
                             device_id,
                             R3V_NATIVE_CELL_KIND_ZMASK_READ_QUADRANT, digest,
                             evidence_dir, kernel, sizeof(kernel), module,
                             sizeof(module));

   printf("r3v native zmask-read-quadrant arming report\n");
   printf("cell_kind=zmask-read-quadrant\n");
   printf("ib_dwords=%u\n", ib_dwords);
   printf("ib_blake3=%s\n", digest);
   printf("  %-22s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
   report("bundle digest", facts.authorized_ib_blake3, facts.actual_ib_blake3);
   report("kernel release", facts.authorized_kernel_release,
          facts.running_kernel_release);
   report("module srcversion", facts.authorized_module_srcversion,
          facts.running_module_srcversion);
   printf("  %-22s %s\n", "evidence directory",
          facts.evidence_dir_present ? "present" : "ABSENT");
   printf("  %-22s %s\n", "one-shot token",
          facts.attempt_token_present ? "PRESENT (already attempted)"
                                      : "absent");
   const enum r3v_native_arming_verdict verdict =
      r3v_native_arming_evaluate(&facts);
   printf("verdict: %s\n", r3v_native_arming_verdict_name(verdict));
   printf("no submission attempted: this runner stops at the "
          "authorization boundary\n");
   return verdict == R3V_NATIVE_ARMING_ARMED ? 0 : 1;
}
