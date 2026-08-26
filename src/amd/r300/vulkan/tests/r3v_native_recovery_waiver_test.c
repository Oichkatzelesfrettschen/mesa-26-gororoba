/*
 * SPDX-License-Identifier: MIT
 *
 * Calibration for the attended run's recovery waiver.
 */

#include "r3v_native_recovery_waiver.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_BOOT "e5fc857e-4aa3-42e7-b3e5-7f31e2250f53"
#define LIVE_ATTEMPT "composed-0007"
#define LIVE_IB "247949a2"
#define LIVE_RUNNER "9e83c4d1"
/* 2026-08-26T12:00:00Z as seconds since the epoch. */
#define LIVE_NOW 1787745600

static const char complete[] =
   "# operator waiver\n"
   "boot_id=" LIVE_BOOT "\n"
   "attempt_id=" LIVE_ATTEMPT "\n"
   "ib_blake3=" LIVE_IB "\n"
   "runner_blake3=" LIVE_RUNNER "\n"
   "timestamp=2026-08-26T11:58:00Z\n"
   "operator_reason=the interval exceeds the grace; standing by to power "
   "cycle\n";

static unsigned checks;
static unsigned failures;

static void
expect(const char *label, const char *document, bool admit,
       const char *marker)
{
   const char *tmpdir = getenv("TMPDIR");
   char path[256];
   snprintf(path, sizeof(path), "%s/r3v-waiver-XXXXXX",
            tmpdir != NULL && tmpdir[0] != '\0' ? tmpdir : ".");
   int fd = mkstemp(path);
   if (fd < 0) {
      printf("  FAIL %s: no fixture file\n", label);
      failures++;
      checks++;
      return;
   }
   FILE *file = fdopen(fd, "w");
   fputs(document, file);
   fclose(file);

   struct r3v_native_recovery_waiver waiver;
   char reason[256];
   const int result = r3v_native_recovery_waiver_admit(
      path, LIVE_BOOT, LIVE_ATTEMPT, LIVE_IB, LIVE_RUNNER, LIVE_NOW,
      &waiver, reason, sizeof(reason));
   remove(path);

   const bool ok = (result == 0) == admit &&
                   (marker == NULL || strstr(reason, marker) != NULL);
   checks++;
   if (!ok) {
      failures++;
      printf("  FAIL %s: result %d reason %s\n", label, result, reason);
   } else {
      printf("  ok   %s\n", label);
   }
}

/* Replaces one line of the complete waiver, so each fixture differs from
 * the admitted document by exactly the binding under test.
 */
static const char *
mutate(const char *key, const char *value)
{
   static char buffer[1024];
   char line[512];
   snprintf(line, sizeof(line), "%s=", key);
   buffer[0] = '\0';
   const char *cursor = complete;
   while (*cursor != '\0') {
      const char *end = strchr(cursor, '\n');
      const size_t length = (size_t)(end - cursor) + 1;
      if (strncmp(cursor, line, strlen(line)) == 0) {
         if (value != NULL)
            snprintf(buffer + strlen(buffer),
                     sizeof(buffer) - strlen(buffer), "%s=%s\n", key, value);
      } else {
         snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer),
                  "%.*s", (int)length, cursor);
      }
      cursor = end + 1;
   }
   return buffer;
}

int
main(void)
{
   expect("known-good", complete, true, NULL);
   expect("foreign-boot", mutate("boot_id", "00000000-0000-0000-0000-0"),
          false, "boot_id");
   expect("foreign-attempt", mutate("attempt_id", "composed-0006"), false,
          "attempt_id");
   expect("foreign-cell", mutate("ib_blake3", "deadbeef"), false,
          "ib_blake3");
   expect("foreign-runner", mutate("runner_blake3", "deadbeef"), false,
          "runner_blake3");
   expect("stale", mutate("timestamp", "2026-08-26T09:00:00Z"), false,
          "age");
   expect("future", mutate("timestamp", "2026-08-26T12:30:00Z"), false,
          "age");
   expect("malformed-timestamp", mutate("timestamp", "yesterday"), false,
          "YYYY-MM-DD");
   expect("missing-reason", mutate("operator_reason", NULL), false,
          "operator_reason");
   expect("repeated-field",
          "boot_id=" LIVE_BOOT "\nboot_id=" LIVE_BOOT "\n", false,
          "repeats");
   expect("unknown-field", mutate("boot_id", NULL), false, "boot_id");

   printf("%u/%u checks pass\n", checks - failures, checks);
   return failures == 0 ? 0 : 1;
}
