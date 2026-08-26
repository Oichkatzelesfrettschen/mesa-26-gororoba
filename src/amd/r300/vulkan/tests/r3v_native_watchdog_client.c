/* SPDX-License-Identifier: MIT */

#include "r3v_native_watchdog_client.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WATCHDOG_MAX_ARGS 8

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Single spaces separate the command from its arguments, and the command
 * is an absolute path: the helper execs directly, so no shell parses the
 * value and no PATH lookup resolves it.
 */
static int
split_command(char *buffer, char *argv[WATCHDOG_MAX_ARGS + 1])
{
   int count = 0;
   char *save = NULL;
   for (char *token = strtok_r(buffer, " ", &save); token != NULL;
        token = strtok_r(NULL, " ", &save)) {
      if (count == WATCHDOG_MAX_ARGS)
         return -1;
      argv[count++] = token;
   }
   argv[count] = NULL;
   if (count == 0 || argv[0][0] != '/')
      return -1;
   return count;
}

static int
read_line(struct r3v_native_watchdog_client *client, char *out, size_t size)
{
   if (fgets(out, (int)size, client->from_helper) == NULL)
      return -1;
   out[strcspn(out, "\r\n")] = '\0';
   return 0;
}

int
r3v_native_watchdog_client_open(struct r3v_native_watchdog_client *client)
{
   memset(client, 0, sizeof(*client));
   client->pid = -1;

   const char *command = getenv("R3V_NATIVE_WATCHDOG_BRACKET_COMMAND");
   if (command == NULL || command[0] == '\0')
      return -1;

   char buffer[512];
   if (strlen(command) >= sizeof(buffer))
      return -1;
   strcpy(buffer, command);
   char *argv[WATCHDOG_MAX_ARGS + 1];
   if (split_command(buffer, argv) < 0)
      return -1;

   int to_child[2], from_child[2];
   if (pipe(to_child) != 0)
      return -1;
   if (pipe(from_child) != 0) {
      close(to_child[0]);
      close(to_child[1]);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0) {
      close(to_child[0]);
      close(to_child[1]);
      close(from_child[0]);
      close(from_child[1]);
      return -1;
   }
   if (pid == 0) {
      dup2(to_child[0], STDIN_FILENO);
      dup2(from_child[1], STDOUT_FILENO);
      close(to_child[0]);
      close(to_child[1]);
      close(from_child[0]);
      close(from_child[1]);
      execv(argv[0], argv);
      _exit(127);
   }

   close(to_child[0]);
   close(from_child[1]);
   client->pid = pid;
   client->to_helper = fdopen(to_child[1], "w");
   client->from_helper = fdopen(from_child[0], "r");
   if (client->to_helper == NULL || client->from_helper == NULL) {
      r3v_native_watchdog_client_close(client);
      return -1;
   }
   setvbuf(client->to_helper, NULL, _IOLBF, 0);

   /* The helper reports its facts and then ready; a refusal arrives as a
    * closed stream, so the absence of ready is the refusal.
    */
   char line[256];
   size_t used = 0;
   while (read_line(client, line, sizeof(line)) == 0) {
      if (strcmp(line, "ready") == 0)
         return 0;
      int written = snprintf(client->facts + used,
                             sizeof(client->facts) - used, "%s\n", line);
      if (written < 0 || (size_t)written >= sizeof(client->facts) - used)
         break;
      used += (size_t)written;
   }
   r3v_native_watchdog_client_close(client);
   return -1;
}

/* Sends one command and retains the answer, so a refusal reports the
 * hardware readings that produced it rather than a bare failure.
 */
static int
command(struct r3v_native_watchdog_client *client, const char *request,
        const char *accepted, char *ack, size_t ack_size)
{
   if (client->to_helper == NULL)
      return -1;
   if (fprintf(client->to_helper, "%s\n", request) < 0)
      return -1;
   if (read_line(client, ack, ack_size) != 0) {
      snprintf(ack, ack_size, "%s: no answer", request);
      return -1;
   }
   return strncmp(ack, accepted, strlen(accepted)) == 0 ? 0 : -1;
}

int
r3v_native_watchdog_client_calibrate(
   struct r3v_native_watchdog_client *client)
{
   return command(client, "calibrate", "calibration verified",
                  client->calibration, sizeof(client->calibration));
}

int
r3v_native_watchdog_client_arm(struct r3v_native_watchdog_client *client)
{
   if (client->armed)
      return -1;
   const int result = command(client, "arm", "armed verified",
                              client->arm_ack, sizeof(client->arm_ack));
   client->armed_ns = now_ns();
   client->armed = true;
   client->arm_verified = result == 0;
   return result;
}

int
r3v_native_watchdog_client_disarm(struct r3v_native_watchdog_client *client)
{
   if (!client->armed)
      return 0;
   client->armed = false;
   client->disarmed_ns = now_ns();
   return command(client, "disarm", "disarmed verified", client->disarm_ack,
                  sizeof(client->disarm_ack));
}

void
r3v_native_watchdog_client_close(struct r3v_native_watchdog_client *client)
{
   if (client->to_helper != NULL) {
      /* Closing the command stream is the helper's disarm-and-exit
       * signal, so a runner that dies still releases the counter.
       */
      fputs("quit\n", client->to_helper);
      fclose(client->to_helper);
      client->to_helper = NULL;
   }
   if (client->from_helper != NULL) {
      fclose(client->from_helper);
      client->from_helper = NULL;
   }
   if (client->pid > 0) {
      int status;
      waitpid(client->pid, &status, 0);
      client->pid = -1;
   }
   client->armed = false;
}
