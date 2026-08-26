/* SPDX-License-Identifier: MIT */

#ifndef R3V_NATIVE_WATCHDOG_CLIENT_H
#define R3V_NATIVE_WATCHDOG_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/* Drives the SB600 TCO bracket co-process across one hazardous
 * interval.  The runner arms at the submission trace's
 * R3V_NATIVE_SUBMISSION_TRACE_CS_IOCTL_ENTER and disarms at
 * ..._COMPLETION_WAIT_RETURN, so the counter covers
 * DRM_IOCTL_RADEON_CS through fence completion and nothing else.
 * armed_ns and disarmed_ns bracket the guarded interval the gate
 * measures against the 1.7-second grace, and they span the co-process
 * round trips, so the interval reported is the one the counter ran
 * across rather than the driver's inner transport bracket.
 */
struct r3v_native_watchdog_client {
   pid_t pid;
   FILE *to_helper;
   FILE *from_helper;
   bool armed;
   /* The helper answers each command with its own state readings, and a
    * "verified" acknowledgement is the only one that admits the run: it
    * says the counter was rewritten, released, and observed falling
    * across a bounded interval, where an unverified one says the
    * hardware did not show the transition the gate depends on.
    */
   bool arm_verified;
   uint64_t armed_ns;
   uint64_t disarmed_ns;
   char facts[1024];
   char calibration[256];
   char arm_ack[128];
   char disarm_ack[128];
};

/* Spawns the command named by R3V_NATIVE_WATCHDOG_BRACKET_COMMAND, an
 * absolute path with optional arguments separated by single spaces, and
 * retains the helper's fact lines.  Returns 0 once the helper reports
 * ready.
 */
int r3v_native_watchdog_client_open(struct r3v_native_watchdog_client *client);

/* Walks the loaded, active, halted, reloaded, and rearmed states, two
 * reads apiece, and retains the line.  Returns 0 when the helper
 * verifies every relation.
 */
int r3v_native_watchdog_client_calibrate(
   struct r3v_native_watchdog_client *client);

int r3v_native_watchdog_client_arm(struct r3v_native_watchdog_client *client);

/* Idempotent: a client that never armed, or already disarmed, succeeds. */
int r3v_native_watchdog_client_disarm(
   struct r3v_native_watchdog_client *client);

void r3v_native_watchdog_client_close(
   struct r3v_native_watchdog_client *client);

#endif
