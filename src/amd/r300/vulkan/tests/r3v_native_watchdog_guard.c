/* SPDX-License-Identifier: MIT */

#include "r3v_native_watchdog_guard.h"

#include "r3v_native.h"

#include "util/mesa-blake3.h"

#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int
op_watchdog_bracket(void *ctx, enum r3v_native_submission_trace_event event)
{
   struct r3v_native_watchdog_guard *guard = ctx;
   switch (event) {
   case R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER:
      return r3v_native_watchdog_client_arm(&guard->client) == 0 ? 0 : -EIO;
   case R3V_NATIVE_SUBMISSION_TRACE_COMPLETION_WAIT_RETURN:
      return r3v_native_watchdog_client_disarm(&guard->client) == 0 ? 0 : -EIO;
   default:
      return 0;
   }
}

static int
read_first_line(const char *path, char *out, size_t size)
{
   FILE *file = fopen(path, "r");
   if (file == NULL)
      return -1;
   char *line = fgets(out, (int)size, file);
   fclose(file);
   if (line == NULL)
      return -1;
   out[strcspn(out, "\r\n")] = '\0';
   return 0;
}

/* The waiver binds to the runner that will submit, so the identity is
 * the executing image's bytes rather than its path.
 */
static int
running_image_digest(char *out)
{
   FILE *image = fopen("/proc/self/exe", "rb");
   if (image == NULL)
      return -1;
   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   unsigned char block[65536];
   size_t read;
   while ((read = fread(block, 1, sizeof(block), image)) > 0)
      _mesa_blake3_update(&ctx, block, read);
   const int failed = ferror(image);
   fclose(image);
   if (failed)
      return -1;
   blake3_hash hash;
   _mesa_blake3_final(&ctx, hash);
   _mesa_blake3_format(out, hash);
   return 0;
}

/* An exported variable outlives the decision it recorded, so the waiver
 * of automatic recovery is a document naming this boot, this evidence
 * directory, this command stream, this runner image, when the operator
 * decided, and why.
 */
static int
waiver_admits(struct r3v_native_watchdog_guard *guard,
              const char *waiver_path, const char *evidence_dir,
              const char *ib_digest)
{
   if (ib_digest == NULL) {
      fprintf(stderr,
              "the watchdog bracket refused and this runner names no cell "
              "digest, so no waiver can bind to it\n");
      return -1;
   }

   char boot_id[64];
   char runner_digest[BLAKE3_OUT_LEN * 2 + 1];
   if (read_first_line("/proc/sys/kernel/random/boot_id", boot_id,
                       sizeof(boot_id)) != 0 ||
       running_image_digest(runner_digest) != 0) {
      fprintf(stderr,
              "the run could not read its own boot and image identity\n");
      return -1;
   }

   char attempt[PATH_MAX];
   snprintf(attempt, sizeof(attempt), "%s", evidence_dir);
   const char *attempt_id = basename(attempt);

   /* The bindings the waiver must carry, so the operator writes what
    * this run is rather than transcribing it from elsewhere.
    */
   if (waiver_path == NULL) {
      fprintf(stderr,
              "the watchdog bracket refused and no waiver was named; set "
              "R3V_NATIVE_WATCHDOG_BRACKET_COMMAND to the helper, or pass "
              "--waiver <path> holding:\n"
              "boot_id=%s\nattempt_id=%s\nib_blake3=%s\n"
              "runner_blake3=%s\ntimestamp=<YYYY-MM-DDTHH:MM:SSZ>\n"
              "operator_reason=<why automatic recovery is waived>\n",
              boot_id, attempt_id, ib_digest, runner_digest);
      return -1;
   }

   char reason[256];
   if (r3v_native_recovery_waiver_admit(
          waiver_path, boot_id, attempt_id, ib_digest, runner_digest,
          (int64_t)time(NULL), &guard->waiver, reason,
          sizeof(reason)) != 0) {
      fprintf(stderr, "the waiver %s admits nothing: %s\n", waiver_path,
              reason);
      return -1;
   }

   guard->waived = true;
   printf("watchdog.waiver_admitted=true attempt=%s written=%s\n",
          guard->waiver.attempt_id, guard->waiver.timestamp);
   printf("watchdog.waiver_operator_reason=%s\n",
          guard->waiver.operator_reason);
   printf("[watchdog] waived: the operator accepts manual power-cycle "
          "recovery for this submission\n");
   fflush(stdout);
   return 0;
}

int
r3v_native_watchdog_guard_open(struct r3v_native_watchdog_guard *guard,
                               const char *waiver_path,
                               const char *evidence_dir,
                               const char *ib_digest)
{
   memset(guard, 0, sizeof(*guard));
   if (r3v_native_watchdog_client_open(&guard->client) != 0)
      return waiver_admits(guard, waiver_path, evidence_dir, ib_digest);

   fputs(guard->client.facts, stdout);
   if (r3v_native_watchdog_client_calibrate(&guard->client) != 0) {
      fprintf(stderr, "the counter refused its state ladder: %s\n",
              guard->client.calibration);
      r3v_native_watchdog_client_close(&guard->client);
      return -1;
   }
   printf("watchdog.%s\n", guard->client.calibration);
   fflush(stdout);
   guard->present = true;
   return 0;
}

void
r3v_native_watchdog_guard_install(struct r3v_native_watchdog_guard *guard,
                                  VkDevice device)
{
   if (!guard->present)
      return;
   r3v_native_device_from_handle(device)->submission_trace =
      (struct r3v_native_submission_trace){
         .ctx = guard,
         .emit = op_watchdog_bracket,
      };
}

int
r3v_native_watchdog_guard_close(struct r3v_native_watchdog_guard *guard,
                                VkResult submit_result)
{
   if (!guard->present)
      return 0;

   /* The hook disarms at COMPLETION_WAIT_RETURN; this covers the paths
    * that reach no completion wait.
    */
   const int disarm = r3v_native_watchdog_client_disarm(&guard->client);
   printf("watchdog.guarded_interval=DRM_IOCTL_RADEON_CS through fence "
          "completion\n");
   printf("watchdog.armed_before_submit=%s\n",
          guard->client.arm_verified ? "true" : "false");
   printf("watchdog.arm_acknowledgement=%s\n", guard->client.arm_ack);
   printf("watchdog.completion_observed=%s\n",
          submit_result == VK_SUCCESS ? "true" : "false");
   printf("watchdog.disarm_result=%s\n",
          disarm == 0 ? guard->client.disarm_ack : "failed");
   /* The counter ran across this span, so it is what the 1.7 s grace is
    * judged against; the driver's inner transport bracket sits inside it
    * and measures less.
    */
   printf("watchdog.guarded_interval_us=%" PRIu64 "\n",
          (guard->client.disarmed_ns - guard->client.armed_ns) / 1000u);
   fflush(stdout);
   r3v_native_watchdog_client_close(&guard->client);
   if (disarm != 0)
      fprintf(stderr, "the watchdog stayed armed after the interval\n");
   return disarm;
}
