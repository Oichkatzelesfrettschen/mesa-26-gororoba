/* SPDX-License-Identifier: MIT */

#ifndef R3V_NATIVE_WATCHDOG_GUARD_H
#define R3V_NATIVE_WATCHDOG_GUARD_H

#include "r3v_native_recovery_waiver.h"
#include "r3v_native_watchdog_client.h"

#include <vulkan/vulkan.h>

/* The watchdog bracket the attended submit gate requires, shared by
 * every runner that reaches DRM_IOCTL_RADEON_CS.  The SB600 TCO counter
 * runs a fixed ~2.0 s window that WDIOC_KEEPALIVE does not reload, so it
 * covers the ioctl through fence completion and nothing wider: arming at
 * CS_IOCTL_ENTER and disarming at COMPLETION_WAIT_RETURN keeps the
 * deferred copies, the completion allocation, and the attempt.token
 * write outside the window, where a stall would fire the counter on a
 * healthy submission.
 */
struct r3v_native_watchdog_guard {
   struct r3v_native_watchdog_client client;
   bool present;
   bool waived;
   struct r3v_native_recovery_waiver waiver;
};

/* Opens the bracket and walks the counter's state ladder before the run
 * reaches the ioctl, because r3v_native_arming_disarm writes
 * attempt.token ahead of the trace: an arm that first fails inside
 * vkQueueSubmit refuses the submission with the attempt already spent.
 * A bracket the host cannot provide falls to the waiver at waiver_path,
 * which binds to this boot, this evidence directory, this cell, and this
 * runner image; a runner with no cell digest to bind admits no waiver.
 */
int r3v_native_watchdog_guard_open(struct r3v_native_watchdog_guard *guard,
                                   const char *waiver_path,
                                   const char *evidence_dir,
                                   const char *ib_digest);

/* Installs the submission trace hook, so the arm and disarm bracket the
 * transport interval rather than the whole queue submission.
 */
void r3v_native_watchdog_guard_install(
   struct r3v_native_watchdog_guard *guard, VkDevice device);

/* Disarms, reports, and releases.  Runs before the first target read,
 * because a fire after a good submission destroys the result and spends
 * the attempt.  Returns 0 when the counter came to rest.
 */
int r3v_native_watchdog_guard_close(struct r3v_native_watchdog_guard *guard,
                                    VkResult submit_result);

#endif
