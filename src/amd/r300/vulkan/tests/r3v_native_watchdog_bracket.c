/*
 * SPDX-License-Identifier: MIT
 *
 * SB600 TCO watchdog bracket co-process for the attended R3V cells.
 */

/* The attended runner holds the Vulkan device and the evidence directory
 * as the invoking user; this helper holds the privilege the watchdog
 * needs, so arming never runs the ICD or the retention writes as root.
 * It speaks one line per command on stdin -- "calibrate", "arm",
 * "disarm", "quit" -- and answers one line on stdout, so the runner's
 * submission-trace hook brackets DRM_IOCTL_RADEON_CS through fence
 * completion with two synchronous round trips.
 *
 * Measured SB600 counter properties, from the retained tick measurement
 * sb600-watchdog-tick-32768hz-pet-ineffective on the Dell Vostro 1000
 * (AMD K8 + RS485M + SB600):
 *
 *   WatchDogCount tick        32.768 kHz
 *   WatchDogCount width       16 bits
 *   full-count window         ~2.0 s, fixed
 *   operational grace         1.7 s, leaving ~0.3 s for the disarm
 *   WDIOC_KEEPALIVE reload    ineffective
 *   confirmed halt            PM index 0x69 bit 0, WatchDogTimerDisable
 *
 * The driver's .start sets START and TRIGGER and its .ping sets TRIGGER
 * alone, so a measured-ineffective ping refutes .start's reload along
 * with it: every arm rewrites WatchDogCount through WDIOC_SETTIMEOUT
 * while the PM bit holds the counter halted, and reads the register back
 * before clearing the bit.  Every state transition is judged by two
 * reads across a bounded interval, because one read distinguishes a
 * running counter from a halted one not at all.
 *
 * PM 0x69 bit 0 is set before the device is ever opened, so the open
 * that starts the countdown finds the counter inhibited and no window
 * opens behind this program's back.  Arming clears the bit and disarming
 * sets it; the descriptor stays open across both.
 *
 * Exit is fail-closed while armed.  A normal exit runs from the disarmed
 * state and keeps the PM halt, and every abnormal exit -- a signal, a
 * closed command stream, a parent that wedged the machine -- leaves an
 * armed counter running, so the reset the gate promises still lands.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/watchdog.h>

#define PM_INDEX 0xCD6
#define PM_DATA 0xCD7
#define PM_WATCHDOG_CONTROL 0x69
#define PM_WATCHDOG_DISABLE_BIT 0x01

#define FULL_COUNT 0xffff
/* Half the 16-bit count: a reload landing under this leaves under a
 * second, which the guarded interval's grace does not fit inside.
 */
#define SAFE_COUNT 0x8000
/* 5 ms is 164 ticks at 32.768 kHz, so a running counter separates from a
 * halted one by two orders of magnitude more than any read jitter.
 */
#define OBSERVE_MS 5

#define WATCHDOG_TICK_HZ 32768u
#define WATCHDOG_COUNTER_BITS 16u
#define WATCHDOG_MAX_WINDOW_MS 2000u
#define WATCHDOG_GRACE_MS 1700u

static const char *const device_path = "/dev/watchdog0";

static int device_fd = -1;
static volatile sig_atomic_t counter_running = 0;

static uint8_t
pm_read_control(void)
{
   outb(PM_WATCHDOG_CONTROL, PM_INDEX);
   return inb(PM_DATA);
}

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

/* An armed counter outlives this program by design: a parent that wedged
 * the machine reaches no disarm, and the reset is the whole point.
 */
static void
signal_exit(int signum)
{
   _exit(128 + signum);
}

static void
observe_interval(void)
{
   const struct timespec delay = {
      .tv_sec = 0,
      .tv_nsec = OBSERVE_MS * 1000000L,
   };
   nanosleep(&delay, NULL);
}

/* The driver returns WatchDogCount verbatim through WDIOC_GETTIMELEFT. */
static int
read_count(void)
{
   int count = -1;
   if (ioctl(device_fd, WDIOC_GETTIMELEFT, &count) != 0)
      return -1;
   return count;
}

static int
reload_count(void)
{
   int timeout = FULL_COUNT;
   if (ioctl(device_fd, WDIOC_SETTIMEOUT, &timeout) != 0)
      return -1;
   return read_count();
}

struct state_pair {
   int first;
   int second;
};

static struct state_pair
observe_state(void)
{
   struct state_pair pair;
   pair.first = read_count();
   observe_interval();
   pair.second = read_count();
   return pair;
}

static void
halt_counter(void)
{
   pm_write_disable_bit(true);
   counter_running = 0;
}

/* Halted is proved, not assumed: two equal reads across the interval say
 * the counter stopped, and a fall between them says the PM bit did not
 * take.
 */
static bool
halted(struct state_pair *out)
{
   *out = observe_state();
   return out->first >= 0 && out->first == out->second;
}

static bool
counting_down(struct state_pair *out)
{
   *out = observe_state();
   return out->first >= 0 && out->first > out->second;
}

static int
arm(void)
{
   struct state_pair rest;
   if (!halted(&rest)) {
      printf("armed unverified counter-not-at-rest %d %d\n", rest.first,
             rest.second);
      return -1;
   }

   int reloaded = reload_count();
   if (reloaded < SAFE_COUNT) {
      printf("armed unverified reload-short %d %d\n", rest.first, reloaded);
      return -1;
   }

   pm_write_disable_bit(false);
   counter_running = 1;
   struct state_pair running;
   if (!counting_down(&running)) {
      halt_counter();
      printf("armed unverified no-countdown %d %d\n", running.first,
             running.second);
      return -1;
   }

   printf("armed verified %d %d %d\n", reloaded, running.first,
          running.second);
   return 0;
}

static int
disarm(void)
{
   halt_counter();
   struct state_pair rest;
   if (!halted(&rest)) {
      printf("disarmed unverified still-counting %d %d\n", rest.first,
             rest.second);
      return -1;
   }
   printf("disarmed verified %d\n", rest.first);
   return 0;
}

/* The ladder the first hardware calibration walks, one bounded interval
 * per state.  Every relation is stated before it is read, and the
 * depleting active phase is what makes the reload observable: a counter
 * halted at its loaded value could not show a rewrite at all.
 *
 *   loaded    L0 >= SAFE_COUNT   heartbeat=65535 reached WatchDogCount
 *   active    A0 > A1            clearing the PM bit starts the count
 *   halted    H0 == H1           setting the PM bit stops it
 *   reloaded  R0 > H0            WDIOC_SETTIMEOUT rewrites the register
 *   rearmed   B0 > B1            the rewritten count runs
 */
static int
calibrate(void)
{
   const int loaded = read_count();
   if (loaded < SAFE_COUNT) {
      printf("calibration unverified loaded-short %d\n", loaded);
      return -1;
   }

   pm_write_disable_bit(false);
   counter_running = 1;
   struct state_pair active;
   const bool active_ok = counting_down(&active);

   halt_counter();
   struct state_pair rest;
   const bool halted_ok = halted(&rest);

   const int reloaded = reload_count();
   const bool reload_ok = reloaded > rest.first && reloaded >= SAFE_COUNT;

   pm_write_disable_bit(false);
   counter_running = 1;
   struct state_pair rearmed;
   const bool rearmed_ok = counting_down(&rearmed);
   halt_counter();

   printf("calibration %s L0=%d A0=%d A1=%d H0=%d H1=%d R0=%d B0=%d B1=%d "
          "observe_ms=%d\n",
          active_ok && halted_ok && reload_ok && rearmed_ok ? "verified"
                                                            : "unverified",
          loaded, active.first, active.second, rest.first, rest.second,
          reloaded, rearmed.first, rearmed.second, OBSERVE_MS);
   return active_ok && halted_ok && reload_ok && rearmed_ok ? 0 : -1;
}

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
   if (read_sysfs_attribute("identity", identity, sizeof(identity)) != 0 ||
       read_sysfs_attribute("timeout", timeout_text,
                            sizeof(timeout_text)) != 0) {
      fprintf(stderr, "watchdog: sysfs capability query failed\n");
      return -1;
   }
   const long timeout = strtol(timeout_text, NULL, 10);
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
   action.sa_handler = signal_exit;
   sigaction(SIGINT, &action, NULL);
   sigaction(SIGTERM, &action, NULL);
   sigaction(SIGHUP, &action, NULL);
   sigaction(SIGPIPE, &action, NULL);

   if (report_facts() != 0)
      return 1;

   /* Halt before the open, so the open's countdown finds the counter
    * inhibited and this program owns every window that follows.
    */
   halt_counter();
   device_fd = open(device_path, O_WRONLY | O_CLOEXEC);
   if (device_fd < 0) {
      fprintf(stderr, "watchdog: %s: %s\n", device_path, strerror(errno));
      return 1;
   }
   if (reload_count() < SAFE_COUNT) {
      fprintf(stderr, "watchdog: the opened counter refused its reload\n");
      return 1;
   }
   printf("ready\n");

   char line[64];
   while (fgets(line, sizeof(line), stdin) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      if (strcmp(line, "calibrate") == 0) {
         if (calibrate() != 0)
            return 1;
      } else if (strcmp(line, "arm") == 0) {
         if (arm() != 0)
            return 1;
      } else if (strcmp(line, "disarm") == 0) {
         if (disarm() != 0)
            return 1;
      } else if (strcmp(line, "quit") == 0) {
         break;
      } else {
         printf("error command %s\n", line);
         return 1;
      }
   }

   if (counter_running) {
      fprintf(stderr, "watchdog: command stream closed while armed; the "
                      "counter runs\n");
      return 1;
   }
   /* Disarmed exit: the counter is proved at rest through the register
    * while the descriptor still reads it, the magic close then releases
    * the device without the driver's nowayout reopen behavior, and the
    * PM control bit answers whether the halt survived the release --
    * a count read cannot, because reopening the device would arm it.
    */
   struct state_pair rest;
   if (!halted(&rest)) {
      fprintf(stderr, "watchdog: the counter runs at release\n");
      return 1;
   }
   int options = WDIOS_DISABLECARD;
   ioctl(device_fd, WDIOC_SETOPTIONS, &options);
   if (write(device_fd, "V", 1) != 1)
      fprintf(stderr, "watchdog: magic close write failed: %s\n",
              strerror(errno));
   close(device_fd);
   device_fd = -1;
   if ((pm_read_control() & PM_WATCHDOG_DISABLE_BIT) == 0) {
      fprintf(stderr, "watchdog: the release cleared the hardware halt\n");
      return 1;
   }
   printf("released halted %d\n", rest.first);
   return 0;
}
