/*
 * SPDX-License-Identifier: MIT
 *
 * Positive ARMED calibration of the native arming gate over a deterministic
 * provider.  The test links the arming translation unit alone: it creates no
 * Vulkan instance, no device, opens no DRM fd, issues no ioctl, and writes
 * no attempt token, so the ARMED verdict here is a property of the decision
 * over exact facts rather than of any machine state.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_native_arming.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The retained cell's pinned digest, standing in for the digest the queue
 * recomputes; any equal pair exercises the comparison, and the pinned value
 * keeps the fixture concrete.
 */
#define FIXTURE_DIGEST \
   "b082188deb8968c53e6e3e59e40448b43ae03cedf8c8ebeb9a06e9df9b81c9a2"
#define FIXTURE_KERNEL "6.16.0-fixture"
#define FIXTURE_SRCVERSION "FIXTURESRCVERSION0000000"
#define FIXTURE_EVIDENCE_DIR "/fixture-evidence"

/* Every fact the deterministic provider serves, one field per refusal leg,
 * so a test mutates the one fact its leg names and leaves the rest exact.
 */
struct fixture {
   const char *hazard_gate;
   const char *authorized_digest;
   const char *authorized_kernel;
   const char *authorized_srcversion;
   const char *running_kernel;
   const char *running_srcversion;
   bool evidence_dir_present;
   bool attempt_token_present;
};

static const struct fixture armed_fixture = {
   .hazard_gate = "1",
   .authorized_digest = FIXTURE_DIGEST,
   .authorized_kernel = FIXTURE_KERNEL,
   .authorized_srcversion = FIXTURE_SRCVERSION,
   .running_kernel = FIXTURE_KERNEL,
   .running_srcversion = FIXTURE_SRCVERSION,
   .evidence_dir_present = true,
   .attempt_token_present = false,
};

static const char *
fixture_read_env(void *ctx, const char *name)
{
   const struct fixture *f = ctx;
   if (strcmp(name, "R3V_NATIVE_SUBMIT_HAZARD_ACCEPTED") == 0)
      return f->hazard_gate;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_IB_BLAKE3") == 0)
      return f->authorized_digest;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_KERNEL_RELEASE") == 0)
      return f->authorized_kernel;
   if (strcmp(name, "R3V_NATIVE_AUTHORIZED_MODULE_SRCVERSION") == 0)
      return f->authorized_srcversion;
   return NULL;
}

static void
fixture_read_kernel_release(void *ctx, char *out, size_t size)
{
   const struct fixture *f = ctx;
   snprintf(out, size, "%s", f->running_kernel != NULL ? f->running_kernel
                                                       : "");
}

static void
fixture_read_module_srcversion(void *ctx, char *out, size_t size)
{
   const struct fixture *f = ctx;
   snprintf(out, size, "%s",
            f->running_srcversion != NULL ? f->running_srcversion : "");
}

static bool
fixture_directory_present(void *ctx, const char *path)
{
   const struct fixture *f = ctx;
   assert(strcmp(path, FIXTURE_EVIDENCE_DIR) == 0);
   return f->evidence_dir_present;
}

static bool
fixture_file_present(void *ctx, const char *path)
{
   const struct fixture *f = ctx;
   assert(strcmp(path, FIXTURE_EVIDENCE_DIR "/attempt.token") == 0);
   return f->attempt_token_present;
}

static enum r3v_native_arming_verdict
evaluate_fixture(const struct fixture *f, uint32_t vendor, uint32_t device,
                 const char *actual_digest)
{
   const struct r3v_native_arming_provider provider = {
      .read_env = fixture_read_env,
      .read_kernel_release = fixture_read_kernel_release,
      .read_module_srcversion = fixture_read_module_srcversion,
      .directory_present = fixture_directory_present,
      .file_present = fixture_file_present,
      .ctx = (void *)f,
   };
   struct r3v_native_arming_facts facts;
   char kernel_storage[128];
   char module_storage[128];
   r3v_native_arming_collect_from(&provider, &facts, vendor, device,
                                  actual_digest, FIXTURE_EVIDENCE_DIR,
                                  kernel_storage, sizeof(kernel_storage),
                                  module_storage, sizeof(module_storage));
   return r3v_native_arming_evaluate(&facts);
}

static enum r3v_native_arming_verdict
evaluate_armed_variant(const struct fixture *f)
{
   return evaluate_fixture(f, R3V_NATIVE_ARMING_PCI_VENDOR,
                           R3V_NATIVE_ARMING_PCI_DEVICE, FIXTURE_DIGEST);
}

int
main(void)
{
   /* The positive verdict: every exact fact at once is ARMED. */
   assert(evaluate_armed_variant(&armed_fixture) == R3V_NATIVE_ARMING_ARMED);

   /* Each refusal leg mutates the one fact it names; the fixture's other
    * facts stay exact, so the named factor is the one that refused.
    */
   struct fixture f;

   f = armed_fixture;
   f.hazard_gate = NULL;
   assert(evaluate_armed_variant(&f) ==
          R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED);
   f.hazard_gate = "0";
   assert(evaluate_armed_variant(&f) ==
          R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED);
   f.hazard_gate = "";
   assert(evaluate_armed_variant(&f) ==
          R3V_NATIVE_ARMING_HAZARD_GATE_CLOSED);

   f = armed_fixture;
   f.authorized_digest = NULL;
   assert(evaluate_armed_variant(&f) ==
          R3V_NATIVE_ARMING_BUNDLE_UNDECLARED);

   f = armed_fixture;
   assert(evaluate_fixture(&f, R3V_NATIVE_ARMING_PCI_VENDOR,
                           R3V_NATIVE_ARMING_PCI_DEVICE,
                           "b082188deb8968c53e6e3e59e40448b43ae03cedf8c8ebeb"
                           "9a06e9df9b81c9a1") ==
          R3V_NATIVE_ARMING_BUNDLE_MISMATCH);

   f = armed_fixture;
   assert(evaluate_fixture(&f, R3V_NATIVE_ARMING_PCI_VENDOR, 0x5854,
                           FIXTURE_DIGEST) ==
          R3V_NATIVE_ARMING_CHIP_MISMATCH);

   f = armed_fixture;
   f.running_kernel = "6.16.0-other";
   assert(evaluate_armed_variant(&f) == R3V_NATIVE_ARMING_KERNEL_MISMATCH);

   f = armed_fixture;
   f.running_srcversion = "OTHERSRCVERSION000000000";
   assert(evaluate_armed_variant(&f) == R3V_NATIVE_ARMING_MODULE_MISMATCH);

   /* A missing module identity remains empty during collection.  The
    * operator cannot turn that absence into an authorized live state by
    * declaring the sentinel string used by older shim fixtures. */
   f = armed_fixture;
   f.authorized_srcversion = "none";
   f.running_srcversion = NULL;
   assert(evaluate_armed_variant(&f) == R3V_NATIVE_ARMING_MODULE_MISMATCH);

   f = armed_fixture;
   f.evidence_dir_present = false;
   assert(evaluate_armed_variant(&f) == R3V_NATIVE_ARMING_EVIDENCE_ABSENT);

   f = armed_fixture;
   f.attempt_token_present = true;
   assert(evaluate_armed_variant(&f) ==
          R3V_NATIVE_ARMING_ALREADY_ATTEMPTED);

   printf("r3v_native_arming_positive: armed on exact facts, refused on "
          "each mutation\n");
   return 0;
}
