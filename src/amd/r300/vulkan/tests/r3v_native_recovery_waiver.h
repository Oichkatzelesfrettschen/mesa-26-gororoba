/* SPDX-License-Identifier: MIT */

#ifndef R3V_NATIVE_RECOVERY_WAIVER_H
#define R3V_NATIVE_RECOVERY_WAIVER_H

#include <stddef.h>
#include <stdint.h>

/* The operator's waiver of automatic watchdog recovery for one attended
 * submission.  An exported environment variable outlives the decision it
 * recorded and authorizes whatever runs next, so the waiver is a
 * document bound to the run it names: the boot it was written in, the
 * evidence directory it authorizes, the command stream that will reach
 * the ring, the binary that will submit it, when the operator decided,
 * and why.  Every field is matched against the live run, so a waiver
 * written for another boot, another attempt, another cell, or another
 * runner admits nothing.
 */
#define R3V_NATIVE_WAIVER_MAX_AGE_SECONDS 3600

struct r3v_native_recovery_waiver {
   char boot_id[64];
   char attempt_id[128];
   char ib_blake3[80];
   char runner_blake3[80];
   char timestamp[32];
   char operator_reason[256];
};

/* Parses the waiver at path and binds it to the live run.  Returns 0 on
 * admission; otherwise reason carries the first binding that failed.
 * now_seconds is the wall clock the age is measured against, so the
 * check is a function of its arguments alone.
 */
int r3v_native_recovery_waiver_admit(
   const char *path, const char *boot_id, const char *attempt_id,
   const char *ib_blake3, const char *runner_blake3, int64_t now_seconds,
   struct r3v_native_recovery_waiver *out, char *reason, size_t reason_size);

#endif
