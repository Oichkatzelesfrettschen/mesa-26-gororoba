/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the producer-plus-re-ingest cell:
 * builds the exact concatenated stream an attended run would submit,
 * reports its digest and every arming factor, and stops at the
 * authorization boundary.  The runner performs no ioctl and creates no
 * Vulkan device, so running it is safe on the target host.  Each cell's
 * digest authorizes only its own stream.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_reingest_pass.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/mesa-blake3.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Builds the reference re-ingest stream and returns its IB digest, the
 * content an authorization declares through
 * R3V_NATIVE_AUTHORIZED_IB_BLAKE3.
 */
static int
cell_digest(char out[BLAKE3_OUT_LEN * 2 + 1], uint32_t *ib_dwords)
{
   struct r300_r2vb_reingest_ib cell;
   if (r300_r2vb_reingest_pass_emit(&cell) != 0)
      return 1;

   r300_triangle_ib_digest_hex(cell.ib, cell.ib_size_dwords, out);
   *ib_dwords = cell.ib_size_dwords;
   r300_r2vb_reingest_pass_release(&cell);
   return 0;
}

/* PCI identity overrides are operator declarations, not convenience
 * formatting.  Parse the complete token before the uint32_t assignment so
 * trailing bytes, signs, whitespace, and overflow remain refusals instead
 * of changing the identity that reaches the gate.
 */
static bool
parse_pci_id(const char *text, uint32_t *value)
{
   if (text == NULL || text[0] == '\0' || text[0] == '+' ||
       text[0] == '-' || isspace((unsigned char)text[0]))
      return false;

   errno = 0;
   char *end = NULL;
   const unsigned long parsed = strtoul(text, &end, 0);
   if (errno == ERANGE || end == text || *end != '\0' ||
       parsed > UINT32_MAX)
      return false;

   *value = (uint32_t)parsed;
   return true;
}

struct fixture_provider {
   const char *evidence_dir;
   const char *digest;
};

static const char *
fixture_read_env(void *ctx, const char *name)
{
   const struct fixture_provider *fixture = ctx;
   if (strcmp(name, "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED") == 0)
      return "1";
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_IB_BLAKE3") == 0)
      return fixture->digest;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE") == 0)
      return "r3v-reingest-arming-fixture-kernel";
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION") == 0)
      return "R3V_REINGEST_ARMING_FIXTURE_MODULE";
   return NULL;
}

static void
fixture_read_kernel_release(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", "r3v-reingest-arming-fixture-kernel");
}

static void
fixture_read_module_srcversion(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", "R3V_REINGEST_ARMING_FIXTURE_MODULE");
}

static bool
fixture_directory_present(void *ctx, const char *path)
{
   const struct fixture_provider *fixture = ctx;
   struct stat status;
   return strcmp(path, fixture->evidence_dir) == 0 &&
          stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool
fixture_file_present(void *ctx, const char *path)
{
   const struct fixture_provider *fixture = ctx;
   char token_path[R3V_NATIVE_ARMING_PATH_MAX];
   const int length = snprintf(token_path, sizeof(token_path), "%s/%s",
                               fixture->evidence_dir, "attempt.token");
   struct stat status;
   return length >= 0 && (size_t)length < sizeof(token_path) &&
          strcmp(path, token_path) == 0 && stat(path, &status) == 0;
}

static void
collect_facts(const char *evidence_dir, const char *digest, bool fixture,
              uint32_t vendor_id, uint32_t device_id,
              struct r3v_native_arming_facts *facts, char *kernel,
              size_t kernel_size, char *module, size_t module_size)
{
   if (!fixture) {
      r3v_native_arming_collect(facts, vendor_id, device_id,
                                R3V_NATIVE_CELL_KIND_R2VB_REINGEST, digest,
                                evidence_dir, kernel, kernel_size, module,
                                module_size);
      return;
   }

   struct fixture_provider fixture_provider = {
      .evidence_dir = evidence_dir,
      .digest = digest,
   };
   const struct r3v_native_arming_provider provider = {
      .read_env = fixture_read_env,
      .read_kernel_release = fixture_read_kernel_release,
      .read_module_srcversion = fixture_read_module_srcversion,
      .directory_present = fixture_directory_present,
      .file_present = fixture_file_present,
      .ctx = &fixture_provider,
   };
   r3v_native_arming_collect_from(
      &provider, facts, vendor_id, device_id,
      R3V_NATIVE_CELL_KIND_R2VB_REINGEST, digest, evidence_dir, kernel,
      kernel_size, module, module_size);
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
    * its freshness is itself an arming factor.  The optional fixture
    * provider supplies a deterministic positive calibration without
    * opening a device or consulting host deployment state.
    */
   if (argc != 2 && argc != 3) {
      fprintf(stderr, "usage: %s <evidence-directory> [--fixture]\n",
              argv[0]);
      return 2;
   }
   const char *evidence_dir = argv[1];
   bool fixture = false;
   if (argc == 3) {
      if (strcmp(argv[2], "--fixture") != 0) {
         fprintf(stderr, "unknown runner option %s\n", argv[2]);
         return 2;
      }
      fixture = true;
   }

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
   uint32_t vendor_id = R3V_NATIVE_ARMING_PCI_VENDOR;
   uint32_t device_id = R3V_NATIVE_ARMING_PCI_DEVICE;
   if (vendor_env != NULL && !parse_pci_id(vendor_env, &vendor_id)) {
      fprintf(stderr, "invalid R3V_NATIVE_RUNNER_PCI_VENDOR override\n");
      return 2;
   }
   if (device_env != NULL && !parse_pci_id(device_env, &device_id)) {
      fprintf(stderr, "invalid R3V_NATIVE_RUNNER_PCI_DEVICE override\n");
      return 2;
   }

   char kernel[128];
   char module[128];
   struct r3v_native_arming_facts facts;
   collect_facts(evidence_dir, digest, fixture, vendor_id, device_id, &facts,
                 kernel, sizeof(kernel), module, sizeof(module));

   printf("r3v native r2vb-reingest arming report\n");
   printf("cell_kind=r2vb-reingest\n");
   printf("provider=%s\n", fixture ? "fixture" : "host");
   printf("ib_dwords=%u\n", ib_dwords);
   printf("ib_blake3=%s\n", digest);
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
