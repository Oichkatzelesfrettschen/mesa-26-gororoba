/*
 * SPDX-License-Identifier: MIT
 *
 * Calibrates the native queue status boundary independently of Vulkan
 * transport.  The accepted-completion case is known-good; rejected-ioctl and
 * accepted-but-unretired cases are known-bad controls for the classifier.
 */

#undef NDEBUG

#include "r3v_native.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
   assert(r3v_native_queue_status_from_transport(true, true) ==
          R3V_NATIVE_QUEUE_STATUS_COMPLETED);
   assert(r3v_native_queue_status_from_transport(false, false) ==
          R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED);
   assert(r3v_native_queue_status_from_transport(true, false) ==
          R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE);
   assert(strcmp(r3v_native_queue_status_name(
                   R3V_NATIVE_QUEUE_STATUS_COMPLETION_FAILURE),
                 "COMPLETION_FAILURE") == 0);
   assert(strcmp(r3v_native_queue_status_name(
                   R3V_NATIVE_QUEUE_STATUS_SUBMISSION_REFUSED),
                 "SUBMISSION_REFUSED") == 0);
   puts("r3v_native_queue_status_test: known-good and known-bad cases pass");
   return 0;
}
