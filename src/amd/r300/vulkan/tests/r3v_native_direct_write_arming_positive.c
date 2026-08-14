/*
 * SPDX-License-Identifier: MIT
 *
 * Positive ARMED calibration of the arming gate under the direct-write
 * control's own digest, over a deterministic provider.  The digest is
 * computed from a live r300_direct_write_emit in this process, so the
 * armed verdict binds to the exact stream bytes the recorder installs.
 * The test links the arming and emitter translation units alone: no
 * Vulkan instance, device, DRM fd, ioctl, or attempt token exists in
 * the run.
 */

/* The asserts carry this test's verdicts, so they stay live under NDEBUG. */
#undef NDEBUG

#include "r3v_native_arming.h"

#include "amd/r300/common/r300_direct_write.h"
#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include "util/mesa-blake3.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_KERNEL "6.16.0-fixture"
#define FIXTURE_SRCVERSION "FIXTURESRCVERSION0000000"
#define FIXTURE_EVIDENCE_DIR "/fixture-evidence"

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
   snprintf(out, size, "%s",
            f->running_kernel != NULL ? f->running_kernel : "");
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
evaluate_fixture(const struct fixture *f, const char *actual_digest)
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
   r3v_native_arming_collect_from(&provider, &facts,
                                  R3V_NATIVE_ARMING_PCI_VENDOR,
                                  R3V_NATIVE_ARMING_PCI_DEVICE,
                                  R3V_NATIVE_CELL_KIND_DIRECT_WRITE,
                                  actual_digest, FIXTURE_EVIDENCE_DIR,
                                  kernel_storage, sizeof(kernel_storage),
                                  module_storage, sizeof(module_storage));
   return r3v_native_arming_evaluate(&facts);
}

int
main(void)
{
   /* Both cells' live digests, from the same emissions the recorders
    * install; the direct-write digest is the authorized value under test
    * and the triangle digest is the wrong-cell negative.
    */
   struct r300_direct_write_ib control;
   assert(r300_direct_write_emit(&control) == 0);
   char control_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(control.ib, control.ib_size_dwords,
                               control_digest);

   struct r300_tcl_bypass_triangle_ib triangle;
   assert(r300_tcl_bypass_triangle_reference_emit(&triangle) == 0);
   char triangle_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(triangle.ib, triangle.ib_size_dwords,
                               triangle_digest);
   r300_tcl_bypass_triangle_release(&triangle);
   assert(strcmp(control_digest, triangle_digest) != 0);

   const struct fixture armed_fixture = {
      .hazard_gate = "1",
      .authorized_digest = control_digest,
      .authorized_kernel = FIXTURE_KERNEL,
      .authorized_srcversion = FIXTURE_SRCVERSION,
      .running_kernel = FIXTURE_KERNEL,
      .running_srcversion = FIXTURE_SRCVERSION,
      .evidence_dir_present = true,
      .attempt_token_present = false,
   };

   /* The positive verdict: every exact fact plus the control's own
    * digest is ARMED.
    */
   assert(evaluate_fixture(&armed_fixture, control_digest) ==
          R3V_NATIVE_ARMING_ARMED);

   struct fixture f;

   /* The triangle digest names a different stream, so declaring it
    * against the control refuses; neither cell's authorization carries
    * to the other.
    */
   f = armed_fixture;
   f.authorized_digest = triangle_digest;
   assert(evaluate_fixture(&f, control_digest) ==
          R3V_NATIVE_ARMING_BUNDLE_MISMATCH);

   /* One mutated stream dword moves the digest, so the declared value
    * stops matching the stream about to travel.
    */
   control.ib[control.ib_size_dwords - 1] ^= 1u;
   char mutated_digest[BLAKE3_OUT_LEN * 2 + 1];
   r300_triangle_ib_digest_hex(control.ib, control.ib_size_dwords,
                               mutated_digest);
   control.ib[control.ib_size_dwords - 1] ^= 1u;
   assert(strcmp(mutated_digest, control_digest) != 0);
   f = armed_fixture;
   assert(evaluate_fixture(&f, mutated_digest) ==
          R3V_NATIVE_ARMING_BUNDLE_MISMATCH);

   /* A stale declared digest -- one hex character off the live value --
    * refuses the same way.
    */
   char stale_digest[BLAKE3_OUT_LEN * 2 + 1];
   memcpy(stale_digest, control_digest, sizeof(stale_digest));
   stale_digest[0] = stale_digest[0] == '0' ? '1' : '0';
   f = armed_fixture;
   f.authorized_digest = stale_digest;
   assert(evaluate_fixture(&f, control_digest) ==
          R3V_NATIVE_ARMING_BUNDLE_MISMATCH);

   f = armed_fixture;
   f.evidence_dir_present = false;
   assert(evaluate_fixture(&f, control_digest) ==
          R3V_NATIVE_ARMING_EVIDENCE_ABSENT);

   f = armed_fixture;
   f.attempt_token_present = true;
   assert(evaluate_fixture(&f, control_digest) ==
          R3V_NATIVE_ARMING_ALREADY_ATTEMPTED);

   f = armed_fixture;
   f.running_srcversion = "OTHERSRCVERSION000000000";
   assert(evaluate_fixture(&f, control_digest) ==
          R3V_NATIVE_ARMING_MODULE_MISMATCH);

   r300_direct_write_release(&control);
   printf("r3v_native_direct_write_arming_positive: armed on the "
          "control's own digest, refused on each mutation\n");
   return 0;
}
