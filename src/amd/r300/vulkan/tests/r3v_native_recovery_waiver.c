/* SPDX-License-Identifier: MIT */

#include "r3v_native_recovery_waiver.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct field {
   const char *key;
   char *value;
   size_t size;
};

/* Days since 1970-01-01 for a proleptic Gregorian date, shifting the
 * year to start in March so the leap day lands last and the month-length
 * series repeats every five months.  UTC alone: the waiver's timestamp
 * carries Z, so no local zone or leap-second table enters the check.
 */
static int64_t
days_from_civil(int64_t year, int64_t month, int64_t day)
{
   year -= month <= 2;
   const int64_t era = (year >= 0 ? year : year - 399) / 400;
   const int64_t year_of_era = year - era * 400;
   const int64_t day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
   const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 -
                              year_of_era / 100 + day_of_year;
   return era * 146097 + day_of_era - 719468;
}

static int
parse_timestamp(const char *text, int64_t *out)
{
   int year, month, day, hour, minute, second;
   char trailer = 0;
   if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%c", &year, &month, &day, &hour,
              &minute, &second, &trailer) != 7 ||
       trailer != 'Z' || strlen(text) != 20)
      return -1;
   if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
       minute > 59 || second > 60 || hour < 0 || minute < 0 || second < 0)
      return -1;
   *out = days_from_civil(year, month, day) * 86400 + hour * 3600 +
          minute * 60 + second;
   return 0;
}

/* One key=value per line, every key required exactly once, and no key
 * repeated: a second boot_id line would otherwise decide which boot the
 * waiver names by parse order.
 */
static int
parse_document(const char *path, struct r3v_native_recovery_waiver *out,
               char *reason, size_t reason_size)
{
   const struct field fields[] = {
      {"boot_id", out->boot_id, sizeof(out->boot_id)},
      {"attempt_id", out->attempt_id, sizeof(out->attempt_id)},
      {"ib_blake3", out->ib_blake3, sizeof(out->ib_blake3)},
      {"runner_blake3", out->runner_blake3, sizeof(out->runner_blake3)},
      {"timestamp", out->timestamp, sizeof(out->timestamp)},
      {"operator_reason", out->operator_reason,
       sizeof(out->operator_reason)},
   };
   const size_t count = sizeof(fields) / sizeof(fields[0]);

   FILE *file = fopen(path, "r");
   if (file == NULL) {
      snprintf(reason, reason_size, "the waiver %s does not open", path);
      return -1;
   }

   char line[512];
   while (fgets(line, sizeof(line), file) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      if (line[0] == '\0' || line[0] == '#')
         continue;
      char *separator = strchr(line, '=');
      if (separator == NULL) {
         snprintf(reason, reason_size, "the waiver line %s carries no key",
                  line);
         fclose(file);
         return -1;
      }
      *separator = '\0';
      const char *value = separator + 1;
      size_t index = 0;
      for (; index < count; index++)
         if (strcmp(line, fields[index].key) == 0)
            break;
      if (index == count) {
         snprintf(reason, reason_size, "the waiver names no field %s", line);
         fclose(file);
         return -1;
      }
      if (fields[index].value[0] != '\0') {
         snprintf(reason, reason_size, "the waiver repeats %s",
                  fields[index].key);
         fclose(file);
         return -1;
      }
      if (value[0] == '\0' || strlen(value) >= fields[index].size) {
         snprintf(reason, reason_size, "the waiver's %s is empty or too long",
                  fields[index].key);
         fclose(file);
         return -1;
      }
      memcpy(fields[index].value, value, strlen(value) + 1);
   }
   fclose(file);

   for (size_t index = 0; index < count; index++) {
      if (fields[index].value[0] == '\0') {
         snprintf(reason, reason_size, "the waiver states no %s",
                  fields[index].key);
         return -1;
      }
   }
   return 0;
}

static int
bind(const char *name, const char *waived, const char *live, char *reason,
     size_t reason_size)
{
   if (strcmp(waived, live) == 0)
      return 0;
   snprintf(reason, reason_size, "the waiver's %s is %s and the run's is %s",
            name, waived, live);
   return -1;
}

int
r3v_native_recovery_waiver_admit(
   const char *path, const char *boot_id, const char *attempt_id,
   const char *ib_blake3, const char *runner_blake3, int64_t now_seconds,
   struct r3v_native_recovery_waiver *out, char *reason, size_t reason_size)
{
   memset(out, 0, sizeof(*out));
   reason[0] = '\0';
   if (parse_document(path, out, reason, reason_size) != 0)
      return -1;

   if (bind("boot_id", out->boot_id, boot_id, reason, reason_size) != 0 ||
       bind("attempt_id", out->attempt_id, attempt_id, reason,
            reason_size) != 0 ||
       bind("ib_blake3", out->ib_blake3, ib_blake3, reason,
            reason_size) != 0 ||
       bind("runner_blake3", out->runner_blake3, runner_blake3, reason,
            reason_size) != 0)
      return -1;

   int64_t written = 0;
   if (parse_timestamp(out->timestamp, &written) != 0) {
      snprintf(reason, reason_size,
               "the waiver's timestamp %s is not YYYY-MM-DDTHH:MM:SSZ",
               out->timestamp);
      return -1;
   }
   const int64_t age = now_seconds - written;
   if (age < 0 || age > R3V_NATIVE_WAIVER_MAX_AGE_SECONDS) {
      snprintf(reason, reason_size,
               "the waiver's age is %" PRId64 " s, outside 0 to %d s",
               age, R3V_NATIVE_WAIVER_MAX_AGE_SECONDS);
      return -1;
   }
   return 0;
}
