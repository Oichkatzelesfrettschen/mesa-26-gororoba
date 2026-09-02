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

/**
 * @file
 *
 * Implements wrappers of libc functions to fake having a DRM device that
 * isn't actually present in the kernel.
 */

/* Prevent glibc from defining open64 when we want to alias it. */
#undef _FILE_OFFSET_BITS
#undef _TIME_BITS
#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <stdarg.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <c11/threads.h>
#include <linux/memfd.h>
#include <drm-uapi/drm.h>

#include "util/set.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"
#include "drm_shim.h"
#include "drm_shim_fcntl_lock.h"

#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif
#ifndef CLOSE_RANGE_UNSHARE
#define CLOSE_RANGE_UNSHARE (1U << 1)
#endif

#define REAL_FUNCTION_POINTER(x) __typeof__(x) *real_##x

static simple_mtx_t shim_lock = SIMPLE_MTX_INITIALIZER;
static simple_mtx_t fd_operation_lock = SIMPLE_MTX_INITIALIZER;
static thread_local unsigned fd_operation_depth;
static pid_t shim_interposition_pid;
struct set *opendir_set;
bool drm_shim_debug;

struct fd_operation_guard {
   int previous_cancel_state;
};

struct cancellation_guard {
   int previous_cancel_state;
};

static void
cancellation_guard_acquire(struct cancellation_guard *guard)
{
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
                              &guard->previous_cancel_state) != 0)
      abort();
}

static void
cancellation_guard_release(struct cancellation_guard *guard)
{
   if (pthread_setcancelstate(guard->previous_cancel_state, NULL) != 0)
      abort();
}

static void
fd_operation_guard_acquire(struct fd_operation_guard *guard)
{
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
                              &guard->previous_cancel_state) != 0)
      abort();
   if (fd_operation_depth++ == 0)
      simple_mtx_lock(&fd_operation_lock);
}

static void
fd_operation_guard_release(struct fd_operation_guard *guard)
{
   if (!fd_operation_depth)
      abort();
   if (--fd_operation_depth == 0)
      simple_mtx_unlock(&fd_operation_lock);
   if (pthread_setcancelstate(guard->previous_cancel_state, NULL) != 0)
      abort();
}

static void
fd_operation_discover_fd(int fd)
{
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   drm_shim_fd_put(shim_fd);
}

/* If /dev/dri doesn't exist, we'll need an arbitrary pointer that wouldn't be
 * returned by any other opendir() call so we can return just our fake node.
 */
DIR *fake_dev_dri = (void *)&opendir_set;

REAL_FUNCTION_POINTER(access);
REAL_FUNCTION_POINTER(close);
REAL_FUNCTION_POINTER(closedir);
REAL_FUNCTION_POINTER(dup);
REAL_FUNCTION_POINTER(dup2);
REAL_FUNCTION_POINTER(dup3);
REAL_FUNCTION_POINTER(euidaccess);
REAL_FUNCTION_POINTER(execl);
REAL_FUNCTION_POINTER(execle);
REAL_FUNCTION_POINTER(execlp);
REAL_FUNCTION_POINTER(execv);
REAL_FUNCTION_POINTER(execve);
REAL_FUNCTION_POINTER(execveat);
REAL_FUNCTION_POINTER(execvp);
REAL_FUNCTION_POINTER(execvpe);
REAL_FUNCTION_POINTER(faccessat);
REAL_FUNCTION_POINTER(fexecve);
REAL_FUNCTION_POINTER(fclose);
REAL_FUNCTION_POINTER(fcntl);
REAL_FUNCTION_POINTER(fdopendir);
REAL_FUNCTION_POINTER(fopen);
REAL_FUNCTION_POINTER(flock);
REAL_FUNCTION_POINTER(freopen);
REAL_FUNCTION_POINTER(fstatat);
REAL_FUNCTION_POINTER(fstatat64);
REAL_FUNCTION_POINTER(ioctl);
REAL_FUNCTION_POINTER(lstat);
REAL_FUNCTION_POINTER(lstat64);
REAL_FUNCTION_POINTER(lockf);
REAL_FUNCTION_POINTER(lockf64);
REAL_FUNCTION_POINTER(lseek);
REAL_FUNCTION_POINTER(lseek64);
REAL_FUNCTION_POINTER(mmap);
REAL_FUNCTION_POINTER(mmap64);
REAL_FUNCTION_POINTER(mremap);
REAL_FUNCTION_POINTER(munmap);
REAL_FUNCTION_POINTER(unshare);
REAL_FUNCTION_POINTER(open);
static int (*real_openat)(int, const char *, int, ...);
REAL_FUNCTION_POINTER(opendir);
REAL_FUNCTION_POINTER(posix_spawn);
REAL_FUNCTION_POINTER(posix_spawnp);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_init);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_destroy);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_addclose);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_adddup2);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_addopen);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_addchdir_np);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_addfchdir_np);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_addclosefrom_np);
REAL_FUNCTION_POINTER(posix_spawn_file_actions_addtcsetpgrp_np);
REAL_FUNCTION_POINTER(read);
REAL_FUNCTION_POINTER(readv);
REAL_FUNCTION_POINTER(readdir);
static int (*real_readdir_r)(DIR *restrict, struct dirent *restrict,
                             struct dirent **restrict);
REAL_FUNCTION_POINTER(readdir64);
static int (*real_readdir64_r)(DIR *restrict, struct dirent64 *restrict,
                               struct dirent64 **restrict);
REAL_FUNCTION_POINTER(scandir);
REAL_FUNCTION_POINTER(scandir64);
REAL_FUNCTION_POINTER(scandirat);
REAL_FUNCTION_POINTER(scandirat64);
REAL_FUNCTION_POINTER(getsockopt);
REAL_FUNCTION_POINTER(recv);
REAL_FUNCTION_POINTER(recvfrom);
REAL_FUNCTION_POINTER(sendmsg);
REAL_FUNCTION_POINTER(sendmmsg);
REAL_FUNCTION_POINTER(send);
REAL_FUNCTION_POINTER(sendto);
REAL_FUNCTION_POINTER(recvmsg);
REAL_FUNCTION_POINTER(recvmmsg);
REAL_FUNCTION_POINTER(socketpair);
REAL_FUNCTION_POINTER(write);
REAL_FUNCTION_POINTER(writev);
REAL_FUNCTION_POINTER(readlink);
REAL_FUNCTION_POINTER(readlinkat);
REAL_FUNCTION_POINTER(realpath);
REAL_FUNCTION_POINTER(statx);
static int (*real_openat2)(int, const char *, const struct open_how *, size_t);
static ssize_t copy_readlink_result(char *buffer, size_t size,
                                    const char *target);

#ifdef __GLIBC__
PUBLIC int __lxstat(int, const char *, struct stat *);
PUBLIC int __lxstat64(int, const char *, struct stat64 *);
PUBLIC int __xstat(int, const char *, struct stat *);
PUBLIC int __xstat64(int, const char *, struct stat64 *);
PUBLIC int __fxstat(int, int, struct stat *);
PUBLIC int __fxstat64(int, int, struct stat64 *);
REAL_FUNCTION_POINTER(__lxstat);
REAL_FUNCTION_POINTER(__lxstat64);
REAL_FUNCTION_POINTER(__xstat);
REAL_FUNCTION_POINTER(__xstat64);
static int (*real___fxstatat)(int, int, const char *, struct stat *, int);
static int (*real___fxstatat64)(int, int, const char *, struct stat64 *, int);
static ssize_t (*real___read_chk)(int, void *, size_t, size_t);
static ssize_t (*real___recv_chk)(int, void *, size_t, size_t, int);
static ssize_t (*real___recvfrom_chk)(int, void *restrict, size_t, size_t,
                                     int, __SOCKADDR_ARG,
                                     socklen_t *restrict);
PUBLIC int __fxstatat(int, int, const char *, struct stat *, int);
PUBLIC int __fxstatat64(int, int, const char *, struct stat64 *, int);
PUBLIC ssize_t __read_chk(int, void *, size_t, size_t);
PUBLIC ssize_t __recv_chk(int, void *, size_t, size_t, int);
PUBLIC ssize_t __recvfrom_chk(int, void *restrict, size_t, size_t, int,
                              __SOCKADDR_ARG, socklen_t *restrict);
PUBLIC ssize_t __readlink_chk(const char *, char *, size_t, size_t);
PUBLIC ssize_t __readlinkat_chk(int, const char *, char *, size_t, size_t);
PUBLIC char *__realpath_chk(const char *, char *, size_t);
#endif

#ifndef HAS_XSTAT
#define HAS_XSTAT (__GLIBC__ == 2 && __GLIBC_MINOR__ < 33)
#endif

#if HAS_XSTAT
REAL_FUNCTION_POINTER(__fxstat);
REAL_FUNCTION_POINTER(__fxstat64);
#else
REAL_FUNCTION_POINTER(stat);
REAL_FUNCTION_POINTER(stat64);
REAL_FUNCTION_POINTER(fstat);
REAL_FUNCTION_POINTER(fstat64);
#endif

#define STRINGIZE2(x) #x
#define STRINGIZE(x) STRINGIZE2(x)

/* Attempts a scoped, non-cache-only openat2 resolution takes before EAGAIN
 * reaches the caller. */
#define OPENAT2_RESOLVE_RETRIES 64

static char render_node_dir[] = "/dev/dri/";
static const char *render_node_path = "/dev/dri/renderD128";
static const char *render_node_dirent_name = "renderD128";
static const char *char_device_path =
   "/sys/dev/char/" STRINGIZE(DRM_MAJOR) ":128";
static const char *device_path = "/sys/dev/char/" STRINGIZE(DRM_MAJOR) ":128/device";
const int render_node_minor = 128;

enum file_override_kind {
   FILE_OVERRIDE_REGULAR,
   FILE_OVERRIDE_LINK,
   FILE_OVERRIDE_DIRECTORY,
   FILE_OVERRIDE_DEVICE,
};

struct file_override {
   char *path;
   char *contents;
   enum file_override_kind kind;
};
static struct file_override file_overrides[64];
static int file_overrides_count;

static char *synthetic_authorities[16];
static int synthetic_authorities_count;
static char *synthetic_directories[128];
static int synthetic_directories_count;
static char synthetic_root_path[] = "/tmp/mesa-drm-shim-XXXXXX";
static int synthetic_root_fd = -1;
static int synthetic_lease_fd = -1;
static const char synthetic_backing_directory[] = ".backing";
static struct stat synthetic_render_status;
static thread_local int atfork_cancel_state;
#ifdef DRM_SHIM_TEST
static int force_reaper_close_range_error;
static bool force_reaper_getdents_eintr_once;
static int force_absolute_path_error;
static int force_path_base_error;
static unsigned force_openat2_eagain_attempts;
static int path_snapshot_barrier_armed;
static int path_snapshot_ready_fd = -1;
static int path_snapshot_release_fd = -1;

void
drm_shim_test_force_reaper_close_range_error(int error)
{
   force_reaper_close_range_error = error;
}

void
drm_shim_test_force_reaper_getdents_eintr_once(bool force)
{
   force_reaper_getdents_eintr_once = force;
}

void
drm_shim_test_force_absolute_path_error(int error)
{
   force_absolute_path_error = error;
}

void
drm_shim_test_force_path_base_error(int error)
{
   force_path_base_error = error;
}

void
drm_shim_test_force_openat2_eagain(unsigned attempts)
{
   force_openat2_eagain_attempts = attempts;
}

void
drm_shim_test_internal_fds(int *root_fd, int *lease_fd)
{
   *root_fd = synthetic_root_fd;
   *lease_fd = synthetic_lease_fd;
}

void
drm_shim_test_arm_path_snapshot_barrier(int ready_fd, int release_fd)
{
   path_snapshot_ready_fd = ready_fd;
   path_snapshot_release_fd = release_fd;
   p_atomic_set(&path_snapshot_barrier_armed, 1);
}
#endif

/* True when the fd names the synthetic render-node backing file itself: a
 * raw open of the synthetic path bypasses the interposer, and identity
 * parsing recognizes the backing inode as this instance's render node.
 */
bool
drm_shim_fd_names_render_backing(int fd)
{
   struct stat status;
   return synthetic_render_status.st_ino != 0 &&
          syscall(SYS_fstat, fd, &status) == 0 &&
          status.st_dev == synthetic_render_status.st_dev &&
          status.st_ino == synthetic_render_status.st_ino;
}

static void
render_stat_set_device(struct stat *status)
{
   *status = synthetic_render_status;
   status->st_rdev = makedev(DRM_MAJOR, render_node_minor);
   status->st_mode =
      (synthetic_render_status.st_mode & ~S_IFMT) | S_IFCHR;
}

static void
render_stat64_set_device(struct stat64 *status)
{
   status->st_dev = synthetic_render_status.st_dev;
   status->st_ino = synthetic_render_status.st_ino;
   status->st_mode =
      (synthetic_render_status.st_mode & ~S_IFMT) | S_IFCHR;
   status->st_nlink = synthetic_render_status.st_nlink;
   status->st_uid = synthetic_render_status.st_uid;
   status->st_gid = synthetic_render_status.st_gid;
   status->st_rdev = makedev(DRM_MAJOR, render_node_minor);
   status->st_size = synthetic_render_status.st_size;
   status->st_blksize = synthetic_render_status.st_blksize;
   status->st_blocks = synthetic_render_status.st_blocks;
   status->st_atim = synthetic_render_status.st_atim;
   status->st_mtim = synthetic_render_status.st_mtim;
   status->st_ctim = synthetic_render_status.st_ctim;
}

static void
render_statx_set_device(struct statx *status)
{
   memset(status, 0, sizeof(*status));
   status->stx_mask = STATX_BASIC_STATS;
   status->stx_blksize = synthetic_render_status.st_blksize;
   status->stx_nlink = synthetic_render_status.st_nlink;
   status->stx_uid = synthetic_render_status.st_uid;
   status->stx_gid = synthetic_render_status.st_gid;
   status->stx_mode =
      (synthetic_render_status.st_mode & ~S_IFMT) | S_IFCHR;
   status->stx_ino = synthetic_render_status.st_ino;
   status->stx_size = synthetic_render_status.st_size;
   status->stx_blocks = synthetic_render_status.st_blocks;
   status->stx_atime.tv_sec = synthetic_render_status.st_atim.tv_sec;
   status->stx_atime.tv_nsec = synthetic_render_status.st_atim.tv_nsec;
   status->stx_btime.tv_sec = 0;
   status->stx_btime.tv_nsec = 0;
   status->stx_ctime.tv_sec = synthetic_render_status.st_ctim.tv_sec;
   status->stx_ctime.tv_nsec = synthetic_render_status.st_ctim.tv_nsec;
   status->stx_mtime.tv_sec = synthetic_render_status.st_mtim.tv_sec;
   status->stx_mtime.tv_nsec = synthetic_render_status.st_mtim.tv_nsec;
   status->stx_rdev_major = DRM_MAJOR;
   status->stx_rdev_minor = render_node_minor;
   status->stx_dev_major = major(synthetic_render_status.st_dev);
   status->stx_dev_minor = minor(synthetic_render_status.st_dev);
}

enum synthetic_path_result {
   SYNTHETIC_PATH_MISS,
   SYNTHETIC_PATH_MAPPED,
   SYNTHETIC_PATH_ERROR,
};

enum hidden_path_kind {
   HIDDEN_PATH_EXACT,
   HIDDEN_PATH_COMPONENT,
};

struct hidden_path {
   enum hidden_path_kind kind;
   char *parent;
   char *basename;
};
static struct hidden_path hidden_paths[20];
static int hidden_paths_count;

enum spawn_file_action_kind {
   SPAWN_FILE_ACTION_OPEN,
   SPAWN_FILE_ACTION_CLOSE,
   SPAWN_FILE_ACTION_DUP2,
   SPAWN_FILE_ACTION_CHDIR,
   SPAWN_FILE_ACTION_FCHDIR,
   SPAWN_FILE_ACTION_CLOSEFROM,
   SPAWN_FILE_ACTION_TCSETPGRP,
};

struct spawn_file_action {
   enum spawn_file_action_kind kind;
   union {
      struct {
         int fd;
         char *path;
         int flags;
         mode_t mode;
      } open;
      struct {
         int fd;
      } close;
      struct {
         int source_fd;
         int target_fd;
      } dup2;
      struct {
         char *path;
      } chdir;
      struct {
         int fd;
      } fchdir;
      struct {
         int first_fd;
      } closefrom;
      struct {
         int fd;
      } tcsetpgrp;
   } data;
   struct spawn_file_action *next;
};

struct spawn_file_actions_state {
   posix_spawn_file_actions_t *actions;
   struct spawn_file_action *head;
   struct spawn_file_action *tail;
   size_t action_count;
   struct spawn_file_actions_state *next;
};

static struct spawn_file_actions_state *spawn_file_actions_states;

enum spawn_virtual_ofd_kind {
   SPAWN_VIRTUAL_OFD_OTHER,
   SPAWN_VIRTUAL_OFD_DIRECTORY,
   SPAWN_VIRTUAL_OFD_RENDER_INHERITED,
   SPAWN_VIRTUAL_OFD_RENDER_ACTION_OPEN,
};

struct spawn_virtual_ofd {
   enum spawn_virtual_ofd_kind kind;
   struct shim_fd *parent_shim_file;
   int directory_snapshot_fd;
   unsigned references;
   struct spawn_virtual_ofd *next;
};

struct spawn_virtual_fd {
   int fd;
   bool open;
   bool cloexec;
   bool path_only;
   struct spawn_virtual_ofd *ofd;
   struct spawn_virtual_fd *next;
};

struct spawn_virtual_state {
   int cwd_snapshot_fd;
   struct spawn_virtual_fd *fds;
   struct spawn_virtual_ofd *ofds;
};

struct spawn_compiled_actions {
   posix_spawn_file_actions_t actions;
   bool initialized;
   int locator_fd;
   bool locator_enables_state;
};

struct scm_message_record {
   struct shim_fd **slots;
   size_t slot_count;
   bool committed;
   struct scm_message_record *next;
};

struct scm_socket_endpoint {
   uint64_t cookie;
   dev_t device;
   ino_t inode;
   bool active;
   struct scm_message_record *head;
   struct scm_message_record *tail;
};

struct scm_socket_pair {
   struct scm_socket_endpoint endpoints[2];
   struct scm_socket_pair *next;
};

static struct scm_socket_pair *scm_socket_pairs;
static pthread_mutex_t scm_send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scm_receive_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scm_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scm_queue_condition = PTHREAD_COND_INITIALIZER;

static void scm_socket_pairs_reset_locked(void);
static void scm_socket_reap_closed(void);

static void
scm_mutex_lock(pthread_mutex_t *mutex)
{
   if (pthread_mutex_lock(mutex) != 0)
      abort();
}

static void
scm_mutex_unlock(pthread_mutex_t *mutex)
{
   if (pthread_mutex_unlock(mutex) != 0)
      abort();
}

static void
scm_mutex_reset_after_fork(pthread_mutex_t *mutex)
{
   pthread_mutex_t unlocked = PTHREAD_MUTEX_INITIALIZER;
   *mutex = unlocked;
}

static void
scm_condition_reset_after_fork(pthread_cond_t *condition)
{
   pthread_cond_t unused = PTHREAD_COND_INITIALIZER;
   *condition = unused;
}

static const struct file_override *
file_override_find(const char *path)
{
   for (int i = 0; i < file_overrides_count; i++) {
      if (strcmp(file_overrides[i].path, path) == 0)
         return &file_overrides[i];
   }

   return NULL;
}

static bool
path_base_at(int dirfd, char base[PATH_MAX])
{
   if (dirfd == AT_FDCWD)
      return getcwd(base, PATH_MAX) != NULL;
   if (!real_readlink)
      return false;

   char proc_path[64];
   int written =
      snprintf(proc_path, sizeof(proc_path),
               "/proc/thread-self/fd/%d", dirfd);
   if (written < 0 || (size_t)written >= sizeof(proc_path))
      return false;

   ssize_t length = real_readlink(proc_path, base, PATH_MAX - 1);
   if (length < 0 || length >= PATH_MAX - 1)
      return false;
   base[length] = '\0';
   return true;
}

static bool
absolute_path_at(int dirfd, const char *path, char absolute[PATH_MAX])
{
   if (!path)
      return false;

   int written;
   if (path[0] == '/') {
      written = snprintf(absolute, PATH_MAX, "%s", path);
   } else {
      char base[PATH_MAX];
      if (!path_base_at(dirfd, base))
         return false;
      written = path[0] ? snprintf(absolute, PATH_MAX, "%s/%s", base, path)
                        : snprintf(absolute, PATH_MAX, "%s", base);
   }

   return written >= 0 && written < PATH_MAX && absolute[0] == '/';
}

static bool
path_component_is(const char *component, size_t length, const char *expected)
{
   size_t expected_length = strlen(expected);
   return length == expected_length &&
          memcmp(component, expected, expected_length) == 0;
}

static void
normalized_path_pop(char normalized[PATH_MAX], size_t *output_length)
{
   while (*output_length > 1 &&
          normalized[*output_length - 1] != '/')
      (*output_length)--;
   if (*output_length > 1)
      (*output_length)--;
   normalized[*output_length] = '\0';
}

static bool
normalized_path_append(char normalized[PATH_MAX], size_t *output_length,
                       const char *component, size_t component_length)
{
   size_t separator_length = *output_length > 1 ? 1 : 0;
   if (*output_length + separator_length + component_length >= PATH_MAX)
      return false;

   if (separator_length)
      normalized[(*output_length)++] = '/';
   memcpy(normalized + *output_length, component, component_length);
   *output_length += component_length;
   normalized[*output_length] = '\0';
   return true;
}

static bool
normalize_absolute_path_at(int dirfd, const char *path,
                           char normalized[PATH_MAX])
{
   char absolute[PATH_MAX];
   if (!absolute_path_at(dirfd, path, absolute))
      return false;

   size_t output_length = 1;
   normalized[0] = '/';
   normalized[1] = '\0';

   const char *cursor = absolute + 1;
   while (*cursor) {
      while (*cursor == '/')
         cursor++;
      if (!*cursor)
         break;

      const char *component = cursor;
      while (*cursor && *cursor != '/')
         cursor++;
      size_t component_length = cursor - component;

      if (path_component_is(component, component_length, "."))
         continue;

      if (path_component_is(component, component_length, "..")) {
         normalized_path_pop(normalized, &output_length);
         continue;
      }

      if (!normalized_path_append(normalized, &output_length, component,
                                  component_length))
         return false;
   }

   return true;
}

static char *
readlink_alloc(const char *path)
{
   size_t capacity = 256;
   while (capacity <= (size_t)SSIZE_MAX) {
      char *target = malloc(capacity + 1);
      if (!target)
         return NULL;

      ssize_t length = real_readlink(path, target, capacity);
      if (length < 0) {
         free(target);
         return NULL;
      }
      if ((size_t)length < capacity) {
         target[length] = '\0';
         return target;
      }

      free(target);
      if (capacity > SIZE_MAX / 2)
         break;
      capacity *= 2;
   }

   errno = ENAMETOOLONG;
   return NULL;
}

static char *
path_base_at_alloc(int dirfd)
{
#ifdef DRM_SHIM_TEST
   if (force_path_base_error) {
      errno = force_path_base_error;
      return NULL;
   }
#endif
   if (dirfd == AT_FDCWD)
      return getcwd(NULL, 0);

   char proc_path[64];
   int written =
      snprintf(proc_path, sizeof(proc_path),
               "/proc/thread-self/fd/%d", dirfd);
   if (written < 0 || (size_t)written >= sizeof(proc_path)) {
      errno = EBADF;
      return NULL;
   }
   return readlink_alloc(proc_path);
}

static char *
absolute_path_at_alloc(int dirfd, const char *path)
{
#ifdef DRM_SHIM_TEST
   if (force_absolute_path_error) {
      errno = force_absolute_path_error;
      return NULL;
   }
#endif
   if (!path) {
      errno = EFAULT;
      return NULL;
   }
   if (path[0] == '/')
      return strdup(path);

   if (path[0] && dirfd != AT_FDCWD) {
      struct stat status;
      if (syscall(SYS_fstat, dirfd, &status) < 0)
         return NULL;
      if (!S_ISDIR(status.st_mode)) {
         errno = ENOTDIR;
         return NULL;
      }
   }

   char *base = path_base_at_alloc(dirfd);
   if (!base)
      return NULL;

   size_t base_length = strlen(base);
   size_t path_length = strlen(path);
   if (base_length > SIZE_MAX - path_length - 2) {
      free(base);
      errno = ENAMETOOLONG;
      return NULL;
   }

   char *absolute = malloc(base_length + path_length + 2);
   if (!absolute) {
      free(base);
      return NULL;
   }
   memcpy(absolute, base, base_length);
   absolute[base_length] = '/';
   memcpy(absolute + base_length + 1, path, path_length + 1);
   free(base);
   return absolute;
}

static char *
normalize_absolute_path_alloc(const char *absolute)
{
   if (!absolute || absolute[0] != '/') {
      errno = EINVAL;
      return NULL;
   }

   size_t absolute_length = strlen(absolute);
   char *normalized = malloc(absolute_length + 2);
   size_t *component_offsets =
      malloc((absolute_length + 1) * sizeof(*component_offsets));
   if (!normalized || !component_offsets) {
      free(normalized);
      free(component_offsets);
      return NULL;
   }

   size_t output_length = 1;
   size_t component_count = 0;
   normalized[0] = '/';
   normalized[1] = '\0';

   const char *cursor = absolute + 1;
   while (*cursor) {
      while (*cursor == '/')
         cursor++;
      if (!*cursor)
         break;

      const char *component = cursor;
      while (*cursor && *cursor != '/')
         cursor++;
      size_t component_length = cursor - component;

      if (path_component_is(component, component_length, "."))
         continue;
      if (path_component_is(component, component_length, "..")) {
         if (component_count)
            output_length = component_offsets[--component_count];
         normalized[output_length] = '\0';
         continue;
      }

      component_offsets[component_count++] = output_length;
      if (output_length > 1)
         normalized[output_length++] = '/';
      memcpy(normalized + output_length, component, component_length);
      output_length += component_length;
      normalized[output_length] = '\0';
   }

   free(component_offsets);
   return normalized;
}

static char *
canonical_path_at_alloc(int dirfd, const char *path)
{
   int path_fd = real_openat(dirfd, path, O_PATH | O_CLOEXEC, 0);
   if (path_fd < 0)
      return NULL;

   char proc_path[64];
   int written =
      snprintf(proc_path, sizeof(proc_path),
               "/proc/thread-self/fd/%d", path_fd);
   char *canonical = NULL;
   if (written >= 0 && (size_t)written < sizeof(proc_path))
      canonical = readlink_alloc(proc_path);
   else
      errno = EIO;

   int saved_errno = errno;
   real_close(path_fd);
   errno = saved_errno;
   return canonical;
}

static void
normalize_proc_root_alias(char normalized[PATH_MAX])
{
   static const char *const root_aliases[] = {
      "/proc/self/root",
      "/proc/thread-self/root",
   };

   for (size_t i = 0; i < ARRAY_SIZE(root_aliases); i++) {
      size_t alias_length = strlen(root_aliases[i]);
      if (strncmp(normalized, root_aliases[i], alias_length) != 0 ||
          (normalized[alias_length] &&
           normalized[alias_length] != '/'))
         continue;

      const char *suffix = normalized + alias_length;
      if (!*suffix) {
         normalized[0] = '/';
         normalized[1] = '\0';
      } else {
         memmove(normalized, suffix, strlen(suffix) + 1);
      }
      return;
   }
}

static bool
normalized_path_matches_hidden(const char *path)
{
   for (int i = 0; i < hidden_paths_count; i++) {
      const struct hidden_path *hidden = &hidden_paths[i];

      if (hidden->kind == HIDDEN_PATH_EXACT) {
         if (strcmp(path, hidden->parent) == 0)
            return true;
         continue;
      }

      size_t parent_length = strlen(hidden->parent);
      if (strncmp(path, hidden->parent, parent_length) != 0 ||
          path[parent_length] != '/')
         continue;

      const char *component = path + parent_length + 1;
      const char *separator = strchr(component, '/');
      if (separator && separator != component &&
          strcmp(separator + 1, hidden->basename) == 0)
         return true;
   }

   return false;
}

#ifdef DRM_SHIM_TEST
static int force_process_vm_readv_error;
static int force_process_vm_writev_error;
static int force_proc_mem_error;
static bool force_statx_symbol_absent;

void
drm_shim_test_force_process_vm_readv_error(int error)
{
   force_process_vm_readv_error = error;
}

void
drm_shim_test_force_process_vm_writev_error(int error)
{
   force_process_vm_writev_error = error;
}

void
drm_shim_test_force_proc_mem_error(int error)
{
   force_proc_mem_error = error;
}

void
drm_shim_test_force_statx_symbol_absent(bool force)
{
   force_statx_symbol_absent = force;
}
#endif

static ssize_t
copy_memory_through_pipe(void *destination, const void *source, size_t size)
{
   int pipe_fds[2];
#ifdef SYS_pipe2
   if (syscall(SYS_pipe2, pipe_fds, O_CLOEXEC) < 0)
      return -1;
#else
   errno = ENOSYS;
   return -1;
#endif

   size_t copied = 0;
   while (copied < size) {
      size_t chunk = MIN2(size - copied, (size_t)4096);
      ssize_t written;
      do {
         written =
            syscall(SYS_write, pipe_fds[1],
                    (const char *)source + copied, chunk);
      } while (written < 0 && errno == EINTR);
      if (written <= 0) {
         int saved_errno = written == 0 ? EFAULT : errno;
         syscall(SYS_close, pipe_fds[0]);
         syscall(SYS_close, pipe_fds[1]);
         errno = saved_errno;
         return -1;
      }

      size_t drained = 0;
      while (drained < (size_t)written) {
         ssize_t length;
         do {
            length =
               syscall(SYS_read, pipe_fds[0],
                       (char *)destination + copied + drained,
                       (size_t)written - drained);
         } while (length < 0 && errno == EINTR);
         if (length <= 0) {
            int saved_errno = length == 0 ? EFAULT : errno;
            syscall(SYS_close, pipe_fds[0]);
            syscall(SYS_close, pipe_fds[1]);
            errno = saved_errno;
            return -1;
         }
         drained += (size_t)length;
      }
      copied += (size_t)written;
   }

   syscall(SYS_close, pipe_fds[0]);
   syscall(SYS_close, pipe_fds[1]);
   return (ssize_t)copied;
}

static ssize_t
copy_memory_from_proc(void *destination, const void *source, size_t size)
{
#ifdef DRM_SHIM_TEST
   if (force_proc_mem_error) {
      errno = force_proc_mem_error;
      return -1;
   }
#endif
   int memory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/self/mem",
              O_RDONLY | O_CLOEXEC, 0);
   if (memory_fd < 0)
      return -1;
#ifdef SYS_pread64
   ssize_t length =
      syscall(SYS_pread64, memory_fd, destination, size,
              (off64_t)(uintptr_t)source);
#else
   ssize_t length = -1;
   errno = ENOSYS;
#endif
   int saved_errno = errno;
   syscall(SYS_close, memory_fd);
   errno = saved_errno;
   return length;
}

static ssize_t
copy_memory_to_proc(void *destination, const void *source, size_t size)
{
#ifdef DRM_SHIM_TEST
   if (force_proc_mem_error) {
      errno = force_proc_mem_error;
      return -1;
   }
#endif
   int memory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/self/mem",
              O_WRONLY | O_CLOEXEC, 0);
   if (memory_fd < 0)
      return -1;
#ifdef SYS_pwrite64
   ssize_t length =
      syscall(SYS_pwrite64, memory_fd, source, size,
              (off64_t)(uintptr_t)destination);
#else
   ssize_t length = -1;
   errno = ENOSYS;
#endif
   int saved_errno = errno;
   syscall(SYS_close, memory_fd);
   errno = saved_errno;
   return length;
}

static bool
copy_fixed_from_user(void *destination, const void *source, size_t size)
{
   if (!source) {
      errno = EFAULT;
      return false;
   }
   struct iovec local = {
      .iov_base = destination,
      .iov_len = size,
   };
   struct iovec remote = {
      .iov_base = (void *)source,
      .iov_len = size,
   };
#ifdef SYS_process_vm_readv
   ssize_t length;
#ifdef DRM_SHIM_TEST
   if (force_process_vm_readv_error) {
      length = -1;
      errno = force_process_vm_readv_error;
   } else
#endif
      length =
         syscall(SYS_process_vm_readv, getpid(), &local,
                 (unsigned long)1, &remote, (unsigned long)1,
                 (unsigned long)0);
#else
   ssize_t length = -1;
   errno = ENOSYS;
#endif
   bool used_proc_memory = false;
   if (length < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES)) {
      used_proc_memory = true;
      length = copy_memory_from_proc(destination, source, size);
   }
   if (length < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES ||
        errno == ENOENT)) {
      length = copy_memory_through_pipe(destination, source, size);
   }
   if (length != (ssize_t)size) {
      if (length >= 0 || errno == ENOSYS ||
          (used_proc_memory && errno == EIO))
         errno = EFAULT;
      return false;
   }
   return true;
}

static bool
copy_fixed_to_user(void *destination, const void *source, size_t size)
{
   if (!destination) {
      errno = EFAULT;
      return false;
   }
   struct iovec local = {
      .iov_base = (void *)source,
      .iov_len = size,
   };
   struct iovec remote = {
      .iov_base = destination,
      .iov_len = size,
   };
#ifdef SYS_process_vm_writev
   ssize_t length;
#ifdef DRM_SHIM_TEST
   if (force_process_vm_writev_error) {
      length = -1;
      errno = force_process_vm_writev_error;
   } else
#endif
      length =
         syscall(SYS_process_vm_writev, getpid(), &local,
                 (unsigned long)1, &remote, (unsigned long)1,
                 (unsigned long)0);
#else
   ssize_t length = -1;
   errno = ENOSYS;
#endif
   bool used_proc_memory = false;
   if (length < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES)) {
      used_proc_memory = true;
      length = copy_memory_to_proc(destination, source, size);
   }
   if (length < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES ||
        errno == ENOENT)) {
      length = copy_memory_through_pipe(destination, source, size);
   }
   if (length != (ssize_t)size) {
      if (length >= 0 || errno == ENOSYS ||
          (used_proc_memory && errno == EIO))
         errno = EFAULT;
      return false;
   }
   return true;
}

static char *
copy_path_argument(const char *path)
{
   if (!path) {
      errno = EFAULT;
      return NULL;
   }

   char *copy = malloc(PATH_MAX);
   if (!copy)
      return NULL;

   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0)
      page_size = 4096;

   int saved_errno = errno;
   size_t copied = 0;
   while (copied < PATH_MAX) {
      uintptr_t address = (uintptr_t)path + copied;
      if (address < (uintptr_t)path) {
         free(copy);
         errno = EFAULT;
         return NULL;
      }
      size_t page_remaining =
         (size_t)page_size - address % (size_t)page_size;
      size_t chunk = MIN2((size_t)PATH_MAX - copied, page_remaining);
      struct iovec local = {
         .iov_base = copy + copied,
         .iov_len = chunk,
      };
      struct iovec remote = {
         .iov_base = (void *)address,
         .iov_len = chunk,
      };
#ifdef SYS_process_vm_readv
      ssize_t length;
#ifdef DRM_SHIM_TEST
      if (force_process_vm_readv_error) {
         length = -1;
         errno = force_process_vm_readv_error;
      } else
#endif
         length =
            syscall(SYS_process_vm_readv, getpid(), &local,
                    (unsigned long)1, &remote, (unsigned long)1,
                    (unsigned long)0);
#else
      ssize_t length = -1;
      errno = ENOSYS;
#endif
      bool used_proc_memory = false;
      if (length < 0 &&
          (errno == ENOSYS || errno == EPERM || errno == EACCES)) {
         used_proc_memory = true;
         length =
            copy_memory_from_proc(copy + copied,
                                  (const void *)address, chunk);
      }
      if (length < 0 &&
          (errno == ENOSYS || errno == EPERM || errno == EACCES ||
           errno == ENOENT)) {
         length =
            copy_memory_through_pipe(copy + copied,
                                     (const void *)address, chunk);
      }
      if (length <= 0) {
         free(copy);
         if (length == 0 || errno == ENOSYS ||
             (used_proc_memory && errno == EIO))
            errno = EFAULT;
         return NULL;
      }

      char *terminator = memchr(copy + copied, '\0', (size_t)length);
      if (terminator) {
#ifdef DRM_SHIM_TEST
         if (p_atomic_xchg(&path_snapshot_barrier_armed, 0)) {
            char byte = 1;
            while (syscall(SYS_write, path_snapshot_ready_fd,
                           &byte, 1) < 0 &&
                   errno == EINTR)
               ;
            while (syscall(SYS_read, path_snapshot_release_fd,
                           &byte, 1) < 0 &&
                   errno == EINTR)
               ;
         }
#endif
         errno = saved_errno;
         return copy;
      }
      copied += (size_t)length;
   }

   free(copy);
   errno = ENAMETOOLONG;
   return NULL;
}

static void
free_path_snapshot(char **snapshot)
{
   free(*snapshot);
}

static int
path_argument_is_empty(const char *path, bool *empty)
{
   if (!path) {
      *empty = true;
      return 0;
   }
   char *safe_path = copy_path_argument(path);
   if (!safe_path)
      return -1;
   *empty = !safe_path[0];
   free(safe_path);
   return 0;
}

static bool
path_is_hidden_at(int dirfd, const char *path)
{
   if (!hidden_paths_count)
      return false;

   char *safe_path = copy_path_argument(path);
   if (!safe_path)
      return false;

   int saved_errno = errno;
   char *canonical = canonical_path_at_alloc(dirfd, safe_path);
   if (canonical) {
      char *normalized = normalize_absolute_path_alloc(canonical);
      free(canonical);
      if (!normalized) {
         free(safe_path);
         errno = saved_errno;
         return false;
      }
      bool hidden = normalized_path_matches_hidden(normalized);
      free(normalized);
      free(safe_path);
      errno = saved_errno;
      return hidden;
   }

   char *absolute = absolute_path_at_alloc(dirfd, safe_path);
   free(safe_path);
   char *normalized =
      absolute ? normalize_absolute_path_alloc(absolute) : NULL;
   free(absolute);
   bool hidden =
      normalized && normalized_path_matches_hidden(normalized);
   free(normalized);
   errno = saved_errno;
   return hidden;
}

static bool
path_has_prefix(const char *path, const char *prefix)
{
   size_t prefix_length = strlen(prefix);
   return strncmp(path, prefix, prefix_length) == 0 &&
          (!path[prefix_length] || path[prefix_length] == '/');
}

static void
strip_proc_root_alias(char *absolute)
{
   static const char *const named_aliases[] = {
      "/proc/self/root",
      "/proc/thread-self/root",
   };
   char numeric_aliases[2][64];
   long thread_id = (long)syscall(SYS_gettid);
   int numeric_lengths[2] = {
      snprintf(numeric_aliases[0], sizeof(numeric_aliases[0]),
               "/proc/%ld/root", (long)getpid()),
      snprintf(numeric_aliases[1], sizeof(numeric_aliases[1]),
               "/proc/%ld/root", thread_id),
   };

   for (size_t i = 0; i < ARRAY_SIZE(named_aliases); i++) {
      size_t alias_length = strlen(named_aliases[i]);
      if (!path_has_prefix(absolute, named_aliases[i]))
         continue;
      bool root_only = absolute[alias_length] == '\0';
      const char *suffix = root_only ? "/" : absolute + alias_length;
      memmove(absolute, suffix, strlen(suffix) + 1);
      return;
   }

   for (size_t i = 0; i < ARRAY_SIZE(numeric_aliases); i++) {
      if (numeric_lengths[i] <= 0 ||
          (size_t)numeric_lengths[i] >= sizeof(numeric_aliases[i]))
         continue;
      size_t alias_length = (size_t)numeric_lengths[i];
      if (!path_has_prefix(absolute, numeric_aliases[i]))
         continue;
      bool root_only = absolute[alias_length] == '\0';
      const char *suffix = root_only ? "/" : absolute + alias_length;
      memmove(absolute, suffix, strlen(suffix) + 1);
      return;
   }
}

static bool
synthetic_path_is_entry(const char *path)
{
   if (file_override_find(path))
      return true;

   for (int i = 0; i < synthetic_authorities_count; i++) {
      if (strcmp(path, synthetic_authorities[i]) == 0)
         return true;
   }
   return false;
}

static bool
path_is_in_synthetic_root(const char *path)
{
   return synthetic_root_fd >= 0 && path_has_prefix(path, synthetic_root_path);
}

/* The shim claims the /dev/dri render-node directory, the
 * /sys/dev/char/<DRM_MAJOR>:<minor> character-device tree, the PCI device
 * directory it registers as a synthetic authority, and its own backing root.
 * The classifier compares strings only, so it still answers after the
 * allocation or fd acquisition a mapping attempt needs has failed, and a
 * failure inside a claimed root becomes ENOENT instead of a miss that reads
 * the host's real DRM node behind the shim.
 */
static bool
path_is_in_claimed_namespace(const char *path)
{
   for (int i = 0; i < synthetic_authorities_count; i++) {
      if (path_has_prefix(path, synthetic_authorities[i]))
         return true;
   }
   return path_has_prefix(path, char_device_path) ||
          path_has_prefix(path, device_path) ||
          path_has_prefix(path, render_node_path) ||
          path_has_prefix(path, synthetic_root_path);
}

static char *
join_paths_alloc(const char *base, const char *path)
{
   size_t base_length = strlen(base);
   size_t path_length = strlen(path);
   bool separator = base_length && base[base_length - 1] != '/' &&
                    path_length && path[0] != '/';
   if (base_length > SIZE_MAX - path_length - separator - 1) {
      errno = ENAMETOOLONG;
      return NULL;
   }

   char *joined = malloc(base_length + path_length + separator + 1);
   if (!joined)
      return NULL;
   memcpy(joined, base, base_length);
   size_t offset = base_length;
   if (separator)
      joined[offset++] = '/';
   memcpy(joined + offset, path, path_length + 1);
   return joined;
}

static char *
synthetic_physical_path_alloc(const char *logical)
{
   if (!logical || logical[0] != '/') {
      errno = EINVAL;
      return NULL;
   }
   return join_paths_alloc(synthetic_root_path, logical);
}

static char *readlinkat_alloc(int dirfd, const char *path);

#ifdef DRM_SHIM_TEST
static bool force_openat2_resolver_enosys;

void
drm_shim_test_force_openat2_resolver_enosys(bool force)
{
   force_openat2_resolver_enosys = force;
}
#endif

static void
synthetic_resolved_path_pop(char resolved[PATH_MAX])
{
   size_t length = strlen(resolved);
   while (length && resolved[length - 1] != '/')
      length--;
   if (length)
      length--;
   resolved[length] = '\0';
}

static bool
synthetic_resolved_path_append(char resolved[PATH_MAX],
                               const char *component)
{
   size_t resolved_length = strlen(resolved);
   size_t component_length = strlen(component);
   size_t separator = resolved_length ? 1 : 0;
   if (resolved_length + separator + component_length >= PATH_MAX) {
      errno = ENAMETOOLONG;
      return false;
   }
   if (separator)
      resolved[resolved_length++] = '/';
   memcpy(resolved + resolved_length, component, component_length + 1);
   return true;
}

static enum synthetic_path_result
synthetic_resolve_logical_path_fallback(const char *logical,
                                        bool follow_final,
                                        char **mapped_path)
{
   const char *relative = logical;
   while (*relative == '/')
      relative++;
   char *remaining = strdup(relative);
   if (!remaining)
      return SYNTHETIC_PATH_ERROR;

   char resolved[PATH_MAX] = "";
   unsigned symlink_depth = 0;
   bool trailing_slash =
      logical[0] && logical[strlen(logical) - 1] == '/';

   while (remaining[0]) {
      char *cursor = remaining;
      while (*cursor == '/')
         cursor++;
      char *separator = strchr(cursor, '/');
      size_t component_length =
         separator ? (size_t)(separator - cursor) : strlen(cursor);
      char *component = strndup(cursor, component_length);
      char *rest = strdup(separator ? separator + 1 : "");
      free(remaining);
      if (!component || !rest) {
         free(component);
         free(rest);
         return SYNTHETIC_PATH_ERROR;
      }
      remaining = rest;

      if (!component[0] || strcmp(component, ".") == 0) {
         free(component);
         continue;
      }
      if (strcmp(component, "..") == 0) {
         synthetic_resolved_path_pop(resolved);
         free(component);
         continue;
      }

      char *candidate = join_paths_alloc(resolved, component);
      free(component);
      if (!candidate) {
         free(remaining);
         return SYNTHETIC_PATH_ERROR;
      }

      int path_fd =
         real_openat(synthetic_root_fd, candidate,
                     O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
      if (path_fd < 0) {
         free(candidate);
         free(remaining);
         return SYNTHETIC_PATH_ERROR;
      }

      struct stat status;
      if (syscall(SYS_fstat, path_fd, &status) < 0) {
         int saved_errno = errno;
         real_close(path_fd);
         free(candidate);
         free(remaining);
         errno = saved_errno;
         return SYNTHETIC_PATH_ERROR;
      }

      bool final_component = !remaining[0];
      if (S_ISLNK(status.st_mode) &&
          (follow_final || !final_component || trailing_slash)) {
         char *target =
            readlinkat_alloc(synthetic_root_fd, candidate);
         int saved_errno = errno;
         real_close(path_fd);
         free(candidate);
         if (!target) {
            free(remaining);
            errno = saved_errno;
            return SYNTHETIC_PATH_ERROR;
         }
         if (++symlink_depth > 40) {
            free(target);
            free(remaining);
            errno = ELOOP;
            return SYNTHETIC_PATH_ERROR;
         }

         if (target[0] == '/')
            resolved[0] = '\0';
         char *target_and_rest = join_paths_alloc(target, remaining);
         free(target);
         free(remaining);
         if (!target_and_rest)
            return SYNTHETIC_PATH_ERROR;
         remaining = target_and_rest;
         continue;
      }

      real_close(path_fd);
      if (!final_component && !S_ISDIR(status.st_mode)) {
         free(candidate);
         free(remaining);
         errno = ENOTDIR;
         return SYNTHETIC_PATH_ERROR;
      }
      if (final_component && trailing_slash && !S_ISDIR(status.st_mode)) {
         free(candidate);
         free(remaining);
         errno = ENOTDIR;
         return SYNTHETIC_PATH_ERROR;
      }
      if (!synthetic_resolved_path_append(resolved,
                                          strrchr(candidate, '/')
                                             ? strrchr(candidate, '/') + 1
                                             : candidate)) {
         free(candidate);
         free(remaining);
         return SYNTHETIC_PATH_ERROR;
      }
      free(candidate);
   }

   free(remaining);
   const char *logical_resolved = resolved[0] ? resolved : ".";
   char *physical =
      join_paths_alloc(synthetic_root_path, logical_resolved);
   if (!physical)
      return SYNTHETIC_PATH_ERROR;
   *mapped_path = physical;
   return SYNTHETIC_PATH_MAPPED;
}

static enum synthetic_path_result
synthetic_resolve_logical_path(const char *logical, bool follow_final,
                               char **mapped_path)
{
   const char *relative = logical;
   while (*relative == '/')
      relative++;
   if (!*relative)
      relative = ".";

#ifdef SYS_openat2
   struct open_how how = {
      .flags = O_PATH | O_CLOEXEC | (follow_final ? 0 : O_NOFOLLOW),
      .resolve = RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS,
   };
   int path_fd;
#ifdef DRM_SHIM_TEST
   if (force_openat2_resolver_enosys) {
      path_fd = -1;
      errno = ENOSYS;
   } else
#endif
   {
      /* A scoped resolve samples the rename sequence, which counts renames
       * anywhere on the system, so a parallel workload touching unrelated
       * directories makes the kernel abort this walk with EAGAIN. openat2(2)
       * defines EAGAIN as the retryable outcome, and the retry sees the
       * synthetic root exactly as the previous attempt left it.
       */
      unsigned attempts = 0;
      do {
         path_fd =
            syscall(SYS_openat2, synthetic_root_fd, relative,
                    &how, sizeof(how));
      } while (path_fd < 0 && errno == EAGAIN &&
               ++attempts < OPENAT2_RESOLVE_RETRIES);
   }
   if (path_fd < 0 && errno == ENOSYS)
      return synthetic_resolve_logical_path_fallback(
         logical, follow_final, mapped_path);
   if (path_fd < 0)
      return SYNTHETIC_PATH_ERROR;

   char *physical = path_base_at_alloc(path_fd);
   int saved_errno = errno;
   real_close(path_fd);
   if (!physical) {
      errno = saved_errno;
      return SYNTHETIC_PATH_ERROR;
   }
   if (!path_is_in_synthetic_root(physical)) {
      free(physical);
      errno = EXDEV;
      return SYNTHETIC_PATH_ERROR;
   }

   *mapped_path = physical;
   return SYNTHETIC_PATH_MAPPED;
#else
   return synthetic_resolve_logical_path_fallback(
      logical, follow_final, mapped_path);
#endif
}

static enum synthetic_path_result
direct_synthetic_path_map_at(int dirfd, const char *path,
                             char **mapped_path, bool follow_final)
{
   *mapped_path = NULL;
   char *absolute = absolute_path_at_alloc(dirfd, path);
   if (!absolute) {
      int saved_errno = errno;
      /* A failed absolute-path snapshot still has a bounded pathname from
       * copy_path_argument. Normalize its components on the stack before
       * checking the claimed roots, so a traversal through `..` cannot make
      * an escaped host path look synthetic or hide a path that enters a
      * claimed root. Relative paths keep the miss because their dirfd is the
      * only authority that can resolve the base.
       */
      char normalized[PATH_MAX];
      bool normalized_available =
         path[0] == '/' && normalize_absolute_path_at(
                              AT_FDCWD, path, normalized);
      if (normalized_available)
         strip_proc_root_alias(normalized);
      if (normalized_available && path_is_in_claimed_namespace(normalized)) {
         errno = saved_errno;
         return SYNTHETIC_PATH_ERROR;
      }
      errno = saved_errno;
      return SYNTHETIC_PATH_MISS;
   }
   strip_proc_root_alias(absolute);

   if (path_is_in_synthetic_root(absolute)) {
      const char *logical = absolute + strlen(synthetic_root_path);
      enum synthetic_path_result result =
         synthetic_resolve_logical_path(logical, follow_final, mapped_path);
      free(absolute);
      return result;
   }

   size_t absolute_length = strlen(absolute);
   char *logical = malloc(absolute_length + 2);
   size_t *component_offsets =
      malloc((absolute_length + 1) * sizeof(*component_offsets));
   if (!logical || !component_offsets) {
      free(component_offsets);
      free(logical);
      free(absolute);
      return SYNTHETIC_PATH_ERROR;
   }

   size_t logical_length = 1;
   size_t component_count = 0;
   logical[0] = '/';
   logical[1] = '\0';

   const char *cursor = absolute + 1;
   while (*cursor) {
      while (*cursor == '/')
         cursor++;
      if (!*cursor)
         break;

      const char *component = cursor;
      while (*cursor && *cursor != '/')
         cursor++;
      size_t component_length = (size_t)(cursor - component);

      if (path_component_is(component, component_length, "."))
         continue;
      if (path_component_is(component, component_length, "..")) {
         if (component_count)
            logical_length = component_offsets[--component_count];
         logical[logical_length] = '\0';
         continue;
      }

      component_offsets[component_count++] = logical_length;
      if (logical_length > 1)
         logical[logical_length++] = '/';
      memcpy(logical + logical_length, component, component_length);
      logical_length += component_length;
      logical[logical_length] = '\0';

      if (!synthetic_path_is_entry(logical))
         continue;

      enum synthetic_path_result result =
         synthetic_resolve_logical_path(absolute, follow_final, mapped_path);
      free(component_offsets);
      free(logical);
      free(absolute);
      return result;
   }

   /* No component named an override or an authority. Inside a claimed root
    * that means the entry table lost the path the shim publishes, so the
    * caller takes ENOENT rather than the host device the path also names.
    */
   enum synthetic_path_result result =
      path_is_in_claimed_namespace(absolute) ? SYNTHETIC_PATH_ERROR
                                             : SYNTHETIC_PATH_MISS;
   free(component_offsets);
   free(logical);
   free(absolute);
   if (result == SYNTHETIC_PATH_ERROR)
      errno = ENOENT;
   return result;
}

static char *
readlinkat_alloc(int dirfd, const char *path)
{
   size_t capacity = 256;
   while (capacity <= (size_t)SSIZE_MAX) {
      char *target = malloc(capacity + 1);
      if (!target)
         return NULL;
      ssize_t length = real_readlinkat(dirfd, path, target, capacity);
      if (length < 0) {
         free(target);
         return NULL;
      }
      if ((size_t)length < capacity) {
         target[length] = '\0';
         return target;
      }
      free(target);
      if (capacity > SIZE_MAX / 2)
         break;
      capacity *= 2;
   }
   errno = ENAMETOOLONG;
   return NULL;
}

static int
open_path_directory(int dirfd)
{
   if (dirfd == AT_FDCWD)
      return real_open(".", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);

   struct stat status;
   if (syscall(SYS_fstat, dirfd, &status) < 0)
      return -1;
   if (!S_ISDIR(status.st_mode)) {
      errno = ENOTDIR;
      return -1;
   }
   return real_fcntl(dirfd, F_DUPFD_CLOEXEC, 3);
}

static char *
external_synthetic_transition_at_alloc(int dirfd, const char *path,
                                       bool follow_final,
                                       bool *synthetic_error)
{
   *synthetic_error = false;
   int current_fd;
   const char *input = path;
   if (path[0] == '/') {
      current_fd = real_open("/", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
      while (*input == '/')
         input++;
   } else {
      current_fd = open_path_directory(dirfd);
   }
   /* The walk needs one descriptor of its own. Losing it under fd pressure
    * says nothing about whether the path crosses into the synthetic
    * namespace, so it reports the error instead of the miss that would send
    * a shim-owned path to the real filesystem. Every internal failure below
    * -- allocation, fstat on an O_PATH descriptor, link readback -- reports
    * the same way; the walk keeps the miss for what the real filesystem
    * itself resolves, so a path that ends at a missing component still
    * reaches open(O_CREAT).
    */
   if (current_fd < 0) {
      *synthetic_error = true;
      return NULL;
   }

   char *remaining = strdup(input);
   if (!remaining) {
      *synthetic_error = true;
      real_close(current_fd);
      return NULL;
   }

   unsigned symlink_depth = 0;
   bool trailing_slash =
      path[0] && path[strlen(path) - 1] == '/';
   while (remaining[0]) {
      char *cursor = remaining;
      while (*cursor == '/')
         cursor++;
      char *separator = strchr(cursor, '/');
      size_t component_length =
         separator ? (size_t)(separator - cursor) : strlen(cursor);
      char *component = strndup(cursor, component_length);
      char *rest = strdup(separator ? separator + 1 : "");
      if (!component || !rest) {
         *synthetic_error = true;
         free(component);
         free(rest);
         free(remaining);
         real_close(current_fd);
         return NULL;
      }
      free(remaining);
      remaining = rest;

      if (!component[0] || strcmp(component, ".") == 0) {
         free(component);
         continue;
      }

      int next_fd = real_openat(current_fd, component,
                                O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
      if (next_fd < 0) {
         free(component);
         free(remaining);
         real_close(current_fd);
         return NULL;
      }

      struct stat status;
      if (syscall(SYS_fstat, next_fd, &status) < 0) {
         *synthetic_error = true;
         free(component);
         free(remaining);
         real_close(next_fd);
         real_close(current_fd);
         return NULL;
      }

      if (S_ISLNK(status.st_mode)) {
         if (!follow_final && !remaining[0]) {
            free(component);
            free(remaining);
            real_close(next_fd);
            real_close(current_fd);
            errno = ENOENT;
            return NULL;
         }

         char *target = readlinkat_alloc(current_fd, component);
         real_close(next_fd);
         free(component);
         if (!target) {
            *synthetic_error = true;
            free(remaining);
            real_close(current_fd);
            return NULL;
         }
         if (++symlink_depth > 40) {
            free(target);
            free(remaining);
            real_close(current_fd);
            errno = ELOOP;
            return NULL;
         }

         bool rest_empty = !remaining[0];
         char *target_and_rest = join_paths_alloc(target, remaining);
         free(remaining);
         if (!target_and_rest) {
            *synthetic_error = true;
            free(target);
            real_close(current_fd);
            return NULL;
         }
         if (trailing_slash && !target_and_rest[0]) {
            free(target_and_rest);
            target_and_rest = strdup("/");
            if (!target_and_rest) {
               *synthetic_error = true;
               free(target);
               real_close(current_fd);
               return NULL;
            }
         } else if (trailing_slash && rest_empty) {
            size_t target_length = strlen(target_and_rest);
            if (target_length &&
                target_and_rest[target_length - 1] != '/') {
               char *with_slash =
                  realloc(target_and_rest, target_length + 2);
               if (!with_slash) {
                  *synthetic_error = true;
                  free(target);
                  free(target_and_rest);
                  real_close(current_fd);
                  return NULL;
               }
               target_and_rest = with_slash;
               target_and_rest[target_length] = '/';
               target_and_rest[target_length + 1] = '\0';
            }
         }
         char *mapped_target = NULL;
         enum synthetic_path_result target_mapping =
            direct_synthetic_path_map_at(
               target[0] == '/' ? AT_FDCWD : current_fd,
               target_and_rest, &mapped_target, follow_final);
         if (target_mapping == SYNTHETIC_PATH_MAPPED) {
            free(target);
            free(target_and_rest);
            real_close(current_fd);
            return mapped_target;
         }
         if (target_mapping == SYNTHETIC_PATH_ERROR) {
            *synthetic_error = true;
            free(target);
            free(target_and_rest);
            real_close(current_fd);
            return NULL;
         }
         if (target[0] == '/') {
            real_close(current_fd);
            current_fd =
               real_open("/", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
            if (current_fd < 0) {
               *synthetic_error = true;
               free(target);
               free(target_and_rest);
               return NULL;
            }
            char *without_root = target_and_rest;
            while (*without_root == '/')
               without_root++;
            remaining = strdup(without_root);
            free(target_and_rest);
         } else {
            remaining = target_and_rest;
         }
         free(target);
         if (!remaining) {
            *synthetic_error = true;
            real_close(current_fd);
            return NULL;
         }
         continue;
      }

      if ((remaining[0] || trailing_slash) &&
          !S_ISDIR(status.st_mode)) {
         free(component);
         free(remaining);
         real_close(next_fd);
         real_close(current_fd);
         errno = ENOTDIR;
         return NULL;
      }

      free(component);
      real_close(current_fd);
      current_fd = next_fd;

      /* The kernel names an open directory through /proc/thread-self/fd, and
       * that readback caps at PATH_MAX. A directory deeper than the cap is
       * longer than every claimed root, so the walk carries on past it; any
       * other naming failure leaves the component unclassified and reports
       * the error.
       */
      char *current_path = path_base_at_alloc(current_fd);
      if (!current_path) {
         /* /proc/thread-self/fd naming is an observation aid, not an
          * authority for ordinary host paths. If procfs is hidden or denies
          * the readback, leave the walk unclassified and let the real
          * filesystem resolve the path. Length overflow remains a safe
          * continuation because the resulting path cannot name a shorter
          * claimed root. */
         if (errno == ENAMETOOLONG || errno == ENOENT || errno == EACCES ||
             errno == EPERM || errno == ENOSYS)
            continue;
         *synthetic_error = true;
         free(remaining);
         real_close(current_fd);
         return NULL;
      }
      char *candidate = join_paths_alloc(current_path, remaining);
      free(current_path);
      if (!candidate) {
         if (errno == ENAMETOOLONG)
            continue;
         *synthetic_error = true;
         free(remaining);
         real_close(current_fd);
         return NULL;
      }
      char *mapped_candidate = NULL;
      enum synthetic_path_result candidate_mapping =
         direct_synthetic_path_map_at(AT_FDCWD, candidate,
                                      &mapped_candidate, follow_final);
      if (candidate_mapping == SYNTHETIC_PATH_MAPPED) {
         free(candidate);
         free(remaining);
         real_close(current_fd);
         return mapped_candidate;
      }
      if (candidate_mapping == SYNTHETIC_PATH_ERROR) {
         *synthetic_error = true;
         free(candidate);
         free(remaining);
         real_close(current_fd);
         return NULL;
      }
      free(candidate);
   }

   free(remaining);
   real_close(current_fd);
   errno = ENOENT;
   return NULL;
}

static enum synthetic_path_result
synthetic_path_map_at_mode(int dirfd, const char *path, char **mapped_path,
                           bool follow_final)
{
   *mapped_path = NULL;
   if (synthetic_root_fd < 0)
      return SYNTHETIC_PATH_MISS;
   char *safe_path = copy_path_argument(path);
   if (!safe_path)
      return SYNTHETIC_PATH_ERROR;
   if (!safe_path[0]) {
      free(safe_path);
      return SYNTHETIC_PATH_MISS;
   }

   enum synthetic_path_result direct =
      direct_synthetic_path_map_at(
         dirfd, safe_path, mapped_path, follow_final);
   if (direct != SYNTHETIC_PATH_MISS) {
      free(safe_path);
      return direct;
   }

   bool synthetic_error;
   char *transition =
      external_synthetic_transition_at_alloc(
         dirfd, safe_path, follow_final, &synthetic_error);
   free(safe_path);
   if (!transition)
      return synthetic_error ? SYNTHETIC_PATH_ERROR
                             : SYNTHETIC_PATH_MISS;
   *mapped_path = transition;
   return SYNTHETIC_PATH_MAPPED;
}

static enum synthetic_path_result
synthetic_path_map_at(int dirfd, const char *path, char **mapped_path)
{
   return synthetic_path_map_at_mode(dirfd, path, mapped_path, true);
}

static enum synthetic_path_result
synthetic_path_map_nofollow_at(int dirfd, const char *path,
                               char **mapped_path)
{
   return synthetic_path_map_at_mode(dirfd, path, mapped_path, false);
}

static const struct file_override *
file_override_find_at(int dirfd, const char *path,
                      char normalized_path[PATH_MAX])
{
   char *absolute = absolute_path_at_alloc(dirfd, path);
   if (!absolute) {
      normalized_path[0] = '\0';
      return NULL;
   }
   strip_proc_root_alias(absolute);
   char *normalized = normalize_absolute_path_alloc(absolute);
   free(absolute);
   if (!normalized) {
      normalized_path[0] = '\0';
      return NULL;
   }
   const struct file_override *override = file_override_find(normalized);
   if (strlen(normalized) < PATH_MAX)
      strcpy(normalized_path, normalized);
   else
      normalized_path[0] = '\0';
   free(normalized);
   return override;
}

static bool
path_is_hidden(const char *path)
{
   return path_is_hidden_at(AT_FDCWD, path);
}

#ifdef DRM_SHIM_TEST
bool
drm_shim_test_path_is_hidden(const char *path)
{
   return path_is_hidden(path);
}

const char *
drm_shim_test_synthetic_root_path(void)
{
   return synthetic_root_path;
}
#endif

static int
nfvasprintf(char **restrict strp, const char *restrict fmt, va_list ap)
{
   int ret = vasprintf(strp, fmt, ap);
   assert(ret >= 0);
   return ret;
}

static int
nfasprintf(char **restrict strp, const char *restrict fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   int ret = nfvasprintf(strp, fmt, ap);
   va_end(ap);
   return ret;
}

static void *get_function_pointer(const char *name)
{
   void *func = dlsym(RTLD_NEXT, name);
   if (!func) {
      fprintf(stderr, "Failed to resolve %s\n", name);
      abort();
   }
   return func;
}

static void *
get_optional_function_pointer(const char *name)
{
   return dlsym(RTLD_NEXT, name);
}

#define GET_FUNCTION_POINTER(x) real_##x = get_function_pointer(#x)

static void
synthetic_record_directory(const char *path)
{
   for (int i = 0; i < synthetic_directories_count; i++) {
      if (strcmp(synthetic_directories[i], path) == 0)
         return;
   }
   assert(synthetic_directories_count < ARRAY_SIZE(synthetic_directories));
   synthetic_directories[synthetic_directories_count++] = strdup(path);
   if (!synthetic_directories[synthetic_directories_count - 1])
      abort();
}

static void
synthetic_ensure_directories(const char *path, bool include_leaf)
{
   char *relative = strdup(path + 1);
   if (!relative)
      abort();

   char *cursor = relative;
   while ((cursor = strchr(cursor, '/'))) {
      *cursor = '\0';
      if (mkdirat(synthetic_root_fd, relative, 0755) == 0) {
         char *logical;
         nfasprintf(&logical, "/%s", relative);
         synthetic_record_directory(logical);
         free(logical);
      } else if (errno != EEXIST) {
         abort();
      }
      *cursor++ = '/';
   }

   if (include_leaf) {
      if (mkdirat(synthetic_root_fd, relative, 0755) == 0) {
         char *logical;
         nfasprintf(&logical, "/%s", relative);
         synthetic_record_directory(logical);
         free(logical);
      } else if (errno != EEXIST) {
         abort();
      }
   }
   free(relative);
}

static char *
synthetic_physical_link_target_alloc(const struct file_override *override)
{
   if (override->contents[0] != '/')
      return strdup(override->contents);

   char *normalized_target =
      normalize_absolute_path_alloc(override->contents);
   if (!normalized_target)
      return NULL;

   const char *leaf = strrchr(override->path, '/');
   unsigned parent_depth = 0;
   for (const char *cursor = override->path + 1; cursor < leaf; cursor++) {
      if (*cursor == '/')
         parent_depth++;
   }
   if (leaf > override->path + 1)
      parent_depth++;

   const char *target_suffix = normalized_target + 1;
   size_t suffix_length = strlen(target_suffix);
   size_t capacity = (size_t)parent_depth * 3 + suffix_length + 2;
   char *physical_target = malloc(capacity);
   if (!physical_target) {
      free(normalized_target);
      return NULL;
   }

   size_t length = 0;
   for (unsigned i = 0; i < parent_depth; i++) {
      memcpy(physical_target + length, "../", 3);
      length += 3;
   }
   if (suffix_length) {
      memcpy(physical_target + length, target_suffix, suffix_length);
      length += suffix_length;
   } else if (length) {
      length--;
   } else {
      physical_target[length++] = '.';
   }
   physical_target[length] = '\0';
   free(normalized_target);
   return physical_target;
}

static void
synthetic_materialize_override(const struct file_override *override)
{
   assert(synthetic_root_fd >= 0);
   const char *relative = override->path + 1;
   if (override->kind == FILE_OVERRIDE_DIRECTORY) {
      synthetic_ensure_directories(override->path, true);
      return;
   }

   synthetic_ensure_directories(override->path, false);
   if (override->kind == FILE_OVERRIDE_LINK) {
      char *physical_target =
         synthetic_physical_link_target_alloc(override);
      if (!physical_target ||
          symlinkat(physical_target, synthetic_root_fd, relative) < 0)
         abort();
      free(physical_target);
      return;
   }

   int fd = real_openat(synthetic_root_fd, relative,
                        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   if (fd < 0)
      abort();

   const char *contents = override->contents ? override->contents : "";
   size_t remaining = strlen(contents);
   while (remaining) {
      ssize_t written = write(fd, contents, remaining);
      if (written < 0 && errno == EINTR)
         continue;
      if (written <= 0)
         abort();
      contents += written;
      remaining -= (size_t)written;
   }
   if (fchmod(fd, override->kind == FILE_OVERRIDE_DEVICE ? 0600 : 0444) < 0)
      abort();
   if (real_close(fd) < 0)
      abort();
}

static void
synthetic_add_override(enum file_override_kind kind, const char *contents,
                       char *path)
{
   assert(file_overrides_count < ARRAY_SIZE(file_overrides));
   if (!path || path[0] != '/')
      abort();
   char *normalized = normalize_absolute_path_alloc(path);
   if (!normalized || strcmp(normalized, path) != 0 ||
       file_override_find(path)) {
      free(normalized);
      abort();
   }
   free(normalized);

   struct file_override *override = &file_overrides[file_overrides_count++];
   override->path = path;
   override->contents = contents ? strdup(contents) : NULL;
   override->kind = kind;
   if (contents && !override->contents)
      abort();
   synthetic_materialize_override(override);
}

static void
synthetic_add_directory(const char *path_format, ...)
{
   char *path;
   va_list ap;
   va_start(ap, path_format);
   nfvasprintf(&path, path_format, ap);
   va_end(ap);
   synthetic_add_override(FILE_OVERRIDE_DIRECTORY, NULL, path);
}

static void
synthetic_add_authority(const char *path_format, ...)
{
   assert(synthetic_authorities_count < ARRAY_SIZE(synthetic_authorities));
   char *path;
   va_list ap;
   va_start(ap, path_format);
   nfvasprintf(&path, path_format, ap);
   va_end(ap);
   char *normalized = normalize_absolute_path_alloc(path);
   if (!normalized || strcmp(normalized, path) != 0)
      abort();
   free(normalized);
   synthetic_authorities[synthetic_authorities_count++] = path;
}

void
drm_shim_override_file(const char *contents, const char *path_format, ...)
{
   char *path;
   va_list ap;
   va_start(ap, path_format);
   nfvasprintf(&path, path_format, ap);
   va_end(ap);
   synthetic_add_override(FILE_OVERRIDE_REGULAR, contents, path);
}

void
drm_shim_override_link(const char *target, const char *path_format, ...)
{
   char *path;
   va_list ap;
   va_start(ap, path_format);
   nfvasprintf(&path, path_format, ap);
   va_end(ap);
   synthetic_add_override(FILE_OVERRIDE_LINK, target, path);
}

void
drm_shim_hide_path(const char *path)
{
   assert(hidden_paths_count < ARRAY_SIZE(hidden_paths));

   char normalized[PATH_MAX];
   if (!path || path[0] != '/' ||
       !normalize_absolute_path_at(AT_FDCWD, path, normalized))
      abort();

   struct hidden_path *hidden = &hidden_paths[hidden_paths_count++];
   hidden->kind = HIDDEN_PATH_EXACT;
   hidden->parent = strdup(normalized);
   if (!hidden->parent)
      abort();
}

void
drm_shim_hide_path_component(const char *parent, const char *basename)
{
   assert(hidden_paths_count < ARRAY_SIZE(hidden_paths));
   if (!parent || parent[0] != '/' || !basename || !basename[0] ||
       strchr(basename, '/') || strcmp(basename, ".") == 0 ||
       strcmp(basename, "..") == 0)
      abort();

   char normalized_parent[PATH_MAX];
   if (!normalize_absolute_path_at(AT_FDCWD, parent, normalized_parent))
      abort();

   struct hidden_path *hidden = &hidden_paths[hidden_paths_count++];
   hidden->kind = HIDDEN_PATH_COMPONENT;
   hidden->parent = strdup(normalized_parent);
   hidden->basename = strdup(basename);
   if (!hidden->parent || !hidden->basename)
      abort();
}

static uint32_t inited = 0;
static simple_mtx_t init_lock = SIMPLE_MTX_INITIALIZER;

static void
get_function_pointers(void)
{
   GET_FUNCTION_POINTER(access);
   GET_FUNCTION_POINTER(close);
   GET_FUNCTION_POINTER(closedir);
   GET_FUNCTION_POINTER(dup);
   GET_FUNCTION_POINTER(dup2);
   real_dup3 = get_optional_function_pointer("dup3");
   GET_FUNCTION_POINTER(euidaccess);
   GET_FUNCTION_POINTER(execl);
   GET_FUNCTION_POINTER(execle);
   GET_FUNCTION_POINTER(execlp);
   GET_FUNCTION_POINTER(execv);
   GET_FUNCTION_POINTER(execve);
   GET_FUNCTION_POINTER(execveat);
   GET_FUNCTION_POINTER(execvp);
   GET_FUNCTION_POINTER(execvpe);
   GET_FUNCTION_POINTER(faccessat);
   GET_FUNCTION_POINTER(fexecve);
   GET_FUNCTION_POINTER(fclose);
   GET_FUNCTION_POINTER(fcntl);
   GET_FUNCTION_POINTER(fdopendir);
   GET_FUNCTION_POINTER(fopen);
   GET_FUNCTION_POINTER(flock);
   GET_FUNCTION_POINTER(freopen);
   GET_FUNCTION_POINTER(fstatat);
   GET_FUNCTION_POINTER(fstatat64);
   GET_FUNCTION_POINTER(ioctl);
   GET_FUNCTION_POINTER(lstat);
   GET_FUNCTION_POINTER(lstat64);
   GET_FUNCTION_POINTER(lockf);
   GET_FUNCTION_POINTER(lockf64);
   GET_FUNCTION_POINTER(lseek);
   GET_FUNCTION_POINTER(lseek64);
   GET_FUNCTION_POINTER(mmap);
   GET_FUNCTION_POINTER(mmap64);
   GET_FUNCTION_POINTER(mremap);
   GET_FUNCTION_POINTER(munmap);
   GET_FUNCTION_POINTER(unshare);
   GET_FUNCTION_POINTER(open);
   GET_FUNCTION_POINTER(openat);
   GET_FUNCTION_POINTER(opendir);
   GET_FUNCTION_POINTER(posix_spawn);
   GET_FUNCTION_POINTER(posix_spawnp);
   GET_FUNCTION_POINTER(posix_spawn_file_actions_init);
   GET_FUNCTION_POINTER(posix_spawn_file_actions_destroy);
   GET_FUNCTION_POINTER(posix_spawn_file_actions_addclose);
   GET_FUNCTION_POINTER(posix_spawn_file_actions_adddup2);
   GET_FUNCTION_POINTER(posix_spawn_file_actions_addopen);
   real_posix_spawn_file_actions_addchdir_np =
      get_optional_function_pointer(
         "posix_spawn_file_actions_addchdir_np");
   real_posix_spawn_file_actions_addfchdir_np =
      get_optional_function_pointer(
         "posix_spawn_file_actions_addfchdir_np");
   real_posix_spawn_file_actions_addclosefrom_np =
      get_optional_function_pointer(
         "posix_spawn_file_actions_addclosefrom_np");
   real_posix_spawn_file_actions_addtcsetpgrp_np =
      get_optional_function_pointer(
         "posix_spawn_file_actions_addtcsetpgrp_np");
   GET_FUNCTION_POINTER(read);
   GET_FUNCTION_POINTER(readv);
   GET_FUNCTION_POINTER(readdir);
   GET_FUNCTION_POINTER(readdir_r);
   GET_FUNCTION_POINTER(readdir64);
   GET_FUNCTION_POINTER(readdir64_r);
   GET_FUNCTION_POINTER(scandir);
   GET_FUNCTION_POINTER(scandir64);
   GET_FUNCTION_POINTER(scandirat);
   GET_FUNCTION_POINTER(scandirat64);
   GET_FUNCTION_POINTER(getsockopt);
   GET_FUNCTION_POINTER(recv);
   GET_FUNCTION_POINTER(recvfrom);
   GET_FUNCTION_POINTER(send);
   GET_FUNCTION_POINTER(sendto);
   GET_FUNCTION_POINTER(sendmsg);
   GET_FUNCTION_POINTER(sendmmsg);
   GET_FUNCTION_POINTER(recvmsg);
   GET_FUNCTION_POINTER(recvmmsg);
   GET_FUNCTION_POINTER(socketpair);
   GET_FUNCTION_POINTER(write);
   GET_FUNCTION_POINTER(writev);
   GET_FUNCTION_POINTER(readlink);
   GET_FUNCTION_POINTER(readlinkat);
   GET_FUNCTION_POINTER(realpath);
   real_statx = dlsym(RTLD_NEXT, "statx");
   real_openat2 = get_optional_function_pointer("openat2");
#ifdef __GLIBC__
   GET_FUNCTION_POINTER(__lxstat);
   GET_FUNCTION_POINTER(__lxstat64);
   GET_FUNCTION_POINTER(__xstat);
   GET_FUNCTION_POINTER(__xstat64);
   real___fxstatat = get_function_pointer("__fxstatat");
   real___fxstatat64 = get_function_pointer("__fxstatat64");
   real___read_chk = get_function_pointer("__read_chk");
   real___recv_chk = get_function_pointer("__recv_chk");
   real___recvfrom_chk = get_function_pointer("__recvfrom_chk");
#endif

#if HAS_XSTAT
   GET_FUNCTION_POINTER(__fxstat);
   GET_FUNCTION_POINTER(__fxstat64);
#else
   GET_FUNCTION_POINTER(stat);
   GET_FUNCTION_POINTER(stat64);
   GET_FUNCTION_POINTER(fstat);
   GET_FUNCTION_POINTER(fstat64);
#endif
}

bool
drm_shim_inited(void)
{
   return p_atomic_read(&inited);
}

struct drm_shim_linux_dirent64 {
   uint64_t ino;
   int64_t offset;
   unsigned short record_length;
   unsigned char type;
   char name[];
};

static void
synthetic_reaper_remove_directory(int directory_fd)
{
   char buffer[4096];
   syscall(SYS_fchmod, directory_fd, 0700);
   while (true) {
#ifdef SYS_getdents64
      long length =
         syscall(SYS_getdents64, directory_fd, buffer, sizeof(buffer));
#else
      long length = -1;
      errno = ENOSYS;
#endif
      if (length < 0 && errno == EINTR)
         continue;
      if (length <= 0)
         break;

      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length)
            break;
         offset += entry->record_length;
         if (entry->name[0] == '.' &&
             (!entry->name[1] ||
              (entry->name[1] == '.' && !entry->name[2])))
            continue;

         int child_fd =
            syscall(SYS_openat, directory_fd, entry->name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
         if (child_fd >= 0) {
            synthetic_reaper_remove_directory(child_fd);
            syscall(SYS_close, child_fd);
            syscall(SYS_unlinkat, directory_fd, entry->name,
                    AT_REMOVEDIR);
         } else {
            syscall(SYS_unlinkat, directory_fd, entry->name, 0);
         }
      }
   }
}

static bool
synthetic_reaper_parse_fd(const char *name, int *fd_out)
{
   if (!name[0])
      return false;
   unsigned fd = 0;
   for (const char *cursor = name; *cursor; cursor++) {
      if (*cursor < '0' || *cursor > '9')
         return false;
      unsigned digit = (unsigned)(*cursor - '0');
      if (fd > ((unsigned)INT_MAX - digit) / 10)
         return false;
      fd = fd * 10 + digit;
   }
   *fd_out = (int)fd;
   return true;
}

static void
synthetic_reaper_close_sweep(int lease_read_fd, int root_fd)
{
   int directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/fd",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
   if (directory_fd >= 0) {
      char buffer[4096];
      bool enumeration_complete = false;
      while (true) {
#ifdef SYS_getdents64
         long length;
#ifdef DRM_SHIM_TEST
         if (force_reaper_getdents_eintr_once) {
            force_reaper_getdents_eintr_once = false;
            length = -1;
            errno = EINTR;
         } else
#endif
            length =
               syscall(SYS_getdents64, directory_fd, buffer,
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
            enumeration_complete = true;
            break;
         }
         for (long offset = 0; offset < length;) {
            struct drm_shim_linux_dirent64 *entry =
               (struct drm_shim_linux_dirent64 *)(buffer + offset);
            if (!entry->record_length)
               break;
            offset += entry->record_length;

            int fd;
            if (synthetic_reaper_parse_fd(entry->name, &fd) &&
                fd != lease_read_fd && fd != root_fd &&
                fd != directory_fd)
               syscall(SYS_close, fd);
         }
      }
      syscall(SYS_close, directory_fd);
      if (enumeration_complete)
         return;
   }

   struct {
      uint64_t current;
      uint64_t maximum;
   } kernel_limit;
   unsigned long maximum = 65536;
#ifdef SYS_prlimit64
   if (syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL,
               &kernel_limit) == 0)
      maximum = kernel_limit.current == UINT64_MAX
                   ? (unsigned long)INT_MAX
                   : MIN2((unsigned long)kernel_limit.current,
                          (unsigned long)INT_MAX);
#endif
   for (unsigned long fd = 0; fd < maximum; fd++) {
      if ((int)fd != lease_read_fd && (int)fd != root_fd)
         syscall(SYS_close, (int)fd);
   }
}

static void
synthetic_reaper_main(int lease_read_fd, int root_fd,
                      dev_t root_device, ino_t root_inode)
{
   synthetic_reaper_close_sweep(lease_read_fd, root_fd);

   char byte;
   while (true) {
      long length = syscall(SYS_read, lease_read_fd, &byte, 1);
      if (length == 0)
         break;
      if (length < 0 && errno == EINTR)
         continue;
      if (length < 0)
         break;
   }
   syscall(SYS_close, lease_read_fd);

   synthetic_reaper_remove_directory(root_fd);
   struct stat path_status;
   if (syscall(SYS_newfstatat, AT_FDCWD, synthetic_root_path,
               &path_status, AT_SYMLINK_NOFOLLOW) == 0 &&
       path_status.st_dev == root_device &&
       path_status.st_ino == root_inode)
      syscall(SYS_unlinkat, AT_FDCWD, synthetic_root_path,
              AT_REMOVEDIR);
   syscall(SYS_close, root_fd);
   syscall(SYS_exit, 0);
   __builtin_unreachable();
}

static void
synthetic_fs_init(void)
{
   int lease_pipe[2];
   if (pipe2(lease_pipe, O_CLOEXEC) < 0)
      abort();
   if (!mkdtemp(synthetic_root_path)) {
      syscall(SYS_close, lease_pipe[0]);
      syscall(SYS_close, lease_pipe[1]);
      abort();
   }
   int bound_root_fd =
      syscall(SYS_openat, AT_FDCWD, synthetic_root_path,
              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
   struct stat bound_root_status;
   if (bound_root_fd < 0 ||
       syscall(SYS_fstat, bound_root_fd, &bound_root_status) < 0) {
      syscall(SYS_close, bound_root_fd);
      syscall(SYS_close, lease_pipe[0]);
      syscall(SYS_close, lease_pipe[1]);
      rmdir(synthetic_root_path);
      abort();
   }

#ifdef SYS_clone
   pid_t reaper =
      syscall(SYS_clone, CLONE_PARENT | SIGCHLD,
              0, NULL, NULL, 0);
   if (reaper < 0 && errno == EINVAL && getpid() == 1)
      reaper =
         syscall(SYS_clone, SIGCHLD, 0, NULL, NULL, 0);
#else
   pid_t reaper = -1;
   errno = ENOSYS;
#endif
   if (reaper < 0) {
      syscall(SYS_close, lease_pipe[0]);
      syscall(SYS_close, lease_pipe[1]);
      syscall(SYS_close, bound_root_fd);
      rmdir(synthetic_root_path);
      abort();
   }
   if (reaper == 0) {
      syscall(SYS_close, lease_pipe[1]);
      synthetic_reaper_main(lease_pipe[0], bound_root_fd,
                            bound_root_status.st_dev,
                            bound_root_status.st_ino);
   }
   synthetic_lease_fd = lease_pipe[1];
   syscall(SYS_close, lease_pipe[0]);

   synthetic_root_fd = bound_root_fd;
   if (mkdirat(synthetic_root_fd, synthetic_backing_directory, 0700) < 0)
      abort();

   synthetic_add_authority("/dev/dri");
}

static void
synthetic_render_init(void)
{
   if (!shim_device.render_marker_length)
      abort();

   char *render_path = strdup(render_node_path);
   if (!render_path)
      abort();
   synthetic_add_override(FILE_OVERRIDE_DEVICE, "", render_path);
   if (real_fstatat(synthetic_root_fd, render_node_path + 1,
                    &synthetic_render_status, 0) < 0)
      abort();
}

static bool
synthetic_fd_is_internal(int fd)
{
   return fd >= 0 &&
          (fd == synthetic_root_fd || fd == synthetic_lease_fd ||
           drm_shim_fd_is_reserved(fd));
}

static bool
synthetic_backing_name(char name[64], uint64_t token)
{
   int length =
      snprintf(name, 64, "%s/%016llx", synthetic_backing_directory,
               (unsigned long long)token);
   return length > 0 && length < 64;
}

int
drm_shim_backing_create(uint64_t token, size_t size)
{
   char name[64];
   if (!synthetic_backing_name(name, token))
      return -ENAMETOOLONG;

   int fd =
      real_openat(synthetic_root_fd, name,
                  O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   if (fd < 0)
      return -errno;
   int ret = 0;
   if (ftruncate(fd, (off_t)size) < 0)
      ret = -errno;
   int saved_errno = errno;
   real_close(fd);
   if (ret)
      unlinkat(synthetic_root_fd, name, 0);
   errno = saved_errno;
   return ret;
}

void *
drm_shim_backing_map(uint64_t token, void *addr, size_t length, int prot,
                     int flags)
{
   char name[64];
   if (!synthetic_backing_name(name, token)) {
      errno = ENAMETOOLONG;
      return MAP_FAILED;
   }
   int open_flags =
      (prot & PROT_WRITE) && (flags & MAP_SHARED) ? O_RDWR : O_RDONLY;
   int fd =
      real_openat(synthetic_root_fd, name, open_flags | O_CLOEXEC, 0);
   if (fd < 0)
      return MAP_FAILED;
   void *mapping = real_mmap64(addr, length, prot, flags, fd, 0);
   int saved_errno = errno;
   real_close(fd);
   errno = saved_errno;
   return mapping;
}

void
drm_shim_backing_destroy(uint64_t token)
{
   char name[64];
   if (synthetic_backing_name(name, token))
      unlinkat(synthetic_root_fd, name, 0);
}

static void
synthetic_set_directory_mode(mode_t mode)
{
   if (mode & S_IWUSR) {
      if (fchmod(synthetic_root_fd, mode) < 0)
         abort();
      for (int i = 0; i < synthetic_directories_count; i++) {
         if (fchmodat(synthetic_root_fd,
                      synthetic_directories[i] + 1, mode, 0) < 0)
            abort();
      }
      return;
   }

   for (int i = synthetic_directories_count - 1; i >= 0; i--) {
      if (fchmodat(synthetic_root_fd,
                   synthetic_directories[i] + 1, mode, 0) < 0)
         abort();
   }
   if (fchmod(synthetic_root_fd, mode) < 0)
      abort();
}

static void
drm_shim_atfork_prepare(void)
{
   if (pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,
                              &atfork_cancel_state) != 0)
      abort();
   simple_mtx_lock(&init_lock);
   simple_mtx_lock(&fd_operation_lock);
   scm_mutex_lock(&scm_queue_lock);
   simple_mtx_lock(&shim_lock);
   drm_shim_device_atfork_prepare();
}

static void
drm_shim_atfork_parent(void)
{
   drm_shim_device_atfork_parent();
   simple_mtx_unlock(&shim_lock);
   scm_mutex_unlock(&scm_queue_lock);
   simple_mtx_unlock(&fd_operation_lock);
   simple_mtx_unlock(&init_lock);
   if (pthread_setcancelstate(atfork_cancel_state, NULL) != 0)
      abort();
}

static void
drm_shim_atfork_child(void)
{
   drm_shim_device_atfork_child();
   scm_socket_pairs_reset_locked();
   shim_interposition_pid = getpid();
   simple_mtx_unlock(&shim_lock);
   scm_mutex_unlock(&scm_queue_lock);
   scm_mutex_reset_after_fork(&scm_send_lock);
   scm_mutex_reset_after_fork(&scm_receive_lock);
   scm_condition_reset_after_fork(&scm_queue_condition);
   simple_mtx_unlock(&fd_operation_lock);
   simple_mtx_unlock(&init_lock);
   if (pthread_setcancelstate(atfork_cancel_state, NULL) != 0)
      _exit(127);
}

static void __attribute__((constructor))
drm_shim_install_atfork_handlers(void)
{
   if (pthread_atfork(drm_shim_atfork_prepare, drm_shim_atfork_parent,
                      drm_shim_atfork_child) != 0)
      abort();
}

/* Initialization, which will be called from the first general library call
 * that might need to be wrapped with the shim.
 */
static void
init_shim(void)
{
   /* Fast path once init has been completed. */
   if (p_atomic_read(&inited))
      return;

   /* Re-entry from the same thread: drm_shim_device_init() and its descendents
    * like drm_shim_driver_init() might call glibc functions that would go to
    * one of our wrappers and land back here.  We just need to be sure that
    * enough of the globals are set up to complete such calls before we call
    * down -- they don't need to get anything actually interposed in terms of
    * the device paths.
    *
    * A thread-local variable is used for this recursion check, since simple_mtx
    * doesn't support recursion.
    */
   static thread_local bool in_init = false;
   if (in_init)
      return;

   simple_mtx_lock(&init_lock);
   if (p_atomic_read(&inited)) {
      simple_mtx_unlock(&init_lock);
      return;
   }
   in_init = true;

   get_function_pointers();

   drm_shim_debug = debug_get_bool_option("DRM_SHIM_DEBUG", false);

   opendir_set = _mesa_pointer_set_create(NULL);

   if (drm_shim_debug) {
      fprintf(stderr, "Initializing DRM shim on %s\n",
              render_node_path);
   }

   synthetic_fs_init();
   drm_shim_device_init();
   synthetic_render_init();
   drm_shim_fd_scan_inherited();
   synthetic_set_directory_mode(0500);

   shim_interposition_pid = getpid();
   p_atomic_set(&inited, 1);
   in_init = false;
   simple_mtx_unlock(&init_lock);
}

static bool is_drm_device_path(const char *path)
{
   if (render_node_minor == -1)
      return false;

   static const char *drm_device_path_prefix = "/sys/dev/char/" STRINGIZE(DRM_MAJOR) ":";
   if (strncmp(path, drm_device_path_prefix, strlen(drm_device_path_prefix)) == 0)
      return true;

   /* String starts with /dev/dri/ */
   if (strncmp(path, render_node_dir, sizeof(render_node_dir) - 1) == 0)
      return true;

   return false;
}

static bool hide_drm_device_path(const char *path)
{
   /* If the path looks like our fake render node device, then don't hide it.
    */
   if (path_has_prefix(path, char_device_path) ||
       strncmp(path, device_path, strlen(device_path)) == 0 ||
       strcmp(path, render_node_path) == 0)
      return false;

   /* String looks like a device but is not the fake render node.
    * We want to hide all other drm devices for the shim.
    */
   return is_drm_device_path(path);
}

enum file_override_open_result {
   FILE_OVERRIDE_MISS,
   FILE_OVERRIDE_OPENED,
   FILE_OVERRIDE_ERROR,
};

static int
register_render_fd_if_needed(int fd)
{
   if (fd < 0)
      return fd;

   struct stat status;
   if (syscall(SYS_fstat, fd, &status) == 0 &&
       status.st_dev == shim_device.lock_backing_dev &&
       status.st_ino == shim_device.lock_backing_ino) {
      int ret = drm_shim_fd_register(fd, NULL);
      if (ret) {
         errno = -ret;
         return -1;
      }
   }
   return fd;
}

static bool
mapped_path_is_render_node(const char *mapped_path)
{
   struct stat mapped_status;
   return real_fstatat(AT_FDCWD, mapped_path, &mapped_status, 0) == 0 &&
          mapped_status.st_dev == synthetic_render_status.st_dev &&
          mapped_status.st_ino == synthetic_render_status.st_ino;
}

static enum file_override_open_result
file_override_open_at(int dirfd, const char *path, int flags, mode_t mode,
                      int *fd_out, unsigned depth)
{
   (void)depth;
   *fd_out = -1;

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return FILE_OVERRIDE_ERROR;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at_mode(dirfd, path, &mapped_path,
                                 !(flags & O_NOFOLLOW));
   if (mapping == SYNTHETIC_PATH_MISS)
      return FILE_OVERRIDE_MISS;
   if (mapping == SYNTHETIC_PATH_ERROR)
      return FILE_OVERRIDE_ERROR;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   bool render_path = mapped_path_is_render_node(mapped_path);
   int fd =
      render_path ? drm_shim_render_node_open(flags)
                  : real_open(mapped_path, flags, mode);
   free(mapped_path);
   if (!render_path && fd >= 0 &&
       register_render_fd_if_needed(fd) < 0) {
      int saved_errno = errno;
      real_close(fd);
      fd = -1;
      errno = saved_errno;
   }
   fd_operation_guard_release(&guard);
   if (fd < 0)
      return FILE_OVERRIDE_ERROR;
   *fd_out = fd;
   return FILE_OVERRIDE_OPENED;
}

/* Override libdrm's reading of various sysfs files for device enumeration. */
PUBLIC FILE *fopen(const char *path, const char *mode)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return NULL;
   path = path_snapshot;
   char *mode_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(mode);
   if (!mode_snapshot)
      return NULL;
   mode = mode_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return NULL;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      bool render_path = mapped_path_is_render_node(mapped_path);
      char render_path_buffer[64];
      const char *call_path = mapped_path;
      if (render_path &&
          drm_shim_render_node_path(render_path_buffer,
                                    sizeof(render_path_buffer)) < 0) {
         fd_operation_guard_release(&guard);
         free(mapped_path);
         return NULL;
      }
      if (render_path)
         call_path = render_path_buffer;
      FILE *file = real_fopen(call_path, mode);
      free(mapped_path);
      if (file && register_render_fd_if_needed(fileno(file)) < 0) {
         int saved_errno = errno;
         real_fclose(file);
         file = NULL;
         errno = saved_errno;
      }
      fd_operation_guard_release(&guard);
      return file;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return NULL;

   return real_fopen(path, mode);
}
PUBLIC FILE *fopen64(const char *path, const char *mode)
   __attribute__((alias("fopen")));

PUBLIC FILE *
freopen(const char *path, const char *mode, FILE *stream)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      path ? copy_path_argument(path) : NULL;
   if (path && !path_snapshot)
      return NULL;
   path = path_snapshot;
   char *mode_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(mode);
   if (!mode_snapshot)
      return NULL;
   mode = mode_snapshot;

   char *mapped_path = NULL;
   enum synthetic_path_result mapping = SYNTHETIC_PATH_MISS;
   if (path)
      mapping = synthetic_path_map_at(AT_FDCWD, path, &mapped_path);

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   int old_fd = fileno(stream);
   if (synthetic_fd_is_internal(old_fd)) {
      fd_operation_guard_release(&guard);
      free(mapped_path);
      errno = EBUSY;
      return NULL;
   }
   if (mapping == SYNTHETIC_PATH_ERROR ||
       (path && path_is_hidden(path))) {
      int saved_errno =
         mapping == SYNTHETIC_PATH_ERROR ? errno : ENOENT;
      if (old_fd >= 0) {
         drm_shim_fd_adopt_raw_aliases(old_fd);
         drm_shim_fd_unregister(old_fd);
      }
      real_fclose(stream);
      fd_operation_guard_release(&guard);
      free(mapped_path);
      scm_socket_reap_closed();
      errno = saved_errno;
      return NULL;
   }

   bool render_path =
      mapping == SYNTHETIC_PATH_MAPPED &&
      mapped_path_is_render_node(mapped_path);
   char render_path_buffer[64];
   const char *call_path =
      mapping == SYNTHETIC_PATH_MAPPED ? mapped_path : path;
   if (render_path) {
      if (drm_shim_render_node_path(render_path_buffer,
                                    sizeof(render_path_buffer)) < 0) {
         int saved_errno = errno;
         if (old_fd >= 0) {
            drm_shim_fd_adopt_raw_aliases(old_fd);
            drm_shim_fd_unregister(old_fd);
         }
         real_fclose(stream);
         fd_operation_guard_release(&guard);
         free(mapped_path);
         scm_socket_reap_closed();
         errno = saved_errno;
         return NULL;
      }
      call_path = render_path_buffer;
   }
   if (path && old_fd >= 0) {
      drm_shim_fd_adopt_raw_aliases(old_fd);
      drm_shim_fd_unregister(old_fd);
   }
   FILE *result = real_freopen(call_path, mode, stream);
   if (result && register_render_fd_if_needed(fileno(result)) < 0) {
      int saved_errno = errno;
      real_fclose(result);
      result = NULL;
      errno = saved_errno;
   }
   int saved_errno = errno;
   fd_operation_guard_release(&guard);
   free(mapped_path);
   scm_socket_reap_closed();
   errno = saved_errno;
   return result;
}
PUBLIC FILE *freopen64(const char *, const char *, FILE *)
   __attribute__((alias("freopen")));

static struct spawn_file_actions_state *
spawn_file_actions_state_find(
   const posix_spawn_file_actions_t *actions)
{
   for (struct spawn_file_actions_state *state =
           spawn_file_actions_states;
        state; state = state->next) {
      if (state->actions == actions)
         return state;
   }
   return NULL;
}

static void
spawn_file_action_free(struct spawn_file_action *action)
{
   if (!action)
      return;
   if (action->kind == SPAWN_FILE_ACTION_OPEN)
      free(action->data.open.path);
   else if (action->kind == SPAWN_FILE_ACTION_CHDIR)
      free(action->data.chdir.path);
   free(action);
}

static void
spawn_file_actions_state_free(
   struct spawn_file_actions_state *state)
{
   if (!state)
      return;
   struct spawn_file_action *action = state->head;
   while (action) {
      struct spawn_file_action *next = action->next;
      spawn_file_action_free(action);
      action = next;
   }
   free(state);
}

static void
spawn_file_actions_append(struct spawn_file_actions_state *state,
                          struct spawn_file_action *action)
{
   action->next = NULL;
   if (state->tail)
      state->tail->next = action;
   else
      state->head = action;
   state->tail = action;
   state->action_count++;
}

static struct spawn_virtual_fd *
spawn_virtual_fd_find(struct spawn_virtual_state *state, int fd)
{
   for (struct spawn_virtual_fd *entry = state->fds;
        entry; entry = entry->next) {
      if (entry->fd == fd)
         return entry;
   }
   return NULL;
}

static struct spawn_virtual_fd *
spawn_virtual_fd_get_or_create(struct spawn_virtual_state *state,
                               int fd)
{
   struct spawn_virtual_fd *entry =
      spawn_virtual_fd_find(state, fd);
   if (entry)
      return entry;

   entry = calloc(1, sizeof(*entry));
   if (!entry)
      return NULL;
   entry->fd = fd;
   entry->next = state->fds;
   state->fds = entry;
   return entry;
}

static struct spawn_virtual_ofd *
spawn_virtual_ofd_create(
   struct spawn_virtual_state *state,
   enum spawn_virtual_ofd_kind kind,
   struct shim_fd *parent_shim_file,
   int directory_snapshot_fd)
{
   struct spawn_virtual_ofd *ofd = calloc(1, sizeof(*ofd));
   if (!ofd)
      return NULL;
   ofd->kind = kind;
   ofd->parent_shim_file = parent_shim_file;
   ofd->directory_snapshot_fd = directory_snapshot_fd;
   ofd->next = state->ofds;
   state->ofds = ofd;
   return ofd;
}

static void
spawn_virtual_fd_close(struct spawn_virtual_fd *entry)
{
   if (!entry || !entry->open)
      return;
   if (!entry->ofd || !entry->ofd->references)
      abort();
   entry->ofd->references--;
   entry->open = false;
   entry->cloexec = false;
   entry->path_only = false;
   entry->ofd = NULL;
}

static int
spawn_virtual_fd_assign(struct spawn_virtual_state *state, int fd,
                        struct spawn_virtual_ofd *ofd, bool cloexec,
                        bool path_only)
{
   struct spawn_virtual_fd *entry =
      spawn_virtual_fd_get_or_create(state, fd);
   if (!entry)
      return ENOMEM;
   spawn_virtual_fd_close(entry);
   entry->open = true;
   entry->cloexec = cloexec;
   entry->path_only = path_only;
   entry->ofd = ofd;
   ofd->references++;
   return 0;
}

static struct spawn_virtual_ofd *
spawn_virtual_inherited_render_ofd(
   struct spawn_virtual_state *state,
   struct shim_fd *shim_file)
{
   for (struct spawn_virtual_ofd *ofd = state->ofds;
        ofd; ofd = ofd->next) {
      if (ofd->kind == SPAWN_VIRTUAL_OFD_RENDER_INHERITED &&
          ofd->parent_shim_file == shim_file)
         return ofd;
   }
   return NULL;
}

static int
spawn_virtual_seed_fd(struct spawn_virtual_state *state, int fd)
{
   if (spawn_virtual_fd_find(state, fd))
      return 0;

   int descriptor_flags = syscall(SYS_fcntl, fd, F_GETFD);
   if (descriptor_flags < 0) {
      if (errno != EBADF)
         return errno;
      if (!spawn_virtual_fd_get_or_create(state, fd))
         return ENOMEM;
      return 0;
   }

   int file_flags = syscall(SYS_fcntl, fd, F_GETFL);
   if (file_flags < 0)
      return errno;

   struct shim_fd *shim_file = drm_shim_fd_get(fd);
   struct spawn_virtual_ofd *ofd = NULL;
   if (shim_file) {
      ofd =
         spawn_virtual_inherited_render_ofd(state, shim_file);
      if (ofd)
         drm_shim_fd_put(shim_file);
      else {
         ofd = spawn_virtual_ofd_create(
            state, SPAWN_VIRTUAL_OFD_RENDER_INHERITED,
            shim_file, -1);
         if (!ofd)
            drm_shim_fd_put(shim_file);
      }
   } else {
      struct stat status;
      int directory_snapshot_fd = -1;
      enum spawn_virtual_ofd_kind kind =
         SPAWN_VIRTUAL_OFD_OTHER;
      if (syscall(SYS_fstat, fd, &status) == 0 &&
          S_ISDIR(status.st_mode)) {
         directory_snapshot_fd =
            syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 0);
         if (directory_snapshot_fd < 0)
            return errno;
         kind = SPAWN_VIRTUAL_OFD_DIRECTORY;
      }
      ofd =
         spawn_virtual_ofd_create(
            state, kind, NULL, directory_snapshot_fd);
      if (!ofd && directory_snapshot_fd >= 0)
         real_close(directory_snapshot_fd);
   }
   if (!ofd)
      return ENOMEM;

   return spawn_virtual_fd_assign(
      state, fd, ofd, descriptor_flags & FD_CLOEXEC,
      (file_flags & O_PATH) == O_PATH);
}

static int
spawn_virtual_collect_open_fds(int **fds_out, size_t *count_out)
{
   *fds_out = NULL;
   *count_out = 0;
   int directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/thread-self/fd",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
   if (directory_fd < 0)
      return errno;

   int *fds = NULL;
   size_t count = 0;
   char buffer[4096];
   int result = 0;
   while (true) {
      long length =
         syscall(SYS_getdents64, directory_fd, buffer,
                 sizeof(buffer));
      if (length < 0 && errno == EINTR)
         continue;
      if (length < 0) {
         result = errno;
         break;
      }
      if (!length)
         break;

      for (long offset = 0; offset < length;) {
         struct drm_shim_linux_dirent64 *entry =
            (struct drm_shim_linux_dirent64 *)(buffer + offset);
         if (!entry->record_length ||
             offset + entry->record_length > length) {
            result = EIO;
            break;
         }
         offset += entry->record_length;
         int fd;
         if (!synthetic_reaper_parse_fd(entry->name, &fd) ||
             fd == directory_fd)
            continue;
         if (count == SIZE_MAX / sizeof(*fds)) {
            result = ENOMEM;
            break;
         }
         int *resized =
            realloc(fds, (count + 1) * sizeof(*fds));
         if (!resized) {
            result = ENOMEM;
            break;
         }
         fds = resized;
         fds[count++] = fd;
      }
      if (result)
         break;
   }

   int saved_errno = errno;
   real_close(directory_fd);
   errno = saved_errno;
   if (result) {
      free(fds);
      return result;
   }
   *fds_out = fds;
   *count_out = count;
   return 0;
}

static int
spawn_virtual_state_init(struct spawn_virtual_state *state)
{
   memset(state, 0, sizeof(*state));
   state->cwd_snapshot_fd = -1;

   int *fds = NULL;
   size_t count = 0;
   int result = spawn_virtual_collect_open_fds(&fds, &count);
   if (result)
      return result;
   for (size_t index = 0; index < count; index++) {
      result = spawn_virtual_seed_fd(state, fds[index]);
      if (result)
         break;
   }
   free(fds);
   if (result)
      return result;

   state->cwd_snapshot_fd =
      real_open(".", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
   return state->cwd_snapshot_fd < 0 ? errno : 0;
}

static void
spawn_virtual_state_finish(struct spawn_virtual_state *state)
{
   if (state->cwd_snapshot_fd >= 0)
      real_close(state->cwd_snapshot_fd);
   struct spawn_virtual_fd *fd = state->fds;
   while (fd) {
      struct spawn_virtual_fd *next = fd->next;
      free(fd);
      fd = next;
   }
   struct spawn_virtual_ofd *ofd = state->ofds;
   while (ofd) {
      struct spawn_virtual_ofd *next = ofd->next;
      drm_shim_fd_put(ofd->parent_shim_file);
      if (ofd->directory_snapshot_fd >= 0)
         real_close(ofd->directory_snapshot_fd);
      free(ofd);
      ofd = next;
   }
   memset(state, 0, sizeof(*state));
   state->cwd_snapshot_fd = -1;
}

static int
spawn_virtual_lowest_free_fd(struct spawn_virtual_state *state)
{
   for (int fd = 0; fd < INT_MAX; fd++) {
      struct spawn_virtual_fd *entry =
         spawn_virtual_fd_find(state, fd);
      if (!entry || !entry->open)
         return fd;
   }
   return -1;
}

static void
spawn_virtual_closefrom(struct spawn_virtual_state *state,
                        int first_fd)
{
   for (struct spawn_virtual_fd *entry = state->fds;
        entry; entry = entry->next) {
      if (entry->fd >= first_fd)
         spawn_virtual_fd_close(entry);
   }
}

static int
spawn_virtual_replace_cwd(struct spawn_virtual_state *state,
                          int directory_fd)
{
   int snapshot_fd =
      syscall(SYS_fcntl, directory_fd, F_DUPFD_CLOEXEC, 0);
   if (snapshot_fd < 0)
      return errno;
   real_close(state->cwd_snapshot_fd);
   state->cwd_snapshot_fd = snapshot_fd;
   return 0;
}

PUBLIC int
posix_spawn_file_actions_init(posix_spawn_file_actions_t *actions)
{
   init_shim();

   int result = real_posix_spawn_file_actions_init(actions);
   if (result)
      return result;

   struct spawn_file_actions_state *state =
      calloc(1, sizeof(*state));
   if (!state) {
      free(state);
      real_posix_spawn_file_actions_destroy(actions);
      return ENOMEM;
   }
   state->actions = actions;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   state->next = spawn_file_actions_states;
   spawn_file_actions_states = state;
   fd_operation_guard_release(&guard);
   return 0;
}

PUBLIC int
posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *actions)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state **link =
      &spawn_file_actions_states;
   struct spawn_file_actions_state *removed = NULL;
   while (*link) {
      if ((*link)->actions == actions) {
         removed = *link;
         *link = removed->next;
         break;
      }
      link = &(*link)->next;
   }
   int result = real_posix_spawn_file_actions_destroy(actions);
   fd_operation_guard_release(&guard);

   spawn_file_actions_state_free(removed);
   return result;
}

PUBLIC int
posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *actions,
                                 int fd, const char *path, int flags,
                                 mode_t mode)
{
   init_shim();

   char *path_snapshot = copy_path_argument(path);
   if (!path_snapshot)
      return errno;
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action) {
      free(path_snapshot);
      return ENOMEM;
   }
   action->kind = SPAWN_FILE_ACTION_OPEN;
   action->data.open.fd = fd;
   action->data.open.path = path_snapshot;
   action->data.open.flags = flags;
   action->data.open.mode = mode;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_addopen(actions, fd, path_snapshot,
                                            flags, mode);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

PUBLIC int
posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *actions,
                                  int fd)
{
   init_shim();
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action)
      return ENOMEM;
   action->kind = SPAWN_FILE_ACTION_CLOSE;
   action->data.close.fd = fd;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_addclose(actions, fd);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

PUBLIC int
posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *actions,
                                 int source_fd, int target_fd)
{
   init_shim();
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action)
      return ENOMEM;
   action->kind = SPAWN_FILE_ACTION_DUP2;
   action->data.dup2.source_fd = source_fd;
   action->data.dup2.target_fd = target_fd;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_adddup2(
         actions, source_fd, target_fd);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

PUBLIC int
posix_spawn_file_actions_addchdir_np(
   posix_spawn_file_actions_t *actions, const char *path)
{
   init_shim();
   if (!real_posix_spawn_file_actions_addchdir_np)
      return ENOSYS;

   char *path_snapshot = copy_path_argument(path);
   if (!path_snapshot)
      return errno;
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action) {
      free(path_snapshot);
      return ENOMEM;
   }
   action->kind = SPAWN_FILE_ACTION_CHDIR;
   action->data.chdir.path = path_snapshot;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_addchdir_np(actions,
                                               path_snapshot);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

PUBLIC int
posix_spawn_file_actions_addfchdir_np(
   posix_spawn_file_actions_t *actions, int fd)
{
   init_shim();
   if (!real_posix_spawn_file_actions_addfchdir_np)
      return ENOSYS;
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action)
      return ENOMEM;
   action->kind = SPAWN_FILE_ACTION_FCHDIR;
   action->data.fchdir.fd = fd;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_addfchdir_np(actions, fd);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

PUBLIC int
posix_spawn_file_actions_addclosefrom_np(
   posix_spawn_file_actions_t *actions, int first_fd)
{
   init_shim();
   if (!real_posix_spawn_file_actions_addclosefrom_np)
      return ENOSYS;
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action)
      return ENOMEM;
   action->kind = SPAWN_FILE_ACTION_CLOSEFROM;
   action->data.closefrom.first_fd = first_fd;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_addclosefrom_np(actions,
                                                    first_fd);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

PUBLIC int
posix_spawn_file_actions_addtcsetpgrp_np(
   posix_spawn_file_actions_t *actions, int fd)
{
   init_shim();
   if (!real_posix_spawn_file_actions_addtcsetpgrp_np)
      return ENOSYS;
   struct spawn_file_action *action =
      calloc(1, sizeof(*action));
   if (!action)
      return ENOMEM;
   action->kind = SPAWN_FILE_ACTION_TCSETPGRP;
   action->data.tcsetpgrp.fd = fd;

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(actions);
   int result =
      real_posix_spawn_file_actions_addtcsetpgrp_np(actions, fd);
   if (!result && state)
      spawn_file_actions_append(state, action);
   else
      spawn_file_action_free(action);
   fd_operation_guard_release(&guard);
   return result;
}

static int
spawn_virtual_map_path(struct spawn_virtual_state *state,
                       const char *path, bool follow_final,
                       enum synthetic_path_result *mapping_out,
                       char **mapped_path_out)
{
   int dirfd = path[0] == '/' ? AT_FDCWD : state->cwd_snapshot_fd;
   if (path[0] != '/' && dirfd < 0)
      return ESTALE;
   if (path_is_hidden_at(dirfd, path))
      return ENOENT;

   enum synthetic_path_result mapping =
      synthetic_path_map_at_mode(dirfd, path, mapped_path_out,
                                 follow_final);
   if (mapping == SYNTHETIC_PATH_ERROR)
      return errno;
   *mapping_out = mapping;
   return 0;
}

static int
spawn_virtual_directory_snapshot(
   struct spawn_virtual_state *state, const char *raw_path,
   enum synthetic_path_result mapping, const char *mapped_path,
   bool nofollow)
{
   int dirfd =
      mapping == SYNTHETIC_PATH_MAPPED
         ? AT_FDCWD
         : (raw_path[0] == '/' ? AT_FDCWD
                               : state->cwd_snapshot_fd);
   const char *path =
      mapping == SYNTHETIC_PATH_MAPPED ? mapped_path : raw_path;
   int flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
   if (nofollow)
      flags |= O_NOFOLLOW;
   return real_openat(dirfd, path, flags, 0);
}

static int
spawn_compile_open_action(
   struct spawn_virtual_state *virtual_state,
   posix_spawn_file_actions_t *compiled,
   const struct spawn_file_action *action)
{
   char *mapped_path = NULL;
   enum synthetic_path_result mapping;
   int result =
      spawn_virtual_map_path(
         virtual_state, action->data.open.path,
         !(action->data.open.flags & O_NOFOLLOW),
         &mapping, &mapped_path);
   if (result)
      return result;

   bool render =
      mapping == SYNTHETIC_PATH_MAPPED &&
      mapped_path_is_render_node(mapped_path);
   char render_authority[64];
   char render_parent_path[96];
   const char *call_path =
      mapping == SYNTHETIC_PATH_MAPPED
         ? mapped_path
         : action->data.open.path;
   int call_flags = action->data.open.flags;
   if (render) {
      if (call_flags & O_DIRECTORY) {
         free(mapped_path);
         return ENOTDIR;
      }
      if ((call_flags & O_CREAT) && (call_flags & O_EXCL)) {
         free(mapped_path);
         return EEXIST;
      }
      if (drm_shim_render_node_path(
             render_authority, sizeof(render_authority)) < 0) {
         result = errno;
         free(mapped_path);
         return result;
      }
      int length =
         snprintf(render_parent_path, sizeof(render_parent_path),
                  "/proc/%ld/fd/%d", (long)getpid(),
                  shim_device.lock_backing_fd);
      if (length < 0 ||
          (size_t)length >= sizeof(render_parent_path)) {
         free(mapped_path);
         return ENAMETOOLONG;
      }
      call_path = render_parent_path;
      call_flags &= ~(O_NOFOLLOW | O_TRUNC);
   }

   result =
      real_posix_spawn_file_actions_addopen(
         compiled, action->data.open.fd, call_path, call_flags,
         action->data.open.mode);
   if (result) {
      free(mapped_path);
      return result;
   }

   struct spawn_virtual_fd *target =
      spawn_virtual_fd_get_or_create(
         virtual_state, action->data.open.fd);
   if (!target) {
      free(mapped_path);
      return ENOMEM;
   }
   spawn_virtual_fd_close(target);
   int opened_fd =
      spawn_virtual_lowest_free_fd(virtual_state);
   if (opened_fd < 0) {
      free(mapped_path);
      return EMFILE;
   }

   int directory_snapshot_fd = -1;
   if (!render) {
      directory_snapshot_fd =
         spawn_virtual_directory_snapshot(
            virtual_state, action->data.open.path, mapping,
            mapped_path,
            action->data.open.flags & O_NOFOLLOW);
   }
   enum spawn_virtual_ofd_kind kind =
      render ? SPAWN_VIRTUAL_OFD_RENDER_ACTION_OPEN
             : (directory_snapshot_fd >= 0
                   ? SPAWN_VIRTUAL_OFD_DIRECTORY
                   : SPAWN_VIRTUAL_OFD_OTHER);
   struct spawn_virtual_ofd *ofd =
      spawn_virtual_ofd_create(
         virtual_state, kind, NULL, directory_snapshot_fd);
   if (!ofd) {
      if (directory_snapshot_fd >= 0)
         real_close(directory_snapshot_fd);
      free(mapped_path);
      return ENOMEM;
   }

   bool cloexec =
      (action->data.open.flags & O_CLOEXEC) &&
      opened_fd == action->data.open.fd;
   result =
      spawn_virtual_fd_assign(
         virtual_state, action->data.open.fd, ofd, cloexec,
         (action->data.open.flags & O_PATH) == O_PATH);
   free(mapped_path);
   return result;
}

static int
spawn_compile_chdir_action(
   struct spawn_virtual_state *virtual_state,
   posix_spawn_file_actions_t *compiled,
   const struct spawn_file_action *action)
{
   char *mapped_path = NULL;
   enum synthetic_path_result mapping;
   int result =
      spawn_virtual_map_path(
         virtual_state, action->data.chdir.path, true,
         &mapping, &mapped_path);
   if (result)
      return result;

   const char *call_path =
      mapping == SYNTHETIC_PATH_MAPPED
         ? mapped_path
         : action->data.chdir.path;
   result =
      real_posix_spawn_file_actions_addchdir_np(
         compiled, call_path);
   if (result) {
      free(mapped_path);
      return result;
   }

   int directory_snapshot_fd =
      spawn_virtual_directory_snapshot(
         virtual_state, action->data.chdir.path, mapping,
         mapped_path, false);
   if (directory_snapshot_fd < 0) {
      result = errno;
      free(mapped_path);
      return result;
   }
   result =
      spawn_virtual_replace_cwd(
         virtual_state, directory_snapshot_fd);
   real_close(directory_snapshot_fd);
   free(mapped_path);
   return result;
}

static int
spawn_compile_file_actions(
   const posix_spawn_file_actions_t *source,
   struct spawn_compiled_actions *compiled)
{
   memset(compiled, 0, sizeof(*compiled));
   compiled->locator_fd = -1;
   if (!source)
      return 0;

   struct spawn_file_actions_state *state =
      spawn_file_actions_state_find(source);
   if (!state)
      return ENOTSUP;

   int result =
      real_posix_spawn_file_actions_init(&compiled->actions);
   if (result)
      return result;
   compiled->initialized = true;

   struct spawn_virtual_state virtual_state;
   result = spawn_virtual_state_init(&virtual_state);
   if (result) {
      spawn_virtual_state_finish(&virtual_state);
      return result;
   }

   for (const struct spawn_file_action *action = state->head;
        action && !result; action = action->next) {
      switch (action->kind) {
      case SPAWN_FILE_ACTION_OPEN:
         result =
            spawn_compile_open_action(
               &virtual_state, &compiled->actions, action);
         break;
      case SPAWN_FILE_ACTION_CLOSE: {
         result =
            real_posix_spawn_file_actions_addclose(
               &compiled->actions, action->data.close.fd);
         if (!result) {
            struct spawn_virtual_fd *entry =
               spawn_virtual_fd_get_or_create(
                  &virtual_state, action->data.close.fd);
            if (!entry)
               result = ENOMEM;
            else
               spawn_virtual_fd_close(entry);
         }
         break;
      }
      case SPAWN_FILE_ACTION_DUP2: {
         result =
            real_posix_spawn_file_actions_adddup2(
               &compiled->actions,
               action->data.dup2.source_fd,
               action->data.dup2.target_fd);
         if (result)
            break;
         struct spawn_virtual_fd *source_entry =
            spawn_virtual_fd_find(
               &virtual_state,
               action->data.dup2.source_fd);
         if (!source_entry || !source_entry->open) {
            result = EBADF;
            break;
         }
         if (action->data.dup2.source_fd ==
             action->data.dup2.target_fd) {
            source_entry->cloexec = false;
            break;
         }
         result =
            spawn_virtual_fd_assign(
               &virtual_state,
               action->data.dup2.target_fd,
               source_entry->ofd, false,
               source_entry->path_only);
         break;
      }
      case SPAWN_FILE_ACTION_CHDIR:
         result =
            spawn_compile_chdir_action(
               &virtual_state, &compiled->actions, action);
         break;
      case SPAWN_FILE_ACTION_FCHDIR: {
         result =
            real_posix_spawn_file_actions_addfchdir_np(
               &compiled->actions, action->data.fchdir.fd);
         if (result)
            break;
         struct spawn_virtual_fd *entry =
            spawn_virtual_fd_find(
               &virtual_state, action->data.fchdir.fd);
         if (!entry || !entry->open) {
            result = EBADF;
            break;
         }
         if (!entry->ofd ||
             entry->ofd->directory_snapshot_fd < 0) {
            result = ENOTDIR;
            break;
         }
         result =
            spawn_virtual_replace_cwd(
               &virtual_state,
               entry->ofd->directory_snapshot_fd);
         break;
      }
      case SPAWN_FILE_ACTION_CLOSEFROM:
         result =
            real_posix_spawn_file_actions_addclosefrom_np(
               &compiled->actions,
               action->data.closefrom.first_fd);
         if (!result)
            spawn_virtual_closefrom(
               &virtual_state,
               action->data.closefrom.first_fd);
         break;
      case SPAWN_FILE_ACTION_TCSETPGRP:
         result =
            real_posix_spawn_file_actions_addtcsetpgrp_np(
               &compiled->actions,
               action->data.tcsetpgrp.fd);
         break;
      }
   }

   if (!result) {
      for (struct spawn_virtual_fd *entry = virtual_state.fds;
           entry; entry = entry->next) {
         if (!entry->open || entry->cloexec ||
             entry->path_only || !entry->ofd ||
             entry->ofd->kind !=
                SPAWN_VIRTUAL_OFD_RENDER_ACTION_OPEN)
            continue;
         compiled->locator_fd = entry->fd;
         compiled->locator_enables_state = true;
         break;
      }
   }
   if (!result && compiled->locator_fd < 0) {
      for (struct spawn_virtual_fd *entry = virtual_state.fds;
           entry; entry = entry->next) {
         if (!entry->open || entry->cloexec ||
             entry->path_only || !entry->ofd ||
             entry->ofd->kind !=
                SPAWN_VIRTUAL_OFD_RENDER_INHERITED)
            continue;
         compiled->locator_fd = entry->fd;
         compiled->locator_enables_state = false;
         break;
      }
   }

   spawn_virtual_state_finish(&virtual_state);
   return result;
}

static void
spawn_compiled_actions_finish(
   struct spawn_compiled_actions *compiled)
{
   if (compiled->initialized)
      real_posix_spawn_file_actions_destroy(&compiled->actions);
   memset(compiled, 0, sizeof(*compiled));
   compiled->locator_fd = -1;
}

PUBLIC int
fclose(FILE *file)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   int fd = fileno(file);
   if (synthetic_fd_is_internal(fd)) {
      fd_operation_guard_release(&guard);
      errno = EBUSY;
      return EOF;
   }
   if (fd >= 0) {
      drm_shim_fd_adopt_raw_aliases(fd);
      drm_shim_fd_unregister(fd);
   }
   int ret = real_fclose(file);
   int saved_errno = errno;
   fd_operation_guard_release(&guard);
   scm_socket_reap_closed();
   errno = saved_errno;
   return ret;
}

/* Intercepts access(render_node_path) to trick drmGetMinorType */
PUBLIC int access(const char *path, int mode)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_access(mapped_path, mode);
      free(mapped_path);
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;

   if (hide_drm_device_path(path)) {
      errno = ENOENT;
      return -1;
   }

   if (strcmp(path, render_node_path) != 0)
      return real_access(path, mode);

   return 0;
}

PUBLIC int
euidaccess(const char *path, int mode)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_euidaccess(mapped_path, mode);
      free(mapped_path);
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;

   if (hide_drm_device_path(path)) {
      errno = ENOENT;
      return -1;
   }
   if (strcmp(path, render_node_path) == 0)
      return 0;
   return real_euidaccess(path, mode);
}
PUBLIC int eaccess(const char *, int) __attribute__((alias("euidaccess")));

PUBLIC int
faccessat(int dirfd, const char *path, int mode, int flags)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at_mode(
         dirfd, path, &mapped_path, !(flags & AT_SYMLINK_NOFOLLOW));
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_faccessat(AT_FDCWD, mapped_path, mode, flags);
      free(mapped_path);
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;

   return real_faccessat(dirfd, path, mode, flags);
}

/* Intercepts open(render_node_path) to redirect it to the simulator. */
PUBLIC int open(const char *path, int flags, ...)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   mode_t mode = 0;
   if (flags & O_CREAT
#ifdef O_TMPFILE
       || (flags & O_TMPFILE) == O_TMPFILE
#endif
   ) {
      va_list ap;
      va_start(ap, flags);
      mode = va_arg(ap, mode_t);
      va_end(ap);
   }

   int fd;
   enum file_override_open_result result =
      file_override_open_at(AT_FDCWD, path, flags, mode, &fd, 0);
   if (result == FILE_OVERRIDE_OPENED)
      return fd;
   if (result == FILE_OVERRIDE_ERROR)
      return -1;

   char normalized_path[PATH_MAX];
   const char *shim_path = path;
   if (normalize_absolute_path_at(AT_FDCWD, path, normalized_path)) {
      normalize_proc_root_alias(normalized_path);
      shim_path = normalized_path;
   }

   if (hide_drm_device_path(shim_path)) {
      errno = ENOENT;
      return -1;
   }

   if (strcmp(shim_path, render_node_path) != 0)
      return real_open(path, flags, mode);

   errno = ENOENT;
   return -1;
}
PUBLIC int open64(const char*, int, ...) __attribute__((alias("open")));
PUBLIC int __open(const char *, int, ...) __attribute__((alias("open")));
PUBLIC int __open64(const char *, int, ...) __attribute__((alias("open")));

/* Fortified open entry points are not declared unless _FORTIFY_SOURCE is set. */
static void
open_without_mode_abort(void)
{
   fputs("DRM_SHIM: fortified open call requires a mode argument\n", stderr);
   abort();
}

PUBLIC int __open_2(const char *path, int flags);
PUBLIC int __open_2(const char *path, int flags)
{
   if (__OPEN_NEEDS_MODE(flags))
      open_without_mode_abort();
   return open(path, flags, 0);
}

PUBLIC int __open64_2(const char *path, int flags);
PUBLIC int __open64_2(const char *path, int flags)
{
   if (__OPEN_NEEDS_MODE(flags))
      open_without_mode_abort();
   return open(path, flags, 0);
}

PUBLIC int
creat(const char *path, mode_t mode)
{
   return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}
PUBLIC int creat64(const char *, mode_t) __attribute__((alias("creat")));

PUBLIC int
openat(int dirfd, const char *path, int flags, ...)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   mode_t mode = 0;
   if (flags & O_CREAT
#ifdef O_TMPFILE
       || (flags & O_TMPFILE) == O_TMPFILE
#endif
   ) {
      va_list ap;
      va_start(ap, flags);
      mode = va_arg(ap, mode_t);
      va_end(ap);
   }

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   int fd;
   enum file_override_open_result result =
      file_override_open_at(dirfd, path, flags, mode, &fd, 0);
   if (result == FILE_OVERRIDE_OPENED)
      return fd;
   if (result == FILE_OVERRIDE_ERROR)
      return -1;

   char normalized_path[PATH_MAX];
   if (normalize_absolute_path_at(dirfd, path, normalized_path)) {
      normalize_proc_root_alias(normalized_path);
      if (hide_drm_device_path(normalized_path)) {
         errno = ENOENT;
         return -1;
      }
      if (strcmp(normalized_path, render_node_path) == 0) {
         errno = ENOENT;
         return -1;
      }
   }

   return real_openat(dirfd, path, flags, mode);
}
PUBLIC int openat64(int, const char *, int, ...)
   __attribute__((alias("openat")));

PUBLIC int __openat_2(int dirfd, const char *path, int flags);
PUBLIC int __openat_2(int dirfd, const char *path, int flags)
{
   if (__OPEN_NEEDS_MODE(flags))
      open_without_mode_abort();
   return openat(dirfd, path, flags, 0);
}

PUBLIC int __openat64_2(int dirfd, const char *path, int flags);
PUBLIC int __openat64_2(int dirfd, const char *path, int flags)
{
   if (__OPEN_NEEDS_MODE(flags))
      open_without_mode_abort();
   return openat(dirfd, path, flags, 0);
}

PUBLIC int drm_shim_openat2(int dirfd, const char *path,
                            const struct open_how *how, size_t how_size)
   __asm__("openat2");

static int
copy_open_how_argument(const struct open_how *how, size_t how_size,
                       struct open_how *copied_how)
{
   if (!how) {
      errno = EFAULT;
      return -1;
   }
   if (how_size < sizeof(*copied_how)) {
      errno = EINVAL;
      return -1;
   }

   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0)
      page_size = 4096;
   if (how_size > (size_t)page_size) {
      errno = E2BIG;
      return -1;
   }

   unsigned char *bytes = malloc(how_size);
   if (!bytes)
      return -1;

   struct iovec local = {
      .iov_base = bytes,
      .iov_len = how_size,
   };
   struct iovec remote = {
      .iov_base = (void *)how,
      .iov_len = how_size,
   };
#ifdef SYS_process_vm_readv
   ssize_t length;
#ifdef DRM_SHIM_TEST
   if (force_process_vm_readv_error) {
      length = -1;
      errno = force_process_vm_readv_error;
   } else
#endif
      length =
         syscall(SYS_process_vm_readv, getpid(), &local,
                 (unsigned long)1, &remote, (unsigned long)1,
                 (unsigned long)0);
#else
   ssize_t length = -1;
   errno = ENOSYS;
#endif
   if (length < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES))
      length = copy_memory_from_proc(bytes, how, how_size);
   if (length < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES ||
        errno == ENOENT)) {
      length = copy_memory_through_pipe(bytes, how, how_size);
   }
   if (length != (ssize_t)how_size) {
      free(bytes);
      errno = EFAULT;
      return -1;
   }

   memcpy(copied_how, bytes, sizeof(*copied_how));
   for (size_t i = sizeof(*copied_how); i < how_size; i++) {
      if (bytes[i]) {
         free(bytes);
         errno = E2BIG;
         return -1;
      }
   }
   free(bytes);
   return 0;
}

static int
bootstrap_openat2(int dirfd, const char *path, const struct open_how *how)
{
   /* The shim re-drives the caller's openat2 here. Scoped BENEATH and IN_ROOT
    * walks can report EAGAIN for a rename race, so they receive the same
    * bounded retry as the synthetic resolver. RESOLVE_CACHED deliberately
    * propagates EAGAIN because the kernel uses it for a cache miss, and an
    * unscoped open has no rename-boundary guarantee to retry. */
   bool retry_eagain =
      (how->resolve & (RESOLVE_BENEATH | RESOLVE_IN_ROOT)) != 0 &&
      (how->resolve & RESOLVE_CACHED) == 0;
   unsigned attempts = 0;
   int fd;
   do {
#ifdef DRM_SHIM_TEST
      if (force_openat2_eagain_attempts) {
         force_openat2_eagain_attempts--;
         errno = EAGAIN;
         fd = -1;
      } else
#endif
      if (real_openat2)
         fd = real_openat2(dirfd, path, how, sizeof(*how));
      else
#ifdef SYS_openat2
         fd = syscall(SYS_openat2, dirfd, path, how, sizeof(*how));
#else
      {
         errno = ENOSYS;
         fd = -1;
      }
#endif
   } while (retry_eagain && fd < 0 && errno == EAGAIN &&
            ++attempts < OPENAT2_RESOLVE_RETRIES);
   return fd;
}

static bool
dirfd_is_synthetic(int dirfd)
{
   char *base = path_base_at_alloc(dirfd);
   if (!base)
      return false;
   bool synthetic = path_is_in_synthetic_root(base);
   free(base);
   return synthetic;
}

static bool
dirfd_is_host_root(int dirfd)
{
   int candidate_fd = dirfd;
   if (candidate_fd == AT_FDCWD) {
      candidate_fd = real_open(".", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
      if (candidate_fd < 0)
         return false;
   }
   int root_fd = real_open("/", O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
   if (root_fd < 0) {
      if (candidate_fd != dirfd)
         real_close(candidate_fd);
      return false;
   }

   struct stat candidate_status;
   struct stat root_status;
   bool root =
      syscall(SYS_fstat, candidate_fd, &candidate_status) == 0 &&
      syscall(SYS_fstat, root_fd, &root_status) == 0 &&
      candidate_status.st_dev == root_status.st_dev &&
      candidate_status.st_ino == root_status.st_ino;
   real_close(root_fd);
   if (candidate_fd != dirfd)
      real_close(candidate_fd);
   return root;
}

static int
synthetic_counterpart_dirfd(int dirfd)
{
   char *base = path_base_at_alloc(dirfd);
   if (!base)
      return -1;
   strip_proc_root_alias(base);

   const char *logical = base;
   if (path_is_in_synthetic_root(base)) {
      logical = base + strlen(synthetic_root_path);
      if (!*logical)
         logical = "/";
   }
   if (*logical != '/') {
      free(base);
      errno = ENOENT;
      return -1;
   }

   char *counterpart;
   nfasprintf(&counterpart, "%s%s", synthetic_root_path, logical);
   free(base);
   int counterpart_fd =
      real_open(counterpart, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
   free(counterpart);
   return counterpart_fd;
}

static bool
path_has_disallowed_synthetic_symlink_at(int dirfd, const char *path,
                                         bool allow_final_symlink)
{
   char *absolute = absolute_path_at_alloc(dirfd, path);
   if (!absolute)
      return false;
   strip_proc_root_alias(absolute);

   char *logical;
   if (path_is_in_synthetic_root(absolute)) {
      const char *suffix = absolute + strlen(synthetic_root_path);
      logical = strdup(*suffix ? suffix : "/");
   } else {
      logical = strdup(absolute);
   }
   free(absolute);
   if (!logical)
      return false;

   size_t normalized_capacity = strlen(logical) + 2;
   char *normalized = calloc(normalized_capacity, 1);
   if (!normalized) {
      free(logical);
      return false;
   }
   normalized[0] = '/';
   size_t normalized_length = 1;
   bool found = false;
   const char *cursor = logical;
   while (*cursor) {
      while (*cursor == '/')
         cursor++;
      if (!*cursor)
         break;
      const char *component = cursor;
      while (*cursor && *cursor != '/')
         cursor++;
      size_t component_length = (size_t)(cursor - component);
      bool trailing_separator = *cursor == '/';
      const char *remaining = cursor;
      while (*remaining == '/')
         remaining++;

      if (path_component_is(component, component_length, "."))
         continue;
      if (path_component_is(component, component_length, "..")) {
         normalized_path_pop(normalized, &normalized_length);
         continue;
      }
      if (!normalized_path_append(normalized, &normalized_length,
                                  component, component_length))
         break;

      for (int i = 0; i < file_overrides_count; i++) {
         const struct file_override *override = &file_overrides[i];
         if (override->kind != FILE_OVERRIDE_LINK ||
             strcmp(normalized, override->path) != 0)
            continue;
         bool final_component =
            !remaining[0] && !trailing_separator;
         if (allow_final_symlink && final_component)
            continue;
         found = true;
         break;
      }
      if (found)
         break;
   }
   free(normalized);
   free(logical);
   return found;
}

PUBLIC int
drm_shim_openat2(int dirfd, const char *path, const struct open_how *how,
                 size_t how_size)
{
   init_shim();

   struct open_how copied_how;
   if (copy_open_how_argument(how, how_size, &copied_how) < 0)
      return -1;

   char *safe_path = copy_path_argument(path);
   if (!safe_path)
      return -1;

   if ((copied_how.resolve & RESOLVE_BENEATH) &&
       safe_path[0] == '/') {
      free(safe_path);
      errno = EXDEV;
      return -1;
   }
   bool allow_final_symlink =
      (copied_how.flags & O_PATH) == O_PATH &&
      (copied_how.flags & O_NOFOLLOW) &&
      safe_path[0] && safe_path[strlen(safe_path) - 1] != '/';
   if ((copied_how.resolve & RESOLVE_NO_SYMLINKS) &&
       path_has_disallowed_synthetic_symlink_at(
          dirfd, safe_path, allow_final_symlink)) {
      free(safe_path);
      errno = ELOOP;
      return -1;
   }

   if (path_is_hidden_at(dirfd, safe_path)) {
      free(safe_path);
      errno = ENOENT;
      return -1;
   }

   bool synthetic_dirfd = dirfd_is_synthetic(dirfd);
   char *mapped_path = NULL;
   enum synthetic_path_result mapping = SYNTHETIC_PATH_MISS;
   bool external_mapping = false;
   bool host_root_overlay =
      (copied_how.resolve & RESOLVE_IN_ROOT) &&
      dirfd_is_host_root(dirfd);
   if ((!(copied_how.resolve & RESOLVE_IN_ROOT) ||
        host_root_overlay) &&
       !(synthetic_dirfd && safe_path[0] != '/')) {
      bool follow_final = !(copied_how.flags & O_NOFOLLOW);
      mapping =
         direct_synthetic_path_map_at(
            dirfd, safe_path, &mapped_path, follow_final);
      if (mapping == SYNTHETIC_PATH_MISS) {
         mapping =
            synthetic_path_map_at_mode(
               dirfd, safe_path, &mapped_path, follow_final);
         external_mapping = mapping == SYNTHETIC_PATH_MAPPED;
      }
   }
   if (mapping == SYNTHETIC_PATH_ERROR) {
      free(safe_path);
      return -1;
   }

   if (external_mapping) {
      struct open_how validation_how = copied_how;
      validation_how.flags =
         O_PATH | O_CLOEXEC | (copied_how.flags & O_NOFOLLOW);
      validation_how.mode = 0;
      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      int validation_fd =
         bootstrap_openat2(dirfd, safe_path, &validation_how);
      int validation_errno = errno;
      if (validation_fd >= 0)
         real_close(validation_fd);
      fd_operation_guard_release(&guard);
      if (validation_fd < 0 && validation_errno != ENOENT) {
         free(mapped_path);
         free(safe_path);
         errno = validation_errno;
         return -1;
      }
   }

   int call_dirfd = dirfd;
   const char *call_path = safe_path;
   char *synthetic_relative = NULL;
   int bounded_synthetic_dirfd = -1;
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      if (!synthetic_dirfd &&
          (copied_how.resolve & RESOLVE_BENEATH) &&
          safe_path[0] != '/') {
         bounded_synthetic_dirfd =
            synthetic_counterpart_dirfd(dirfd);
         if (bounded_synthetic_dirfd < 0) {
            int saved_errno = errno;
            free(mapped_path);
            free(safe_path);
            errno = saved_errno;
            return -1;
         }
         call_dirfd = bounded_synthetic_dirfd;
         call_path = safe_path;
      } else if (!synthetic_dirfd) {
         const char *relative =
            mapped_path + strlen(synthetic_root_path);
         while (*relative == '/')
            relative++;
         synthetic_relative = strdup(*relative ? relative : ".");
         if (!synthetic_relative) {
            free(mapped_path);
            free(safe_path);
            return -1;
         }
         call_dirfd = synthetic_root_fd;
         call_path = synthetic_relative;
      }
   }

   bool render_path =
      mapping == SYNTHETIC_PATH_MAPPED &&
      mapped_path_is_render_node(mapped_path);

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   int fd;
   if (render_path) {
      int validation_fd =
         bootstrap_openat2(call_dirfd, call_path, &copied_how);
      if (validation_fd < 0) {
         fd = -1;
      } else {
         real_close(validation_fd);
         fd = drm_shim_render_node_open((int)copied_how.flags);
      }
   } else {
      fd = bootstrap_openat2(call_dirfd, call_path, &copied_how);
   }
   if (!render_path && fd >= 0 &&
       register_render_fd_if_needed(fd) < 0) {
      int saved_errno = errno;
      real_close(fd);
      fd = -1;
      errno = saved_errno;
   }
   fd_operation_guard_release(&guard);
   if (bounded_synthetic_dirfd >= 0)
      real_close(bounded_synthetic_dirfd);
   free(synthetic_relative);
   free(mapped_path);
   free(safe_path);
   return fd;
}

struct scm_socket_identity {
   bool tracked;
   uint64_t cookie;
   dev_t device;
   ino_t inode;
};

static bool
scm_socket_identity_get(int socket_fd,
                        struct scm_socket_identity *identity)
{
   memset(identity, 0, sizeof(*identity));
   socklen_t cookie_length = sizeof(identity->cookie);
   struct stat status;
   if (real_getsockopt(socket_fd, SOL_SOCKET, SO_COOKIE,
                       &identity->cookie, &cookie_length) < 0 ||
       cookie_length != sizeof(identity->cookie) ||
       syscall(SYS_fstat, socket_fd, &status) < 0 ||
       !S_ISSOCK(status.st_mode))
      return false;
   identity->tracked = true;
   identity->device = status.st_dev;
   identity->inode = status.st_ino;
   return true;
}

static bool
scm_socket_process_has_alias(dev_t device, ino_t inode)
{
   int task_directory_fd =
      syscall(SYS_openat, AT_FDCWD, "/proc/self/task",
              O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
   if (task_directory_fd < 0)
      return true;

   bool complete = true;
   bool found = false;
   char task_buffer[4096];
   while (!found) {
      long task_length =
         syscall(SYS_getdents64, task_directory_fd,
                 task_buffer, sizeof(task_buffer));
      if (task_length < 0 && errno == EINTR)
         continue;
      if (task_length < 0) {
         complete = false;
         break;
      }
      if (!task_length)
         break;

      for (long task_offset = 0;
           task_offset < task_length && !found;) {
         struct drm_shim_linux_dirent64 *task_entry =
            (struct drm_shim_linux_dirent64 *)
               (task_buffer + task_offset);
         if (!task_entry->record_length ||
             task_offset + task_entry->record_length > task_length) {
            complete = false;
            break;
         }
         task_offset += task_entry->record_length;
         if (task_entry->name[0] == '.' &&
             (!task_entry->name[1] ||
              (task_entry->name[1] == '.' &&
               !task_entry->name[2])))
            continue;

         int thread_directory_fd =
            syscall(SYS_openat, task_directory_fd, task_entry->name,
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
         if (thread_directory_fd < 0) {
            if (errno != ENOENT)
               complete = false;
            continue;
         }
         int fd_directory_fd =
            syscall(SYS_openat, thread_directory_fd, "fd",
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
         syscall(SYS_close, thread_directory_fd);
         if (fd_directory_fd < 0) {
            if (errno != ENOENT)
               complete = false;
            continue;
         }

         char fd_buffer[4096];
         while (!found) {
            long fd_length =
               syscall(SYS_getdents64, fd_directory_fd,
                       fd_buffer, sizeof(fd_buffer));
            if (fd_length < 0 && errno == EINTR)
               continue;
            if (fd_length < 0) {
               complete = false;
               break;
            }
            if (!fd_length)
               break;
            for (long fd_offset = 0;
                 fd_offset < fd_length && !found;) {
               struct drm_shim_linux_dirent64 *fd_entry =
                  (struct drm_shim_linux_dirent64 *)
                     (fd_buffer + fd_offset);
               if (!fd_entry->record_length ||
                   fd_offset + fd_entry->record_length > fd_length) {
                  complete = false;
                  break;
               }
               fd_offset += fd_entry->record_length;
               if (fd_entry->name[0] == '.' &&
                   (!fd_entry->name[1] ||
                    (fd_entry->name[1] == '.' &&
                     !fd_entry->name[2])))
                  continue;
               struct stat status;
               errno = 0;
               if (syscall(SYS_newfstatat, fd_directory_fd,
                           fd_entry->name, &status, 0) == 0 &&
                   S_ISSOCK(status.st_mode) &&
                   status.st_dev == device &&
                   status.st_ino == inode)
                  found = true;
               else if (errno && errno != ENOENT && errno != EBADF)
                  complete = false;
            }
         }
         syscall(SYS_close, fd_directory_fd);
      }
   }
   syscall(SYS_close, task_directory_fd);
   return found || !complete;
}

static void
scm_message_record_destroy(struct scm_message_record *record)
{
   if (!record)
      return;
   for (size_t index = 0; index < record->slot_count; index++)
      drm_shim_fd_put(record->slots[index]);
   free(record->slots);
   free(record);
}

static void
scm_socket_endpoint_drop_records_locked(
   struct scm_socket_endpoint *endpoint)
{
   struct scm_message_record *record = endpoint->head;
   endpoint->head = NULL;
   endpoint->tail = NULL;
   while (record) {
      struct scm_message_record *next = record->next;
      scm_message_record_destroy(record);
      record = next;
   }
}

static void
scm_socket_pairs_reset_locked(void)
{
   struct scm_socket_pair *pair = scm_socket_pairs;
   scm_socket_pairs = NULL;
   while (pair) {
      struct scm_socket_pair *next = pair->next;
      scm_socket_endpoint_drop_records_locked(&pair->endpoints[0]);
      scm_socket_endpoint_drop_records_locked(&pair->endpoints[1]);
      free(pair);
      pair = next;
   }
   if (pthread_cond_broadcast(&scm_queue_condition) != 0)
      abort();
}

static struct scm_socket_endpoint *
scm_socket_endpoint_find_locked(
   const struct scm_socket_identity *identity,
   struct scm_socket_pair **pair_out,
   unsigned *endpoint_index_out)
{
   if (!identity->tracked)
      return NULL;

   for (struct scm_socket_pair *pair = scm_socket_pairs;
        pair; pair = pair->next) {
      for (unsigned index = 0; index < 2; index++) {
         struct scm_socket_endpoint *endpoint =
            &pair->endpoints[index];
         if (endpoint->active &&
             endpoint->cookie == identity->cookie &&
             endpoint->device == identity->device &&
             endpoint->inode == identity->inode) {
            if (pair_out)
               *pair_out = pair;
            if (endpoint_index_out)
               *endpoint_index_out = index;
            return endpoint;
         }
      }
   }
   return NULL;
}

static void
scm_socket_reap_closed(void)
{
   scm_mutex_lock(&scm_send_lock);
   scm_mutex_lock(&scm_queue_lock);
   struct scm_socket_pair **link = &scm_socket_pairs;
   while (*link) {
      struct scm_socket_pair *pair = *link;
      for (unsigned index = 0; index < 2; index++) {
         struct scm_socket_endpoint *endpoint =
            &pair->endpoints[index];
         if (endpoint->active &&
             !scm_socket_process_has_alias(endpoint->device,
                                           endpoint->inode)) {
            endpoint->active = false;
            scm_socket_endpoint_drop_records_locked(endpoint);
         }
      }
      if (pair->endpoints[0].active ||
          pair->endpoints[1].active) {
         link = &pair->next;
         continue;
      }
      *link = pair->next;
      free(pair);
   }
   if (pthread_cond_broadcast(&scm_queue_condition) != 0)
      abort();
   scm_mutex_unlock(&scm_queue_lock);
   scm_mutex_unlock(&scm_send_lock);
}

PUBLIC int
socketpair(int domain, int type, int protocol, int sockets[2])
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   int created[2];
   int ret = real_socketpair(domain, type, protocol, created);
   if (ret < 0) {
      fd_operation_guard_release(&guard);
      return ret;
   }

   struct scm_socket_pair *pair = NULL;
   int base_type = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
   if (domain == AF_UNIX &&
       (base_type == SOCK_DGRAM ||
        base_type == SOCK_SEQPACKET)) {
      struct scm_socket_identity first;
      struct scm_socket_identity second;
      if (!scm_socket_identity_get(created[0], &first) ||
          !scm_socket_identity_get(created[1], &second)) {
         real_close(created[0]);
         real_close(created[1]);
         fd_operation_guard_release(&guard);
         errno = EIO;
         return -1;
      }
      pair = calloc(1, sizeof(*pair));
      if (!pair) {
         real_close(created[0]);
         real_close(created[1]);
         fd_operation_guard_release(&guard);
         return -1;
      }
      pair->endpoints[0] = (struct scm_socket_endpoint) {
         .cookie = first.cookie,
         .device = first.device,
         .inode = first.inode,
         .active = true,
      };
      pair->endpoints[1] = (struct scm_socket_endpoint) {
         .cookie = second.cookie,
         .device = second.device,
         .inode = second.inode,
         .active = true,
      };
   }

   if (!copy_fixed_to_user(sockets, created, sizeof(created))) {
      free(pair);
      real_close(created[0]);
      real_close(created[1]);
      fd_operation_guard_release(&guard);
      return -1;
   }
   if (pair) {
      scm_mutex_lock(&scm_queue_lock);
      pair->next = scm_socket_pairs;
      scm_socket_pairs = pair;
      scm_mutex_unlock(&scm_queue_lock);
   }
   fd_operation_guard_release(&guard);
   return 0;
}

PUBLIC int close(int fd)
{
   init_shim();

   if (synthetic_fd_is_internal(fd)) {
      errno = EBUSY;
      return -1;
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   drm_shim_fd_adopt_raw_aliases(fd);
   drm_shim_fd_unregister(fd);
   int ret = real_close(fd);
   if (ret == 0)
      drm_shim_fd_reap_diverged();
   fd_operation_guard_release(&guard);
   if (ret == 0)
      scm_socket_reap_closed();
   return ret;
}
PUBLIC int __close(int) __attribute__((alias("close")));

static int
compare_ints(const void *left, const void *right)
{
   int left_value = *(const int *)left;
   int right_value = *(const int *)right;
   return (left_value > right_value) - (left_value < right_value);
}

static int
close_range_interval(unsigned int first_fd, unsigned int last_fd,
                     int flags, bool unshared)
{
#ifdef SYS_close_range
   int ret = syscall(SYS_close_range, first_fd, last_fd,
                     flags & ~CLOSE_RANGE_UNSHARE);
   if (ret == 0 && !(flags & CLOSE_RANGE_CLOEXEC) && !unshared)
      drm_shim_fd_unregister_range(first_fd, last_fd);
   return ret;
#else
   errno = ENOSYS;
   return -1;
#endif
}

static int
close_range_apply(unsigned int first_fd, unsigned int last_fd, int flags,
                  bool unshared)
{
   int *identity_fds = NULL;
   size_t identity_count = 0;
   int collect_error =
      drm_shim_fd_collect_internal(&identity_fds, &identity_count);
   if (collect_error) {
      errno = -collect_error;
      return -1;
   }

   if (identity_count > (SIZE_MAX / sizeof(*identity_fds)) - 3) {
      free(identity_fds);
      errno = ENOMEM;
      return -1;
   }
   int *internal_fds =
      realloc(identity_fds,
              (identity_count + 2) * sizeof(*internal_fds));
   if (!internal_fds) {
      free(identity_fds);
      return -1;
   }
   size_t internal_count = identity_count;
   if (synthetic_root_fd >= 0)
      internal_fds[internal_count++] = synthetic_root_fd;
   if (synthetic_lease_fd >= 0)
      internal_fds[internal_count++] = synthetic_lease_fd;
   qsort(internal_fds, internal_count, sizeof(*internal_fds),
         compare_ints);

   unsigned int interval_start = first_fd;
   int result = 0;
   for (size_t i = 0; i < internal_count; i++) {
      int internal_fd = internal_fds[i];
      if (internal_fd < 0 || (unsigned)internal_fd < interval_start)
         continue;
      if ((unsigned)internal_fd > last_fd)
         break;
      if ((unsigned)internal_fd > interval_start &&
          close_range_interval(interval_start,
                               (unsigned)internal_fd - 1,
                               flags, unshared) < 0) {
         result = -1;
         break;
      }
      interval_start = (unsigned)internal_fd + 1;
   }
   if (result == 0 && interval_start <= last_fd)
      result =
         close_range_interval(interval_start, last_fd, flags, unshared);
   free(internal_fds);
   return result;
}

PUBLIC int
unshare(int flags)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   int ret = real_unshare(flags);
   if (ret == 0 && (flags & CLONE_FILES))
      drm_shim_fd_tables_unshared();
   fd_operation_guard_release(&guard);
   return ret;
}

PUBLIC int
close_range(unsigned int first_fd, unsigned int last_fd, int flags)
{
   init_shim();

   if (first_fd > last_fd ||
       (flags & ~(CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE))) {
      errno = EINVAL;
      return -1;
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   if (!(flags & CLOSE_RANGE_CLOEXEC)) {
      int adopt_error =
         drm_shim_fd_adopt_raw_aliases_range(first_fd, last_fd);
      if (adopt_error) {
         fd_operation_guard_release(&guard);
         errno = -adopt_error;
         return -1;
      }
   }
   bool unshared = flags & CLOSE_RANGE_UNSHARE;
   if (unshared) {
#ifdef SYS_unshare
      if (syscall(SYS_unshare, CLONE_FILES) < 0) {
         fd_operation_guard_release(&guard);
         return -1;
      }
      drm_shim_fd_tables_unshared();
#else
      errno = ENOSYS;
      fd_operation_guard_release(&guard);
      return -1;
#endif
   }

   int ret = close_range_apply(first_fd, last_fd, flags, unshared);
   if (ret == 0 && (flags & CLOSE_RANGE_CLOEXEC))
      drm_shim_fd_update_cloexec_range(first_fd, last_fd);
   if (ret == 0 && !(flags & CLOSE_RANGE_CLOEXEC))
      drm_shim_fd_reap_diverged();
   int saved_errno = errno;
   fd_operation_guard_release(&guard);
   if (ret == 0 && !(flags & CLOSE_RANGE_CLOEXEC))
      scm_socket_reap_closed();
   errno = saved_errno;
   return ret;
}

PUBLIC void
closefrom(int first_fd)
{
   if (first_fd < 0)
      first_fd = 0;
   (void)close_range((unsigned)first_fd, UINT_MAX, 0);
}

#if HAS_XSTAT
PUBLIC int __xstat(int ver, const char *path, struct stat *st)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real___xstat(ver, mapped_path, st);
      free(mapped_path);
      if (ret == 0 &&
          st->st_dev == synthetic_render_status.st_dev &&
          st->st_ino == synthetic_render_status.st_ino) {
         st->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         st->st_mode = (st->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (hide_drm_device_path(path)) {
      errno = ENOENT;
      return -1;
   }
   return real___xstat(ver, path, st);
}

PUBLIC int __xstat64(int ver, const char *path, struct stat64 *st)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real___xstat64(ver, mapped_path, st);
      free(mapped_path);
      if (ret == 0 &&
          st->st_dev == synthetic_render_status.st_dev &&
          st->st_ino == synthetic_render_status.st_ino) {
         st->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         st->st_mode = (st->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (hide_drm_device_path(path)) {
      errno = ENOENT;
      return -1;
   }
   return real___xstat64(ver, path, st);
}

/* Fakes fstat to return character device stuff for our fake render node. */
PUBLIC int __fxstat(int ver, int fd, struct stat *st)
{
   init_shim();

   fd_operation_discover_fd(fd);
   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   int ret = real___fxstat(ver, fd, st);
   if (ret == 0 && shim_fd)
      render_stat_set_device(st);
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   return ret;
}

PUBLIC int __fxstat64(int ver, int fd, struct stat64 *st)
{
   init_shim();

   fd_operation_discover_fd(fd);
   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   int ret = real___fxstat64(ver, fd, st);
   if (ret == 0 && shim_fd)
      render_stat64_set_device(st);
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   return ret;
}

#else

PUBLIC int stat(const char* path, struct stat* stat_buf)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_stat(mapped_path, stat_buf);
      free(mapped_path);
      if (ret == 0 &&
          stat_buf->st_dev == synthetic_render_status.st_dev &&
          stat_buf->st_ino == synthetic_render_status.st_ino) {
         stat_buf->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         stat_buf->st_mode =
            (stat_buf->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (hide_drm_device_path(path)) {
      errno = ENOENT;
      return -1;
   }
   return real_stat(path, stat_buf);
}

PUBLIC int stat64(const char* path, struct stat64* stat_buf)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_stat64(mapped_path, stat_buf);
      free(mapped_path);
      if (ret == 0 &&
          stat_buf->st_dev == synthetic_render_status.st_dev &&
          stat_buf->st_ino == synthetic_render_status.st_ino) {
         stat_buf->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         stat_buf->st_mode =
            (stat_buf->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (hide_drm_device_path(path)) {
      errno = ENOENT;
      return -1;
   }
   return real_stat64(path, stat_buf);
}

PUBLIC int fstat(int fd, struct stat* stat_buf)
{
   init_shim();

   fd_operation_discover_fd(fd);
   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   int ret = real_fstat(fd, stat_buf);
   if (ret == 0 && shim_fd)
      render_stat_set_device(stat_buf);
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   return ret;
}

PUBLIC int fstat64(int fd, struct stat64* stat_buf)
{
   init_shim();

   fd_operation_discover_fd(fd);
   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   int ret = real_fstat64(fd, stat_buf);
   if (ret == 0 && shim_fd)
      render_stat64_set_device(stat_buf);
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   return ret;
}
#endif

#if defined(__GLIBC__) && !HAS_XSTAT
PUBLIC int
__fxstat(int version, int fd, struct stat *stat_buffer)
{
   (void)version;
   return fstat(fd, stat_buffer);
}

PUBLIC int
__fxstat64(int version, int fd, struct stat64 *stat_buffer)
{
   (void)version;
   return fstat64(fd, stat_buffer);
}
#endif

#if defined(__GLIBC__) && !HAS_XSTAT
PUBLIC int
__xstat(int version, const char *path, struct stat *stat_buffer)
{
   (void)version;
   return stat(path, stat_buffer);
}

PUBLIC int
__xstat64(int version, const char *path, struct stat64 *stat_buffer)
{
   (void)version;
   return stat64(path, stat_buffer);
}
#endif

#ifdef __GLIBC__
PUBLIC int
__lxstat(int version, const char *path, struct stat *stat_buffer)
{
   (void)version;
   return lstat(path, stat_buffer);
}

PUBLIC int
__lxstat64(int version, const char *path, struct stat64 *stat_buffer)
{
   (void)version;
   return lstat64(path, stat_buffer);
}
#endif

PUBLIC int
lstat(const char *path, struct stat *stat_buf)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_nofollow_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_lstat(mapped_path, stat_buf);
      free(mapped_path);
      if (ret == 0 &&
          stat_buf->st_dev == synthetic_render_status.st_dev &&
          stat_buf->st_ino == synthetic_render_status.st_ino) {
         stat_buf->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         stat_buf->st_mode =
            (stat_buf->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   return real_lstat(path, stat_buf);
}

PUBLIC int
lstat64(const char *path, struct stat64 *stat_buf)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_nofollow_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_lstat64(mapped_path, stat_buf);
      free(mapped_path);
      if (ret == 0 &&
          stat_buf->st_dev == synthetic_render_status.st_dev &&
          stat_buf->st_ino == synthetic_render_status.st_ino) {
         stat_buf->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         stat_buf->st_mode =
            (stat_buf->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   return real_lstat64(path, stat_buf);
}

PUBLIC int
fstatat(int dirfd, const char *path, struct stat *stat_buf, int flags)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      path ? copy_path_argument(path) : NULL;
   if (path && !path_snapshot)
      return -1;
   path = path_snapshot;

   bool empty_path;
   if (path_argument_is_empty(path, &empty_path) < 0)
      return -1;
   if (empty_path && (flags & AT_EMPTY_PATH)) {
      fd_operation_discover_fd(dirfd);
      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      struct shim_fd *shim_fd = drm_shim_fd_get(dirfd);
      if (shim_fd || !path) {
         int ret =
            real_fstatat(dirfd, path ? "" : path, stat_buf,
                         flags | AT_EMPTY_PATH);
         if (ret == 0 && shim_fd)
            render_stat_set_device(stat_buf);
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return ret;
      }
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
   }
   if (!path) {
      errno = EFAULT;
      return -1;
   }

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at_mode(
         dirfd, path, &mapped_path, !(flags & AT_SYMLINK_NOFOLLOW));
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_fstatat(AT_FDCWD, mapped_path, stat_buf, flags);
      free(mapped_path);
      if (ret == 0 &&
          stat_buf->st_dev == synthetic_render_status.st_dev &&
          stat_buf->st_ino == synthetic_render_status.st_ino) {
         stat_buf->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         stat_buf->st_mode =
            (stat_buf->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   return real_fstatat(dirfd, path, stat_buf, flags);
}

PUBLIC int
fstatat64(int dirfd, const char *path, struct stat64 *stat_buf, int flags)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      path ? copy_path_argument(path) : NULL;
   if (path && !path_snapshot)
      return -1;
   path = path_snapshot;

   bool empty_path;
   if (path_argument_is_empty(path, &empty_path) < 0)
      return -1;
   if (empty_path && (flags & AT_EMPTY_PATH)) {
      fd_operation_discover_fd(dirfd);
      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      struct shim_fd *shim_fd = drm_shim_fd_get(dirfd);
      if (shim_fd || !path) {
         int ret =
            real_fstatat64(dirfd, path ? "" : path, stat_buf,
                           flags | AT_EMPTY_PATH);
         if (ret == 0 && shim_fd)
            render_stat64_set_device(stat_buf);
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return ret;
      }
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
   }
   if (!path) {
      errno = EFAULT;
      return -1;
   }

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at_mode(
         dirfd, path, &mapped_path, !(flags & AT_SYMLINK_NOFOLLOW));
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      int ret = real_fstatat64(AT_FDCWD, mapped_path, stat_buf, flags);
      free(mapped_path);
      if (ret == 0 &&
          stat_buf->st_dev == synthetic_render_status.st_dev &&
          stat_buf->st_ino == synthetic_render_status.st_ino) {
         stat_buf->st_rdev = makedev(DRM_MAJOR, render_node_minor);
         stat_buf->st_mode =
            (stat_buf->st_mode & ~S_IFMT) | S_IFCHR;
      }
      return ret;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   return real_fstatat64(dirfd, path, stat_buf, flags);
}

#ifdef __GLIBC__
PUBLIC int
__fxstatat(int ver, int dirfd, const char *path, struct stat *stat_buf,
           int flags)
{
   (void)ver;
   return fstatat(dirfd, path, stat_buf, flags);
}

PUBLIC int
__fxstatat64(int ver, int dirfd, const char *path, struct stat64 *stat_buf,
             int flags)
{
   (void)ver;
   return fstatat64(dirfd, path, stat_buf, flags);
}
#endif

static int
bootstrap_statx(int dirfd, const char *path, int flags, unsigned mask,
                struct statx *statx_buf)
{
#ifdef DRM_SHIM_TEST
   if (!force_statx_symbol_absent && real_statx)
#else
   if (real_statx)
#endif
      return real_statx(dirfd, path, flags, mask, statx_buf);

#ifdef SYS_statx
   return syscall(SYS_statx, dirfd, path, flags, mask, statx_buf);
#else
   errno = ENOSYS;
   return -1;
#endif
}

PUBLIC int
statx(int dirfd, const char *path, int flags, unsigned mask,
      struct statx *statx_buf)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      path ? copy_path_argument(path) : NULL;
   if (path && !path_snapshot)
      return -1;
   path = path_snapshot;

   bool empty_path;
   if (path_argument_is_empty(path, &empty_path) < 0)
      return -1;
   if (empty_path && (flags & AT_EMPTY_PATH)) {
      fd_operation_discover_fd(dirfd);
      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      struct shim_fd *shim_fd = drm_shim_fd_get(dirfd);
      if (shim_fd || !path) {
         int ret =
            bootstrap_statx(dirfd, path, flags | AT_EMPTY_PATH, mask,
                            statx_buf);
         if (ret == 0 && shim_fd)
            render_statx_set_device(statx_buf);
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return ret;
      }
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
   }
   if (!path) {
      errno = EFAULT;
      return -1;
   }

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at_mode(
         dirfd, path, &mapped_path, !(flags & AT_SYMLINK_NOFOLLOW));
   int ret;
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      ret =
         bootstrap_statx(AT_FDCWD, mapped_path, flags, mask, statx_buf);
      free(mapped_path);
   } else if (mapping == SYNTHETIC_PATH_ERROR) {
      return -1;
   } else {
      ret = bootstrap_statx(dirfd, path, flags, mask, statx_buf);
   }
   if (ret == 0 &&
       statx_buf->stx_dev_major == major(synthetic_render_status.st_dev) &&
       statx_buf->stx_dev_minor == minor(synthetic_render_status.st_dev) &&
       statx_buf->stx_ino == synthetic_render_status.st_ino) {
      statx_buf->stx_mode =
         (statx_buf->stx_mode & ~S_IFMT) | S_IFCHR;
      statx_buf->stx_rdev_major = DRM_MAJOR;
      statx_buf->stx_rdev_minor = render_node_minor;
      statx_buf->stx_mask |= STATX_TYPE | STATX_MODE;
   }
   return ret;
}

static bool
path_is_synthetic_dri_directory(const char *path)
{
   char *canonical = real_realpath(path, NULL);
   if (!canonical)
      return false;
   char *expected = synthetic_physical_path_alloc(render_node_dir);
   if (!expected) {
      free(canonical);
      return false;
   }
   size_t expected_length = strlen(expected);
   while (expected_length > 1 && expected[expected_length - 1] == '/')
      expected[--expected_length] = '\0';
   bool matches = strcmp(canonical, expected) == 0;
   free(expected);
   free(canonical);
   return matches;
}

static thread_local int (*synthetic_scandir_filter)(
   const struct dirent *);
static thread_local int (*synthetic_scandir64_filter)(
   const struct dirent64 *);

static int
synthetic_scandir_filter_entry(const struct dirent *entry)
{
   struct dirent *mutable_entry = (struct dirent *)entry;
   if (strcmp(entry->d_name, render_node_dirent_name) == 0)
      mutable_entry->d_type = DT_CHR;
   return !synthetic_scandir_filter ||
          synthetic_scandir_filter(entry);
}

static int
synthetic_scandir64_filter_entry(const struct dirent64 *entry)
{
   struct dirent64 *mutable_entry = (struct dirent64 *)entry;
   if (strcmp(entry->d_name, render_node_dirent_name) == 0)
      mutable_entry->d_type = DT_CHR;
   return !synthetic_scandir64_filter ||
          synthetic_scandir64_filter(entry);
}

PUBLIC int
scandir(const char *path, struct dirent ***entries,
        int (*filter)(const struct dirent *),
        int (*compare)(const struct dirent **,
                       const struct dirent **))
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (mapping == SYNTHETIC_PATH_MISS)
      return real_scandir(path, entries, filter, compare);

   bool synthetic_dri = path_is_synthetic_dri_directory(mapped_path);
   if (!synthetic_dri) {
      int ret = real_scandir(mapped_path, entries, filter, compare);
      free(mapped_path);
      return ret;
   }

   int (*saved_filter)(const struct dirent *) =
      synthetic_scandir_filter;
   synthetic_scandir_filter = filter;
   int ret = real_scandir(mapped_path, entries,
                          synthetic_scandir_filter_entry, compare);
   synthetic_scandir_filter = saved_filter;
   free(mapped_path);
   return ret;
}

PUBLIC int
scandir64(const char *path, struct dirent64 ***entries,
          int (*filter)(const struct dirent64 *),
          int (*compare)(const struct dirent64 **,
                         const struct dirent64 **))
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (mapping == SYNTHETIC_PATH_MISS)
      return real_scandir64(path, entries, filter, compare);

   bool synthetic_dri = path_is_synthetic_dri_directory(mapped_path);
   if (!synthetic_dri) {
      int ret = real_scandir64(mapped_path, entries, filter, compare);
      free(mapped_path);
      return ret;
   }

   int (*saved_filter)(const struct dirent64 *) =
      synthetic_scandir64_filter;
   synthetic_scandir64_filter = filter;
   int ret = real_scandir64(mapped_path, entries,
                            synthetic_scandir64_filter_entry, compare);
   synthetic_scandir64_filter = saved_filter;
   free(mapped_path);
   return ret;
}

PUBLIC int
scandirat(int dirfd, const char *path, struct dirent ***entries,
          int (*filter)(const struct dirent *),
          int (*compare)(const struct dirent **,
                         const struct dirent **))
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(dirfd, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (mapping == SYNTHETIC_PATH_MISS)
      return real_scandirat(dirfd, path, entries, filter, compare);

   bool synthetic_dri = path_is_synthetic_dri_directory(mapped_path);
   if (!synthetic_dri) {
      int ret =
         real_scandirat(AT_FDCWD, mapped_path, entries, filter, compare);
      free(mapped_path);
      return ret;
   }

   int (*saved_filter)(const struct dirent *) =
      synthetic_scandir_filter;
   synthetic_scandir_filter = filter;
   int ret =
      real_scandirat(AT_FDCWD, mapped_path, entries,
                     synthetic_scandir_filter_entry, compare);
   synthetic_scandir_filter = saved_filter;
   free(mapped_path);
   return ret;
}

PUBLIC int
scandirat64(int dirfd, const char *path, struct dirent64 ***entries,
            int (*filter)(const struct dirent64 *),
            int (*compare)(const struct dirent64 **,
                           const struct dirent64 **))
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(dirfd, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (mapping == SYNTHETIC_PATH_MISS)
      return real_scandirat64(dirfd, path, entries, filter, compare);

   bool synthetic_dri = path_is_synthetic_dri_directory(mapped_path);
   if (!synthetic_dri) {
      int ret =
         real_scandirat64(AT_FDCWD, mapped_path, entries, filter, compare);
      free(mapped_path);
      return ret;
   }

   int (*saved_filter)(const struct dirent64 *) =
      synthetic_scandir64_filter;
   synthetic_scandir64_filter = filter;
   int ret =
      real_scandirat64(AT_FDCWD, mapped_path, entries,
                       synthetic_scandir64_filter_entry, compare);
   synthetic_scandir64_filter = saved_filter;
   free(mapped_path);
   return ret;
}

/* Tracks if the opendir was on /dev/dri. */
PUBLIC DIR *
opendir(const char *name)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(name);
   if (!path_snapshot)
      return NULL;
   name = path_snapshot;

   if (path_is_hidden(name)) {
      errno = ENOENT;
      return NULL;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, name, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MAPPED) {
      DIR *dir = real_opendir(mapped_path);
      bool synthetic_dri =
         dir && path_is_synthetic_dri_directory(mapped_path);
      free(mapped_path);
      if (synthetic_dri && opendir_set) {
         simple_mtx_lock(&shim_lock);
         _mesa_set_add(opendir_set, dir);
         simple_mtx_unlock(&shim_lock);
      }
      return dir;
   }
   if (mapping == SYNTHETIC_PATH_ERROR)
      return NULL;

   DIR *dir = real_opendir(name);
   if (strcmp(name, "/dev/dri") == 0) {
      if (!dir) {
         /* If /dev/dri didn't exist, we still want to be able to return our
          * fake /dev/dri/render* even though we probably can't
          * mkdir("/dev/dri").  Return a fake DIR pointer for that.
          */
         dir = fake_dev_dri;
      }

      simple_mtx_lock(&shim_lock);
      _mesa_set_add(opendir_set, dir);
      simple_mtx_unlock(&shim_lock);
   }

   return dir;
}

PUBLIC DIR *
fdopendir(int fd)
{
   init_shim();

   if (synthetic_fd_is_internal(fd)) {
      errno = EBUSY;
      return NULL;
   }

   char *path = path_base_at_alloc(fd);
   bool synthetic_dri =
      path && path_is_synthetic_dri_directory(path);
   free(path);

   DIR *dir = real_fdopendir(fd);
   if (dir && synthetic_dri && opendir_set) {
      simple_mtx_lock(&shim_lock);
      _mesa_set_add(opendir_set, dir);
      simple_mtx_unlock(&shim_lock);
   }
   return dir;
}

static bool
directory_is_synthetic_dri(DIR *dir)
{
   bool synthetic_dri;
   simple_mtx_lock(&shim_lock);
   synthetic_dri =
      opendir_set && _mesa_set_search(opendir_set, dir) != NULL;
   simple_mtx_unlock(&shim_lock);
   return synthetic_dri;
}

PUBLIC struct dirent *
readdir(DIR *dir)
{
   init_shim();

   bool synthetic_dri = directory_is_synthetic_dri(dir);
   struct dirent *ent = real_readdir(dir);
   if (synthetic_dri && ent &&
       strcmp(ent->d_name, render_node_dirent_name) == 0)
      ent->d_type = DT_CHR;
   return ent;
}

PUBLIC int
readdir_r(DIR *restrict dir, struct dirent *restrict entry,
          struct dirent **restrict result)
{
   init_shim();

   bool synthetic_dri = directory_is_synthetic_dri(dir);
   int ret = real_readdir_r(dir, entry, result);
   if (ret == 0 && synthetic_dri && *result &&
       strcmp((*result)->d_name, render_node_dirent_name) == 0)
      (*result)->d_type = DT_CHR;
   return ret;
}

PUBLIC struct dirent64 *
readdir64(DIR *dir)
{
   init_shim();

   bool synthetic_dri = directory_is_synthetic_dri(dir);
   struct dirent64 *ent = real_readdir64(dir);
   if (synthetic_dri && ent &&
       strcmp(ent->d_name, render_node_dirent_name) == 0)
      ent->d_type = DT_CHR;
   return ent;
}

PUBLIC int
readdir64_r(DIR *restrict dir, struct dirent64 *restrict entry,
            struct dirent64 **restrict result)
{
   init_shim();

   bool synthetic_dri = directory_is_synthetic_dri(dir);
   int ret = real_readdir64_r(dir, entry, result);
   if (ret == 0 && synthetic_dri && *result &&
       strcmp((*result)->d_name, render_node_dirent_name) == 0)
      (*result)->d_type = DT_CHR;
   return ret;
}

/* Cleans up tracking of opendir("/dev/dri") */
PUBLIC int
closedir(DIR *dir)
{
   init_shim();

   simple_mtx_lock(&shim_lock);
   if (opendir_set)
      _mesa_set_remove_key(opendir_set, dir);
   simple_mtx_unlock(&shim_lock);

   return real_closedir(dir);
}

/* Handles libdrm's readlink to figure out what kind of device we have. */
static ssize_t
bootstrap_readlinkat(int dirfd, const char *path, char *buf, size_t size)
{
   if (dirfd == AT_FDCWD && real_readlink)
      return real_readlink(path, buf, size);
   if (real_readlinkat)
      return real_readlinkat(dirfd, path, buf, size);
#ifdef SYS_readlinkat
   return syscall(SYS_readlinkat, dirfd, path, buf, size);
#else
   errno = ENOSYS;
   return -1;
#endif
}

static ssize_t
file_override_readlink_at(int dirfd, const char *path, char *buf, size_t size)
{
   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_nofollow_at(dirfd, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MISS)
      return -2;
   if (mapping == SYNTHETIC_PATH_ERROR)
      return -1;
   if (path_is_in_synthetic_root(mapped_path)) {
      const char *logical =
         mapped_path + strlen(synthetic_root_path);
      const struct file_override *override = file_override_find(logical);
      if (override && override->kind == FILE_OVERRIDE_LINK &&
          override->contents[0] == '/') {
         ssize_t ret =
            copy_readlink_result(buf, size, override->contents);
         free(mapped_path);
         return ret;
      }
   }
   ssize_t ret = real_readlink(mapped_path, buf, size);
   free(mapped_path);
   return ret;
}

static bool
parse_canonical_decimal(const char *text, size_t length, int *value_out)
{
   if (!length || (length > 1 && text[0] == '0'))
      return false;

   unsigned value = 0;
   for (size_t i = 0; i < length; i++) {
      if (text[i] < '0' || text[i] > '9')
         return false;
      unsigned digit = (unsigned)(text[i] - '0');
      if (value > ((unsigned)INT_MAX - digit) / 10)
         return false;
      value = value * 10 + digit;
   }
   *value_out = (int)value;
   return true;
}

static bool
proc_tid_belongs_to_process(int tid)
{
   if (tid <= 0)
      return false;

   char status_path[64];
   int path_length =
      snprintf(status_path, sizeof(status_path), "/proc/%d/status", tid);
   if (path_length < 0 || (size_t)path_length >= sizeof(status_path))
      return false;

   int status_fd =
      syscall(SYS_openat, AT_FDCWD, status_path, O_RDONLY | O_CLOEXEC, 0);
   if (status_fd < 0)
      return false;
   char status[4096];
   ssize_t total = 0;
   while ((size_t)total < sizeof(status) - 1) {
      ssize_t length =
         read(status_fd, status + total, sizeof(status) - 1 - total);
      if (length > 0) {
         total += length;
         continue;
      }
      if (length < 0 && errno == EINTR)
         continue;
      break;
   }
   syscall(SYS_close, status_fd);
   if (total <= 0)
      return false;
   status[total] = '\0';

   const char *line = status;
   while (*line) {
      const char *line_end = strchr(line, '\n');
      if (!line_end)
         line_end = line + strlen(line);
      static const char prefix[] = "Tgid:";
      if ((size_t)(line_end - line) >= sizeof(prefix) - 1 &&
          memcmp(line, prefix, sizeof(prefix) - 1) == 0) {
         const char *value = line + sizeof(prefix) - 1;
         while (value < line_end &&
                (*value == ' ' || *value == '\t'))
            value++;
         int tgid;
         return parse_canonical_decimal(
                   value, (size_t)(line_end - value), &tgid) &&
                tgid == getpid();
      }
      line = *line_end ? line_end + 1 : line_end;
   }
   return false;
}

static bool
proc_fd_directory_owner(const char *directory, pid_t *tid_out)
{
   static const char proc_prefix[] = "/proc/";
   if (strncmp(directory, proc_prefix, sizeof(proc_prefix) - 1) != 0)
      return false;

   const char *cursor = directory + sizeof(proc_prefix) - 1;
   const char *slash = strchr(cursor, '/');
   if (!slash)
      return false;
   int first_id;
   if (!parse_canonical_decimal(
          cursor, (size_t)(slash - cursor), &first_id))
      return false;

   if (strcmp(slash, "/fd") == 0) {
      if (!proc_tid_belongs_to_process(first_id))
         return false;
      *tid_out = first_id;
      return true;
   }

   static const char task_component[] = "/task/";
   if (strncmp(slash, task_component, sizeof(task_component) - 1) != 0 ||
       first_id != getpid())
      return false;
   cursor = slash + sizeof(task_component) - 1;
   slash = strchr(cursor, '/');
   if (!slash || strcmp(slash, "/fd") != 0)
      return false;
   int tid;
   if (!parse_canonical_decimal(
          cursor, (size_t)(slash - cursor), &tid) ||
       !proc_tid_belongs_to_process(tid))
      return false;
   *tid_out = tid;
   return true;
}

static bool
parse_proc_fd_path_at(int dirfd, const char *path, int *fd_out,
                      char **target_path_out)
{
   *target_path_out = NULL;
   char *safe_path = copy_path_argument(path);
   if (!safe_path)
      return false;
   size_t path_length = strlen(safe_path);
   if (!path_length || safe_path[path_length - 1] == '/') {
      free(safe_path);
      return false;
   }

   char *slash = strrchr(safe_path, '/');
   const char *descriptor = slash ? slash + 1 : safe_path;
   int descriptor_fd;
   if (!parse_canonical_decimal(
          descriptor, strlen(descriptor), &descriptor_fd)) {
      free(safe_path);
      return false;
   }

   char *parent;
   if (!slash) {
      parent = strdup(".");
   } else if (slash == safe_path) {
      parent = strdup("/");
   } else {
      parent = strndup(safe_path, (size_t)(slash - safe_path));
   }
   free(safe_path);
   if (!parent)
      return false;

   char *absolute_parent = absolute_path_at_alloc(dirfd, parent);
   free(parent);
   if (!absolute_parent)
      return false;
   char *resolved_parent = real_realpath(absolute_parent, NULL);
   free(absolute_parent);
   if (!resolved_parent)
      return false;
   pid_t target_tid;
   if (!proc_fd_directory_owner(resolved_parent, &target_tid)) {
      free(resolved_parent);
      return false;
   }
   (void)target_tid;

   char *target_path;
   nfasprintf(&target_path, "%s/%d", resolved_parent, descriptor_fd);
   free(resolved_parent);

   *fd_out = descriptor_fd;
   *target_path_out = target_path;
   return true;
}

static ssize_t
copy_readlink_result(char *buffer, size_t size, const char *target)
{
   if (!size) {
      errno = EINVAL;
      return -1;
   }
   size_t length = MIN2(size, strlen(target));
   struct iovec local = {
      .iov_base = (void *)target,
      .iov_len = length,
   };
   struct iovec remote = {
      .iov_base = buffer,
      .iov_len = length,
   };
#ifdef SYS_process_vm_writev
   ssize_t written;
#ifdef DRM_SHIM_TEST
   if (force_process_vm_writev_error) {
      written = -1;
      errno = force_process_vm_writev_error;
   } else
#endif
      written =
         syscall(SYS_process_vm_writev, getpid(), &local,
                 (unsigned long)1, &remote, (unsigned long)1,
                 (unsigned long)0);
#else
   ssize_t written = -1;
   errno = ENOSYS;
#endif
   bool used_proc_memory = false;
   if (written < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES)) {
      used_proc_memory = true;
      written = copy_memory_to_proc(buffer, target, length);
   }
   if (written < 0 &&
       (errno == ENOSYS || errno == EPERM || errno == EACCES ||
        errno == ENOENT)) {
      written = copy_memory_through_pipe(buffer, target, length);
   }
   if (written != (ssize_t)length) {
      if (written >= 0 || (used_proc_memory && errno == EIO))
         errno = EFAULT;
      return -1;
   }
   return written;
}

static ssize_t
tracked_fd_readlink_at(int dirfd, const char *path, char *buffer, size_t size)
{
   int fd;
   char *target_path;
   if (!parse_proc_fd_path_at(dirfd, path, &fd, &target_path))
      return -2;
   (void)fd;

   int target_fd =
      syscall(SYS_openat, AT_FDCWD, target_path,
              O_RDONLY | O_CLOEXEC, 0);
   free(target_path);
   if (target_fd < 0)
      return -2;
   bool selected = drm_shim_fd_reports_selected_device(target_fd);
   syscall(SYS_close, target_fd);
   if (!selected)
      return -2;
   return copy_readlink_result(buffer, size, render_node_path);
}

PUBLIC ssize_t
readlink(const char *path, char *buf, size_t size)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (!drm_shim_inited())
      return bootstrap_readlinkat(AT_FDCWD, path, buf, size);

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return -1;
   }

   ssize_t override_length =
      file_override_readlink_at(AT_FDCWD, path, buf, size);
   if (override_length != -2)
      return override_length;
   override_length =
      tracked_fd_readlink_at(AT_FDCWD, path, buf, size);
   if (override_length != -2)
      return override_length;

   bool hidden = hide_drm_device_path(path);
   if (hidden) {
      errno = ENOENT;
      return -1;
   }

   return real_readlink(path, buf, size);
}

#ifdef __GLIBC__
/* Identical to readlink, but with buffer overflow check */
PUBLIC ssize_t
__readlink_chk(const char *path, char *buf, size_t size, size_t buflen)
{
   if (size > buflen)
      abort();
   return readlink(path, buf, size);
}
#endif

PUBLIC ssize_t
readlinkat(int dirfd, const char *path, char *buf, size_t size)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return -1;
   path = path_snapshot;

   if (!drm_shim_inited())
      return bootstrap_readlinkat(dirfd, path, buf, size);

   if (path_is_hidden_at(dirfd, path)) {
      errno = ENOENT;
      return -1;
   }

   ssize_t override_length =
      file_override_readlink_at(dirfd, path, buf, size);
   if (override_length != -2)
      return override_length;
   override_length = tracked_fd_readlink_at(dirfd, path, buf, size);
   if (override_length != -2)
      return override_length;

   return real_readlinkat(dirfd, path, buf, size);
}

#ifdef __GLIBC__
PUBLIC ssize_t
__readlinkat_chk(int dirfd, const char *path, char *buf, size_t size,
                 size_t buflen)
{
   if (size > buflen)
      abort();
   return readlinkat(dirfd, path, buf, size);
}
#endif

/* Handles libdrm's realpath to figure out what kind of device we have. */
PUBLIC char *
realpath(const char *path, char *resolved_path)
{
   init_shim();

   char *path_snapshot __attribute__((cleanup(free_path_snapshot))) =
      copy_path_argument(path);
   if (!path_snapshot)
      return NULL;
   path = path_snapshot;

   if (path_is_hidden(path)) {
      errno = ENOENT;
      return NULL;
   }

   char *mapped_path;
   enum synthetic_path_result mapping =
      synthetic_path_map_at(AT_FDCWD, path, &mapped_path);
   if (mapping == SYNTHETIC_PATH_MISS)
      return real_realpath(path, resolved_path);
   if (mapping == SYNTHETIC_PATH_ERROR)
      return NULL;

   char *physical = real_realpath(mapped_path, NULL);
   free(mapped_path);
   if (!physical)
      return NULL;
   if (!path_is_in_synthetic_root(physical)) {
      free(physical);
      errno = EXDEV;
      return NULL;
   }

   const char *logical = physical + strlen(synthetic_root_path);
   if (!logical[0])
      logical = "/";
   if (!resolved_path) {
      char *allocated = strdup(logical);
      free(physical);
      return allocated;
   }
   strcpy(resolved_path, logical);
   free(physical);
   return resolved_path;
}

PUBLIC char *
canonicalize_file_name(const char *path)
{
   return realpath(path, NULL);
}

#ifdef __GLIBC__
PUBLIC char *
__realpath_chk(const char *path, char *resolved_path, size_t resolved_size)
{
   if (resolved_path && resolved_size < PATH_MAX)
      abort();
   return realpath(path, resolved_path);
}
#endif

struct exec_fd_guard {
   struct fd_operation_guard operation;
   int *fds;
   int *descriptor_flags;
   size_t count;
   char *environment_entry;
   char **environment;
   bool vfork_direct;
};

static bool
exec_fd_guard_acquire(struct exec_fd_guard *guard)
{
   memset(guard, 0, sizeof(*guard));
   if (p_atomic_read(&inited) &&
       shim_interposition_pid != getpid()) {
      guard->vfork_direct = true;
      return true;
   }
   init_shim();
   fd_operation_guard_acquire(&guard->operation);
   int prepare_error =
      drm_shim_fd_prepare_exec(&guard->fds,
                               &guard->descriptor_flags,
                               &guard->count,
                               &guard->environment_entry);
   if (!prepare_error)
      return true;
   fd_operation_guard_release(&guard->operation);
   errno = -prepare_error;
   return false;
}

static int
exec_fd_guard_failed(struct exec_fd_guard *guard, int result)
{
   if (guard->vfork_direct)
      return result;

   int saved_errno = errno;
   drm_shim_fd_restore_exec(guard->fds, guard->descriptor_flags,
                            guard->count);
   free(guard->fds);
   free(guard->descriptor_flags);
   free(guard->environment_entry);
   free(guard->environment);
   fd_operation_guard_release(&guard->operation);
   errno = saved_errno;
   return result;
}

static bool
exec_environment_is_locator(const char *entry)
{
   static const char prefix[] = DRM_SHIM_EXEC_LOCATOR_ENV "=";
   return entry &&
          strncmp(entry, prefix, sizeof(prefix) - 1) == 0;
}

static char **
exec_fd_guard_environment(struct exec_fd_guard *guard,
                          char *const environment[])
{
   if (guard->vfork_direct)
      return (char **)environment;

   size_t count = 0;
   size_t retained = 0;
   while (environment && environment[count]) {
      if (count == SIZE_MAX / sizeof(*guard->environment) - 2) {
         errno = E2BIG;
         return NULL;
      }
      if (!exec_environment_is_locator(environment[count]))
         retained++;
      count++;
   }

   size_t added = guard->environment_entry ? 1 : 0;
   guard->environment =
      malloc((retained + added + 1) *
             sizeof(*guard->environment));
   if (!guard->environment)
      return NULL;

   size_t output = 0;
   for (size_t index = 0; index < count; index++) {
      if (!exec_environment_is_locator(environment[index]))
         guard->environment[output++] = environment[index];
   }
   if (guard->environment_entry)
      guard->environment[output++] = guard->environment_entry;
   guard->environment[output] = NULL;
   return guard->environment;
}

static char **
exec_argument_vector(const char *first_argument, va_list arguments,
                     bool has_environment,
                     char *const **environment_out)
{
   size_t count = 0;
   const char *argument = first_argument;
   va_list counter;
   va_copy(counter, arguments);
   while (argument) {
      if (count == SIZE_MAX / sizeof(char *) - 1) {
         va_end(counter);
         errno = E2BIG;
         return NULL;
      }
      count++;
      argument = va_arg(counter, const char *);
   }
   if (has_environment)
      *environment_out = va_arg(counter, char *const *);
   va_end(counter);

   char **vector = malloc((count + 1) * sizeof(*vector));
   if (!vector)
      return NULL;
   argument = first_argument;
   va_list filler;
   va_copy(filler, arguments);
   for (size_t index = 0; index < count; index++) {
      vector[index] = (char *)argument;
      argument = va_arg(filler, const char *);
   }
   va_end(filler);
   vector[count] = NULL;
   return vector;
}

PUBLIC int
execve(const char *path, char *const argv[], char *const envp[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return -1;
   char **environment =
      exec_fd_guard_environment(&guard, envp);
   if (!environment && !guard.vfork_direct)
      return exec_fd_guard_failed(&guard, -1);
   int result = real_execve(path, argv, environment);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execveat(int dirfd, const char *path, char *const argv[],
         char *const envp[], int flags)
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return -1;
   char **environment =
      exec_fd_guard_environment(&guard, envp);
   if (!environment && !guard.vfork_direct)
      return exec_fd_guard_failed(&guard, -1);
   int result =
      real_execveat(dirfd, path, argv, environment, flags);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
fexecve(int fd, char *const argv[], char *const envp[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return -1;
   char **environment =
      exec_fd_guard_environment(&guard, envp);
   if (!environment && !guard.vfork_direct)
      return exec_fd_guard_failed(&guard, -1);
   int result = real_fexecve(fd, argv, environment);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execv(const char *path, char *const argv[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return -1;
   char **environment =
      exec_fd_guard_environment(&guard, environ);
   if (!environment)
      return exec_fd_guard_failed(&guard, -1);
   int result = real_execve(path, argv, environment);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execvp(const char *file, char *const argv[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return -1;
   char **environment =
      exec_fd_guard_environment(&guard, environ);
   if (!environment)
      return exec_fd_guard_failed(&guard, -1);
   int result = real_execvpe(file, argv, environment);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execvpe(const char *file, char *const argv[], char *const envp[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return -1;
   char **environment =
      exec_fd_guard_environment(&guard, envp);
   if (!environment && !guard.vfork_direct)
      return exec_fd_guard_failed(&guard, -1);
   int result = real_execvpe(file, argv, environment);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execl(const char *path, const char *argument, ...)
{
   va_list arguments;
   va_start(arguments, argument);
   char **vector =
      exec_argument_vector(argument, arguments, false, NULL);
   va_end(arguments);
   if (!vector)
      return -1;
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard)) {
      free(vector);
      return -1;
   }
   char **environment =
      exec_fd_guard_environment(&guard, environ);
   if (!environment) {
      free(vector);
      return exec_fd_guard_failed(&guard, -1);
   }
   int result = real_execve(path, vector, environment);
   int saved_errno = errno;
   free(vector);
   errno = saved_errno;
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execlp(const char *file, const char *argument, ...)
{
   va_list arguments;
   va_start(arguments, argument);
   char **vector =
      exec_argument_vector(argument, arguments, false, NULL);
   va_end(arguments);
   if (!vector)
      return -1;
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard)) {
      free(vector);
      return -1;
   }
   char **environment =
      exec_fd_guard_environment(&guard, environ);
   if (!environment) {
      free(vector);
      return exec_fd_guard_failed(&guard, -1);
   }
   int result = real_execvpe(file, vector, environment);
   int saved_errno = errno;
   free(vector);
   errno = saved_errno;
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
execle(const char *path, const char *argument, ...)
{
   va_list arguments;
   va_start(arguments, argument);
   char *const *environment = NULL;
   char **vector =
      exec_argument_vector(argument, arguments, true, &environment);
   va_end(arguments);
   if (!vector)
      return -1;
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard)) {
      free(vector);
      return -1;
   }
   char **exec_environment =
      exec_fd_guard_environment(
         &guard, (char *const *)environment);
   if (!exec_environment && !guard.vfork_direct) {
      free(vector);
      return exec_fd_guard_failed(&guard, -1);
   }
   int result =
      real_execve(path, vector, exec_environment);
   int saved_errno = errno;
   free(vector);
   errno = saved_errno;
   return exec_fd_guard_failed(&guard, result);
}

static int
exec_fd_guard_compile_spawn(
   struct exec_fd_guard *guard,
   const posix_spawn_file_actions_t *actions,
   struct spawn_compiled_actions *compiled)
{
   memset(compiled, 0, sizeof(*compiled));
   compiled->locator_fd = -1;
   if (guard->vfork_direct || !actions)
      return 0;

   int result = spawn_compile_file_actions(actions, compiled);
   if (result)
      return result;

   free(guard->environment_entry);
   guard->environment_entry = NULL;
   if (compiled->locator_fd < 0)
      return 0;

   result =
      drm_shim_exec_locator_environment(
         compiled->locator_fd,
         compiled->locator_enables_state,
         &guard->environment_entry);
   return result < 0 ? -result : result;
}

PUBLIC int
posix_spawn(pid_t *pid, const char *path,
            const posix_spawn_file_actions_t *actions,
            const posix_spawnattr_t *attributes,
            char *const arguments[],
            char *const environment[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return errno;
   struct spawn_compiled_actions compiled;
   int compile_error =
      exec_fd_guard_compile_spawn(
         &guard, actions, &compiled);
   if (compile_error) {
      spawn_compiled_actions_finish(&compiled);
      return exec_fd_guard_failed(&guard, compile_error);
   }
   char **exec_environment =
      exec_fd_guard_environment(&guard, environment);
   if (!exec_environment && !guard.vfork_direct) {
      int result = errno;
      spawn_compiled_actions_finish(&compiled);
      return exec_fd_guard_failed(&guard, result);
   }
   const posix_spawn_file_actions_t *call_actions =
      compiled.initialized ? &compiled.actions : actions;
   int result =
      real_posix_spawn(pid, path, call_actions, attributes, arguments,
                       exec_environment);
   spawn_compiled_actions_finish(&compiled);
   return exec_fd_guard_failed(&guard, result);
}

PUBLIC int
posix_spawnp(pid_t *pid, const char *file,
             const posix_spawn_file_actions_t *actions,
             const posix_spawnattr_t *attributes,
             char *const arguments[],
             char *const environment[])
{
   struct exec_fd_guard guard;
   if (!exec_fd_guard_acquire(&guard))
      return errno;
   struct spawn_compiled_actions compiled;
   int compile_error =
      exec_fd_guard_compile_spawn(
         &guard, actions, &compiled);
   if (compile_error) {
      spawn_compiled_actions_finish(&compiled);
      return exec_fd_guard_failed(&guard, compile_error);
   }
   char **exec_environment =
      exec_fd_guard_environment(&guard, environment);
   if (!exec_environment && !guard.vfork_direct) {
      int result = errno;
      spawn_compiled_actions_finish(&compiled);
      return exec_fd_guard_failed(&guard, result);
   }
   const posix_spawn_file_actions_t *call_actions =
      compiled.initialized ? &compiled.actions : actions;
   int result =
      real_posix_spawnp(pid, file, call_actions, attributes, arguments,
                        exec_environment);
   spawn_compiled_actions_finish(&compiled);
   return exec_fd_guard_failed(&guard, result);
}

struct scm_shim_slots {
   struct shim_fd **slots;
   size_t count;
   bool has_shim;
};

struct scm_send_context {
   struct scm_socket_endpoint *receiver;
   struct scm_message_record *record;
};

struct scm_received_rights {
   int *fds;
   size_t count;
   bool truncated;
};

static void
scm_shim_slots_put(struct scm_shim_slots *slots)
{
   for (size_t index = 0; index < slots->count; index++)
      drm_shim_fd_put(slots->slots[index]);
   free(slots->slots);
   memset(slots, 0, sizeof(*slots));
}

static int
scm_shim_slots_append(struct scm_shim_slots *slots,
                      struct shim_fd *shim_fd)
{
   if (slots->count == SIZE_MAX / sizeof(*slots->slots)) {
      drm_shim_fd_put(shim_fd);
      return -ENOMEM;
   }
   struct shim_fd **resized =
      realloc(slots->slots,
              (slots->count + 1) * sizeof(*slots->slots));
   if (!resized) {
      drm_shim_fd_put(shim_fd);
      return -ENOMEM;
   }
   slots->slots = resized;
   slots->slots[slots->count++] = shim_fd;
   slots->has_shim |= shim_fd != NULL;
   return 0;
}

static int
scm_shim_slots_collect(const struct msghdr *user_message,
                       struct scm_shim_slots *slots)
{
   struct msghdr message;
   if (!copy_fixed_from_user(&message, user_message, sizeof(message)))
      return 0;
   if (!message.msg_control || !message.msg_controllen)
      return 0;

   unsigned char *control = malloc(message.msg_controllen);
   if (!control)
      return -ENOMEM;
   if (!copy_fixed_from_user(control, message.msg_control,
                             message.msg_controllen)) {
      free(control);
      return 0;
   }

   size_t offset = 0;
   while (message.msg_controllen - offset >= sizeof(struct cmsghdr)) {
      struct cmsghdr header;
      memcpy(&header, control + offset, sizeof(header));
      size_t minimum_length = CMSG_LEN(0);
      if (header.cmsg_len < minimum_length ||
          header.cmsg_len > message.msg_controllen - offset)
         break;
      size_t data_length = header.cmsg_len - minimum_length;
      if (header.cmsg_level == SOL_SOCKET &&
          header.cmsg_type == SCM_RIGHTS &&
          data_length % sizeof(int) == 0) {
         const unsigned char *data =
            control + offset + CMSG_ALIGN(sizeof(struct cmsghdr));
         for (size_t data_offset = 0;
              data_offset < data_length;
              data_offset += sizeof(int)) {
            int fd;
            memcpy(&fd, data + data_offset, sizeof(fd));
            int append_error =
               scm_shim_slots_append(slots, drm_shim_fd_get(fd));
            if (append_error) {
               free(control);
               return append_error;
            }
         }
      }
      size_t aligned_length = CMSG_ALIGN(header.cmsg_len);
      if (aligned_length < header.cmsg_len ||
          aligned_length > message.msg_controllen - offset)
         break;
      offset += aligned_length;
   }
   free(control);
   return 0;
}

static int
scm_received_rights_append(struct scm_received_rights *rights, int fd)
{
   if (rights->count == SIZE_MAX / sizeof(*rights->fds))
      return -ENOMEM;
   int *resized =
      realloc(rights->fds,
              (rights->count + 1) * sizeof(*rights->fds));
   if (!resized)
      return -ENOMEM;
   rights->fds = resized;
   rights->fds[rights->count++] = fd;
   return 0;
}

static int
scm_received_rights_collect(const struct msghdr *user_message,
                            struct scm_received_rights *rights)
{
   struct msghdr message;
   if (!copy_fixed_from_user(&message, user_message, sizeof(message)))
      return -EFAULT;
   rights->truncated = message.msg_flags & MSG_CTRUNC;
   if (!message.msg_control || !message.msg_controllen)
      return 0;

   unsigned char *control = malloc(message.msg_controllen);
   if (!control)
      return -ENOMEM;
   if (!copy_fixed_from_user(control, message.msg_control,
                             message.msg_controllen)) {
      free(control);
      return 0;
   }

   size_t offset = 0;
   while (message.msg_controllen - offset >= sizeof(struct cmsghdr)) {
      struct cmsghdr header;
      memcpy(&header, control + offset, sizeof(header));
      size_t minimum_length = CMSG_LEN(0);
      if (header.cmsg_len < minimum_length ||
          header.cmsg_len > message.msg_controllen - offset)
         break;
      size_t data_length = header.cmsg_len - minimum_length;
      if (header.cmsg_level == SOL_SOCKET &&
          header.cmsg_type == SCM_RIGHTS &&
          data_length % sizeof(int) == 0) {
         const unsigned char *data =
            control + offset + CMSG_ALIGN(sizeof(struct cmsghdr));
         for (size_t data_offset = 0;
              data_offset < data_length;
              data_offset += sizeof(int)) {
            int fd;
            memcpy(&fd, data + data_offset, sizeof(fd));
            int append_error =
               scm_received_rights_append(rights, fd);
            if (append_error) {
               free(control);
               return append_error;
            }
         }
      }
      size_t aligned_length = CMSG_ALIGN(header.cmsg_len);
      if (aligned_length < header.cmsg_len ||
          aligned_length > message.msg_controllen - offset)
         break;
      offset += aligned_length;
   }
   free(control);
   return 0;
}

static struct scm_message_record *
scm_message_record_create(struct scm_shim_slots *slots)
{
   struct scm_message_record *record = calloc(1, sizeof(*record));
   if (!record)
      return NULL;
   record->slots = slots->slots;
   record->slot_count = slots->count;
   slots->slots = NULL;
   slots->count = 0;
   slots->has_shim = false;
   return record;
}

static int
scm_send_begin(int socket_fd, struct scm_shim_slots *slots,
               struct scm_send_context *context)
{
   memset(context, 0, sizeof(*context));
   struct scm_socket_identity identity;
   scm_socket_identity_get(socket_fd, &identity);

   scm_mutex_lock(&scm_queue_lock);
   struct scm_socket_pair *pair = NULL;
   unsigned endpoint_index = 0;
   struct scm_socket_endpoint *sender =
      scm_socket_endpoint_find_locked(&identity, &pair,
                                      &endpoint_index);
   struct scm_socket_endpoint *receiver =
      sender ? &pair->endpoints[1 - endpoint_index] : NULL;
   if (!sender || !receiver->active) {
      scm_mutex_unlock(&scm_queue_lock);
      return slots->has_shim ? -EOPNOTSUPP : 0;
   }

   struct scm_message_record *record =
      scm_message_record_create(slots);
   if (!record) {
      scm_mutex_unlock(&scm_queue_lock);
      return -ENOMEM;
   }
   if (receiver->tail)
      receiver->tail->next = record;
   else
      receiver->head = record;
   receiver->tail = record;
   context->receiver = receiver;
   context->record = record;
   scm_mutex_unlock(&scm_queue_lock);
   return 0;
}

static void
scm_send_finish(struct scm_send_context *context, bool sent)
{
   if (!context->record)
      return;

   scm_mutex_lock(&scm_queue_lock);
   if (sent) {
      context->record->committed = true;
   } else {
      struct scm_message_record **link =
         &context->receiver->head;
      while (*link && *link != context->record)
         link = &(*link)->next;
      if (*link) {
         *link = context->record->next;
         if (context->receiver->tail == context->record) {
            context->receiver->tail = NULL;
            for (struct scm_message_record *tail =
                    context->receiver->head;
                 tail; tail = tail->next)
               context->receiver->tail = tail;
         }
      }
      scm_message_record_destroy(context->record);
   }
   if (pthread_cond_broadcast(&scm_queue_condition) != 0)
      abort();
   scm_mutex_unlock(&scm_queue_lock);
   memset(context, 0, sizeof(*context));
}

static bool
scm_socket_receive_identity(int socket_fd,
                            struct scm_socket_identity *identity)
{
   scm_socket_identity_get(socket_fd, identity);
   scm_mutex_lock(&scm_queue_lock);
   bool tracked =
      scm_socket_endpoint_find_locked(identity, NULL, NULL) != NULL;
   scm_mutex_unlock(&scm_queue_lock);
   return tracked;
}

static struct scm_message_record *
scm_receive_record_locked(
   const struct scm_socket_identity *identity, bool peek)
{
   while (true) {
      struct scm_socket_endpoint *endpoint =
         scm_socket_endpoint_find_locked(identity, NULL, NULL);
      if (!endpoint || !endpoint->head)
         return NULL;
      if (!endpoint->head->committed) {
         if (pthread_cond_wait(&scm_queue_condition,
                               &scm_queue_lock) != 0)
            abort();
         continue;
      }
      struct scm_message_record *record = endpoint->head;
      if (!peek) {
         endpoint->head = record->next;
         if (!endpoint->head)
            endpoint->tail = NULL;
         record->next = NULL;
      }
      return record;
   }
}

static void
scm_received_rights_close(struct scm_received_rights *rights)
{
   for (size_t index = 0; index < rights->count; index++) {
      drm_shim_fd_unregister(rights->fds[index]);
      real_close(rights->fds[index]);
   }
}

static int
scm_receive_apply(
   const struct scm_socket_identity *identity,
   struct scm_received_rights *rights, bool peek,
   bool ancillary_discarded)
{
   int result = 0;
   scm_mutex_lock(&scm_queue_lock);
   struct scm_message_record *record =
      scm_receive_record_locked(identity, peek);
   if (!record) {
      if (rights && rights->count)
         result = -EPROTO;
      goto unlock;
   }

   if (!ancillary_discarded && rights) {
      bool count_matches =
         rights->count == record->slot_count ||
         (rights->truncated &&
          rights->count <= record->slot_count);
      if (!count_matches) {
         result = -EPROTO;
      } else {
         for (size_t index = 0; index < rights->count; index++) {
            if (!record->slots[index])
               continue;
            int register_error =
               drm_shim_fd_register(rights->fds[index],
                                    record->slots[index]);
            if (register_error) {
               result = register_error;
               break;
            }
         }
      }
   }

unlock:
   scm_mutex_unlock(&scm_queue_lock);
   if (record && !peek)
      scm_message_record_destroy(record);
   if (result && rights)
      scm_received_rights_close(rights);
   return result;
}

static ssize_t
scm_plain_send(int socket_fd, ssize_t (*send_call)(void *),
               void *call_data, bool zero_is_record)
{
   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_send_lock);
   struct scm_shim_slots slots = {0};
   struct scm_send_context context;
   int prepare_error =
      scm_send_begin(socket_fd, &slots, &context);
   ssize_t ret = -1;
   if (prepare_error) {
      errno = -prepare_error;
   } else {
      ret = send_call(call_data);
      int saved_errno = errno;
      scm_send_finish(&context,
                      ret > 0 || (ret == 0 && zero_is_record));
      errno = saved_errno;
   }
   scm_mutex_unlock(&scm_send_lock);
   cancellation_guard_release(&cancellation);
   return ret;
}

struct scm_send_args {
   int socket_fd;
   const void *buffer;
   size_t length;
   int flags;
   const struct sockaddr *address;
   socklen_t address_length;
};

static ssize_t
scm_real_send_call(void *data)
{
   const struct scm_send_args *args = data;
   return real_send(args->socket_fd, args->buffer, args->length,
                    args->flags);
}

static ssize_t
scm_real_sendto_call(void *data)
{
   const struct scm_send_args *args = data;
   return real_sendto(args->socket_fd, args->buffer, args->length,
                      args->flags, args->address,
                      args->address_length);
}

struct scm_write_args {
   int fd;
   const void *buffer;
   size_t length;
};

static ssize_t
scm_real_write_call(void *data)
{
   const struct scm_write_args *args = data;
   return real_write(args->fd, args->buffer, args->length);
}

struct scm_writev_args {
   int fd;
   const struct iovec *iov;
   int iov_count;
};

static ssize_t
scm_real_writev_call(void *data)
{
   const struct scm_writev_args *args = data;
   return real_writev(args->fd, args->iov, args->iov_count);
}

PUBLIC ssize_t
send(int socket_fd, const void *buffer, size_t length, int flags)
{
   init_shim();
   struct scm_send_args args = {
      .socket_fd = socket_fd,
      .buffer = buffer,
      .length = length,
      .flags = flags,
   };
   return scm_plain_send(socket_fd, scm_real_send_call, &args, true);
}

PUBLIC ssize_t
sendto(int socket_fd, const void *buffer, size_t length, int flags,
       const struct sockaddr *address, socklen_t address_length)
{
   init_shim();
   struct scm_send_args args = {
      .socket_fd = socket_fd,
      .buffer = buffer,
      .length = length,
      .flags = flags,
      .address = address,
      .address_length = address_length,
   };
   return scm_plain_send(socket_fd, scm_real_sendto_call, &args, true);
}

PUBLIC ssize_t
write(int fd, const void *buffer, size_t length)
{
   init_shim();
   struct scm_write_args args = {
      .fd = fd,
      .buffer = buffer,
      .length = length,
   };
   return scm_plain_send(fd, scm_real_write_call, &args, true);
}
PUBLIC ssize_t __write(int, const void *, size_t)
   __attribute__((alias("write")));

PUBLIC ssize_t
writev(int fd, const struct iovec *iov, int iov_count)
{
   init_shim();
   struct scm_writev_args args = {
      .fd = fd,
      .iov = iov,
      .iov_count = iov_count,
   };
   return scm_plain_send(fd, scm_real_writev_call, &args, false);
}

PUBLIC ssize_t
sendmsg(int socket_fd, const struct msghdr *message, int flags)
{
   init_shim();

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   struct fd_operation_guard operation;
   fd_operation_guard_acquire(&operation);
   struct scm_shim_slots slots = {0};
   int collect_error = scm_shim_slots_collect(message, &slots);
   fd_operation_guard_release(&operation);
   ssize_t ret = -1;
   if (collect_error) {
      errno = -collect_error;
      goto out;
   }

   scm_mutex_lock(&scm_send_lock);
   struct scm_send_context context;
   int prepare_error =
      scm_send_begin(socket_fd, &slots, &context);
   if (prepare_error) {
      errno = -prepare_error;
   } else {
      ret = real_sendmsg(socket_fd, message, flags);
      int saved_errno = errno;
      scm_send_finish(&context, ret >= 0);
      errno = saved_errno;
   }
   scm_mutex_unlock(&scm_send_lock);

out:
   scm_shim_slots_put(&slots);
   cancellation_guard_release(&cancellation);
   return ret;
}

PUBLIC int
sendmmsg(int socket_fd, struct mmsghdr *messages, unsigned int count,
         int flags)
{
   init_shim();

   struct scm_shim_slots *message_slots =
      calloc(count, sizeof(*message_slots));
   struct scm_send_context *contexts =
      calloc(count, sizeof(*contexts));
   if (count && (!message_slots || !contexts)) {
      free(message_slots);
      free(contexts);
      return -1;
   }

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   struct fd_operation_guard operation;
   fd_operation_guard_acquire(&operation);
   int result = -1;
   for (unsigned int index = 0; index < count; index++) {
      struct mmsghdr message;
      if (!copy_fixed_from_user(&message, &messages[index],
                                sizeof(message)))
         continue;
      int collect_error =
         scm_shim_slots_collect(&message.msg_hdr,
                                &message_slots[index]);
      if (collect_error) {
         errno = -collect_error;
         fd_operation_guard_release(&operation);
         goto out;
      }
   }
   fd_operation_guard_release(&operation);

   scm_mutex_lock(&scm_send_lock);
   unsigned int prepared = 0;
   for (; prepared < count; prepared++) {
      int prepare_error =
         scm_send_begin(socket_fd, &message_slots[prepared],
                        &contexts[prepared]);
      if (prepare_error) {
         errno = -prepare_error;
         break;
      }
   }
   if (prepared == count) {
      result = real_sendmmsg(socket_fd, messages, count, flags);
      int saved_errno = errno;
      for (unsigned int index = 0; index < count; index++)
         scm_send_finish(&contexts[index],
                         result > 0 &&
                         index < (unsigned int)result);
      errno = saved_errno;
   } else {
      for (unsigned int index = 0; index < prepared; index++)
         scm_send_finish(&contexts[index], false);
   }
   scm_mutex_unlock(&scm_send_lock);

out:
   for (unsigned int index = 0; index < count; index++)
      scm_shim_slots_put(&message_slots[index]);
   free(message_slots);
   free(contexts);
   cancellation_guard_release(&cancellation);
   return result;
}

static ssize_t
scm_plain_receive(int flags, ssize_t result, bool can_consume,
                  const struct scm_socket_identity *identity)
{
   if (result < 0 || !can_consume)
      return result;
   int apply_error =
      scm_receive_apply(identity, NULL, flags & MSG_PEEK, true);
   if (apply_error) {
      errno = -apply_error;
      return -1;
   }
   return result;
}

PUBLIC ssize_t
recvmsg(int socket_fd, struct msghdr *message, int flags)
{
   init_shim();

   struct scm_socket_identity identity;
   if (!scm_socket_receive_identity(socket_fd, &identity))
      return real_recvmsg(socket_fd, message, flags);

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_receive_lock);
   ssize_t ret = real_recvmsg(socket_fd, message, flags);
   int saved_errno = errno;
   if (ret >= 0) {
      struct scm_received_rights rights = {0};
      int collect_error =
         scm_received_rights_collect(message, &rights);
      int apply_error =
         collect_error
            ? scm_receive_apply(&identity, NULL,
                                flags & MSG_PEEK, true)
            : scm_receive_apply(&identity, &rights,
                                flags & MSG_PEEK, false);
      if (collect_error)
         scm_received_rights_close(&rights);
      free(rights.fds);
      if (collect_error || apply_error) {
         ret = -1;
         saved_errno =
            collect_error ? -collect_error : -apply_error;
      }
   }
   scm_mutex_unlock(&scm_receive_lock);
   cancellation_guard_release(&cancellation);
   errno = saved_errno;
   return ret;
}

PUBLIC int
recvmmsg(int socket_fd, struct mmsghdr *messages, unsigned int count,
         int flags, struct timespec *timeout)
{
   init_shim();

   struct scm_socket_identity identity;
   if (!scm_socket_receive_identity(socket_fd, &identity))
      return real_recvmmsg(socket_fd, messages, count, flags, timeout);

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_receive_lock);
   int ret =
      real_recvmmsg(socket_fd, messages, count, flags, timeout);
   int saved_errno = errno;
   int message_error = 0;
   for (int index = 0; index < ret; index++) {
      struct mmsghdr message;
      if (!copy_fixed_from_user(&message, &messages[index],
                                sizeof(message))) {
         int apply_error =
            scm_receive_apply(&identity, NULL,
                              flags & MSG_PEEK, true);
         if (!message_error)
            message_error = apply_error ? apply_error : -EFAULT;
         continue;
      }
      struct scm_received_rights rights = {0};
      int collect_error =
         scm_received_rights_collect(&message.msg_hdr, &rights);
      int apply_error =
         collect_error
            ? scm_receive_apply(&identity, NULL,
                                flags & MSG_PEEK, true)
            : scm_receive_apply(&identity, &rights,
                                flags & MSG_PEEK, false);
      if (collect_error)
         scm_received_rights_close(&rights);
      free(rights.fds);
      if (!message_error)
         message_error = collect_error ? collect_error : apply_error;
   }
   if (message_error) {
      ret = -1;
      saved_errno = -message_error;
   }
   scm_mutex_unlock(&scm_receive_lock);
   cancellation_guard_release(&cancellation);
   errno = saved_errno;
   return ret;
}

PUBLIC ssize_t
recv(int socket_fd, void *buffer, size_t length, int flags)
{
   init_shim();
   struct scm_socket_identity identity;
   if (!scm_socket_receive_identity(socket_fd, &identity))
      return real_recv(socket_fd, buffer, length, flags);

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_receive_lock);
   ssize_t ret = real_recv(socket_fd, buffer, length, flags);
   int saved_errno = errno;
   ret =
      scm_plain_receive(flags, ret, length > 0, &identity);
   if (ret < 0)
      saved_errno = errno;
   scm_mutex_unlock(&scm_receive_lock);
   cancellation_guard_release(&cancellation);
   errno = saved_errno;
   return ret;
}

#ifdef __GLIBC__
PUBLIC ssize_t
__recv_chk(int socket_fd, void *buffer, size_t length,
           size_t buffer_length, int flags)
{
   if (length > buffer_length)
      return real___recv_chk(socket_fd, buffer, length,
                             buffer_length, flags);
   return recv(socket_fd, buffer, length, flags);
}
#endif

PUBLIC ssize_t
recvfrom(int socket_fd, void *restrict buffer, size_t length, int flags,
         __SOCKADDR_ARG address,
         socklen_t *restrict address_length)
{
   init_shim();
   struct scm_socket_identity identity;
   if (!scm_socket_receive_identity(socket_fd, &identity))
      return real_recvfrom(socket_fd, buffer, length, flags,
                           address, address_length);

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_receive_lock);
   ssize_t ret =
      real_recvfrom(socket_fd, buffer, length, flags,
                    address, address_length);
   int saved_errno = errno;
   ret =
      scm_plain_receive(flags, ret, length > 0, &identity);
   if (ret < 0)
      saved_errno = errno;
   scm_mutex_unlock(&scm_receive_lock);
   cancellation_guard_release(&cancellation);
   errno = saved_errno;
   return ret;
}

#ifdef __GLIBC__
PUBLIC ssize_t
__recvfrom_chk(int socket_fd, void *restrict buffer, size_t length,
               size_t buffer_length, int flags,
               __SOCKADDR_ARG address,
               socklen_t *restrict address_length)
{
   if (length > buffer_length)
      return real___recvfrom_chk(socket_fd, buffer, length,
                                 buffer_length, flags,
                                 address, address_length);
   return recvfrom(socket_fd, buffer, length, flags,
                   address, address_length);
}
#endif

PUBLIC ssize_t
read(int fd, void *buffer, size_t length)
{
   init_shim();
   struct scm_socket_identity identity;
   if (!scm_socket_receive_identity(fd, &identity))
      return real_read(fd, buffer, length);

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_receive_lock);
   ssize_t ret = real_read(fd, buffer, length);
   int saved_errno = errno;
   ret = scm_plain_receive(0, ret, length > 0, &identity);
   if (ret < 0)
      saved_errno = errno;
   scm_mutex_unlock(&scm_receive_lock);
   cancellation_guard_release(&cancellation);
   errno = saved_errno;
   return ret;
}
PUBLIC ssize_t __read(int, void *, size_t)
   __attribute__((alias("read")));

#ifdef __GLIBC__
PUBLIC ssize_t
__read_chk(int fd, void *buffer, size_t length, size_t buffer_length)
{
   if (length > buffer_length)
      return real___read_chk(fd, buffer, length, buffer_length);
   return read(fd, buffer, length);
}
#endif

PUBLIC ssize_t
readv(int fd, const struct iovec *iov, int iov_count)
{
   init_shim();
   struct scm_socket_identity identity;
   if (!scm_socket_receive_identity(fd, &identity))
      return real_readv(fd, iov, iov_count);

   struct cancellation_guard cancellation;
   cancellation_guard_acquire(&cancellation);
   scm_mutex_lock(&scm_receive_lock);
   ssize_t ret = real_readv(fd, iov, iov_count);
   int saved_errno = errno;
   bool can_consume = ret > 0;
   if (ret == 0 && iov_count > 0) {
      for (int index = 0; index < iov_count; index++) {
         struct iovec entry;
         if (!copy_fixed_from_user(&entry, &iov[index],
                                   sizeof(entry)))
            break;
         if (entry.iov_len) {
            can_consume = true;
            break;
         }
      }
   }
   ret = scm_plain_receive(0, ret, can_consume, &identity);
   if (ret < 0)
      saved_errno = errno;
   scm_mutex_unlock(&scm_receive_lock);
   cancellation_guard_release(&cancellation);
   errno = saved_errno;
   return ret;
}

/* Main entrypoint to DRM drivers: the ioctl syscall.  We send all ioctls on
 * our DRM fd to drm_shim_ioctl().
 */
PUBLIC int
ioctl(int fd, unsigned long request, ...)
{
   init_shim();

   bool descriptor_flag_ioctl =
      request == FIOCLEX || request == FIONCLEX;
   va_list ap;
   void *arg = NULL;
   if (!descriptor_flag_ioctl) {
      va_start(ap, request);
      arg = va_arg(ap, void *);
      va_end(ap);
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   if (!shim_fd) {
      fd_operation_guard_release(&guard);
      return real_ioctl(fd, request, arg);
   }
   if (descriptor_flag_ioctl) {
      int ret = real_ioctl(fd, request, NULL);
      if (ret == 0)
         drm_shim_fd_update_cloexec(fd);
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      return ret;
   }
   if (shim_fd->path_only) {
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      errno = EBADF;
      return -1;
   }

   int ret = drm_shim_ioctl(shim_fd, fd, request, arg);
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   if (ret < 0) {
      errno = -ret;
      return -1;
   }
   return ret;
}

PUBLIC off64_t
lseek64(int fd, off64_t offset, int whence)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   if (!shim_fd) {
      fd_operation_guard_release(&guard);
      return real_lseek64(fd, offset, whence);
   }
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   return real_lseek64(fd, offset, whence);
}

PUBLIC off_t
lseek(int fd, off_t offset, int whence)
{
   off64_t result = lseek64(fd, (off64_t)offset, whence);
   return (off_t)result;
}

PUBLIC off_t __lseek(int, off_t, int)
   __attribute__((alias("lseek")));
PUBLIC off64_t __lseek64(int, off64_t, int)
   __attribute__((alias("lseek64")));

PUBLIC int
flock(int fd, int operation)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   if (!shim_fd) {
      fd_operation_guard_release(&guard);
      return real_flock(fd, operation);
   }
   bool path_only = shim_fd->path_only;
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);
   if (path_only) {
      errno = EBADF;
      return -1;
   }
   return real_flock(fd, operation);
}

/* Gallium uses this to dup the incoming fd on gbm screen creation */
enum fcntl_argument_kind {
   FCNTL_NO_ARGUMENT,
   FCNTL_INT_ARGUMENT,
   FCNTL_POINTER_ARGUMENT,
};

static enum fcntl_argument_kind
fcntl_argument_kind(int cmd)
{
   switch (cmd) {
   case F_GETFD:
   case F_GETFL:
   case F_GETOWN:
#ifdef F_GETSIG
   case F_GETSIG:
#endif
#ifdef F_GETLEASE
   case F_GETLEASE:
#endif
#ifdef F_GETPIPE_SZ
   case F_GETPIPE_SZ:
#endif
#ifdef F_GET_SEALS
   case F_GET_SEALS:
#endif
      return FCNTL_NO_ARGUMENT;

   case F_DUPFD:
   case F_DUPFD_CLOEXEC:
#ifdef F_DUPFD_QUERY
   case F_DUPFD_QUERY:
#endif
   case F_SETFD:
   case F_SETFL:
   case F_SETOWN:
#ifdef F_SETSIG
   case F_SETSIG:
#endif
#ifdef F_SETLEASE
   case F_SETLEASE:
#endif
#ifdef F_NOTIFY
   case F_NOTIFY:
#endif
#ifdef F_SETPIPE_SZ
   case F_SETPIPE_SZ:
#endif
#ifdef F_ADD_SEALS
   case F_ADD_SEALS:
#endif
      return FCNTL_INT_ARGUMENT;

   default:
      return FCNTL_POINTER_ARGUMENT;
   }
}

static bool
fcntl_is_lock_command(int cmd)
{
   if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
      return true;
#if defined(F_GETLK64) && F_GETLK64 != F_GETLK
   if (cmd == F_GETLK64)
      return true;
#endif
#if defined(F_SETLK64) && F_SETLK64 != F_SETLK
   if (cmd == F_SETLK64)
      return true;
#endif
#if defined(F_SETLKW64) && F_SETLKW64 != F_SETLKW
   if (cmd == F_SETLKW64)
      return true;
#endif
#ifdef F_OFD_GETLK
   if (cmd == F_OFD_GETLK)
      return true;
#endif
#ifdef F_OFD_SETLK
   if (cmd == F_OFD_SETLK)
      return true;
#endif
#ifdef F_OFD_SETLKW
   if (cmd == F_OFD_SETLKW)
      return true;
#endif
   return false;
}

static bool
fcntl_is_lock_query(int cmd)
{
   if (cmd == F_GETLK)
      return true;
#if defined(F_GETLK64) && F_GETLK64 != F_GETLK
   if (cmd == F_GETLK64)
      return true;
#endif
#ifdef F_OFD_GETLK
   if (cmd == F_OFD_GETLK)
      return true;
#endif
   return false;
}

PUBLIC int
fcntl(int fd, int cmd, ...)
{
   init_shim();

   if (synthetic_fd_is_internal(fd)) {
      errno = EBUSY;
      return -1;
   }

   if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      struct shim_fd *shim_fd = drm_shim_fd_get(fd);
      va_list ap;
      va_start(ap, cmd);
      int minimum_fd = va_arg(ap, int);
      va_end(ap);

      int ret = real_fcntl(fd, cmd, minimum_fd);
      if (ret < 0) {
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return ret;
      }

      int registration_error = 0;
      if (shim_fd) {
         registration_error = drm_shim_fd_register(ret, shim_fd);
      } else {
         if (register_render_fd_if_needed(fd) < 0) {
            registration_error = -errno;
         } else {
            struct shim_fd *detected_shim_fd = drm_shim_fd_get(fd);
            if (detected_shim_fd) {
               registration_error =
                  drm_shim_fd_register(ret, detected_shim_fd);
               drm_shim_fd_put(detected_shim_fd);
            }
         }
      }
      drm_shim_fd_put(shim_fd);
      if (registration_error) {
         real_close(ret);
         errno = -registration_error;
         ret = -1;
      }
      fd_operation_guard_release(&guard);
      return ret;
   }

   if (fcntl_is_lock_command(cmd)) {
      va_list ap;
      va_start(ap, cmd);
      void *user_lock = va_arg(ap, void *);
      va_end(ap);

      struct fd_operation_guard guard;
      fd_operation_guard_acquire(&guard);
      struct shim_fd *shim_fd = drm_shim_fd_get(fd);
      if (!shim_fd) {
         fd_operation_guard_release(&guard);
         return real_fcntl(fd, cmd, user_lock);
      }

      union drm_shim_fcntl_lock lock_storage;
      enum drm_shim_fcntl_lock_layout lock_layout =
         drm_shim_fcntl_command_lock_layout(cmd);
      size_t lock_size = drm_shim_fcntl_lock_size(lock_layout);
      if (!copy_fixed_from_user(&lock_storage, user_lock, lock_size)) {
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return -1;
      }
      short lock_type =
         drm_shim_fcntl_lock_type(&lock_storage, lock_layout);

      if (shim_fd->path_only) {
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         errno = EBADF;
         return -1;
      }

      int status_flags = real_fcntl(fd, F_GETFL);
      bool query = fcntl_is_lock_query(cmd);
      if (status_flags < 0) {
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return -1;
      }
      if ((!query && lock_type == F_RDLCK &&
           (status_flags & O_ACCMODE) == O_WRONLY) ||
          (!query && lock_type == F_WRLCK &&
           (status_flags & O_ACCMODE) == O_RDONLY)) {
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         errno = EBADF;
         return -1;
      }

      off64_t base;
      switch (drm_shim_fcntl_lock_whence(&lock_storage,
                                         lock_layout)) {
      case SEEK_SET:
      case SEEK_END:
         base = 0;
         break;
      case SEEK_CUR:
         base = lseek64(fd, 0, SEEK_CUR);
         if (base < 0) {
            drm_shim_fd_put(shim_fd);
            fd_operation_guard_release(&guard);
            return -1;
         }
         break;
      default:
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         errno = EINVAL;
         return -1;
      }
      if (!drm_shim_fcntl_normalize_lock(&lock_storage,
                                         lock_layout, base)) {
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         return -1;
      }

      fd_operation_guard_release(&guard);
      void *normalized_lock =
         lock_layout == DRM_SHIM_FCNTL_LOCK_FLOCK64
            ? (void *)&lock_storage.large
            : (void *)&lock_storage.native;
      int ret = real_fcntl(fd, cmd, normalized_lock);
      drm_shim_fd_put(shim_fd);
      if (ret == 0 && fcntl_is_lock_query(cmd) &&
          !copy_fixed_to_user(user_lock, &lock_storage, lock_size))
         return -1;
      return ret;
   }

   enum fcntl_argument_kind argument_kind = fcntl_argument_kind(cmd);
   if (argument_kind == FCNTL_NO_ARGUMENT) {
#ifdef F_GET_SEALS
      if (cmd == F_GET_SEALS) {
         struct fd_operation_guard guard;
         fd_operation_guard_acquire(&guard);
         struct shim_fd *shim_fd = drm_shim_fd_get(fd);
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         if (shim_fd) {
            errno = EINVAL;
            return -1;
         }
      }
#endif
      return real_fcntl(fd, cmd);
   }

   va_list ap;
   va_start(ap, cmd);
   if (argument_kind == FCNTL_INT_ARGUMENT) {
      int argument = va_arg(ap, int);
      va_end(ap);
#ifdef F_ADD_SEALS
      if (cmd == F_ADD_SEALS) {
         struct fd_operation_guard guard;
         fd_operation_guard_acquire(&guard);
         struct shim_fd *shim_fd = drm_shim_fd_get(fd);
         drm_shim_fd_put(shim_fd);
         fd_operation_guard_release(&guard);
         if (shim_fd) {
            errno = EINVAL;
            return -1;
         }
      }
#endif
      if (cmd == F_SETFD) {
         struct fd_operation_guard guard;
         fd_operation_guard_acquire(&guard);
         int ret = real_fcntl(fd, cmd, argument);
         if (ret == 0)
            drm_shim_fd_update_cloexec(fd);
         fd_operation_guard_release(&guard);
         return ret;
      }
      return real_fcntl(fd, cmd, argument);
   }

   void *argument = va_arg(ap, void *);
   va_end(ap);
   return real_fcntl(fd, cmd, argument);
}
PUBLIC int fcntl64(int, int, ...)
   __attribute__((alias("fcntl")));
PUBLIC int __fcntl(int, int, ...) __attribute__((alias("fcntl")));

static int
drm_shim_lockf64(int fd, int command, off64_t length)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   if (!shim_fd) {
      fd_operation_guard_release(&guard);
      return real_lockf64(fd, command, length);
   }
   if (shim_fd->path_only) {
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      errno = EBADF;
      return -1;
   }

   int status_flags = real_fcntl(fd, F_GETFL);
   if (status_flags < 0 ||
       ((command == F_LOCK || command == F_TLOCK) &&
        (status_flags & O_ACCMODE) == O_RDONLY)) {
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      if (status_flags >= 0)
         errno = EBADF;
      return -1;
   }

   off64_t position = lseek64(fd, 0, SEEK_CUR);
   if (position < 0) {
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      return -1;
   }
   struct flock64 lock = {
      .l_type = command == F_ULOCK ? F_UNLCK : F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = position,
      .l_len = length,
   };
   int fcntl_command;
   switch (command) {
   case F_LOCK:
      fcntl_command = F_SETLKW64;
      break;
   case F_TLOCK:
   case F_ULOCK:
      fcntl_command = F_SETLK64;
      break;
   case F_TEST:
      fcntl_command = F_GETLK64;
      break;
   default:
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      errno = EINVAL;
      return -1;
   }

   fd_operation_guard_release(&guard);
   int ret = real_fcntl(fd, fcntl_command, &lock);
   if (ret == 0 && command == F_TEST &&
       lock.l_type != F_UNLCK && lock.l_pid != getpid()) {
      ret = -1;
      errno = EACCES;
   }
   drm_shim_fd_put(shim_fd);
   return ret;
}

PUBLIC int
lockf64(int fd, int command, off64_t length)
{
   return drm_shim_lockf64(fd, command, length);
}

PUBLIC int
lockf(int fd, int command, off_t length)
{
   init_shim();
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   drm_shim_fd_put(shim_fd);
   if (!shim_fd)
      return real_lockf(fd, command, length);
   return drm_shim_lockf64(fd, command, (off64_t)length);
}

/* I wrote this when trying to fix gallium screen creation, leaving it around
 * since it's probably good to have.
 */
PUBLIC int
dup(int fd)
{
   init_shim();

   if (synthetic_fd_is_internal(fd)) {
      errno = EBUSY;
      return -1;
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   int ret = real_dup(fd);
   if (shim_fd && ret >= 0) {
      int registration_error = drm_shim_fd_register(ret, shim_fd);
      if (registration_error) {
         real_close(ret);
         errno = -registration_error;
         ret = -1;
      }
   }
   drm_shim_fd_put(shim_fd);
   fd_operation_guard_release(&guard);

   return ret;
}

PUBLIC int
dup2(int old_fd, int new_fd)
{
   init_shim();

   if (synthetic_fd_is_internal(old_fd) ||
       synthetic_fd_is_internal(new_fd)) {
      errno = EBUSY;
      return -1;
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(old_fd);
   struct shim_fd *replaced_shim_fd =
      old_fd == new_fd ? NULL : drm_shim_fd_get(new_fd);
   int ret = real_dup2(old_fd, new_fd);
   bool replacement_performed = ret >= 0 && old_fd != new_fd;
   if (ret >= 0 && old_fd != new_fd) {
      drm_shim_file_release_posix_locks(replaced_shim_fd);
      if (shim_fd) {
         int registration_error =
            drm_shim_fd_register(new_fd, shim_fd);
         if (registration_error) {
            real_close(new_fd);
            errno = -registration_error;
            ret = -1;
         }
      } else {
         drm_shim_fd_unregister(new_fd);
      }
   }
   drm_shim_fd_put(replaced_shim_fd);
   drm_shim_fd_put(shim_fd);
   int saved_errno = errno;
   fd_operation_guard_release(&guard);
   if (replacement_performed)
      scm_socket_reap_closed();
   errno = saved_errno;
   return ret;
}
PUBLIC int __dup2(int, int) __attribute__((alias("dup2")));

PUBLIC int
dup3(int old_fd, int new_fd, int flags)
{
   init_shim();

   if (synthetic_fd_is_internal(old_fd) ||
       synthetic_fd_is_internal(new_fd)) {
      errno = EBUSY;
      return -1;
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(old_fd);
   struct shim_fd *replaced_shim_fd =
      old_fd == new_fd ? NULL : drm_shim_fd_get(new_fd);
   int ret;
   if (real_dup3) {
      ret = real_dup3(old_fd, new_fd, flags);
   } else {
#ifdef SYS_dup3
      ret = syscall(SYS_dup3, old_fd, new_fd, flags);
#else
      errno = ENOSYS;
      ret = -1;
#endif
   }

   bool replacement_performed = ret >= 0;
   if (ret >= 0) {
      drm_shim_file_release_posix_locks(replaced_shim_fd);
      if (shim_fd) {
         int registration_error =
            drm_shim_fd_register(new_fd, shim_fd);
         if (registration_error) {
            real_close(new_fd);
            errno = -registration_error;
            ret = -1;
         }
      } else {
         drm_shim_fd_unregister(new_fd);
      }
   }
   drm_shim_fd_put(replaced_shim_fd);
   drm_shim_fd_put(shim_fd);
   int saved_errno = errno;
   fd_operation_guard_release(&guard);
   if (replacement_performed)
      scm_socket_reap_closed();
   errno = saved_errno;
   return ret;
}

PUBLIC void *
mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   if (shim_fd) {
      void *mapping =
         drm_shim_mmap(shim_fd, addr, length, prot, flags, fd, offset);
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      return mapping;
   }

   void *mapping =
      real_mmap(addr, length, prot, flags, fd, offset);
   if (mapping != MAP_FAILED)
      drm_shim_mapping_remove(mapping, length);
   fd_operation_guard_release(&guard);
   return mapping;
}

PUBLIC void *
mmap64(void* addr, size_t length, int prot, int flags, int fd, off64_t offset)
{
   init_shim();

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_fd *shim_fd = drm_shim_fd_get(fd);
   if (shim_fd) {
      void *mapping =
         drm_shim_mmap(shim_fd, addr, length, prot, flags, fd, offset);
      drm_shim_fd_put(shim_fd);
      fd_operation_guard_release(&guard);
      return mapping;
   }

   void *mapping =
      real_mmap64(addr, length, prot, flags, fd, offset);
   if (mapping != MAP_FAILED)
      drm_shim_mapping_remove(mapping, length);
   fd_operation_guard_release(&guard);
   return mapping;
}

PUBLIC int
munmap(void *address, size_t length)
{
   get_function_pointers();

   if (!drm_shim_inited())
      return real_munmap(address, length);

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   int ret = real_munmap(address, length);
   if (ret == 0)
      drm_shim_mapping_remove(address, length);
   fd_operation_guard_release(&guard);
   return ret;
}

PUBLIC void *
mremap(void *old_address, size_t old_size, size_t new_size, int flags, ...)
{
   get_function_pointers();

   void *new_address = NULL;
   if (flags & MREMAP_FIXED) {
      va_list arguments;
      va_start(arguments, flags);
      new_address = va_arg(arguments, void *);
      va_end(arguments);
   }

   if (!drm_shim_inited()) {
      return flags & MREMAP_FIXED
                ? real_mremap(old_address, old_size, new_size, flags,
                              new_address)
                : real_mremap(old_address, old_size, new_size, flags);
   }

   struct fd_operation_guard guard;
   fd_operation_guard_acquire(&guard);
   struct shim_bo *bo =
      drm_shim_mapping_get(old_address, old_size);
   void *result =
      flags & MREMAP_FIXED
         ? real_mremap(old_address, old_size, new_size, flags,
                       new_address)
         : real_mremap(old_address, old_size, new_size, flags);
   if (result != MAP_FAILED) {
      drm_shim_mapping_remove(old_address, old_size);
      if (bo)
         drm_shim_mapping_replace(bo, result, new_size);
      else
         drm_shim_mapping_remove(result, new_size);
   }
   if (bo)
      drm_shim_bo_put(bo);
   fd_operation_guard_release(&guard);
   return result;
}

void *
drm_shim_mmap_real(void *addr, size_t length, int prot, int flags,
                   int fd, off64_t offset)
{
   get_function_pointers();
   return real_mmap64(addr, length, prot, flags, fd, offset);
}

void
drm_shim_pci_device_setup(uint16_t vendor_id, uint16_t device_id,
                          const char *pci_slot, const char *driver_name)
{
   shim_device.bus_type = DRM_BUS_PCI;
   shim_device.driver_name = driver_name;
   int marker_length =
      snprintf(
         shim_device.render_marker,
         sizeof(shim_device.render_marker),
         DRM_SHIM_RENDER_MARKER_VERSION "\n"
         "bus=pci\n"
         "driver=%s\n"
         "vendor=%04x\n"
         "device=%04x\n",
         driver_name, (unsigned)vendor_id, (unsigned)device_id);
   if (marker_length <= 0 ||
       marker_length >= (int)sizeof(shim_device.render_marker))
      abort();
   shim_device.render_marker_length = (size_t)marker_length;

   synthetic_add_authority("/sys/dev/char/%d:%d", DRM_MAJOR,
                           render_node_minor);
   synthetic_add_authority("/sys/devices/pci0000:00/%s", pci_slot);
   synthetic_add_directory("/sys/bus/pci");
   synthetic_add_directory("/sys/devices/pci0000:00/%s", pci_slot);
   synthetic_add_directory("/sys/devices/pci0000:00/%s/drm", pci_slot);
   synthetic_add_directory(
      "/sys/devices/pci0000:00/%s/drm/renderD%d", pci_slot,
      render_node_minor);
   char *char_target, *device_target;
   nfasprintf(&char_target,
              "../../devices/pci0000:00/%s/drm/renderD%d", pci_slot,
              render_node_minor);
   nfasprintf(&device_target, "../../../%s", pci_slot);
   drm_shim_override_link(
      char_target, "/sys/dev/char/%d:%d", DRM_MAJOR, render_node_minor);
   drm_shim_override_link(
      device_target, "/sys/devices/pci0000:00/%s/drm/renderD%d/device",
      pci_slot, render_node_minor);
   drm_shim_override_link(
      "../../../bus/pci",
      "/sys/devices/pci0000:00/%s/subsystem", pci_slot);
   free(char_target);
   free(device_target);

   char *uevent_content, *modalias_content, *vendor_id_str, *device_id_str;

   nfasprintf(&uevent_content,
            "DRIVER=%s\n"
            "PCI_CLASS=30000\n"
            "PCI_ID=%04X:%04X\n"
            "PCI_SUBSYS_ID=1234:1234\n"
            "PCI_SLOT_NAME=%s\n"
            "MODALIAS=pci:v%08Xd%08Xsv00001234sd00001234bc03sc00i00\n",
            driver_name, (unsigned)vendor_id, (unsigned)device_id, pci_slot,
            (unsigned)vendor_id, (unsigned)device_id);
   nfasprintf(
      &modalias_content,
      "pci:v%08Xd%08Xsv00001234sd00001234bc03sc00i00\n",
      (unsigned)vendor_id, (unsigned)device_id);
   nfasprintf(&vendor_id_str, "0x%04x\n", (unsigned)vendor_id);
   nfasprintf(&device_id_str, "0x%04x\n", (unsigned)device_id);

   drm_shim_override_file(uevent_content,
                          "/sys/devices/pci0000:00/%s/uevent", pci_slot);
   drm_shim_override_file(modalias_content,
                          "/sys/devices/pci0000:00/%s/modalias", pci_slot);
   drm_shim_override_file("0x00\n",
                          "/sys/devices/pci0000:00/%s/revision", pci_slot);
   drm_shim_override_file(vendor_id_str,
                          "/sys/devices/pci0000:00/%s/vendor", pci_slot);
   drm_shim_override_file(device_id_str,
                          "/sys/devices/pci0000:00/%s/device", pci_slot);
   drm_shim_override_file("0x1234\n",
                          "/sys/devices/pci0000:00/%s/subsystem_vendor", pci_slot);
   drm_shim_override_file("0x1234\n",
                          "/sys/devices/pci0000:00/%s/subsystem_device", pci_slot);

   free(uevent_content);
   free(modalias_content);
   free(vendor_id_str);
   free(device_id_str);
}

void
drm_shim_platform_device_setup(const char *driver_name, const char *fullname, const char *compatible)
{
   shim_device.bus_type = DRM_BUS_PLATFORM;
   shim_device.driver_name = driver_name;
   int marker_length =
      snprintf(
         shim_device.render_marker,
         sizeof(shim_device.render_marker),
         DRM_SHIM_RENDER_MARKER_VERSION "\n"
         "bus=platform\n"
         "driver=%s\n"
         "fullname=%s\n"
         "compatible=%s\n",
         driver_name, fullname, compatible);
   if (marker_length <= 0 ||
       marker_length >= (int)sizeof(shim_device.render_marker))
      abort();
   shim_device.render_marker_length = (size_t)marker_length;

   synthetic_add_authority("/sys/dev/char/%d:%d", DRM_MAJOR,
                           render_node_minor);
   synthetic_add_authority("/sys/devices/platform/%s", driver_name);
   synthetic_add_directory("/sys/bus/platform");
   synthetic_add_directory("/sys/devices/platform/%s", driver_name);
   synthetic_add_directory("/sys/devices/platform/%s/drm", driver_name);
   synthetic_add_directory(
      "/sys/devices/platform/%s/drm/renderD%d", driver_name,
      render_node_minor);
   char *char_target, *device_target;
   nfasprintf(&char_target,
              "../../devices/platform/%s/drm/renderD%d", driver_name,
              render_node_minor);
   nfasprintf(&device_target, "../../../%s", driver_name);
   drm_shim_override_link(
      char_target, "/sys/dev/char/%d:%d", DRM_MAJOR, render_node_minor);
   drm_shim_override_link(
      device_target, "/sys/devices/platform/%s/drm/renderD%d/device",
      driver_name, render_node_minor);
   drm_shim_override_link(
      "../../../bus/platform",
      "/sys/devices/platform/%s/subsystem", driver_name);
   free(char_target);
   free(device_target);

   char *uevent_content;
   nfasprintf(&uevent_content, "DRIVER=%s\n"
                          "OF_FULLNAME=%s\n"
                          "OF_COMPATIBLE_0=%s\n"
                          "OF_COMPATIBLE_N=1\n", driver_name, fullname, compatible);

   drm_shim_override_file(uevent_content,
                          "/sys/devices/platform/%s/uevent", driver_name);

   free(uevent_content);
}
