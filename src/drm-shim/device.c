/*
 * Copyright © 2018 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/** @file
 *
 * Implements core GEM support (particularly ioctls) underneath the libc ioctl
 * wrappers, and calls into the driver-specific code as necessary.
 */

#include <c11/threads.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/memfd.h>
#include <linux/kcmp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include "drm-uapi/drm.h"
#include "drm_shim.h"
#include "util/hash_table.h"
#include "util/u_atomic.h"
#include "util/u_math.h"

#define SHIM_MEM_SIZE (4ull * 1024 * 1024 * 1024)
#define DRM_SHIM_STATE_TOKEN_NAME_VERSION "mesa-drm-shim-state-v1"
#define DRM_SHIM_STATE_TOKEN_MAGIC UINT64_C(0x4d44524d53484d31)
#define DRM_SHIM_STATE_TOKEN_VERSION 1
#define DRM_SHIM_STATE_TOKEN_IDENTIFIER_LENGTH 32
#define DRM_SHIM_STATE_TOKEN_NAME_CAPACITY 256
#define DRM_SHIM_STATE_TOKEN_LOCK_BASE INT64_C(0x7000000000000000)

#ifndef HAVE_MEMFD_CREATE
static inline int
memfd_create(const char *name, unsigned int flags)
{
   return syscall(SYS_memfd_create, name, flags);
}
#endif

/* Global state for the shim shared between libc, core, and driver. */
struct shim_device shim_device;

long shim_page_size;

static thread_local int dispatch_fd = -1;
static thread_local struct shim_fd *dispatch_shim_fd;
static bool device_initialized;
static int identity_lock_counter;

struct shim_mapping {
   uintptr_t start;
   uintptr_t end;
   struct shim_bo *bo;
   struct shim_mapping *next;
};

struct drm_shim_scm_pin {
   uint64_t receiver_cookie;
   struct shim_fd *shim_fd;
   struct drm_shim_scm_pin *next;
};

struct drm_shim_linux_dirent64 {
   uint64_t inode;
   int64_t offset;
   unsigned short record_length;
   unsigned char type;
   char name[];
};

static bool
drm_shim_fd_backing_matches(const struct shim_fd *shim_fd, int fd);
static void drm_shim_forget_non_cloexec_fd_locked(int fd);
void
drm_shim_file_release_posix_locks(struct shim_fd *shim_fd);
/* Live count of BO backing files the shim holds open.  A harness that
 * preloads the shim reads it through dlsym() to compare resource censuses
 * around a prepare or a teardown, so the counter and its reader live in
 * every shim build, and the reader is exported past the library's hidden
 * default visibility.
 */
static int live_bo_backing_files;
static int live_bos;

PUBLIC int
drm_shim_test_live_bo_backing_files(void)
{
   return p_atomic_read(&live_bo_backing_files);
}

/* Live count of GEM objects the shim owns in this process.  A BO that is
 * never mapped (a 4-byte completion object waited through
 * DRM_RADEON_GEM_WAIT_IDLE) holds no backing file, so the object census
 * is the one that observes its creation and release.
 */
PUBLIC int
drm_shim_test_live_bos(void)
{
   return p_atomic_read(&live_bos);
}

#ifdef DRM_SHIM_TEST
static int force_duplicate_query_error;
static int force_kcmp_error;
static bool force_kcmp_result;
static int forced_kcmp_result;
static int fd_discovery_barrier_ready_fd = -1;
static int fd_discovery_barrier_release_fd = -1;
static int fd_discovery_barrier_remaining;

void
drm_shim_test_force_fd_identity_errors(int duplicate_query_error,
                                       int kcmp_error)
{
   force_duplicate_query_error = duplicate_query_error;
   force_kcmp_error = kcmp_error;
}

void
drm_shim_test_force_kcmp_result(bool force, int result)
{
   force_kcmp_result = force;
   forced_kcmp_result = result;
}

void
drm_shim_test_arm_fd_discovery_barrier(int ready_fd, int release_fd)
{
   fd_discovery_barrier_ready_fd = ready_fd;
   fd_discovery_barrier_release_fd = release_fd;
   p_atomic_set(&fd_discovery_barrier_remaining, 2);
}
#endif

static uint32_t
uint_key_hash(const void *key)
{
   return (uintptr_t)key;
}

static bool
uint_key_compare(const void *a, const void *b)
{
   return a == b;
}

struct drm_shim_render_identity {
   pid_t origin_pid;
   bool current_instance;
};

struct drm_shim_state_token_header {
   uint64_t magic;
   uint32_t version;
   uint32_t header_size;
   uint64_t page_size;
   uint64_t next_record_offset;
   uint64_t next_namespace_id;
   uint64_t namespace_head_offset;
   char instance[DRM_SHIM_STATE_TOKEN_IDENTIFIER_LENGTH + 1];
   char marker[DRM_SHIM_STATE_TOKEN_IDENTIFIER_LENGTH + 1];
   char state_id[DRM_SHIM_STATE_TOKEN_IDENTIFIER_LENGTH + 1];
   char driver_name[64];
   pthread_mutex_t lock;
};

enum drm_shim_state_namespace_state {
   DRM_SHIM_STATE_NAMESPACE_PREPARED = 1,
   DRM_SHIM_STATE_NAMESPACE_ACTIVE = 2,
   DRM_SHIM_STATE_NAMESPACE_POISONED = 3,
};

struct drm_shim_state_namespace {
   uint64_t record_size;
   uint64_t next_offset;
   uint64_t namespace_id;
   int64_t identity_lock_offset;
   uint64_t snapshot_offset;
   uint64_t snapshot_size;
   uint64_t generation;
   uint32_t state;
   uint32_t reserved;
};

static bool
drm_shim_identity_name_read(int fd, char *name, size_t capacity,
                            size_t *length_out)
{
   struct stat status;
   if (syscall(SYS_fstat, fd, &status) < 0 ||
       !S_ISREG(status.st_mode) || status.st_size != 0)
      return false;

#ifdef F_GET_SEALS
   int seals = syscall(SYS_fcntl, fd, F_GET_SEALS);
   const int required_seals =
      F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
   if (seals < 0 || (seals & required_seals) != required_seals)
      return false;
#else
   return false;
#endif

   char proc_path[64];
   int path_length =
      snprintf(proc_path, sizeof(proc_path),
               "/proc/thread-self/fd/%d", fd);
   if (path_length < 0 || path_length >= (int)sizeof(proc_path))
      return false;

   char link_target[DRM_SHIM_RENDER_IDENTITY_CAPACITY + 32];
   ssize_t link_length =
      syscall(SYS_readlinkat, AT_FDCWD, proc_path, link_target,
              sizeof(link_target) - 1);
   if (link_length <= 0 ||
       link_length >= (ssize_t)sizeof(link_target))
      return false;
   link_target[link_length] = '\0';

   static const char prefix[] = "/memfd:";
   static const char suffix[] = " (deleted)";
   const size_t prefix_length = sizeof(prefix) - 1;
   const size_t suffix_length = sizeof(suffix) - 1;
   if ((size_t)link_length <= prefix_length + suffix_length ||
       memcmp(link_target, prefix, prefix_length) != 0 ||
       memcmp(link_target + link_length - suffix_length, suffix,
              suffix_length) != 0)
      return false;

   size_t name_length =
      (size_t)link_length - prefix_length - suffix_length;
   if (name_length >= capacity)
      return false;
   memcpy(name, link_target + prefix_length, name_length);
   name[name_length] = '\0';
   *length_out = name_length;
   return true;
}

static int
drm_shim_readable_witness(int fd)
{
   int status_flags = syscall(SYS_fcntl, fd, F_GETFL);
   if (status_flags < 0)
      return -1;
   if ((status_flags & O_PATH) != O_PATH &&
       (status_flags & O_ACCMODE) != O_WRONLY)
      return syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 3);

   char proc_path[64];
   int length =
      snprintf(proc_path, sizeof(proc_path),
               "/proc/thread-self/fd/%d", fd);
   if (length < 0 || length >= (int)sizeof(proc_path)) {
      errno = ENAMETOOLONG;
      return -1;
   }
   int witness =
      syscall(SYS_openat, AT_FDCWD, proc_path, O_RDONLY | O_CLOEXEC, 0);
   if (witness < 0)
      return -1;

   struct stat original_status;
   struct stat witness_status;
   if (syscall(SYS_fstat, fd, &original_status) < 0 ||
       syscall(SYS_fstat, witness, &witness_status) < 0 ||
       original_status.st_dev != witness_status.st_dev ||
       original_status.st_ino != witness_status.st_ino) {
      int saved_errno = errno ? errno : ESTALE;
      syscall(SYS_close, witness);
      errno = saved_errno;
      return -1;
   }
   return witness;
}

static bool
drm_shim_parse_decimal_field(const char **cursor, const char *end,
                             char terminator, uint64_t maximum,
                             uint64_t *value_out)
{
   const char *position = *cursor;
   if (position == end || *position < '0' || *position > '9')
      return false;

   uint64_t value = 0;
   while (position < end && *position >= '0' && *position <= '9') {
      unsigned digit = (unsigned)(*position - '0');
      if (value > (maximum - digit) / 10)
         return false;
      value = value * 10 + digit;
      position++;
   }
   if (terminator) {
      if (position == end || *position != terminator)
         return false;
      position++;
   } else if (position != end) {
      return false;
   }
   *cursor = position;
   *value_out = value;
   return true;
}

static bool
drm_shim_parse_instance_field(const char **cursor, const char *end,
                              char instance[33])
{
   if ((size_t)(end - *cursor) < 33)
      return false;
   const char *position = *cursor;
   for (unsigned index = 0; index < 32; index++) {
      char byte = position[index];
      if (!((byte >= '0' && byte <= '9') ||
            (byte >= 'a' && byte <= 'f')))
         return false;
      instance[index] = byte;
   }
   instance[32] = '\0';
   position += 32;
   if (position == end || *position != ':')
      return false;
   *cursor = position + 1;
   return true;
}

static void
drm_shim_render_marker_token(char token[33])
{
   uint64_t low = UINT64_C(1469598103934665603);
   uint64_t high = UINT64_C(7809847782465536322);
   for (size_t index = 0; index < shim_device.render_marker_length;
        index++) {
      uint8_t byte = (uint8_t)shim_device.render_marker[index];
      low ^= byte;
      low *= UINT64_C(1099511628211);
      high ^= (uint8_t)(byte + index);
      high *= UINT64_C(14029467366897019727);
   }
   int length =
      snprintf(token, 33, "%016llx%016llx",
               (unsigned long long)high,
               (unsigned long long)low);
   if (length != 32)
      abort();
}

static bool
drm_shim_memfd_name_read(int fd, char *name, size_t capacity,
                         size_t *length_out)
{
   char proc_path[64];
   int path_length =
      snprintf(proc_path, sizeof(proc_path),
               "/proc/thread-self/fd/%d", fd);
   if (path_length < 0 || path_length >= (int)sizeof(proc_path))
      return false;

   char link_target[DRM_SHIM_STATE_TOKEN_NAME_CAPACITY + 32];
   ssize_t link_length =
      syscall(SYS_readlinkat, AT_FDCWD, proc_path, link_target,
              sizeof(link_target) - 1);
   if (link_length <= 0 ||
       link_length >= (ssize_t)sizeof(link_target))
      return false;
   link_target[link_length] = '\0';

   static const char prefix[] = "/memfd:";
   static const char suffix[] = " (deleted)";
   const size_t prefix_length = sizeof(prefix) - 1;
   const size_t suffix_length = sizeof(suffix) - 1;
   if ((size_t)link_length <= prefix_length + suffix_length ||
       memcmp(link_target, prefix, prefix_length) != 0 ||
       memcmp(link_target + link_length - suffix_length, suffix,
              suffix_length) != 0)
      return false;

   size_t name_length =
      (size_t)link_length - prefix_length - suffix_length;
   if (name_length >= capacity)
      return false;
   memcpy(name, link_target + prefix_length, name_length);
   name[name_length] = '\0';
   *length_out = name_length;
   return true;
}

static bool
drm_shim_state_token_identifier_valid(const char identifier[33])
{
   for (unsigned index = 0; index < 32; index++) {
      char byte = identifier[index];
      if (!((byte >= '0' && byte <= '9') ||
            (byte >= 'a' && byte <= 'f')))
         return false;
   }
   return identifier[32] == '\0';
}

static bool
drm_shim_state_token_name_parse(int fd,
                                struct drm_shim_state_token_header *header)
{
   struct stat status;
   if (syscall(SYS_fstat, fd, &status) < 0 ||
       !S_ISREG(status.st_mode) ||
       status.st_size < (off_t)sysconf(_SC_PAGESIZE))
      return false;

   /* The raw syscall keeps the header read outside the shim's own pread
    * interposer, which takes the device lock. */
   struct drm_shim_state_token_header candidate;
   ssize_t read_length =
      syscall(SYS_pread64, fd, &candidate, sizeof(candidate), 0);
   if (read_length != (ssize_t)sizeof(candidate) ||
       candidate.magic != DRM_SHIM_STATE_TOKEN_MAGIC ||
       candidate.version != DRM_SHIM_STATE_TOKEN_VERSION ||
       candidate.header_size != sizeof(candidate) ||
       candidate.page_size != (uint64_t)sysconf(_SC_PAGESIZE) ||
       candidate.driver_name[sizeof(candidate.driver_name) - 1] != '\0' ||
       candidate.instance[32] != '\0' ||
       candidate.marker[32] != '\0' ||
       candidate.state_id[32] != '\0' ||
       !drm_shim_state_token_identifier_valid(candidate.instance) ||
       !drm_shim_state_token_identifier_valid(candidate.marker) ||
       !drm_shim_state_token_identifier_valid(candidate.state_id) ||
       strcmp(candidate.driver_name, shim_device.driver_name) != 0)
      return false;

   char marker[33];
   drm_shim_render_marker_token(marker);
   if (memcmp(candidate.marker, marker, sizeof(marker)) != 0)
      return false;

   char name[DRM_SHIM_STATE_TOKEN_NAME_CAPACITY];
   size_t name_length;
   if (!drm_shim_memfd_name_read(fd, name, sizeof(name), &name_length))
      return false;

   int expected_length =
      snprintf(NULL, 0, "%s:%s:%s:%s:%s",
               DRM_SHIM_STATE_TOKEN_NAME_VERSION,
               candidate.instance, candidate.marker,
               candidate.state_id, candidate.driver_name);
   if (expected_length < 0 || (size_t)expected_length != name_length)
      return false;

   char expected_name[DRM_SHIM_STATE_TOKEN_NAME_CAPACITY];
   if (snprintf(expected_name, sizeof(expected_name),
                "%s:%s:%s:%s:%s",
                DRM_SHIM_STATE_TOKEN_NAME_VERSION,
                candidate.instance, candidate.marker,
                candidate.state_id, candidate.driver_name) !=
       expected_length ||
       memcmp(name, expected_name, name_length) != 0)
      return false;

   *header = candidate;
   return true;
}

/* Recovers a state token's identity from its memfd name alone.  An O_PATH
 * descriptor carries no read access, so the header pread that
 * drm_shim_state_token_name_parse cross-checks against is unavailable,
 * and the same descriptor survives exec with only its name reachable.
 * The name is shim-authored and carries every field the header does:
 * accepting it requires the version prefix, three well-formed
 * identifiers, this process's render marker, and this driver's name, so
 * the descriptor is the same class of artifact the two-sided parse
 * admits.  State operations stay closed on a path-only descriptor
 * through shim_fd->state_available, so this recovers identity alone.
 */
static bool
drm_shim_state_token_name_only_parse(int fd, char instance[33])
{
   /* Only a path-only descriptor takes this route: a readable descriptor
    * that fails the header parse is not a token this shim authored, and
    * admitting it by name alone would let a stale or foreign descriptor
    * enter the fd registry.
    */
   int status_flags = syscall(SYS_fcntl, fd, F_GETFL);
   if (status_flags < 0 || (status_flags & O_PATH) != O_PATH)
      return false;

   struct stat status;
   if (syscall(SYS_fstat, fd, &status) < 0 || !S_ISREG(status.st_mode) ||
       status.st_size < (off_t)sysconf(_SC_PAGESIZE))
      return false;

   char name[DRM_SHIM_STATE_TOKEN_NAME_CAPACITY];
   size_t name_length;
   if (!drm_shim_memfd_name_read(fd, name, sizeof(name), &name_length))
      return false;

   /* version:instance:marker:state_id:driver_name */
   static const char version_prefix[] = DRM_SHIM_STATE_TOKEN_NAME_VERSION;
   const size_t version_length = sizeof(version_prefix) - 1;
   if (name_length <= version_length ||
       memcmp(name, version_prefix, version_length) != 0 ||
       name[version_length] != ':')
      return false;

   char candidate_instance[33];
   char candidate_marker[33];
   char candidate_state_id[33];
   const char *cursor = name + version_length + 1;
   const char *name_end = name + name_length;
   char *const fields[] = {candidate_instance, candidate_marker,
                           candidate_state_id};
   for (unsigned i = 0; i < 3; i++) {
      if (name_end - cursor < 33 || cursor[32] != ':')
         return false;
      memcpy(fields[i], cursor, 32);
      fields[i][32] = '\0';
      if (!drm_shim_state_token_identifier_valid(fields[i]))
         return false;
      cursor += 33;
   }

   if (strcmp(cursor, shim_device.driver_name) != 0)
      return false;

   char marker[33];
   drm_shim_render_marker_token(marker);
   if (memcmp(candidate_marker, marker, sizeof(marker)) != 0)
      return false;

   memcpy(instance, candidate_instance, sizeof(candidate_instance));
   return true;
}

static bool
drm_shim_render_identity_name_parse(int fd, char instance[33])
{
   char name[DRM_SHIM_RENDER_IDENTITY_CAPACITY];
   size_t name_length;
   if (!drm_shim_identity_name_read(fd, name, sizeof(name),
                                    &name_length))
      return false;

   static const char separator[] = ":";
   const size_t version_length =
      strlen(DRM_SHIM_RENDER_IDENTITY_NAME_VERSION);
   if (name_length != version_length + 1 + 32 + 1 + 32 ||
       memcmp(name, DRM_SHIM_RENDER_IDENTITY_NAME_VERSION,
              version_length) != 0 ||
       memcmp(name + version_length, separator, 1) != 0 ||
       memcmp(name + version_length + 1 + 32, separator, 1) != 0)
      return false;

   const char *instance_start = name + version_length + 1;
   for (unsigned index = 0; index < 32; index++) {
      char byte = instance_start[index];
      if (!((byte >= '0' && byte <= '9') ||
            (byte >= 'a' && byte <= 'f')))
         return false;
   }

   char marker_token[33];
   drm_shim_render_marker_token(marker_token);
   if (memcmp(instance_start + 33, marker_token, 32) != 0)
      return false;

   memcpy(instance, instance_start, 32);
   instance[32] = '\0';
   return true;
}

static bool
drm_shim_render_identity_parse(int fd,
                               struct drm_shim_render_identity *identity)
{
   struct stat status;
   if (syscall(SYS_fstat, fd, &status) < 0)
      return false;
   if (status.st_dev == shim_device.lock_backing_dev &&
       status.st_ino == shim_device.lock_backing_ino) {
      identity->origin_pid = shim_device.render_owner_pid;
      identity->current_instance = true;
      return true;
   }

   /* drm_shim_render_node_open hands out per-open state-token memfds, so a
    * render fd normally reaches here with an inode distinct from the
    * identity anchor.  The token header carries the instance identifier;
    * a header that parses under this driver's marker is a render fd, and
    * an instance match makes it current.
    */
   struct drm_shim_state_token_header header;
   if (drm_shim_state_token_name_parse(fd, &header)) {
      identity->origin_pid = shim_device.render_owner_pid;
      identity->current_instance =
         memcmp(header.instance, shim_device.render_instance,
                sizeof(shim_device.render_instance)) == 0;
      return true;
   }

   /* A path-only or exec-inherited state token reaches here with its
    * header unreadable; its memfd name carries the same identity fields.
    */
   char token_instance[33];
   if (drm_shim_state_token_name_only_parse(fd, token_instance)) {
      identity->origin_pid = shim_device.render_owner_pid;
      identity->current_instance =
         memcmp(token_instance, shim_device.render_instance,
                sizeof(shim_device.render_instance)) == 0;
      return true;
   }

   /* An identity-anchor memfd inherited across exec has the anchor's name
    * but a reopened inode; the name parse recovers the instance.
    */
   char instance[33];
   if (drm_shim_render_identity_name_parse(fd, instance)) {
      identity->origin_pid = shim_device.render_owner_pid;
      identity->current_instance =
         memcmp(instance, shim_device.render_instance,
                sizeof(instance)) == 0;
      return true;
   }

   /* A raw open of the synthetic render-node path bypasses the interposer
    * and lands on the backing file itself; the backing inode is this
    * instance's render node.
    */
   if (drm_shim_fd_names_render_backing(fd)) {
      identity->origin_pid = shim_device.render_owner_pid;
      identity->current_instance = true;
      return true;
   }

   return false;
}

static void
drm_shim_seal_empty_identity(int fd)
{
   if (syscall(SYS_fcntl, fd, F_ADD_SEALS,
               F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK |
                  F_SEAL_SEAL) < 0)
      abort();
}

static void
drm_shim_generate_identifier(char identifier[33])
{
   unsigned char random_bytes[16];
   size_t offset = 0;
#ifdef SYS_getrandom
   while (offset < sizeof(random_bytes)) {
      ssize_t result =
         syscall(SYS_getrandom, random_bytes + offset,
                 sizeof(random_bytes) - offset, 0);
      if (result < 0 && errno == EINTR)
         continue;
      if (result < 0 && errno == ENOSYS)
         break;
      if (result <= 0)
         abort();
      offset += (size_t)result;
   }
#endif
   if (offset < sizeof(random_bytes)) {
      int random_fd =
         syscall(SYS_openat, AT_FDCWD, "/dev/urandom",
                 O_RDONLY | O_CLOEXEC, 0);
      if (random_fd < 0)
         abort();
      while (offset < sizeof(random_bytes)) {
         ssize_t result =
            syscall(SYS_read, random_fd, random_bytes + offset,
                    sizeof(random_bytes) - offset);
         if (result < 0 && errno == EINTR)
            continue;
         if (result <= 0)
            abort();
         offset += (size_t)result;
      }
      syscall(SYS_close, random_fd);
   }

   static const char hex[] = "0123456789abcdef";
   for (unsigned index = 0; index < sizeof(random_bytes); index++) {
      identifier[index * 2] =
         hex[random_bytes[index] >> 4];
      identifier[index * 2 + 1] =
         hex[random_bytes[index] & 0xf];
   }
   identifier[32] = '\0';
}

static void
drm_shim_generate_instance(void)
{
   drm_shim_generate_identifier(shim_device.render_instance);
}

static int
drm_shim_identity_anchor_from_fd(int fd)
{
   char path[64];
   int length =
      snprintf(path, sizeof(path), "/proc/thread-self/fd/%d", fd);
   if (length < 0 || length >= (int)sizeof(path)) {
      errno = ENAMETOOLONG;
      return -1;
   }
   return syscall(SYS_openat, AT_FDCWD, path,
                  O_PATH | O_CLOEXEC, 0);
}

static int drm_shim_create_identity_anchor(void);

static int
drm_shim_inherited_identity_locator(char instance[33])
{
   const char *environment_value =
      getenv(DRM_SHIM_EXEC_LOCATOR_ENV);
   if (!environment_value) {
      errno = ENOENT;
      return -1;
   }

   char *value = strdup(environment_value);
   if (!value)
      return -1;
   if (unsetenv(DRM_SHIM_EXEC_LOCATOR_ENV) < 0) {
      int saved_errno = errno;
      free(value);
      errno = saved_errno;
      return -1;
   }

   static const char inherited_version[] = "v1i:";
   static const char fresh_version[] = "v1f:";
   const char *cursor = value;
   const char *end = value + strlen(value);
   uint64_t locator_value;
   char locator_instance[33];
   bool enable_state = false;
   bool valid = false;
   if ((size_t)(end - cursor) >
          sizeof(inherited_version) - 1 &&
       memcmp(cursor, inherited_version,
              sizeof(inherited_version) - 1) == 0) {
      cursor += sizeof(inherited_version) - 1;
      valid = true;
   } else if ((size_t)(end - cursor) >
                 sizeof(fresh_version) - 1 &&
              memcmp(cursor, fresh_version,
                     sizeof(fresh_version) - 1) == 0) {
      cursor += sizeof(fresh_version) - 1;
      enable_state = true;
      valid = true;
   }
   valid =
      valid &&
      drm_shim_parse_decimal_field(&cursor, end, ':', INT_MAX,
                                   &locator_value) &&
      drm_shim_parse_instance_field(&cursor, end,
                                    locator_instance) &&
      cursor < end;
   if (!valid) {
      free(value);
      errno = EINVAL;
      return -1;
   }
   if ((size_t)(end - cursor) != strlen(shim_device.driver_name) ||
       memcmp(cursor, shim_device.driver_name,
              (size_t)(end - cursor)) != 0) {
      free(value);
      errno = ENOENT;
      return -1;
   }

   int locator = (int)locator_value;
   int status_flags = syscall(SYS_fcntl, locator, F_GETFL);
   char observed_instance[33];
   bool locator_is_anchor = false;
   if (status_flags < 0 || (status_flags & O_PATH) == O_PATH) {
      free(value);
      errno = EBADF;
      return -1;
   }
   if (drm_shim_render_identity_name_parse(locator,
                                           observed_instance)) {
      locator_is_anchor = true;
   } else {
      /* drm_shim_fd_prepare_exec exports an inherited render fd,
       * and drm_shim_render_node_open backs each render fd with a
       * per-open state-token memfd whose
       * drm_shim_state_token_header names the instance under this
       * driver's marker; the header parse validates all three.
       */
      struct drm_shim_state_token_header header;
      if (!drm_shim_state_token_name_parse(locator, &header)) {
         free(value);
         errno = EBADF;
         return -1;
      }
      memcpy(observed_instance, header.instance,
             sizeof(observed_instance));
   }
   if (memcmp(observed_instance, locator_instance,
              sizeof(observed_instance)) != 0) {
      free(value);
      errno = EBADF;
      return -1;
   }

   int anchor;
   if (locator_is_anchor) {
      anchor = drm_shim_identity_anchor_from_fd(locator);
   } else {
      /* A state token's inode is its own memfd, so the anchor's
       * backing inode does not cross the exec.
       * drm_shim_create_identity_anchor rebuilds the sealed anchor
       * memfd under the inherited instance name, and inherited
       * descriptors match through the instance-name parse paths in
       * drm_shim_render_identity_parse.
       */
      memcpy(shim_device.render_instance, locator_instance,
             sizeof(shim_device.render_instance));
      anchor = drm_shim_create_identity_anchor();
   }
   if (anchor >= 0) {
      memcpy(instance, locator_instance, sizeof(locator_instance));
      shim_device.exec_locator_fd = locator;
      shim_device.exec_locator_enables_state = enable_state;
   }
   int saved_errno = errno;
   free(value);
   errno = saved_errno;
   return anchor;
}

static int
drm_shim_create_identity_anchor(void)
{
   char marker_token[33];
   drm_shim_render_marker_token(marker_token);

   char name[DRM_SHIM_RENDER_IDENTITY_CAPACITY];
   int length =
      snprintf(name, sizeof(name), "%s:%s:%s",
               DRM_SHIM_RENDER_IDENTITY_NAME_VERSION,
               shim_device.render_instance, marker_token);
   if (length < 0 || length >= (int)sizeof(name)) {
      errno = ENAMETOOLONG;
      return -1;
   }

   int identity =
      memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (identity < 0)
      return -1;
   if (syscall(SYS_fcntl, identity, F_ADD_SEALS,
               F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK |
                  F_SEAL_SEAL) < 0) {
      int saved_errno = errno;
      syscall(SYS_close, identity);
      errno = saved_errno;
      return -1;
   }

   int anchor = drm_shim_identity_anchor_from_fd(identity);
   int saved_errno = errno;
   syscall(SYS_close, identity);
   errno = saved_errno;
   return anchor;
}

static void
drm_shim_identity_init(void)
{
   char inherited_instance[33];
   int anchor =
      drm_shim_inherited_identity_locator(inherited_instance);
   if (anchor >= 0) {
      memcpy(shim_device.render_instance, inherited_instance,
             sizeof(inherited_instance));
   } else {
      if (errno != ENOENT)
         abort();
      drm_shim_generate_instance();
      anchor = drm_shim_create_identity_anchor();
      if (anchor < 0)
         abort();
   }

   struct stat status;
   if (syscall(SYS_fstat, anchor, &status) < 0)
      abort();
   shim_device.lock_backing_fd = anchor;
   shim_device.lock_backing_dev = status.st_dev;
   shim_device.lock_backing_ino = status.st_ino;
}

int
drm_shim_render_node_path(char *path, size_t capacity)
{
   if (!path) {
      errno = EFAULT;
      return -1;
   }
   if (!device_initialized || shim_device.lock_backing_fd < 0) {
      errno = ENODEV;
      return -1;
   }

   int status_flags =
      syscall(SYS_fcntl, shim_device.lock_backing_fd, F_GETFL);
   struct stat status;
   if (status_flags < 0 ||
       (status_flags & O_PATH) != O_PATH ||
       syscall(SYS_fstat, shim_device.lock_backing_fd, &status) < 0 ||
       !S_ISREG(status.st_mode) || status.st_size != 0 ||
       status.st_dev != shim_device.lock_backing_dev ||
       status.st_ino != shim_device.lock_backing_ino) {
      errno = ESTALE;
      return -1;
   }

   char marker_token[33];
   drm_shim_render_marker_token(marker_token);
   char expected_target[DRM_SHIM_RENDER_IDENTITY_CAPACITY + 32];
   int expected_length =
      snprintf(expected_target, sizeof(expected_target),
               "/memfd:%s:%s:%s (deleted)",
               DRM_SHIM_RENDER_IDENTITY_NAME_VERSION,
               shim_device.render_instance, marker_token);
   if (expected_length < 0 ||
       expected_length >= (int)sizeof(expected_target)) {
      errno = ENAMETOOLONG;
      return -1;
   }

   int length =
      snprintf(path, capacity, "/proc/thread-self/fd/%d",
               shim_device.lock_backing_fd);
   if (length < 0 || (size_t)length >= capacity) {
      errno = ENAMETOOLONG;
      return -1;
   }

   char target[DRM_SHIM_RENDER_IDENTITY_CAPACITY + 32];
   ssize_t target_length =
      syscall(SYS_readlinkat, AT_FDCWD, path, target,
              sizeof(target) - 1);
   if (target_length != expected_length ||
       memcmp(target, expected_target, (size_t)expected_length) != 0) {
      errno = ESTALE;
      return -1;
   }
   return 0;
}

static int
drm_shim_state_token_create(void)
{
   char marker[33];
   char state_id[33];
   drm_shim_render_marker_token(marker);
   drm_shim_generate_identifier(state_id);

   char name[DRM_SHIM_STATE_TOKEN_NAME_CAPACITY];
   int name_length =
      snprintf(name, sizeof(name), "%s:%s:%s:%s:%s",
               DRM_SHIM_STATE_TOKEN_NAME_VERSION,
               shim_device.render_instance, marker, state_id,
               shim_device.driver_name);
   if (name_length < 0 || name_length >= (int)sizeof(name)) {
      errno = ENAMETOOLONG;
      return -1;
   }

   int token_fd =
      memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (token_fd < 0)
      return -1;

   const long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0 ||
       ftruncate(token_fd, (off_t)page_size) < 0) {
      int saved_errno = errno ? errno : EIO;
      syscall(SYS_close, token_fd);
      errno = saved_errno;
      return -1;
   }

   void *mapping = mmap(NULL, (size_t)page_size,
                        PROT_READ | PROT_WRITE, MAP_SHARED,
                        token_fd, 0);
   if (mapping == MAP_FAILED) {
      int saved_errno = errno;
      syscall(SYS_close, token_fd);
      errno = saved_errno;
      return -1;
   }

   struct drm_shim_state_token_header *header = mapping;
   memset(header, 0, sizeof(*header));
   header->magic = DRM_SHIM_STATE_TOKEN_MAGIC;
   header->version = DRM_SHIM_STATE_TOKEN_VERSION;
   header->header_size = sizeof(*header);
   header->page_size = (uint64_t)page_size;
   header->next_record_offset = (uint64_t)page_size;
   header->next_namespace_id = 1;
   memcpy(header->instance, shim_device.render_instance,
          sizeof(header->instance));
   memcpy(header->marker, marker, sizeof(header->marker));
   memcpy(header->state_id, state_id, sizeof(header->state_id));
   if (snprintf(header->driver_name, sizeof(header->driver_name),
                "%s", shim_device.driver_name) < 0) {
      int saved_errno = EIO;
      munmap(mapping, (size_t)page_size);
      syscall(SYS_close, token_fd);
      errno = saved_errno;
      return -1;
   }

   pthread_mutexattr_t attributes;
   int mutex_error = pthread_mutexattr_init(&attributes);
   if (!mutex_error)
      mutex_error = pthread_mutexattr_setpshared(
         &attributes, PTHREAD_PROCESS_SHARED);
#ifdef PTHREAD_MUTEX_ROBUST
   if (!mutex_error)
      mutex_error = pthread_mutexattr_setrobust(
         &attributes, PTHREAD_MUTEX_ROBUST);
#endif
   if (!mutex_error)
      mutex_error = pthread_mutex_init(&header->lock, &attributes);
   if (pthread_mutexattr_destroy(&attributes) != 0 && !mutex_error)
      mutex_error = EIO;
   if (mutex_error) {
      int saved_errno = mutex_error;
      munmap(mapping, (size_t)page_size);
      syscall(SYS_close, token_fd);
      errno = saved_errno;
      return -1;
   }

   if (msync(mapping, (size_t)page_size, MS_SYNC) < 0) {
      int saved_errno = errno;
      pthread_mutex_destroy(&header->lock);
      munmap(mapping, (size_t)page_size);
      syscall(SYS_close, token_fd);
      errno = saved_errno;
      return -1;
   }
   munmap(mapping, (size_t)page_size);
   return token_fd;
}

static int
drm_shim_state_token_open(int token_fd, int flags)
{
   char path[64];
   int path_length =
      snprintf(path, sizeof(path), "/proc/thread-self/fd/%d", token_fd);
   if (path_length < 0 || path_length >= (int)sizeof(path)) {
      errno = ENAMETOOLONG;
      return -1;
   }

   int open_flags;
   if (flags & O_PATH) {
      open_flags = O_PATH | (flags & O_CLOEXEC);
   } else {
      open_flags =
         (flags & O_ACCMODE) | (flags & (O_CLOEXEC | O_NONBLOCK));
   }
   return syscall(SYS_openat, AT_FDCWD, path, open_flags, 0);
}

int
drm_shim_render_node_open(int flags)
{
   if (flags & O_DIRECTORY
#ifdef O_TMPFILE
       || (flags & O_TMPFILE) == O_TMPFILE
#endif
   ) {
      errno = ENOTDIR;
      return -1;
   }
   if ((flags & O_CREAT) && (flags & O_EXCL)) {
      errno = EEXIST;
      return -1;
   }

   int carrier_fd = drm_shim_state_token_create();
   if (carrier_fd < 0)
      return -1;
   int fd = drm_shim_state_token_open(carrier_fd, flags);
   int saved_errno = errno;
   syscall(SYS_close, carrier_fd);
   if (fd < 0) {
      errno = saved_errno;
      return -1;
   }

   int registration_error = drm_shim_fd_register(fd, NULL);
   if (registration_error) {
      int saved_errno = -registration_error;
      syscall(SYS_close, fd);
      errno = saved_errno;
      return -1;
   }
   return fd;
}

/**
 * Called when the first libc shim is called, to initialize GEM simulation
 * state (other than the shims themselves).
 */
void
drm_shim_device_init(void)
{
   shim_device.fd_map = _mesa_hash_table_create(NULL,
                                                uint_key_hash,
                                                uint_key_compare);
   shim_device.non_cloexec_fd_map =
      _mesa_hash_table_create(NULL, uint_key_hash, uint_key_compare);

   shim_device.offset_map = _mesa_hash_table_u64_create(NULL);

   mtx_init(&shim_device.lock, mtx_plain);

   /* The man page for mmap() says
    *
    *    offset must be a multiple of the page size as returned by
    *    sysconf(_SC_PAGE_SIZE).
    *
    * Depending on the configuration of the kernel, this may not be 4096. Get
    * this page size once and use it as the page size throughout, ensuring that
    * are offsets are page-size aligned as required. Otherwise, mmap will fail
    * with EINVAL.
    */

   shim_page_size = sysconf(_SC_PAGESIZE);
   shim_device.next_mmap_offset = shim_page_size;
   util_vma_heap_init(&shim_device.mem_heap, shim_page_size,
                      SHIM_MEM_SIZE - shim_page_size);

   drm_shim_driver_init();

   shim_device.exec_locator_fd = -1;
   shim_device.exec_locator_enables_state = false;
   drm_shim_identity_init();
   shim_device.render_owner_pid = getpid();

   device_initialized = true;
}

void
drm_shim_device_atfork_prepare(void)
{
   if (device_initialized)
      mtx_lock(&shim_device.lock);
}

void
drm_shim_device_atfork_parent(void)
{
   if (device_initialized)
      mtx_unlock(&shim_device.lock);
}

void
drm_shim_device_atfork_child(void)
{
   if (!device_initialized)
      return;
   mtx_unlock(&shim_device.lock);
}

static int
drm_shim_install_render_identity_locked(int fd)
{
   int status_flags = syscall(SYS_fcntl, fd, F_GETFL);
   int descriptor_flags = syscall(SYS_fcntl, fd, F_GETFD);
   if (status_flags < 0 || descriptor_flags < 0)
      return -1;
   bool path_only = (status_flags & O_PATH) == O_PATH;

   char identity_path[64];
   int path_length =
      snprintf(identity_path, sizeof(identity_path),
               "/proc/thread-self/fd/%d",
               shim_device.lock_backing_fd);
   if (path_length < 0 ||
       path_length >= (int)sizeof(identity_path)) {
      errno = ENAMETOOLONG;
      return -1;
   }
   int open_flags =
      path_only ? O_PATH | O_CLOEXEC
                : (status_flags & O_ACCMODE) | O_CLOEXEC;
   int replacement =
      syscall(SYS_openat, AT_FDCWD, identity_path, open_flags, 0);
   if (replacement < 0)
      return -1;
   if (!path_only &&
       syscall(SYS_fcntl, replacement, F_SETFL, status_flags) < 0) {
      int saved_errno = errno;
      syscall(SYS_close, replacement);
      errno = saved_errno;
      return -1;
   }
#ifdef SYS_dup3
   int duplicate_result =
      syscall(SYS_dup3, replacement, fd,
              descriptor_flags & FD_CLOEXEC ? O_CLOEXEC : 0);
#else
   int duplicate_result = -1;
   errno = ENOSYS;
#endif
   int saved_errno = errno;
   syscall(SYS_close, replacement);
   if (duplicate_result < 0) {
      errno = saved_errno;
      return -1;
   }
   return 1;
}

static int
drm_shim_collect_placeholder_aliases(int fd, int **aliases_out,
                                     size_t *alias_count_out)
{
   *aliases_out = NULL;
   *alias_count_out = 0;
   int status_flags = syscall(SYS_fcntl, fd, F_GETFL);
   if (status_flags < 0)
      return -1;
   if ((status_flags & O_PATH) == O_PATH) {
      int *aliases = malloc(sizeof(*aliases));
      if (!aliases)
         return -1;
      aliases[0] = fd;
      *aliases_out = aliases;
      *alias_count_out = 1;
      return 0;
   }

#if defined(F_OFD_SETLK) && defined(F_OFD_GETLK)
   off64_t lock_offset = 0;
   short lock_type =
      (status_flags & O_ACCMODE) == O_RDONLY ? F_RDLCK : F_WRLCK;
   for (unsigned attempt = 0; attempt < 1048576; attempt++) {
      uint32_t sequence =
         (uint32_t)p_atomic_inc_return(&identity_lock_counter);
      off64_t candidate_offset =
         (off64_t)((UINT64_C(1) << 48) + sequence);
      struct flock lock = {
         .l_type = lock_type,
         .l_whence = SEEK_SET,
         .l_start = candidate_offset,
         .l_len = 1,
      };
      if (syscall(SYS_fcntl, fd, F_OFD_SETLK, &lock) == 0) {
         lock_offset = candidate_offset;
         break;
      }
      if (errno != EACCES && errno != EAGAIN)
         return -1;
   }
   if (!lock_offset) {
      errno = EAGAIN;
      return -1;
   }

   struct stat reference_status;
   if (syscall(SYS_fstat, fd, &reference_status) < 0)
      return -1;
   int directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/fd",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
   if (directory_fd < 0)
      return -1;

   int *aliases = NULL;
   size_t alias_count = 0;
   char buffer[4096];
   while (true) {
      long length =
         syscall(SYS_getdents64, directory_fd, buffer, sizeof(buffer));
      if (length < 0 && errno == EINTR)
         continue;
      if (length < 0) {
         free(aliases);
         syscall(SYS_close, directory_fd);
         return -1;
      }
      if (length == 0)
         break;
      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length ||
             offset + entry->record_length > length)
            abort();
         offset += entry->record_length;

         char *end;
         errno = 0;
         long fd_value = strtol(entry->name, &end, 10);
         if (errno || *entry->name == '\0' || *end != '\0' ||
             fd_value < 0 || fd_value > INT_MAX ||
             fd_value == directory_fd)
            continue;
         int candidate = (int)fd_value;
         struct stat candidate_status;
         if (syscall(SYS_fstat, candidate, &candidate_status) < 0 ||
             candidate_status.st_dev != reference_status.st_dev ||
             candidate_status.st_ino != reference_status.st_ino)
            continue;
         struct flock query = {
            .l_type = F_WRLCK,
            .l_whence = SEEK_SET,
            .l_start = lock_offset,
            .l_len = 1,
         };
         if (syscall(SYS_fcntl, candidate, F_OFD_GETLK, &query) < 0 ||
             query.l_type != F_UNLCK)
            continue;
         int *resized =
            realloc(aliases, (alias_count + 1) * sizeof(*aliases));
         if (!resized) {
            free(aliases);
            syscall(SYS_close, directory_fd);
            return -1;
         }
         aliases = resized;
         aliases[alias_count++] = candidate;
      }
   }
   syscall(SYS_close, directory_fd);
   if (!alias_count) {
      free(aliases);
      errno = ESTALE;
      return -1;
   }
   *aliases_out = aliases;
   *alias_count_out = alias_count;
   return 0;
#else
   int *aliases = malloc(sizeof(*aliases));
   if (!aliases)
      return -1;
   aliases[0] = fd;
   *aliases_out = aliases;
   *alias_count_out = 1;
   return 0;
#endif
}

static struct shim_fd *
drm_shim_file_create(int fd, bool enable_state)
{
   struct drm_shim_render_identity render_identity;
   if (!drm_shim_render_identity_parse(fd, &render_identity)) {
      errno = ENODEV;
      return NULL;
   }

   struct shim_fd *shim_fd = calloc(1, sizeof(*shim_fd));
   if (!shim_fd)
      return NULL;

   /* The backing identity is the fd's own file: render_node_open hands out
    * per-open state-token memfds, so the anchor's dev/ino describes only
    * the anchor and drm_shim_fd_matches revalidates each fd against the
    * file it was registered with.
    */
   struct stat backing_status;
   if (syscall(SYS_fstat, fd, &backing_status) < 0) {
      free(shim_fd);
      return NULL;
   }
   shim_fd->fd = fd;
   shim_fd->backing_dev = backing_status.st_dev;
   shim_fd->backing_ino = backing_status.st_ino;
   shim_fd->owner_pid = getpid();
   int status_flags = syscall(SYS_fcntl, fd, F_GETFL);
   if (status_flags < 0) {
      free(shim_fd);
      return NULL;
   }
   shim_fd->path_only = (status_flags & O_PATH) == O_PATH;
   shim_fd->state_available =
      enable_state && render_identity.current_instance &&
      !shim_fd->path_only;
   shim_fd->identity_fd = -1;
   shim_fd->lock_proxy_fd = -1;
   shim_fd->lock_proxy_anchor_fd = -1;
   p_atomic_set(&shim_fd->refcount, 1);
   mtx_init(&shim_fd->handle_lock, mtx_plain);
   shim_fd->handles = _mesa_hash_table_create(NULL,
                                              uint_key_hash,
                                              uint_key_compare);
   if (!shim_fd->handles) {
      mtx_destroy(&shim_fd->handle_lock);
      free(shim_fd);
      return NULL;
   }
   return shim_fd;
}

static void drm_shim_bo_put_handle(struct shim_bo *bo);
static void handle_delete_fxn(struct hash_entry *entry);

static void
drm_shim_file_destroy(struct shim_fd *shim_fd)
{
   _mesa_hash_table_destroy(shim_fd->handles, handle_delete_fxn);
   mtx_destroy(&shim_fd->handle_lock);
   free(shim_fd);
}

void
drm_shim_fd_put(struct shim_fd *shim_fd)
{
   if (shim_fd && p_atomic_dec_zero(&shim_fd->refcount) &&
       shim_fd->owner_pid == getpid())
      drm_shim_file_destroy(shim_fd);
}

void
drm_shim_fd_tables_unshared(void)
{
   mtx_lock(&shim_device.lock);
   shim_device.fd_tables_diverged = true;
   mtx_unlock(&shim_device.lock);
}

static bool
drm_shim_parse_decimal_fd(const char *text, int *value_out)
{
   if (!text[0])
      return false;

   unsigned value = 0;
   for (const char *cursor = text; *cursor; cursor++) {
      if (*cursor < '0' || *cursor > '9')
         return false;
      unsigned digit = (unsigned)(*cursor - '0');
      if (value > ((unsigned)INT_MAX - digit) / 10)
         return false;
      value = value * 10 + digit;
   }
   *value_out = (int)value;
   return true;
}

static bool
drm_shim_fdinfo_has_identity_lock(int fdinfo_directory_fd,
                                  const char *fd_name,
                                  off64_t expected_offset)
{
   int fdinfo_fd =
      syscall(SYS_openat, fdinfo_directory_fd, fd_name,
              O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
   if (fdinfo_fd < 0)
      return false;

   char buffer[8192];
   size_t used = 0;
   while (used + 1 < sizeof(buffer)) {
      ssize_t length =
         syscall(SYS_read, fdinfo_fd, buffer + used,
                 sizeof(buffer) - used - 1);
      if (length < 0 && errno == EINTR)
         continue;
      if (length <= 0)
         break;
      used += (size_t)length;
   }
   syscall(SYS_close, fdinfo_fd);
   buffer[used] = '\0';

   for (char *line = buffer; line && *line;) {
      char *next = strchr(line, '\n');
      if (next)
         *next = '\0';
      if (strncmp(line, "lock:", 5) == 0 &&
          strstr(line, " OFDLCK ")) {
         char *end_text = strrchr(line, ' ');
         if (end_text && end_text[1]) {
            char *end_parse = NULL;
            int64_t end = strtoll(end_text + 1, &end_parse, 10);
            *end_text = '\0';
            char *start_text = strrchr(line, ' ');
            if (start_text && start_text[1]) {
               char *start_parse = NULL;
               int64_t start =
                  strtoll(start_text + 1, &start_parse, 10);
               if (start_parse && !*start_parse &&
                   end_parse && !*end_parse &&
                   start == expected_offset &&
                   end == expected_offset)
                  return true;
            }
         }
      }
      line = next ? next + 1 : NULL;
   }
   return false;
}

static bool
drm_shim_task_has_external_alias(const struct shim_fd *shim_fd,
                                 int task_directory_fd,
                                 const char *task_name)
{
   int task_fd =
      syscall(SYS_openat, task_directory_fd, task_name,
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
   if (task_fd < 0)
      return errno != ENOENT;
   int fd_directory_fd =
      syscall(SYS_openat, task_fd, "fd",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
   int fdinfo_directory_fd =
      syscall(SYS_openat, task_fd, "fdinfo",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
   syscall(SYS_close, task_fd);
   if (fd_directory_fd < 0 || fdinfo_directory_fd < 0) {
      syscall(SYS_close, fd_directory_fd);
      syscall(SYS_close, fdinfo_directory_fd);
      return true;
   }

   bool found = false;
   char buffer[8192];
   while (!found) {
#ifdef SYS_getdents64
      long length =
         syscall(SYS_getdents64, fd_directory_fd, buffer,
                 sizeof(buffer));
#else
      long length = -1;
      errno = ENOSYS;
#endif
      if (length < 0 && errno == EINTR)
         continue;
      if (length < 0)
         return true;
      if (!length)
         break;

      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length)
            break;
         offset += entry->record_length;

         int fd;
         if (!drm_shim_parse_decimal_fd(entry->name, &fd))
            continue;
         struct stat status;
#ifdef SYS_newfstatat
         if (syscall(SYS_newfstatat, fd_directory_fd, entry->name,
                     &status, 0) < 0)
            continue;
#else
         continue;
#endif
         if (status.st_dev != shim_fd->backing_dev ||
             status.st_ino != shim_fd->backing_ino)
            continue;
         if (drm_shim_fdinfo_has_identity_lock(
                fdinfo_directory_fd, entry->name,
                shim_fd->identity_lock_offset)) {
            found = true;
            break;
         }
      }
   }

   syscall(SYS_close, fd_directory_fd);
   syscall(SYS_close, fdinfo_directory_fd);
   return found;
}

static bool
drm_shim_process_has_external_alias(const struct shim_fd *shim_fd)
{
   int task_directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/self/task",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
   if (task_directory_fd < 0)
      return true;

   bool complete = false;
   bool found = false;
   char buffer[8192];
   while (!found) {
#ifdef SYS_getdents64
      long length =
         syscall(SYS_getdents64, task_directory_fd, buffer,
                 sizeof(buffer));
#else
      long length = -1;
      errno = ENOSYS;
#endif
      if (length < 0 && errno == EINTR)
         continue;
      if (length < 0)
         break;
      if (!length) {
         complete = true;
         break;
      }
      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length)
            break;
         offset += entry->record_length;
         int task_id;
         if (!drm_shim_parse_decimal_fd(entry->name, &task_id))
            continue;
         if (drm_shim_task_has_external_alias(
                shim_fd, task_directory_fd, entry->name)) {
            found = true;
            break;
         }
      }
   }
   syscall(SYS_close, task_directory_fd);
   return found || !complete;
}

static unsigned
drm_shim_fd_remove_map_refs_locked(struct shim_fd *shim_fd)
{
   unsigned removed = 0;
   while (true) {
      struct hash_entry *matching = NULL;
      hash_table_foreach(shim_device.fd_map, entry) {
         if (entry->data == shim_fd) {
            matching = entry;
            break;
         }
      }
      if (!matching)
         return removed;
      int fd = (int)((uintptr_t)matching->key - 1);
      drm_shim_forget_non_cloexec_fd_locked(fd);
      _mesa_hash_table_remove(shim_device.fd_map, matching);
      removed++;
   }
}

void
drm_shim_fd_reap_diverged(void)
{
   if (!device_initialized)
      return;

   struct shim_fd **candidates = NULL;
   size_t candidate_count = 0;
   mtx_lock(&shim_device.lock);
   for (struct shim_fd *candidate = shim_device.diverged_fd_objects;
        candidate; candidate = candidate->next_diverged)
      candidate_count++;
   if (candidate_count)
      candidates = malloc(candidate_count * sizeof(*candidates));
   if (candidate_count && !candidates) {
      mtx_unlock(&shim_device.lock);
      return;
   }
   size_t index = 0;
   for (struct shim_fd *candidate = shim_device.diverged_fd_objects;
        candidate; candidate = candidate->next_diverged) {
      p_atomic_inc(&candidate->refcount);
      candidates[index++] = candidate;
   }
   mtx_unlock(&shim_device.lock);

   for (size_t i = 0; i < candidate_count; i++) {
      struct shim_fd *candidate = candidates[i];
      unsigned map_refs = 0;
      bool release_pin = false;
      if (!drm_shim_process_has_external_alias(candidate)) {
         mtx_lock(&shim_device.lock);
         if (candidate->diverged_pinned) {
            struct shim_fd **link = &shim_device.diverged_fd_objects;
            while (*link && *link != candidate)
               link = &(*link)->next_diverged;
            if (*link == candidate) {
               *link = candidate->next_diverged;
               candidate->next_diverged = NULL;
               candidate->diverged_pinned = false;
               map_refs =
                  drm_shim_fd_remove_map_refs_locked(candidate);
               release_pin = true;
            }
         }
         mtx_unlock(&shim_device.lock);
      }
      while (map_refs--)
         drm_shim_fd_put(candidate);
      if (release_pin)
         drm_shim_fd_put(candidate);
      drm_shim_fd_put(candidate);
   }
   free(candidates);
}

static void
drm_shim_set_descriptor_cloexec(int fd, bool cloexec)
{
   int descriptor_flags = syscall(SYS_fcntl, fd, F_GETFD);
   if (descriptor_flags < 0)
      abort();
   int updated_flags =
      cloexec ? descriptor_flags | FD_CLOEXEC
              : descriptor_flags & ~FD_CLOEXEC;
   if (updated_flags != descriptor_flags &&
       syscall(SYS_fcntl, fd, F_SETFD, updated_flags) < 0)
      abort();
}

static void
drm_shim_update_persistence_locked(struct shim_fd *shim_fd,
                                   int alias_delta)
{
   if (alias_delta > 0) {
      shim_fd->non_cloexec_aliases += (unsigned)alias_delta;
      shim_device.non_cloexec_aliases += (unsigned)alias_delta;
   } else if (alias_delta < 0) {
      unsigned decrement = (unsigned)-alias_delta;
      if (shim_fd->non_cloexec_aliases < decrement ||
          shim_device.non_cloexec_aliases < decrement)
         abort();
      shim_fd->non_cloexec_aliases -= decrement;
      shim_device.non_cloexec_aliases -= decrement;
   }
}

static void
drm_shim_forget_non_cloexec_fd_locked(int fd)
{
   void *key = (void *)(uintptr_t)(fd + 1);
   struct hash_entry *entry =
      _mesa_hash_table_search(shim_device.non_cloexec_fd_map, key);
   if (!entry)
      return;
   struct shim_fd *shim_fd = entry->data;
   _mesa_hash_table_remove(shim_device.non_cloexec_fd_map, entry);
   drm_shim_update_persistence_locked(shim_fd, -1);
}

static void
drm_shim_record_descriptor_flags_locked(int fd,
                                        struct shim_fd *shim_fd)
{
   drm_shim_forget_non_cloexec_fd_locked(fd);
   int descriptor_flags = syscall(SYS_fcntl, fd, F_GETFD);
   if (descriptor_flags < 0)
      abort();
   if (descriptor_flags & FD_CLOEXEC)
      return;
   struct hash_entry *entry =
      _mesa_hash_table_insert(
         shim_device.non_cloexec_fd_map,
         (void *)(uintptr_t)(fd + 1), shim_fd);
   if (!entry)
      abort();
   drm_shim_update_persistence_locked(shim_fd, 1);
}

/**
 * Called when the libc shims have interposed an open or dup of our simulated
 * DRM device.
 */
int
drm_shim_fd_register(int fd, struct shim_fd *shim_fd)
{
   if (fd < 0)
      return -EBADF;

   if (!shim_fd)
      shim_fd = drm_shim_file_create(fd, true);
   else
      p_atomic_inc(&shim_fd->refcount);
   if (!shim_fd)
      return errno ? -errno : -ENOMEM;

   mtx_lock(&shim_device.lock);
   struct hash_entry *old_entry =
      _mesa_hash_table_search(shim_device.fd_map,
                              (void *)(uintptr_t)(fd + 1));
   struct shim_fd *old_shim_fd = old_entry ? old_entry->data : NULL;
   struct hash_entry *new_entry =
      _mesa_hash_table_insert(shim_device.fd_map,
                              (void *)(uintptr_t)(fd + 1), shim_fd);
   if (new_entry) {
      shim_fd->fd = fd;
      drm_shim_record_descriptor_flags_locked(fd, shim_fd);
   }
   mtx_unlock(&shim_device.lock);

   if (!new_entry) {
      drm_shim_fd_put(shim_fd);
      return -ENOMEM;
   }
   if (old_shim_fd && old_shim_fd != shim_fd)
      drm_shim_file_release_posix_locks(old_shim_fd);
   drm_shim_fd_put(old_shim_fd);
   return 0;
}

static void handle_delete_fxn(struct hash_entry *entry)
{
   drm_shim_bo_put_handle(entry->data);
}

void
drm_shim_file_release_posix_locks(struct shim_fd *shim_fd)
{
   (void)shim_fd;
}

void drm_shim_fd_unregister(int fd)
{
   if (fd == -1)
      return;

   mtx_lock(&shim_device.lock);
   struct hash_entry *entry =
         _mesa_hash_table_search(shim_device.fd_map, (void *)(uintptr_t)(fd + 1));
   if (!entry) {
      mtx_unlock(&shim_device.lock);
      return;
   }
   struct shim_fd *shim_fd = entry->data;
   if (shim_fd->fd == fd) {
      shim_fd->fd = -1;
      hash_table_foreach(shim_device.fd_map, candidate) {
         int candidate_fd =
            (int)((uintptr_t)candidate->key - 1);
         if (candidate_fd != fd &&
             candidate->data == shim_fd) {
            shim_fd->fd = candidate_fd;
            break;
         }
      }
   }
   drm_shim_forget_non_cloexec_fd_locked(fd);
   _mesa_hash_table_remove(shim_device.fd_map, entry);
   mtx_unlock(&shim_device.lock);

   drm_shim_file_release_posix_locks(shim_fd);
   drm_shim_fd_put(shim_fd);
}

void
drm_shim_fd_release_posix_locks(int fd)
{
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   drm_shim_file_release_posix_locks(shim_fd);
   drm_shim_fd_put(shim_fd);
}

void
drm_shim_fd_update_cloexec(int fd)
{
   mtx_lock(&shim_device.lock);
   struct hash_entry *entry =
      _mesa_hash_table_search(
         shim_device.fd_map, (void *)(uintptr_t)(fd + 1));
   if (entry)
      drm_shim_record_descriptor_flags_locked(fd, entry->data);
   else
      drm_shim_forget_non_cloexec_fd_locked(fd);
   mtx_unlock(&shim_device.lock);
}

void
drm_shim_fd_update_cloexec_range(unsigned first_fd,
                                 unsigned last_fd)
{
   mtx_lock(&shim_device.lock);
   hash_table_foreach(shim_device.fd_map, entry) {
      unsigned fd = (unsigned)((uintptr_t)entry->key - 1);
      if (fd >= first_fd && fd <= last_fd)
         drm_shim_record_descriptor_flags_locked(
            (int)fd, entry->data);
   }
   mtx_unlock(&shim_device.lock);
}

void
drm_shim_fd_unregister_range(unsigned first_fd, unsigned last_fd)
{
   while (true) {
      struct shim_fd *shim_fd = NULL;

      mtx_lock(&shim_device.lock);
      hash_table_foreach(shim_device.fd_map, entry) {
         uintptr_t key = (uintptr_t)entry->key;
         unsigned fd = (unsigned)(key - 1);
         if (fd < first_fd || fd > last_fd)
            continue;

         shim_fd = entry->data;
         if (shim_fd->fd == (int)fd) {
            shim_fd->fd = -1;
            hash_table_foreach(shim_device.fd_map, candidate) {
               unsigned candidate_fd =
                  (unsigned)((uintptr_t)candidate->key - 1);
               if (candidate_fd != fd &&
                   (candidate_fd < first_fd ||
                    candidate_fd > last_fd) &&
                   candidate->data == shim_fd) {
                  shim_fd->fd = (int)candidate_fd;
                  break;
               }
            }
         }
         drm_shim_forget_non_cloexec_fd_locked((int)fd);
         _mesa_hash_table_remove(shim_device.fd_map, entry);
         break;
      }
      mtx_unlock(&shim_device.lock);

      if (!shim_fd)
         return;
      drm_shim_file_release_posix_locks(shim_fd);
      drm_shim_fd_put(shim_fd);
   }
}

int
drm_shim_fd_collect_internal(int **fds, size_t *count)
{
   *fds = NULL;
   *count = 0;
   if (!device_initialized)
      return 0;

   mtx_lock(&shim_device.lock);
   size_t identity_count = 1;
   hash_table_foreach(shim_device.fd_map, entry) {
      const struct shim_fd *shim_fd = entry->data;
      identity_count++;
      if (shim_fd->lock_proxy_fd >= 0)
         identity_count++;
      if (shim_fd->owns_lock_proxy_anchor &&
          shim_fd->lock_proxy_anchor_fd >= 0)
         identity_count++;
   }
   for (const struct shim_fd *shim_fd =
           shim_device.diverged_fd_objects;
        shim_fd; shim_fd = shim_fd->next_diverged) {
      identity_count++;
      if (shim_fd->lock_proxy_fd >= 0)
         identity_count++;
      if (shim_fd->owns_lock_proxy_anchor &&
          shim_fd->lock_proxy_anchor_fd >= 0)
         identity_count++;
   }
   for (const struct drm_shim_scm_pin *pin = shim_device.scm_pins;
        pin; pin = pin->next) {
      const struct shim_fd *shim_fd = pin->shim_fd;
      identity_count++;
      if (shim_fd->lock_proxy_fd >= 0)
         identity_count++;
      if (shim_fd->owns_lock_proxy_anchor &&
          shim_fd->lock_proxy_anchor_fd >= 0)
         identity_count++;
   }

   int *identity_fds =
      identity_count ? malloc(identity_count * sizeof(*identity_fds)) : NULL;
   if (identity_count && !identity_fds) {
      mtx_unlock(&shim_device.lock);
      return -ENOMEM;
   }

   size_t index = 0;
   struct stat lock_status;
   if (shim_device.lock_backing_fd >= 0 &&
       syscall(SYS_fstat, shim_device.lock_backing_fd,
               &lock_status) == 0 &&
       lock_status.st_dev == shim_device.lock_backing_dev &&
       lock_status.st_ino == shim_device.lock_backing_ino)
      identity_fds[index++] = shim_device.lock_backing_fd;
   hash_table_foreach(shim_device.fd_map, entry) {
      const struct shim_fd *shim_fd = entry->data;
      if (shim_fd->identity_fd >= 0 &&
          drm_shim_fd_backing_matches(
             shim_fd, shim_fd->identity_fd))
         identity_fds[index++] = shim_fd->identity_fd;
      if (shim_fd->lock_proxy_fd >= 0 &&
          syscall(SYS_fstat, shim_fd->lock_proxy_fd,
                  &lock_status) == 0 &&
          lock_status.st_dev == shim_device.lock_backing_dev &&
          lock_status.st_ino == shim_device.lock_backing_ino)
         identity_fds[index++] = shim_fd->lock_proxy_fd;
      if (shim_fd->owns_lock_proxy_anchor &&
          shim_fd->lock_proxy_anchor_fd >= 0 &&
          syscall(SYS_fstat, shim_fd->lock_proxy_anchor_fd,
                  &lock_status) == 0 &&
          lock_status.st_dev == shim_device.lock_backing_dev &&
          lock_status.st_ino == shim_device.lock_backing_ino)
         identity_fds[index++] = shim_fd->lock_proxy_anchor_fd;
   }
   for (const struct drm_shim_scm_pin *pin = shim_device.scm_pins;
        pin; pin = pin->next) {
      const struct shim_fd *shim_fd = pin->shim_fd;
      if (shim_fd->identity_fd >= 0 &&
          drm_shim_fd_backing_matches(
             shim_fd, shim_fd->identity_fd))
         identity_fds[index++] = shim_fd->identity_fd;
      if (shim_fd->lock_proxy_fd >= 0 &&
          syscall(SYS_fstat, shim_fd->lock_proxy_fd,
                  &lock_status) == 0 &&
          lock_status.st_dev == shim_device.lock_backing_dev &&
          lock_status.st_ino == shim_device.lock_backing_ino)
         identity_fds[index++] = shim_fd->lock_proxy_fd;
      if (shim_fd->owns_lock_proxy_anchor &&
          shim_fd->lock_proxy_anchor_fd >= 0 &&
          syscall(SYS_fstat, shim_fd->lock_proxy_anchor_fd,
                  &lock_status) == 0 &&
          lock_status.st_dev == shim_device.lock_backing_dev &&
          lock_status.st_ino == shim_device.lock_backing_ino)
         identity_fds[index++] = shim_fd->lock_proxy_anchor_fd;
   }
   for (const struct shim_fd *shim_fd =
           shim_device.diverged_fd_objects;
        shim_fd; shim_fd = shim_fd->next_diverged) {
      if (shim_fd->identity_fd >= 0 &&
          drm_shim_fd_backing_matches(
             shim_fd, shim_fd->identity_fd))
         identity_fds[index++] = shim_fd->identity_fd;
      if (shim_fd->lock_proxy_fd >= 0 &&
          syscall(SYS_fstat, shim_fd->lock_proxy_fd,
                  &lock_status) == 0 &&
          lock_status.st_dev == shim_device.lock_backing_dev &&
          lock_status.st_ino == shim_device.lock_backing_ino)
         identity_fds[index++] = shim_fd->lock_proxy_fd;
      if (shim_fd->owns_lock_proxy_anchor &&
          shim_fd->lock_proxy_anchor_fd >= 0 &&
          syscall(SYS_fstat, shim_fd->lock_proxy_anchor_fd,
                  &lock_status) == 0 &&
          lock_status.st_dev == shim_device.lock_backing_dev &&
          lock_status.st_ino == shim_device.lock_backing_ino)
         identity_fds[index++] = shim_fd->lock_proxy_anchor_fd;
   }
   mtx_unlock(&shim_device.lock);

   *fds = identity_fds;
   *count = index;
   return 0;
}

int
drm_shim_exec_locator_environment(int fd, bool enable_state,
                                  char **environment_entry)
{
   *environment_entry = NULL;
   if (!device_initialized || fd < 0)
      return -EINVAL;

   int length =
      snprintf(NULL, 0, "%s=v1%c:%d:%s:%s",
               DRM_SHIM_EXEC_LOCATOR_ENV,
               enable_state ? 'f' : 'i', fd,
               shim_device.render_instance, shim_device.driver_name);
   if (length < 0)
      return -ENOMEM;

   char *entry = malloc((size_t)length + 1);
   if (!entry)
      return -ENOMEM;
   if (snprintf(entry, (size_t)length + 1,
                "%s=v1%c:%d:%s:%s",
                DRM_SHIM_EXEC_LOCATOR_ENV,
                enable_state ? 'f' : 'i', fd,
                shim_device.render_instance,
                shim_device.driver_name) != length) {
      free(entry);
      return -ENOMEM;
   }

   *environment_entry = entry;
   return 0;
}

int
drm_shim_fd_prepare_exec(int **fds, int **descriptor_flags,
                         size_t *count, char **environment_entry)
{
   *fds = NULL;
   *descriptor_flags = NULL;
   *count = 0;
   *environment_entry = NULL;
   if (!device_initialized)
      return 0;

   int *internal_fds = NULL;
   size_t internal_count = 0;
   int collect_error =
      drm_shim_fd_collect_internal(&internal_fds, &internal_count);
   if (collect_error)
      return collect_error;

   int *saved_flags =
      internal_count
         ? malloc(internal_count * sizeof(*saved_flags))
         : NULL;
   if (internal_count && !saved_flags) {
      free(internal_fds);
      return -ENOMEM;
   }

   int locator_fd = -1;
   mtx_lock(&shim_device.lock);
   hash_table_foreach(shim_device.non_cloexec_fd_map, entry) {
      int fd = (int)((uintptr_t)entry->key - 1);
      struct shim_fd *shim_fd = entry->data;
      int descriptor_status = syscall(SYS_fcntl, fd, F_GETFD);
      int file_status = syscall(SYS_fcntl, fd, F_GETFL);
      /* Map membership already proves render identity: every shim_fd was
       * identity-parsed at registration, and a per-open state token backs
       * it rather than the anchor file.
       */
      (void)shim_fd;
      if (descriptor_status >= 0 &&
          !(descriptor_status & FD_CLOEXEC) &&
          file_status >= 0 && (file_status & O_PATH) != O_PATH) {
         locator_fd = fd;
         break;
      }
   }
   mtx_unlock(&shim_device.lock);

   size_t unique_count = 0;
   for (size_t index = 0; index < internal_count; index++) {
      bool duplicate = false;
      for (size_t previous = 0; previous < unique_count; previous++) {
         if (internal_fds[previous] == internal_fds[index]) {
            duplicate = true;
            break;
         }
      }
      if (duplicate)
         continue;
      int flags =
         syscall(SYS_fcntl, internal_fds[index], F_GETFD);
      if (flags < 0)
         continue;
      internal_fds[unique_count] = internal_fds[index];
      saved_flags[unique_count] = flags;
      drm_shim_set_descriptor_cloexec(internal_fds[index], true);
      unique_count++;
   }

   if (locator_fd >= 0) {
      int environment_error =
         drm_shim_exec_locator_environment(
            locator_fd, false, environment_entry);
      if (environment_error) {
         for (size_t index = 0; index < unique_count; index++) {
            drm_shim_set_descriptor_cloexec(
               internal_fds[index],
               saved_flags[index] & FD_CLOEXEC);
         }
         free(saved_flags);
         free(internal_fds);
         *environment_entry = NULL;
         return environment_error;
      }
   }

   *fds = internal_fds;
   *descriptor_flags = saved_flags;
   *count = unique_count;
   return 0;
}

void
drm_shim_fd_restore_exec(const int *fds,
                         const int *descriptor_flags,
                         size_t count)
{
   for (size_t index = 0; index < count; index++) {
      if (syscall(SYS_fcntl, fds[index], F_SETFD,
                  descriptor_flags[index]) < 0 &&
          errno != EBADF)
         abort();
   }
}

enum drm_shim_fd_match {
   DRM_SHIM_FD_MATCH_NO,
   DRM_SHIM_FD_MATCH_YES,
   DRM_SHIM_FD_MATCH_UNKNOWN,
};

static enum drm_shim_fd_match
drm_shim_fd_matches(const struct shim_fd *shim_fd, int fd)
{
   if (shim_fd->fd < 0)
      return DRM_SHIM_FD_MATCH_UNKNOWN;

   struct stat witness_status;
   if (syscall(SYS_fstat, shim_fd->fd, &witness_status) < 0 ||
       witness_status.st_dev != shim_fd->backing_dev ||
       witness_status.st_ino != shim_fd->backing_ino)
      return DRM_SHIM_FD_MATCH_NO;

#ifdef F_DUPFD_QUERY
   int query;
#ifdef DRM_SHIM_TEST
   if (force_duplicate_query_error) {
      query = -1;
      errno = force_duplicate_query_error;
   } else
#endif
      query =
         syscall(SYS_fcntl, shim_fd->fd, F_DUPFD_QUERY, fd);
   if (query >= 0)
      return query == 1 ? DRM_SHIM_FD_MATCH_YES
                        : DRM_SHIM_FD_MATCH_NO;
   if (errno != EINVAL && errno != ENOTTY && errno != ENOSYS &&
       errno != EPERM && errno != EACCES)
      return DRM_SHIM_FD_MATCH_NO;
#endif

#ifdef SYS_kcmp
   int result;
#ifdef DRM_SHIM_TEST
   if (force_kcmp_result) {
      result = forced_kcmp_result;
   } else if (force_kcmp_error) {
      result = -1;
      errno = force_kcmp_error;
   } else {
#endif
      pid_t caller_tid = syscall(SYS_gettid);
      result =
         syscall(SYS_kcmp, caller_tid, caller_tid, KCMP_FILE,
                 shim_fd->fd, fd);
#ifdef DRM_SHIM_TEST
   }
#endif
   if (result == 0)
      return DRM_SHIM_FD_MATCH_YES;
   if (result > 0)
      return DRM_SHIM_FD_MATCH_NO;
   if (errno == ENOSYS || errno == EPERM || errno == EACCES)
      return DRM_SHIM_FD_MATCH_UNKNOWN;
   return DRM_SHIM_FD_MATCH_NO;
#else
   return DRM_SHIM_FD_MATCH_UNKNOWN;
#endif
}

static bool
drm_shim_fd_metadata_matches(const struct shim_fd *shim_fd, int fd)
{
   struct stat status;
   return syscall(SYS_fstat, fd, &status) == 0 &&
          status.st_dev == shim_fd->backing_dev &&
          status.st_ino == shim_fd->backing_ino;
}

bool
drm_shim_fd_is_internal(int fd)
{
   if (!device_initialized || fd < 0)
      return false;

   bool internal = false;
   mtx_lock(&shim_device.lock);
   struct stat status;
   if (fd == shim_device.lock_backing_fd &&
       syscall(SYS_fstat, fd, &status) == 0 &&
       status.st_dev == shim_device.lock_backing_dev &&
       status.st_ino == shim_device.lock_backing_ino)
      internal = true;
   hash_table_foreach(shim_device.fd_map, entry) {
      const struct shim_fd *shim_fd = entry->data;
      if ((shim_fd->identity_fd == fd &&
           drm_shim_fd_backing_matches(shim_fd, fd)) ||
          ((shim_fd->lock_proxy_fd == fd ||
            (shim_fd->owns_lock_proxy_anchor &&
             shim_fd->lock_proxy_anchor_fd == fd)) &&
           syscall(SYS_fstat, fd, &status) == 0 &&
           status.st_dev == shim_device.lock_backing_dev &&
           status.st_ino == shim_device.lock_backing_ino)) {
         internal = true;
         break;
      }
   }
   if (!internal) {
      for (const struct shim_fd *shim_fd =
              shim_device.diverged_fd_objects;
           shim_fd; shim_fd = shim_fd->next_diverged) {
         if ((shim_fd->identity_fd == fd &&
              drm_shim_fd_backing_matches(shim_fd, fd)) ||
             ((shim_fd->lock_proxy_fd == fd ||
               (shim_fd->owns_lock_proxy_anchor &&
                shim_fd->lock_proxy_anchor_fd == fd)) &&
              syscall(SYS_fstat, fd, &status) == 0 &&
              status.st_dev == shim_device.lock_backing_dev &&
              status.st_ino == shim_device.lock_backing_ino)) {
            internal = true;
            break;
         }
      }
   }
   if (!internal) {
      for (const struct drm_shim_scm_pin *pin = shim_device.scm_pins;
           pin; pin = pin->next) {
         const struct shim_fd *shim_fd = pin->shim_fd;
         if ((shim_fd->identity_fd == fd &&
              drm_shim_fd_backing_matches(shim_fd, fd)) ||
             ((shim_fd->lock_proxy_fd == fd ||
               (shim_fd->owns_lock_proxy_anchor &&
                shim_fd->lock_proxy_anchor_fd == fd)) &&
              syscall(SYS_fstat, fd, &status) == 0 &&
              status.st_dev == shim_device.lock_backing_dev &&
              status.st_ino == shim_device.lock_backing_ino)) {
            internal = true;
            break;
         }
      }
   }
   mtx_unlock(&shim_device.lock);
   return internal;
}

static bool
drm_shim_fd_backing_matches(const struct shim_fd *shim_fd, int fd)
{
   (void)shim_fd;
   (void)fd;
   return false;
}

static struct shim_fd *
drm_shim_fd_find_locked(int fd, struct shim_fd **replaced_out)
{
   struct hash_entry *direct =
      _mesa_hash_table_search(shim_device.fd_map,
                              (void *)(uintptr_t)(fd + 1));
   struct shim_fd *direct_shim_fd = direct ? direct->data : NULL;
   enum drm_shim_fd_match direct_match = DRM_SHIM_FD_MATCH_NO;
   if (direct) {
      direct_match = drm_shim_fd_matches(direct->data, fd);
      if (direct_match == DRM_SHIM_FD_MATCH_YES)
         return direct_shim_fd;
      if (direct_match == DRM_SHIM_FD_MATCH_UNKNOWN) {
         if (direct_shim_fd->path_only &&
             drm_shim_fd_metadata_matches(direct_shim_fd, fd))
            return direct_shim_fd;
         errno = EOPNOTSUPP;
         return NULL;
      }
   }
   bool direct_is_stale =
      direct && direct_match == DRM_SHIM_FD_MATCH_NO;

   struct shim_fd *result = NULL;
   hash_table_foreach(shim_device.fd_map, entry) {
      struct shim_fd *candidate = entry->data;
      if (candidate == direct_shim_fd)
         continue;
      enum drm_shim_fd_match candidate_match =
         drm_shim_fd_matches(candidate, fd);
      if (candidate_match == DRM_SHIM_FD_MATCH_YES) {
         result = candidate;
         break;
      }
   }
   if (!result) {
      for (struct shim_fd *candidate =
              shim_device.diverged_fd_objects;
           candidate; candidate = candidate->next_diverged) {
         if (candidate == direct_shim_fd)
            continue;
         if (drm_shim_fd_matches(candidate, fd) ==
             DRM_SHIM_FD_MATCH_YES) {
            result = candidate;
            break;
         }
      }
   }
   if (!result) {
      for (struct drm_shim_scm_pin *pin = shim_device.scm_pins;
           pin; pin = pin->next) {
         struct shim_fd *candidate = pin->shim_fd;
         if (candidate == direct_shim_fd)
            continue;
         if (drm_shim_fd_matches(candidate, fd) ==
             DRM_SHIM_FD_MATCH_YES) {
            result = candidate;
            break;
         }
      }
   }
   if (!result) {
      if (direct_is_stale) {
         drm_shim_forget_non_cloexec_fd_locked(fd);
         _mesa_hash_table_remove(shim_device.fd_map, direct);
         *replaced_out = direct_shim_fd;
      }
      return NULL;
   }

   p_atomic_inc(&result->refcount);
   struct hash_entry *new_entry =
      _mesa_hash_table_insert(shim_device.fd_map,
                              (void *)(uintptr_t)(fd + 1), result);
   if (!new_entry) {
      p_atomic_dec(&result->refcount);
      return NULL;
   }
   drm_shim_record_descriptor_flags_locked(fd, result);
   *replaced_out = direct_shim_fd;
   return result;
}

bool
drm_shim_fd_reports_selected_device(int fd)
{
   struct drm_shim_render_identity identity;
   return drm_shim_render_identity_parse(fd, &identity);
}

static int
drm_shim_fd_register_detected(int fd, bool enable_state)
{
   struct shim_fd *new_shim_fd =
      drm_shim_file_create(fd, enable_state);
   if (!new_shim_fd)
      return errno ? -errno : -ENOMEM;

#ifdef DRM_SHIM_TEST
   if (p_atomic_read(&fd_discovery_barrier_remaining) > 0) {
      char byte = 1;
      ssize_t written;
      do {
         written =
            syscall(SYS_write, fd_discovery_barrier_ready_fd,
                    &byte, sizeof(byte));
      } while (written < 0 && errno == EINTR);
      ssize_t length;
      do {
         length =
            syscall(SYS_read, fd_discovery_barrier_release_fd,
                    &byte, sizeof(byte));
      } while (length < 0 && errno == EINTR);
      if (p_atomic_dec_zero(&fd_discovery_barrier_remaining)) {
         fd_discovery_barrier_ready_fd = -1;
         fd_discovery_barrier_release_fd = -1;
      }
   }
#endif

   struct shim_fd *replaced = NULL;
   mtx_lock(&shim_device.lock);
   struct shim_fd *existing =
      drm_shim_fd_find_locked(fd, &replaced);
   if (existing) {
      mtx_unlock(&shim_device.lock);
      drm_shim_fd_put(replaced);
      drm_shim_fd_put(new_shim_fd);
      return 0;
   }

   struct hash_entry *old_entry =
      _mesa_hash_table_search(shim_device.fd_map,
                              (void *)(uintptr_t)(fd + 1));
   struct shim_fd *old_shim_fd = old_entry ? old_entry->data : NULL;
   struct hash_entry *new_entry =
      _mesa_hash_table_insert(shim_device.fd_map,
                              (void *)(uintptr_t)(fd + 1),
                              new_shim_fd);
   if (new_entry) {
      new_shim_fd->fd = fd;
      drm_shim_record_descriptor_flags_locked(fd, new_shim_fd);
   }
   mtx_unlock(&shim_device.lock);

   if (!new_entry) {
      drm_shim_fd_put(new_shim_fd);
      return -ENOMEM;
   }
   if (old_shim_fd && old_shim_fd != new_shim_fd)
      drm_shim_file_release_posix_locks(old_shim_fd);
   drm_shim_fd_put(old_shim_fd);
   return 0;
}

void
drm_shim_fd_scan_inherited(void)
{
   int directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/fd",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
   if (directory_fd < 0)
      abort();

   char buffer[4096];
   while (true) {
      long length =
         syscall(SYS_getdents64, directory_fd, buffer, sizeof(buffer));
      if (length < 0 && errno == EINTR)
         continue;
      if (length < 0)
         abort();
      if (length == 0)
         break;

      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length ||
             offset + entry->record_length > length)
            abort();
         offset += entry->record_length;

         char *end;
         errno = 0;
         long fd_value = strtol(entry->name, &end, 10);
         if (errno || *entry->name == '\0' || *end != '\0' ||
             fd_value < 0 || fd_value > INT_MAX ||
             fd_value == directory_fd)
            continue;
         int fd = (int)fd_value;
         struct drm_shim_render_identity identity;
         if (!drm_shim_render_identity_parse(fd, &identity))
            continue;
         bool enable_state =
            fd == shim_device.exec_locator_fd &&
            shim_device.exec_locator_enables_state;
         int ret =
            drm_shim_fd_register_detected(fd, enable_state);
         if (ret)
            abort();
      }
   }
   syscall(SYS_close, directory_fd);
   shim_device.exec_locator_fd = -1;
   shim_device.exec_locator_enables_state = false;
}

int
drm_shim_file_pin_scm(struct shim_fd *shim_fd,
                      uint64_t receiver_cookie)
{
   if (!shim_fd || !shim_fd->state_available ||
       shim_fd->owner_pid != getpid())
      return 0;

   struct drm_shim_scm_pin *pin = malloc(sizeof(*pin));
   if (!pin)
      return -ENOMEM;
   pin->receiver_cookie = receiver_cookie;
   pin->shim_fd = shim_fd;

   mtx_lock(&shim_device.lock);
   if (!shim_fd->state_available ||
       shim_fd->owner_pid != getpid()) {
      mtx_unlock(&shim_device.lock);
      free(pin);
      return 0;
   }
   p_atomic_inc(&shim_fd->refcount);
   pin->next = shim_device.scm_pins;
   shim_device.scm_pins = pin;
   mtx_unlock(&shim_device.lock);
   return 0;
}

void
drm_shim_file_unpin_scm(struct shim_fd *shim_fd,
                        uint64_t receiver_cookie)
{
   if (!shim_fd)
      return;

   struct drm_shim_scm_pin *removed = NULL;
   mtx_lock(&shim_device.lock);
   struct drm_shim_scm_pin **link = &shim_device.scm_pins;
   while (*link) {
      if ((*link)->shim_fd == shim_fd &&
          (!receiver_cookie ||
           (*link)->receiver_cookie == receiver_cookie)) {
         removed = *link;
         *link = removed->next;
         break;
      }
      link = &(*link)->next;
   }
   mtx_unlock(&shim_device.lock);

   if (removed) {
      drm_shim_fd_put(removed->shim_fd);
      free(removed);
   }
}

void
drm_shim_scm_drop_receiver(uint64_t receiver_cookie)
{
   if (!receiver_cookie)
      return;

   struct drm_shim_scm_pin *removed = NULL;
   mtx_lock(&shim_device.lock);
   struct drm_shim_scm_pin **link = &shim_device.scm_pins;
   while (*link) {
      if ((*link)->receiver_cookie != receiver_cookie) {
         link = &(*link)->next;
         continue;
      }
      struct drm_shim_scm_pin *pin = *link;
      *link = pin->next;
      pin->next = removed;
      removed = pin;
   }
   mtx_unlock(&shim_device.lock);

   while (removed) {
      struct drm_shim_scm_pin *next = removed->next;
      drm_shim_fd_put(removed->shim_fd);
      free(removed);
      removed = next;
   }
}

void
drm_shim_fd_adopt_raw_aliases(int fd)
{
   struct shim_fd *source = drm_shim_fd_get(fd);
   if (!source || source->path_only)
      goto out;

   int directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/fd",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
   if (directory_fd < 0)
      goto out;

   char buffer[4096];
   while (true) {
      long length =
         syscall(SYS_getdents64, directory_fd, buffer, sizeof(buffer));
      if (length < 0 && errno == EINTR)
         continue;
      if (length <= 0)
         break;

      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length ||
             offset + entry->record_length > length)
            abort();
         offset += entry->record_length;

         char *end;
         errno = 0;
         long candidate_value = strtol(entry->name, &end, 10);
         if (errno || *entry->name == '\0' || *end != '\0' ||
             candidate_value < 0 || candidate_value > INT_MAX)
            continue;
         int candidate = (int)candidate_value;
         if (candidate == fd || candidate == directory_fd ||
             candidate == source->identity_fd ||
             candidate == source->lock_proxy_fd ||
             candidate == source->lock_proxy_anchor_fd ||
             candidate == shim_device.lock_backing_fd ||
             drm_shim_fd_is_internal(candidate))
            continue;

         mtx_lock(&shim_device.lock);
         struct hash_entry *existing =
            _mesa_hash_table_search(
               shim_device.fd_map,
               (void *)(uintptr_t)(candidate + 1));
         enum drm_shim_fd_match candidate_match =
            drm_shim_fd_matches(source, candidate);
         bool still_same =
            candidate_match == DRM_SHIM_FD_MATCH_YES ||
            (candidate_match == DRM_SHIM_FD_MATCH_UNKNOWN &&
             drm_shim_fd_backing_matches(source, candidate));
         if (!existing && still_same) {
            p_atomic_inc(&source->refcount);
            struct hash_entry *inserted =
               _mesa_hash_table_insert(
                  shim_device.fd_map,
                  (void *)(uintptr_t)(candidate + 1), source);
            if (!inserted)
               abort();
            drm_shim_record_descriptor_flags_locked(candidate, source);
         }
         mtx_unlock(&shim_device.lock);
      }
   }
   syscall(SYS_close, directory_fd);

out:
   drm_shim_fd_put(source);
}

int
drm_shim_fd_adopt_raw_aliases_range(unsigned first_fd,
                                    unsigned last_fd)
{
   size_t count = 0;
   mtx_lock(&shim_device.lock);
   hash_table_foreach(shim_device.fd_map, entry) {
      unsigned fd = (unsigned)((uintptr_t)entry->key - 1);
      if (fd >= first_fd && fd <= last_fd)
         count++;
   }
   if (!count) {
      mtx_unlock(&shim_device.lock);
      return 0;
   }

   int *fds = malloc(count * sizeof(*fds));
   if (!fds) {
      mtx_unlock(&shim_device.lock);
      return -ENOMEM;
   }
   size_t index = 0;
   hash_table_foreach(shim_device.fd_map, entry) {
      unsigned fd = (unsigned)((uintptr_t)entry->key - 1);
      if (fd >= first_fd && fd <= last_fd)
         fds[index++] = (int)fd;
   }
   mtx_unlock(&shim_device.lock);

   for (size_t fd_index = 0; fd_index < index; fd_index++)
      drm_shim_fd_adopt_raw_aliases(fds[fd_index]);
   free(fds);
   return 0;
}

struct shim_fd *
drm_shim_fd_lookup(int fd)
{
   if (fd == dispatch_fd)
      return dispatch_shim_fd;

   struct shim_fd *result = drm_shim_fd_get(fd);
   drm_shim_fd_put(result);
   return result;
}

struct shim_fd *
drm_shim_fd_get(int fd)
{
   if (!drm_shim_inited() || fd < 0)
      return NULL;

retry:
   mtx_lock(&shim_device.lock);
   errno = 0;
   struct shim_fd *replaced = NULL;
   struct shim_fd *result =
      drm_shim_fd_find_locked(fd, &replaced);
   int discovery_error = errno;
   if (result)
      p_atomic_inc(&result->refcount);
   mtx_unlock(&shim_device.lock);
   drm_shim_fd_put(replaced);
   if (replaced)
      drm_shim_fd_reap_diverged();

   if (!result && discovery_error != EOPNOTSUPP &&
       drm_shim_fd_register_detected(fd, false) == 0)
      goto retry;

   if (!result && discovery_error == EOPNOTSUPP)
      errno = discovery_error;
   return result;
}

/* ioctl used by drmGetVersion() */
static int
drm_shim_ioctl_version(int fd, unsigned long request, void *arg)
{
   struct drm_version *args = arg;
   const char *date = "20190320";
   const char *desc = "shim";

   args->version_major = shim_device.version_major;
   args->version_minor = shim_device.version_minor;
   args->version_patchlevel = shim_device.version_patchlevel;

   if (args->name)
      strncpy(args->name, shim_device.driver_name, args->name_len);
   if (args->date)
      strncpy(args->date, date, args->date_len);
   if (args->desc)
      strncpy(args->desc, desc, args->desc_len);
   args->name_len = strlen(shim_device.driver_name);
   args->date_len = strlen(date);
   args->desc_len = strlen(desc);

   return 0;
}

static int
drm_shim_ioctl_get_unique(int fd, unsigned long request, void *arg)
{
   struct drm_unique *gu = arg;

   if (gu->unique && shim_device.unique)
      strncpy(gu->unique, shim_device.unique, gu->unique_len);
   gu->unique_len = shim_device.unique ? strlen(shim_device.unique) : 0;

   return 0;
}

static int
drm_shim_ioctl_get_cap(int fd, unsigned long request, void *arg)
{
   struct drm_get_cap *gc = arg;

   switch (gc->capability) {
   case DRM_CAP_PRIME:
   case DRM_CAP_SYNCOBJ:
   case DRM_CAP_SYNCOBJ_TIMELINE:
   case DRM_CAP_ADDFB2_MODIFIERS:
      gc->value = 1;
      return 0;

   default:
      fprintf(stderr, "DRM_IOCTL_GET_CAP: unhandled 0x%x\n",
              (int)gc->capability);
      return -EINVAL;
   }
}

static int
drm_shim_ioctl_gem_close(int fd, unsigned long request, void *arg)
{
   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct drm_gem_close *c = arg;

   if (!c->handle)
      return 0;

   mtx_lock(&shim_fd->handle_lock);
   struct hash_entry *entry =
      _mesa_hash_table_search(shim_fd->handles, (void *)(uintptr_t)c->handle);
   if (!entry) {
      mtx_unlock(&shim_fd->handle_lock);
      return -EINVAL;
   }

   struct shim_bo *bo = entry->data;
   _mesa_hash_table_remove(shim_fd->handles, entry);
   drm_shim_bo_put_handle(bo);
   mtx_unlock(&shim_fd->handle_lock);
   return 0;
}

static int
drm_shim_ioctl_syncobj_create(int fd, unsigned long request, void *arg)
{
   struct drm_syncobj_create *create = arg;

   create->handle = 1; /* 0 is invalid */

   return 0;
}

static int
drm_shim_ioctl_stub(int fd, unsigned long request, void *arg)
{
   return 0;
}

ioctl_fn_t core_ioctls[] = {
   [_IOC_NR(DRM_IOCTL_VERSION)] = drm_shim_ioctl_version,
   [_IOC_NR(DRM_IOCTL_GET_UNIQUE)] = drm_shim_ioctl_get_unique,
   [_IOC_NR(DRM_IOCTL_GET_CAP)] = drm_shim_ioctl_get_cap,
   [_IOC_NR(DRM_IOCTL_GEM_CLOSE)] = drm_shim_ioctl_gem_close,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_CREATE)] = drm_shim_ioctl_syncobj_create,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_DESTROY)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_WAIT)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_TRANSFER)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_RESET)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT)] = drm_shim_ioctl_stub,
   [_IOC_NR(DRM_IOCTL_SYNCOBJ_QUERY)] = drm_shim_ioctl_stub,
};

/**
 * Implements the GEM core ioctls, and calls into driver-specific ioctls.
 */
int
drm_shim_ioctl(struct shim_fd *shim_fd, int fd, unsigned long request,
               void *arg)
{
   int nr = _IOC_NR(request);
   if ((!shim_fd->state_available ||
        shim_fd->owner_pid != getpid()) &&
       nr != _IOC_NR(DRM_IOCTL_VERSION) &&
       nr != _IOC_NR(DRM_IOCTL_GET_UNIQUE) &&
       nr != _IOC_NR(DRM_IOCTL_GET_CAP))
      return -EOPNOTSUPP;

   int previous_dispatch_fd = dispatch_fd;
   struct shim_fd *previous_dispatch_shim_fd = dispatch_shim_fd;
   dispatch_fd = fd;
   dispatch_shim_fd = shim_fd;

   ASSERTED int type = _IOC_TYPE(request);
   assert(type == DRM_IOCTL_BASE);

   int ret = -EINVAL;
   if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
      int driver_nr = nr - DRM_COMMAND_BASE;

      if (driver_nr < shim_device.driver_ioctl_count &&
          shim_device.driver_ioctls[driver_nr]) {
         ret = shim_device.driver_ioctls[driver_nr](fd, request, arg);
         goto out;
      }
   } else {
      if (nr < ARRAY_SIZE(core_ioctls) && core_ioctls[nr]) {
         ret = core_ioctls[nr](fd, request, arg);
         goto out;
      }
   }

   if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
      fprintf(stderr,
              "DRM_SHIM: unhandled driver DRM ioctl %d (0x%x) (0x%08lx)\n",
              nr - DRM_COMMAND_BASE, nr - DRM_COMMAND_BASE, request);
   } else {
      fprintf(stderr,
              "DRM_SHIM: unhandled core DRM ioctl 0x%X (0x%08lx)\n",
              nr, request);
   }

out:
   dispatch_fd = previous_dispatch_fd;
   dispatch_shim_fd = previous_dispatch_shim_fd;
   return ret;
}

int
drm_shim_bo_init(struct shim_bo *bo, size_t size)
{
   if (!size || size > UINT32_MAX ||
       size > SHIM_MEM_SIZE - (uint64_t)shim_page_size)
      return !size ? -EINVAL : -ENOSPC;

   mtx_lock(&shim_device.lock);
   bo->mem_addr =
      util_vma_heap_alloc(&shim_device.mem_heap, size, shim_page_size);
   mtx_unlock(&shim_device.lock);

   if (!bo->mem_addr)
      return -ENOSPC;

   bo->size = (uint32_t)size;
   bo->owner_pid = getpid();
   p_atomic_set(&bo->refcount, 1);
   p_atomic_inc(&live_bos);

   return 0;
}

struct shim_bo *
drm_shim_bo_lookup(struct shim_fd *shim_fd, uint32_t handle)
{
   if (!handle || handle > INT_MAX)
      return NULL;

   mtx_lock(&shim_fd->handle_lock);
   struct hash_entry *entry =
      _mesa_hash_table_search(shim_fd->handles, (void *)(uintptr_t)handle);
   struct shim_bo *bo = entry ? entry->data : NULL;
   if (bo)
      p_atomic_inc(&bo->refcount);
   mtx_unlock(&shim_fd->handle_lock);

   return bo;
}

void
drm_shim_bo_get(struct shim_bo *bo)
{
   p_atomic_inc(&bo->refcount);
}

void
drm_shim_bo_put(struct shim_bo *bo)
{
   mtx_lock(&shim_device.lock);
   if (p_atomic_dec_return(&bo->refcount) != 0 ||
       bo->owner_pid != getpid()) {
      mtx_unlock(&shim_device.lock);
      return;
   }
   if (bo->mmap_offset &&
       _mesa_hash_table_u64_search(shim_device.offset_map,
                                   bo->mmap_offset) == bo)
      _mesa_hash_table_u64_remove(shim_device.offset_map, bo->mmap_offset);
   util_vma_heap_free(&shim_device.mem_heap, bo->mem_addr, bo->size);
   mtx_unlock(&shim_device.lock);

   if (shim_device.driver_bo_free)
      shim_device.driver_bo_free(bo);

   if (bo->mmap_offset) {
      drm_shim_backing_destroy(bo->mmap_offset);
      p_atomic_dec(&live_bo_backing_files);
   }
   p_atomic_dec(&live_bos);
   free(bo);
}

static void
drm_shim_bo_put_handle(struct shim_bo *bo)
{
   uint64_t mmap_offset = 0;
   mtx_lock(&shim_device.lock);
   if (p_atomic_dec_zero(&bo->handle_count) && bo->mmap_offset) {
      mmap_offset = bo->mmap_offset;
      if (_mesa_hash_table_u64_search(shim_device.offset_map,
                                      mmap_offset) == bo)
         _mesa_hash_table_u64_remove(shim_device.offset_map,
                                     mmap_offset);
      bo->mmap_offset = 0;
   }
   mtx_unlock(&shim_device.lock);

   if (mmap_offset) {
      drm_shim_backing_destroy(mmap_offset);
      p_atomic_dec(&live_bo_backing_files);
   }
   drm_shim_bo_put(bo);
}

int
drm_shim_bo_get_handle(struct shim_fd *shim_fd, struct shim_bo *bo)
{
   /* We should probably have some real datastructure for finding the free
    * number.
    */
   mtx_lock(&shim_fd->handle_lock);
   for (int new_handle = 1; ; new_handle++) {
      void *key = (void *)(uintptr_t)new_handle;
      if (!_mesa_hash_table_search(shim_fd->handles, key)) {
         drm_shim_bo_get(bo);
         _mesa_hash_table_insert(shim_fd->handles, key, bo);
         p_atomic_inc(&bo->handle_count);
         mtx_unlock(&shim_fd->handle_lock);
         return new_handle;
      }
   }
   mtx_unlock(&shim_fd->handle_lock);

   return 0;
}

/* Creates an mmap offset for the BO in the DRM fd.
 */
int
drm_shim_bo_get_mmap_offset(struct shim_fd *shim_fd, struct shim_bo *bo,
                            uint64_t *offset)
{
   (void)shim_fd;
   mtx_lock(&shim_device.lock);
   if (!bo->mmap_offset) {
      uint64_t allocation_size = align64(bo->size, shim_page_size);
      if (!allocation_size)
         allocation_size = shim_page_size;
      int ret;
      do {
         if (allocation_size >
             INT64_MAX - shim_device.next_mmap_offset) {
            mtx_unlock(&shim_device.lock);
            return -ENOSPC;
         }

         uint64_t candidate_offset = shim_device.next_mmap_offset;
         shim_device.next_mmap_offset += allocation_size;
         ret = drm_shim_backing_create(candidate_offset, bo->size);
         if (!ret)
            bo->mmap_offset = candidate_offset;
      } while (ret == -EEXIST);
      if (ret) {
         mtx_unlock(&shim_device.lock);
         return ret;
      }
      p_atomic_inc(&live_bo_backing_files);
      _mesa_hash_table_u64_insert(shim_device.offset_map, bo->mmap_offset,
                                  bo);
   }
   *offset = bo->mmap_offset;
   mtx_unlock(&shim_device.lock);

   return 0;
}

void
drm_shim_init_iomem_region(off64_t offset, size_t size,
                           void *(*mmap_handler)(size_t, int, int, off64_t))
{
   shim_device.iomem_region.mmap = mmap_handler;
   shim_device.iomem_region.start = offset;
   shim_device.iomem_region.size = size;
}

void
drm_shim_set_mem_addr_range(uint64_t start, uint64_t end)
{
   assert(start < end);
   assert(start % (uint64_t)shim_page_size == 0);
   assert(end % (uint64_t)shim_page_size == 0);

   util_vma_heap_finish(&shim_device.mem_heap);
   util_vma_heap_init(&shim_device.mem_heap, start, end - start);
}

static bool
mapping_range(void *address, size_t length, uintptr_t *start,
              uintptr_t *end)
{
   *start = (uintptr_t)address;
   if (!length || length > UINTPTR_MAX - *start)
      return false;
   *end = *start + length;
   return true;
}

void
drm_shim_mapping_remove(void *address, size_t length)
{
   uintptr_t remove_start;
   uintptr_t remove_end;
   if (!mapping_range(address, length, &remove_start, &remove_end))
      return;

   while (true) {
      struct shim_mapping *removed = NULL;
      mtx_lock(&shim_device.lock);
      struct shim_mapping **link = &shim_device.mappings;
      while (*link) {
         struct shim_mapping *mapping = *link;
         if (remove_end <= mapping->start ||
             remove_start >= mapping->end) {
            link = &mapping->next;
            continue;
         }

         if (remove_start <= mapping->start &&
             remove_end >= mapping->end) {
            *link = mapping->next;
            removed = mapping;
            break;
         }
         if (remove_start <= mapping->start) {
            mapping->start = remove_end;
            break;
         }
         if (remove_end >= mapping->end) {
            mapping->end = remove_start;
            break;
         }

         struct shim_mapping *right = malloc(sizeof(*right));
         if (!right)
            abort();
         right->start = remove_end;
         right->end = mapping->end;
         right->bo = mapping->bo;
         p_atomic_inc(&right->bo->refcount);
         right->next = mapping->next;
         mapping->end = remove_start;
         mapping->next = right;
         break;
      }
      mtx_unlock(&shim_device.lock);

      if (!removed)
         return;
      drm_shim_bo_put(removed->bo);
      free(removed);
   }
}

struct shim_bo *
drm_shim_mapping_get(void *address, size_t length)
{
   uintptr_t lookup_start;
   uintptr_t lookup_end;
   if (!mapping_range(address, length, &lookup_start, &lookup_end))
      return NULL;

   struct shim_bo *bo = NULL;
   mtx_lock(&shim_device.lock);
   for (struct shim_mapping *mapping = shim_device.mappings;
        mapping; mapping = mapping->next) {
      if (lookup_start >= mapping->start &&
          lookup_end <= mapping->end) {
         bo = mapping->bo;
         p_atomic_inc(&bo->refcount);
         break;
      }
   }
   mtx_unlock(&shim_device.lock);
   return bo;
}

void
drm_shim_mapping_replace(struct shim_bo *bo, void *address,
                         size_t length)
{
   uintptr_t mapping_start;
   uintptr_t mapping_end;
   if (!mapping_range(address, length, &mapping_start, &mapping_end))
      return;

   drm_shim_mapping_remove(address, length);

   struct shim_mapping *mapping = malloc(sizeof(*mapping));
   if (!mapping)
      abort();
   mapping->start = mapping_start;
   mapping->end = mapping_end;
   mapping->bo = bo;
   drm_shim_bo_get(bo);

   mtx_lock(&shim_device.lock);
   mapping->next = shim_device.mappings;
   shim_device.mappings = mapping;
   mtx_unlock(&shim_device.lock);
}

/* For mmap() on the DRM fd, look up the BO from the "offset" and map the BO's
 * fd.
 */
void *
drm_shim_mmap(struct shim_fd *shim_fd, void *addr, size_t length, int prot,
              int flags, int fd, off64_t offset)
{
   if (!shim_fd->state_available ||
       shim_fd->owner_pid != getpid()) {
      errno = EOPNOTSUPP;
      return MAP_FAILED;
   }

   if (shim_fd->path_only) {
      errno = EBADF;
      return MAP_FAILED;
   }

   if (shim_device.iomem_region.mmap &&
       offset >= shim_device.iomem_region.start &&
       offset + length <= shim_device.iomem_region.start + shim_device.iomem_region.size) {
      return shim_device.iomem_region.mmap(length, prot, flags, offset);
   }

   mtx_lock(&shim_device.lock);
   struct shim_bo *bo = _mesa_hash_table_u64_search(shim_device.offset_map, offset);
   if (bo)
      drm_shim_bo_get(bo);
   mtx_unlock(&shim_device.lock);

   if (!bo) {
      errno = EINVAL;
      return MAP_FAILED;
   }

   if (length > bo->size) {
      drm_shim_bo_put(bo);
      errno = EINVAL;
      return MAP_FAILED;
   }

   /* The offset we pass to mmap must be aligned to the page size */
   assert((bo->mem_addr & (shim_page_size - 1)) == 0);

   void *mapping =
      drm_shim_backing_map(bo->mmap_offset, addr, length, prot, flags);
   if (mapping != MAP_FAILED)
      drm_shim_mapping_replace(bo, mapping, length);
   drm_shim_bo_put(bo);
   return mapping;
}
