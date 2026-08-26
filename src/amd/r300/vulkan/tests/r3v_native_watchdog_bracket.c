/*
 * SPDX-License-Identifier: MIT
 *
 * SB600 TCO watchdog bracket co-process for the attended R3V cells.
 */

/* The attended runner holds the Vulkan device and the evidence directory
 * as the invoking user; this helper holds the privilege the watchdog
 * needs, so arming never runs the ICD or the retention writes as root.
 * It speaks one line per command on stdin -- "arm", "disarm", "quit" --
 * and answers one line on stdout, so the runner's submission-trace hook
 * brackets DRM_IOCTL_RADEON_CS through fence completion with two writes.
 *
 * Measured SB600 counter properties, from the retained tick measurement
 * sb600-watchdog-tick-32768hz-pet-ineffective on the Dell Vostro 1000
 * (AMD K8 + RS482 + SB600):
 *
 *   WatchDogCount tick        32.768 kHz
 *   WatchDogCount width       16 bits
 *   full-count window         ~2.0 s, fixed
 *   operational grace         1.7 s, leaving ~0.3 s for the disarm
 *   WDIOC_KEEPALIVE reload    ineffective; the window admits no extension
 *   confirmed disarm          PM index 0x69 bit 0, WatchDogTimerDisable
 *
 * open() on the device starts the count from whatever WatchDogCount
 * already holds, so sp5100_tco loads with heartbeat=65535 and this
 * program refuses to arm while the timeout readback sits under a
 * half-count -- an arm from a small count fires before the guarded
 * interval opens.  Every exit path disarms through the PM port, whose
 * effect is independent of driver state; losing the parent closes stdin,
 * and the EOF disarms.  A parent that wedges the machine schedules
 * nothing further, so no disarm reaches the port and the counter fires.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/watchdog.h>

#define PM_INDEX 0xCD6
#define PM_DATA 0xCD7
#define PM_WATCHDOG_CONTROL 0x69
#define PM_WATCHDOG_DISABLE_BIT 0x01

/* Half of the 16-bit count: an arm below this leaves under a second. */
#define SAFE_COUNT 0x8000

#define WATCHDOG_TICK_HZ 32768u
#define WATCHDOG_COUNTER_BITS 16u
#define WATCHDOG_MAX_WINDOW_MS 2000u
#define WATCHDOG_GRACE_MS 1700u

static const char *const device_path = "/dev/watchdog0";

static volatile sig_atomic_t armed_fd = -1;

/* The PM port halts the counter in hardware, so it runs first and the
 * driver-level disarm follows; a signal path reaches only this half.
 */
static void
pm_write_disable_bit(bool set)
{
   outb(PM_WATCHDOG_CONTROL, PM_INDEX);
   uint8_t value = inb(PM_DATA);
   value = set ? (value | PM_WATCHDOG_DISABLE_BIT)
               : (value & (uint8_t)~PM_WATCHDOG_DISABLE_BIT);
   outb(PM_WATCHDOG_CONTROL, PM_INDEX);
   outb(value, PM_DATA);
}

static void
pm_disable_counter(void)
{
   pm_write_disable_bit(true);
}

static void
signal_disarm(int signum)
{
   pm_disable_counter();
   _exit(128 + signum);
}

static int
disarm(void)
{
   if (armed_fd < 0)
      return 0;

   pm_disable_counter();

   int fd = armed_fd;
   armed_fd = -1;
   int options = WDIOS_DISABLECARD;
   int result = ioctl(fd, WDIOC_SETOPTIONS, &options) == 0 ? 0 : errno;
   /* Magic close: the character 'V' releases the device without the
    * driver's nowayout reopen behavior.
    */
   if (write(fd, "V", 1) != 1 && result == 0)
      result = errno;
   close(fd);
   return result;
}

static void
disarm_at_exit(void)
{
   disarm();
}

/* Arming is the open itself.  The count is already loaded, so the
 * device holds no state this program sets afterward.
 */
static int
arm(void)
{
   if (armed_fd >= 0)
      return EALREADY;
   /* A prior disarm left WatchDogTimerDisable set, and the counter stays
    * halted in hardware while it is; clearing it ahead of the open is
    * what makes the open count.
    */
   pm_write_disable_bit(false);
   int fd = open(device_path, O_WRONLY | O_CLOEXEC);
   if (fd < 0)
      return errno;
   armed_fd = fd;
   return 0;
}

/* sysfs answers the capability query without an open, and an open is
 * an arm: the count runs from whatever it holds at .start, so a probe
 * open would spend part of the window the guarded interval needs.
 */
static int
read_sysfs_attribute(const char *name, char *out, size_t size)
{
   char path[128];
   snprintf(path, sizeof(path), "/sys/class/watchdog/watchdog0/%s", name);
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

static int
report_facts(void)
{
   char identity[64];
   char timeout_text[32];
   if (access(device_path, F_OK) != 0) {
      fprintf(stderr, "watchdog: %s: %s\n", device_path, strerror(errno));
      return -1;
   }
   if (read_sysfs_attribute("identity", identity, sizeof(identity)) != 0 ||
       read_sysfs_attribute("timeout", timeout_text,
                            sizeof(timeout_text)) != 0) {
      fprintf(stderr, "watchdog: sysfs capability query failed\n");
      return -1;
   }

   long timeout = strtol(timeout_text, NULL, 10);
   if (timeout < SAFE_COUNT) {
      fprintf(stderr,
              "watchdog: timeout readback %ld below %d; load sp5100_tco "
              "with heartbeat=65535\n",
              timeout, SAFE_COUNT);
      return -1;
   }

   printf("watchdog.driver=%s\n", identity);
   printf("watchdog.device=%s\n", device_path);
   printf("watchdog.timeout_readback=%ld\n", timeout);
   printf("watchdog.tick_hz=%u\n", WATCHDOG_TICK_HZ);
   printf("watchdog.counter_bits=%u\n", WATCHDOG_COUNTER_BITS);
   printf("watchdog.measured_max_window_ms=%u\n", WATCHDOG_MAX_WINDOW_MS);
   printf("watchdog.operational_grace_ms=%u\n", WATCHDOG_GRACE_MS);
   printf("watchdog.keepalive_reload_effective=false\n");
   /* Firing the counter is the only demonstration that the reset path
    * works, and an attended submission does not fire it.
    */
   printf("watchdog.reset_path_verified=unverified "
          "(carried by the guard qualification; this run does not fire)\n");
   return 0;
}

int
main(void)
{
   setvbuf(stdout, NULL, _IOLBF, 0);

   if (ioperm(PM_INDEX, 2, 1) != 0) {
      fprintf(stderr, "watchdog: ioperm(0x%x): %s\n", PM_INDEX,
              strerror(errno));
      return 1;
   }

   struct sigaction action;
   memset(&action, 0, sizeof(action));
   action.sa_handler = signal_disarm;
   sigaction(SIGINT, &action, NULL);
   sigaction(SIGTERM, &action, NULL);
   sigaction(SIGHUP, &action, NULL);
   sigaction(SIGPIPE, &action, NULL);
   atexit(disarm_at_exit);

   if (report_facts() != 0)
      return 1;
   printf("ready\n");

   char line[64];
   while (fgets(line, sizeof(line), stdin) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      if (strcmp(line, "arm") == 0) {
         int result = arm();
         if (result != 0) {
            printf("error arm %s\n", strerror(result));
            return 1;
         }
         printf("armed\n");
      } else if (strcmp(line, "disarm") == 0) {
         int result = disarm();
         printf("disarmed %s\n", result == 0 ? "ok" : strerror(result));
         if (result != 0)
            return 1;
      } else if (strcmp(line, "quit") == 0) {
         break;
      } else {
         printf("error command %s\n", line);
         return 1;
      }
   }
   return disarm() == 0 ? 0 : 1;
}
