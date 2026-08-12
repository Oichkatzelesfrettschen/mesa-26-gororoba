/*
 * SPDX-License-Identifier: MIT
 *
 * Calibrates the native queue status classifier and, when given the native
 * direct-write harness, drives CS refusal, completion-wait failure, and
 * zero-IB ordering through the controlled transport path.
 */

#undef NDEBUG

#include "r3v_native.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int
run_harness_case(const char *harness, const char *mode)
{
   pid_t child = fork();
   if (child == -1)
      return errno;
   if (child == 0) {
      execl(harness, harness, mode, (char *)NULL);
      _exit(127);
   }

   int status = 0;
   while (waitpid(child, &status, 0) == -1) {
      if (errno != EINTR)
         return errno;
   }
   if (!WIFEXITED(status))
      return 128;
   return WEXITSTATUS(status);
}

int
main(int argc, char **argv)
{
   assert(argc == 1 || argc == 2);
   assert(r3v_native_queue_status_from_transport(true, true) ==
          R3V_NATIVE_QUEUE_STATUS_COMPLETED);
   assert(r3v_native_queue_status_from_transport(false, false) ==
          R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
   assert(r3v_native_queue_status_from_transport(true, false) ==
          R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE);
   /* A successful submit containing only zero-IB work has no transport
    * boundary to report.
    */
   assert(r3v_native_queue_status_finalize_submit(
             R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED, false) ==
          R3V_NATIVE_QUEUE_STATUS_NO_SUBMISSION);
   /* A zero-IB buffer before an executable buffer leaves a later refusal
    * classified as a refusal.
    */
   assert(r3v_native_queue_status_finalize_submit(
             R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED, true) ==
          R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
   /* A zero-IB buffer after an accepted completion preserves completion. */
   assert(r3v_native_queue_status_finalize_submit(
             R3V_NATIVE_QUEUE_STATUS_COMPLETED, true) ==
          R3V_NATIVE_QUEUE_STATUS_COMPLETED);
   assert(r3v_native_queue_status_finalize_submit(
             R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE, true) ==
          R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE);
   assert(strcmp(r3v_native_queue_status_name(
                   R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE),
                 "COMPLETION_FAILURE") == 0);
   assert(strcmp(r3v_native_queue_status_name(
                   R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED),
                 "SUBMISSION_REFUSED") == 0);
   if (argc == 2) {
      const char *cases[] = {"reject", "completion-failure", "mixed"};
      for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
         assert(run_harness_case(argv[1], cases[i]) == 0);
   }

   puts("r3v_native_queue_status_test: calibrated cases pass");
   return 0;
}
