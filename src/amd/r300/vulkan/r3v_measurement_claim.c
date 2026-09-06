/* SPDX-License-Identifier: MIT */

#include "r3v_measurement_claim.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The characters a declared name may carry into a directory entry.  A
 * separator, a NUL, and every other byte are refused, so the composed
 * name names one entry in the pinned directory. */
static bool
name_is_portable(const char *name)
{
   if (name == NULL || name[0] == '\0' || name[0] == '.')
      return false;
   for (const char *c = name; *c != '\0'; c++) {
      const bool ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
                      (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' ||
                      *c == '-';
      if (!ok)
         return false;
   }
   return true;
}

void
r3v_measurement_claim_init(struct r3v_measurement_claim *claim)
{
   if (claim == NULL)
      return;
   memset(claim, 0, sizeof(*claim));
   claim->root_fd = -1;
}

int
r3v_measurement_claim_bind(struct r3v_measurement_claim *claim,
                           const char *declaration_path,
                           const char *session_nonce, const char *arm_name)
{
   if (claim == NULL || declaration_path == NULL)
      return -EINVAL;
   if (!name_is_portable(session_nonce) || !name_is_portable(arm_name))
      return -EINVAL;

   int written =
      snprintf(claim->name, sizeof(claim->name),
               "measurement-claim-%s-%s.token", session_nonce, arm_name);
   if (written < 0 || (size_t)written >= sizeof(claim->name)) {
      claim->name[0] = '\0';
      return -ENAMETOOLONG;
   }

   /* The campaign root is the declaration's own directory.  A path with
    * no separator names a declaration in the working directory, and "."
    * is that directory. */
   const char *slash = strrchr(declaration_path, '/');
   char root[4096];
   if (slash == NULL) {
      root[0] = '.';
      root[1] = '\0';
   } else {
      const size_t length = slash == declaration_path ? 1u
                                                      : (size_t)(slash -
                                                                 declaration_path);
      if (length >= sizeof(root)) {
         claim->name[0] = '\0';
         return -ENAMETOOLONG;
      }
      memcpy(root, declaration_path, length);
      root[length] = '\0';
   }

   const int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (fd < 0) {
      claim->name[0] = '\0';
      return -errno;
   }
   if (claim->root_fd >= 0)
      close(claim->root_fd);
   claim->root_fd = fd;
   claim->held = false;
   return 0;
}

int
r3v_measurement_claim_acquire(
   struct r3v_measurement_claim *claim,
   const struct r3v_measurement_claim_record *record)
{
   if (claim == NULL || record == NULL || claim->root_fd < 0 ||
       claim->name[0] == '\0')
      return -EINVAL;
   if (claim->held)
      return -EALREADY;

   /* Exclusive creation is the winner-selection operation: the kernel
    * admits one creator among every racer, and the loser observes
    * -EEXIST rather than a file it has to interpret. */
   const int fd = openat(claim->root_fd, claim->name,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
   if (fd < 0)
      return -errno;

   struct timespec now;
   clock_gettime(CLOCK_REALTIME, &now);
   char self[512];
   const ssize_t self_length =
      readlink("/proc/self/exe", self, sizeof(self) - 1);
   self[self_length > 0 ? (size_t)self_length : 0] = '\0';

   char body[2048];
   const int body_length = snprintf(
      body, sizeof(body),
      "session_nonce: %s\n"
      "declaration_blake3: %s\n"
      "arm: %s\n"
      "platform: %s\n"
      "pci: %04x:%04x\n"
      "kernel_release: %s\n"
      "module_srcversion: %s\n"
      "claim_scope: %s\n"
      "pid: %lld\n"
      "executable: %s\n"
      "unix_time: %lld.%09ld\n",
      record->session_nonce != NULL ? record->session_nonce : "(undeclared)",
      record->declaration_digest != NULL ? record->declaration_digest
                                         : "(undeclared)",
      record->arm_name != NULL ? record->arm_name : "(undeclared)",
      record->platform_name != NULL ? record->platform_name : "(unresolved)",
      record->pci_vendor_id, record->pci_device_id,
      record->kernel_release != NULL ? record->kernel_release : "(unread)",
      record->module_srcversion != NULL ? record->module_srcversion
                                        : "(unread)",
      record->claim_scope != NULL ? record->claim_scope : "(unstated)",
      (long long)getpid(), self[0] != '\0' ? self : "(unread)",
      (long long)now.tv_sec, now.tv_nsec);

   int result = 0;
   if (body_length < 0 || (size_t)body_length >= sizeof(body)) {
      result = -EIO;
   } else {
      size_t total = 0;
      while (result == 0 && total < (size_t)body_length) {
         const ssize_t chunk =
            write(fd, body + total, (size_t)body_length - total);
         if (chunk < 0) {
            if (errno == EINTR)
               continue;
            result = -errno;
            break;
         }
         if (chunk == 0) {
            result = -EIO;
            break;
         }
         total += (size_t)chunk;
      }
   }
   if (result == 0 && fsync(fd) != 0)
      result = -errno;
   if (close(fd) != 0 && result == 0)
      result = -errno;
   /* The directory entry itself reaches storage here, so a claim this
    * reports as won is one a power failure past the ioctl cannot erase.
    * A failure leaves the entry: it records that an attempt started. */
   if (result == 0 && fsync(claim->root_fd) != 0)
      result = -errno;
   if (result != 0)
      return result;

   claim->held = true;
   return 0;
}

void
r3v_measurement_claim_finish(struct r3v_measurement_claim *claim)
{
   if (claim == NULL)
      return;
   if (claim->root_fd >= 0)
      close(claim->root_fd);
   claim->root_fd = -1;
   claim->held = false;
   claim->name[0] = '\0';
}
