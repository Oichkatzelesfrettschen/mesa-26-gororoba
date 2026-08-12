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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --extent selects bounded cell dimensions for extent-specific emission and
 * the resulting IB digest.
 */
static uint32_t cell_width = R300_TRIANGLE_TARGET_WIDTH;
static uint32_t cell_height = R300_TRIANGLE_TARGET_HEIGHT;

/* Builds the cell at the selected extent and returns its IB digest,
 * the content an authorization declares through
 * R3V_NATIVE_AUTHORIZED_IB_BLAKE3.  The recorder's
 * r3v_native_record_tcl_bypass_triangle_carrier() at
 * src/amd/r300/vulkan/r3v_native_cell.c:312 calls
 * r3v_native_cmd_buffer_install_ib() at
 * src/amd/r300/vulkan/r3v_native_cmd.c:53; the queue's
 * r3v_native_queue_submit() at src/amd/r300/vulkan/r3v_native_queue.c:428
 * recomputes r300_triangle_ib_digest_hex() at line 572 from the installed
 * IB.  The emission is the same construction, so the armed digest names
 * the submitted bytes.  Symbol discovery uses
 * (rg --fixed-strings r3v_native_record_tcl_bypass_triangle_carrier
 * src/amd/r300/vulkan/; rg --fixed-strings r3v_native_cmd_buffer_install_ib
 * src/amd/r300/vulkan/; rg --fixed-strings r3v_native_queue_submit
 * src/amd/r300/vulkan/; rg --fixed-strings r300_triangle_ib_digest_hex
 * src/amd/r300/vulkan/).
 */
static int
cell_digest(char out[BLAKE3_OUT_LEN * 2 + 1], uint32_t *ib_dwords)
{
   struct r300_tcl_bypass_triangle_ib cell;
   if (r300_tcl_bypass_triangle_extent_emit(cell_width, cell_height,
                                            &cell) != 0)
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

/* Writes the reference cell's serialized bytes, the independent
 * comparison source for a recorded-IB manifest: the emission is the
 * direct reference construction, so equality with a retained ib.bin
 * proves the recording route reproduced the qualified cell.
 */
static int
emit_reference_ib(const char *path)
{
   struct r300_tcl_bypass_triangle_ib cell;
   if (r300_tcl_bypass_triangle_extent_emit(cell_width, cell_height,
                                            &cell) != 0) {
      fprintf(stderr, "cell construction failed\n");
      return 2;
   }
   int status = 0;
   uint8_t *bytes = malloc((size_t)cell.ib_size_dwords * 4);
   FILE *out = bytes != NULL ? fopen(path, "wb") : NULL;
   if (out == NULL) {
      fprintf(stderr, "emit-ib: cannot write %s\n", path);
      status = 2;
   } else {
      r300_triangle_ib_serialize(cell.ib, cell.ib_size_dwords, bytes);
      const size_t written =
         fwrite(bytes, 1, (size_t)cell.ib_size_dwords * 4, out);
      const int close_error = fclose(out);
      if (written != (size_t)cell.ib_size_dwords * 4 || close_error != 0) {
         fprintf(stderr, "emit-ib: short write to %s\n", path);
         status = 2;
      }
   }
   free(bytes);
   r300_tcl_bypass_triangle_release(&cell);
   return status;
}

int
main(int argc, char **argv)
{
   int argi = 1;
   if (argc >= argi + 3 && strcmp(argv[argi], "--extent") == 0) {
      /* Authorization input parses fail-closed: the value is judged in
       * the unnarrowed type against errno, the end pointer, and the
       * admitted bounds before any assignment, so a declaration
       * congruent to an admitted extent modulo 2^32 refuses instead of
       * authorizing the wrong cell.
       */
      const unsigned long bounds[2] = { R300_TRIANGLE_TARGET_WIDTH,
                                        R300_TRIANGLE_TARGET_HEIGHT };
      unsigned long parsed[2];
      for (int axis = 0; axis < 2; axis++) {
         const char *text = argv[argi + 1 + axis];
         /* C23 and POSIX.1-2024 strtoul(3) accept leading white space
          * and an optional sign, so the decimal token is vetted before
          * the numeric parse.
          */
         bool digits_only = text[0] != '\0';
         for (const char *c = text; *c != '\0'; c++) {
            if (*c < '0' || *c > '9')
               digits_only = false;
         }
         if (!digits_only) {
            fprintf(stderr,
                    "extent outside the admitted 1..%u x 1..%u\n",
                    R300_TRIANGLE_TARGET_WIDTH,
                    R300_TRIANGLE_TARGET_HEIGHT);
            return 2;
         }
         char *end = NULL;
         errno = 0;
         parsed[axis] = strtoul(text, &end, 10);
         if (errno != 0 || end == text || *end != '\0' ||
             parsed[axis] < 1 || parsed[axis] > bounds[axis]) {
            fprintf(stderr,
                    "extent outside the admitted 1..%u x 1..%u\n",
                    R300_TRIANGLE_TARGET_WIDTH,
                    R300_TRIANGLE_TARGET_HEIGHT);
            return 2;
         }
      }
      cell_width = (uint32_t)parsed[0];
      cell_height = (uint32_t)parsed[1];
      argi += 3;
   }

   if (argc == argi + 2 && strcmp(argv[argi], "--emit-ib") == 0)
      return emit_reference_ib(argv[argi + 1]);

   if (cell_width != R300_TRIANGLE_TARGET_WIDTH ||
       cell_height != R300_TRIANGLE_TARGET_HEIGHT) {
      fprintf(stderr,
              "non-maximum extent is available only with --emit-ib; "
              "arming reports use the 64x64 reference cell\n");
      return 2;
   }

   /* The runner takes the evidence directory an attended run would use;
    * its freshness is itself an arming factor.
    */
   if (argc != argi + 1) {
      fprintf(stderr,
              "usage: %s [--extent <w> <h>] <evidence-directory> | "
              "[--extent <w> <h>] --emit-ib <path>\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[argi];

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
