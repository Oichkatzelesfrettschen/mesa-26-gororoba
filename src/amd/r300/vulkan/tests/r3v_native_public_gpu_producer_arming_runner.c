/*
 * SPDX-License-Identifier: MIT
 *
 * Non-submitting arming runner for the public GPU-producer route:
 * composes the exact stream an attended run submits -- the producer
 * pass over the application's records ahead of the consumer cell --
 * reports its digest and every arming factor, and stops at the
 * authorization boundary.  The runner performs no ioctl and creates no
 * Vulkan device, so running it is safe on the target host.  Each cell's
 * digest authorizes only its own stream.
 */

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_r2vb_public_route.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/mesa-blake3.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Composes the route stream and returns its IB digest, the content an
 * authorization declares through R3V_NATIVE_AUTHORIZED_IB_BLAKE3.  The
 * composition takes the fixed triangle's own vertex records, which is
 * what the attended runner writes into the vertex buffer, so the
 * producer half the driver emits from the gathered records is these
 * bytes and the consumer half is the recorded cell.
 */
static int
route_digest(char out[BLAKE3_OUT_LEN * 2 + 1], uint32_t *ib_dwords,
             uint32_t *consumer_start_dwords)
{
   struct r300_r2vb_public_route_ib route;
   if (r300_r2vb_public_route_reference_compose(&route) != 0)
      return 1;
   if (r300_r2vb_public_route_validate_reloc_sites(&route) != 0) {
      r300_r2vb_public_route_release(&route);
      return 1;
   }

   r300_triangle_ib_digest_hex(route.ib, route.ib_size_dwords, out);
   *ib_dwords = route.ib_size_dwords;
   *consumer_start_dwords = route.consumer_start_dwords;
   r300_r2vb_public_route_release(&route);
   return 0;
}

/* The fixture supplies an entirely local, declared arming environment for
 * the positive calibration.  Production collection remains host-backed and
 * therefore reads the running kernel and radeon module identity.
 */
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
      return "r3v-public-producer-arming-fixture-kernel";
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION") == 0)
      return "R3V_PUBLIC_PRODUCER_ARMING_FIXTURE_MODULE";
   return NULL;
}

static void
fixture_read_kernel_release(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", "r3v-public-producer-arming-fixture-kernel");
}

static void
fixture_read_module_srcversion(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", "R3V_PUBLIC_PRODUCER_ARMING_FIXTURE_MODULE");
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
      r3v_native_arming_collect(
         facts, R3V_NATIVE_ARMING_PLATFORM, vendor_id, device_id,
         R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC, digest, evidence_dir,
         kernel, kernel_size, module, module_size);
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
      &provider, facts, R3V_NATIVE_ARMING_PLATFORM, vendor_id, device_id,
      R3V_NATIVE_CELL_KIND_R2VB_GPU_PRODUCER_PUBLIC, digest, evidence_dir,
      kernel, kernel_size, module, module_size);
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
   if (argc != 2 && argc != 3) {
      fprintf(stderr, "usage: %s <evidence-directory> [--fixture]\n", argv[0]);
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
   uint32_t consumer_start_dwords = 0;
   if (route_digest(digest, &ib_dwords, &consumer_start_dwords) != 0) {
      fprintf(stderr, "route composition failed\n");
      return 2;
   }

   /* The board identity an attended run would resolve is supplied
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
   collect_facts(evidence_dir, digest, fixture, vendor_id, device_id, &facts,
                 kernel, sizeof(kernel), module, sizeof(module));

   printf("r3v native r2vb-gpu-producer-public arming report\n");
   printf("cell_kind=r2vb-gpu-producer-public\n");
   printf("provider=%s\n", fixture ? "fixture" : "host");
   printf("ib_dwords=%u\n", ib_dwords);
   printf("consumer_start_dwords=%u\n", consumer_start_dwords);
   printf("ib_blake3=%s\n", digest);
   /* The route resolves only under both delivery gates, so the report
    * carries them beside the hazard gate: a run armed on everything
    * else still takes the CPU route with either gate unset.
    */
   const char *delivery_gate = getenv("R3V_NATIVE_R2VB_DELIVERY_EXPERIMENTAL");
   const char *gpu_gate = getenv("R3V_NATIVE_R2VB_GPU_DELIVERY_EXPERIMENTAL");
   printf("  %-22s declared=%-34s observed=%-34s %s\n", "hazard gate",
          facts.hazard_gate != NULL ? facts.hazard_gate : "(unset)", "1",
          facts.hazard_gate != NULL && strcmp(facts.hazard_gate, "1") == 0
             ? "match"
             : "CLOSED");
   report("r2vb delivery gate", delivery_gate, "1");
   report("r2vb gpu gate", gpu_gate, "1");
   report("bundle digest", facts.authorized_ib_blake3,
          facts.actual_ib_blake3);
   printf("  %-22s declared=0x%04x:0x%04x%-22s observed=0x%04x:0x%04x%-20s "
          "%s\n",
          "board identity", R3V_NATIVE_ARMING_PCI_VENDOR,
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
   const bool gates_open =
      delivery_gate != NULL && strcmp(delivery_gate, "1") == 0 &&
      gpu_gate != NULL && strcmp(gpu_gate, "1") == 0;
   printf("route: %s\n",
          gates_open ? "gpu-producer (both delivery gates open)"
                     : "cpu (a delivery gate is unset; this digest names "
                       "the composed stream)");
   printf("no submission attempted: this runner stops at the "
          "authorization boundary\n");
   return verdict == R3V_NATIVE_ARMING_ARMED && gates_open ? 0 : 1;
}
