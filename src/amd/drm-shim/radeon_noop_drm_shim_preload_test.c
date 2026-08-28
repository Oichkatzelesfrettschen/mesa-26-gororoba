/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm-uapi/drm.h>
#include <radeon_drm.h>

#define DRM_SHIM_MAJOR 226
#define DRM_SHIM_MINOR 128
#define EXPECTED_DEVICE_ID UINT32_C(0x5974)

static const char render_path[] = "/dev/dri/renderD128";
static const char vendor_path[] =
   "/sys/dev/char/226:128/device/vendor";
static const char device_path[] =
   "/sys/dev/char/226:128/device/device";
static const char char_path[] = "/sys/dev/char/226:128";
static const char char_target[] =
   "../../devices/pci0000:00/0000:01:00.0/drm/renderD128";

static unsigned failures;
static bool use_legacy_stat;
static bool atexit_checks_enabled;

#define CHECK(condition, ...)                       \
   do {                                             \
      if (!(condition)) {                           \
         fprintf(stderr, "FAIL: " __VA_ARGS__);     \
         fprintf(stderr, "\n");                     \
         failures++;                                \
      }                                             \
   } while (0)

typedef int (*xstat_fn)(int, const char *, struct stat *);
typedef int (*xstat64_fn)(int, const char *, struct stat64 *);
typedef int (*fxstat_fn)(int, int, struct stat *);
typedef int (*fxstat64_fn)(int, int, struct stat64 *);
typedef int (*fxstatat_fn)(int, int, const char *, struct stat *, int);
typedef int (*fxstatat64_fn)(
   int, int, const char *, struct stat64 *, int);

static xstat_fn legacy_xstat;
static xstat64_fn legacy_xstat64;
static fxstat_fn legacy_fxstat;
static fxstat64_fn legacy_fxstat64;
static xstat_fn legacy_lxstat;
static xstat64_fn legacy_lxstat64;
static fxstatat_fn legacy_fxstatat;
static fxstatat64_fn legacy_fxstatat64;
static int legacy_stat_version = -1;

static const char *
expected_dso(void)
{
   const char *path = getenv("DRM_SHIM_EXPECTED_DSO");
   if (!path || !path[0]) {
      fprintf(stderr, "FAIL: DRM_SHIM_EXPECTED_DSO is unset\n");
      failures++;
      return "";
   }
   return path;
}

static bool
same_file(const char *left, const char *right)
{
   struct stat left_status;
   struct stat right_status;
   return stat(left, &left_status) == 0 &&
          stat(right, &right_status) == 0 &&
          left_status.st_dev == right_status.st_dev &&
          left_status.st_ino == right_status.st_ino;
}

static void *
require_provider(const char *symbol_name)
{
   dlerror();
   void *symbol = dlsym(RTLD_DEFAULT, symbol_name);
   const char *error = dlerror();
   CHECK(symbol && !error, "symbol %s is unavailable: %s",
         symbol_name, error ? error : "unknown");
   if (!symbol)
      return NULL;

   Dl_info info;
   memset(&info, 0, sizeof(info));
   CHECK(dladdr(symbol, &info) != 0 && info.dli_fname,
         "symbol %s has no provider", symbol_name);
   if (info.dli_fname)
      CHECK(same_file(info.dli_fname, expected_dso()),
            "symbol %s provider %s differs from %s",
            symbol_name, info.dli_fname, expected_dso());
   return symbol;
}

static bool
is_render_status(const struct stat *status)
{
   return S_ISCHR(status->st_mode) &&
          major(status->st_rdev) == DRM_SHIM_MAJOR &&
          minor(status->st_rdev) == DRM_SHIM_MINOR;
}

static bool
is_render_status64(const struct stat64 *status)
{
   return S_ISCHR(status->st_mode) &&
          major(status->st_rdev) == DRM_SHIM_MAJOR &&
          minor(status->st_rdev) == DRM_SHIM_MINOR;
}

static int
call_fstat(int fd, struct stat *status)
{
   if (use_legacy_stat)
      return legacy_fxstat(legacy_stat_version, fd, status);
   return fstat(fd, status);
}

static int
call_stat(const char *path, struct stat *status)
{
   if (use_legacy_stat)
      return legacy_xstat(legacy_stat_version, path, status);
   return stat(path, status);
}

static bool
query_driver(int fd, char *name, size_t name_size)
{
   memset(name, 0, name_size);
   struct drm_version version = {
      .name_len = name_size,
      .name = name,
   };
   return ioctl(fd, DRM_IOCTL_VERSION, &version) == 0 &&
          version.name_len == strlen("radeon") &&
          memcmp(name, "radeon", strlen("radeon")) == 0;
}

static bool
query_device_id_matches(int fd, uint32_t expected_device_id)
{
   uint32_t device_id = UINT32_MAX;
   struct drm_radeon_info info = {
      .request = RADEON_INFO_DEVICE_ID,
      .value = (uintptr_t)&device_id,
   };
   return ioctl(fd, DRM_IOCTL_RADEON_INFO, &info) == 0 &&
          device_id == expected_device_id;
}

static bool
query_device_id(int fd)
{
   return query_device_id_matches(fd, EXPECTED_DEVICE_ID);
}

static void
check_readlink_exact(const char *path, const char *expected)
{
   char buffer[512];
   memset(buffer, 0xa5, sizeof(buffer));
   ssize_t length = readlink(path, buffer, sizeof(buffer) - 1);
   size_t expected_length = strlen(expected);
   CHECK(length == (ssize_t)expected_length &&
            memcmp(buffer, expected, expected_length) == 0 &&
            (unsigned char)buffer[expected_length] == 0xa5,
         "readlink %s returned %zd bytes", path, length);
}

static void
check_text_file(const char *path, const char *expected)
{
   int fd = open(path, O_RDONLY | O_CLOEXEC);
   CHECK(fd >= 0, "open %s failed with errno %d", path, errno);
   if (fd < 0)
      return;
   char buffer[64] = {0};
   ssize_t length = read(fd, buffer, sizeof(buffer));
   int saved_errno = errno;
   close(fd);
   CHECK(length == (ssize_t)strlen(expected) &&
            memcmp(buffer, expected, (size_t)length) == 0,
         "read %s returned %zd bytes errno %d",
         path, length, saved_errno);
}

static void
check_render_fd(int fd, bool path_only, bool state_available)
{
   struct stat status;
   memset(&status, 0, sizeof(status));
   int ret = call_fstat(fd, &status);
   CHECK(ret == 0 && is_render_status(&status),
         "render fstat returned %d mode 0%o device %u:%u errno %d",
         ret, status.st_mode, major(status.st_rdev),
         minor(status.st_rdev), errno);

   char proc_path[64];
   snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
   check_readlink_exact(proc_path, render_path);

   uint32_t device_id = UINT32_MAX;
   struct drm_radeon_info info = {
      .request = RADEON_INFO_DEVICE_ID,
      .value = (uintptr_t)&device_id,
   };
   errno = 0;
   ret = ioctl(fd, DRM_IOCTL_RADEON_INFO, &info);
   if (path_only) {
      CHECK(ret == -1 && errno == EBADF &&
               device_id == UINT32_MAX,
            "O_PATH ioctl returned %d errno %d device 0x%x",
            ret, errno, device_id);
   } else if (!state_available) {
      CHECK(ret == -1 && errno == EOPNOTSUPP &&
               device_id == UINT32_MAX,
            "state-disabled ioctl returned %d errno %d device 0x%x",
            ret, errno, device_id);
   } else {
      CHECK(ret == 0 && device_id == EXPECTED_DEVICE_ID,
            "render device query returned %d errno %d device 0x%x",
            ret, errno, device_id);
   }
}

static void
load_legacy_stat_symbols(void)
{
   legacy_xstat = require_provider("__xstat");
   legacy_xstat64 = require_provider("__xstat64");
   legacy_fxstat = require_provider("__fxstat");
   legacy_fxstat64 = require_provider("__fxstat64");
   legacy_lxstat = require_provider("__lxstat");
   legacy_lxstat64 = require_provider("__lxstat64");
   legacy_fxstatat = require_provider("__fxstatat");
   legacy_fxstatat64 = require_provider("__fxstatat64");
   if (!legacy_xstat || !legacy_xstat64 || !legacy_fxstat ||
       !legacy_fxstat64 || !legacy_lxstat || !legacy_lxstat64 ||
       !legacy_fxstatat || !legacy_fxstatat64)
      return;

   void *libc = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
   CHECK(libc, "dlopen libc.so.6 failed: %s", dlerror());
   if (!libc)
      return;
   xstat_fn libc_xstat = (xstat_fn)dlsym(libc, "__xstat");
   Dl_info info;
   memset(&info, 0, sizeof(info));
   CHECK(libc_xstat && dladdr((void *)libc_xstat, &info) != 0 &&
            info.dli_fname && strstr(info.dli_fname, "libc.so"),
         "libc __xstat calibration provider is invalid");
   if (libc_xstat) {
      for (int version = 0; version < 16; version++) {
         struct stat status;
         if (libc_xstat(version, "/dev/null", &status) == 0) {
            legacy_stat_version = version;
            break;
         }
      }
   }
   CHECK(legacy_stat_version >= 0,
         "no accepted glibc stat ABI version was found");
   dlclose(libc);
}

static void
check_legacy_stat_surface(int fd, int path_fd)
{
   if (legacy_stat_version < 0)
      return;
   struct stat status;
   struct stat64 status64;
   int ret = legacy_xstat(legacy_stat_version, render_path, &status);
   CHECK(ret == 0 && is_render_status(&status),
         "__xstat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);
   ret =
      legacy_xstat64(legacy_stat_version, render_path, &status64);
   CHECK(ret == 0 && is_render_status64(&status64),
         "__xstat64 returned %d mode 0%o errno %d",
         ret, status64.st_mode, errno);
   ret = legacy_fxstat(legacy_stat_version, fd, &status);
   CHECK(ret == 0 && is_render_status(&status),
         "__fxstat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);
   ret = legacy_fxstat64(legacy_stat_version, path_fd, &status64);
   CHECK(ret == 0 && is_render_status64(&status64),
         "__fxstat64 returned %d mode 0%o errno %d",
         ret, status64.st_mode, errno);
   ret = legacy_lxstat(legacy_stat_version, char_path, &status);
   CHECK(ret == 0 && S_ISLNK(status.st_mode),
         "__lxstat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);
   ret =
      legacy_lxstat64(legacy_stat_version, char_path, &status64);
   CHECK(ret == 0 && S_ISLNK(status64.st_mode),
         "__lxstat64 returned %d mode 0%o errno %d",
         ret, status64.st_mode, errno);
   ret = legacy_fxstatat(
      legacy_stat_version, path_fd, "", &status, AT_EMPTY_PATH);
   CHECK(ret == 0 && is_render_status(&status),
         "__fxstatat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);
   ret = legacy_fxstatat64(
      legacy_stat_version, path_fd, "", &status64, AT_EMPTY_PATH);
   CHECK(ret == 0 && is_render_status64(&status64),
         "__fxstatat64 returned %d mode 0%o errno %d",
         ret, status64.st_mode, errno);
}

static void
check_directory_surface(void)
{
   DIR *directory = opendir("/dev/dri");
   CHECK(directory, "opendir /dev/dri failed with errno %d", errno);
   if (!directory)
      return;
   unsigned entries = 0;
   bool found_render = false;
   struct dirent *entry;
   while ((entry = readdir(directory))) {
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0)
         continue;
      entries++;
      if (strcmp(entry->d_name, "renderD128") == 0 &&
          entry->d_type == DT_CHR)
         found_render = true;
   }
   CHECK(entries == 1 && found_render,
         "synthetic /dev/dri has %u non-dot entries and render=%d",
         entries, found_render);
   closedir(directory);
}

static void
check_freopen_surface(void)
{
   FILE *stream = fopen(render_path, "r+");
   CHECK(stream, "fopen render node failed with errno %d", errno);
   if (!stream)
      return;
   int fd = fileno(stream);
   CHECK(query_device_id(fd), "fopen render node lost Radeon identity");

   stream = freopen("/dev/null", "r", stream);
   CHECK(stream, "freopen /dev/null failed with errno %d", errno);
   if (!stream)
      return;
   fd = fileno(stream);
   char driver_name[32];
   errno = 0;
   CHECK(!query_driver(fd, driver_name, sizeof(driver_name)) &&
            errno == ENOTTY,
         "freopen /dev/null retained shim routing with errno %d",
         errno);

   stream = freopen(render_path, "r+", stream);
   CHECK(stream, "freopen render node failed with errno %d", errno);
   if (!stream)
      return;
   CHECK(query_device_id(fileno(stream)),
         "freopen render node did not restore Radeon identity");
   fclose(stream);
}

static void
check_provider_surface(bool legacy)
{
   static const char *const symbols[] = {
      "open", "__open_2", "readlink", "__readlink_chk",
      "opendir", "readdir", "closedir", "fopen", "freopen", "ioctl",
   };
   for (size_t i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
      require_provider(symbols[i]);
   if (!legacy) {
      require_provider("stat");
      require_provider("fstat");
   }
}

static void
check_render_open_modes(void)
{
   static const struct {
      const char *name;
      int open_flags;
      bool path_only;
   } open_modes[] = {
      {"O_WRONLY", O_WRONLY, false},
      {"O_RDONLY", O_RDONLY, false},
      {"O_RDWR", O_RDWR, false},
      {"O_PATH", O_PATH, true},
   };
   for (size_t mode_index = 0;
        mode_index < sizeof(open_modes) / sizeof(open_modes[0]);
        mode_index++) {
      int mode_fd =
         open(render_path, open_modes[mode_index].open_flags | O_CLOEXEC);
      CHECK(mode_fd >= 0, "%s render open failed with errno %d",
            open_modes[mode_index].name, errno);
      if (mode_fd < 0)
         continue;

      int status_flags = fcntl(mode_fd, F_GETFL);
      if (open_modes[mode_index].path_only) {
         CHECK(status_flags >= 0 &&
                  (status_flags & O_PATH) == O_PATH,
               "%s render open returned status flags 0x%x",
               open_modes[mode_index].name, status_flags);
      } else {
         CHECK(status_flags >= 0 &&
                  (status_flags & O_ACCMODE) ==
                     open_modes[mode_index].open_flags,
               "%s render open returned status flags 0x%x",
               open_modes[mode_index].name, status_flags);
      }
      check_render_fd(mode_fd, open_modes[mode_index].path_only, true);
      close(mode_fd);
   }
}

static void
check_loaded_surface(void)
{
   check_render_open_modes();
   int fd = open(render_path, O_RDWR);
   CHECK(fd >= 0, "open render node failed with errno %d", errno);
   if (fd < 0)
      return;
   int path_fd = open(render_path, O_PATH);
   CHECK(path_fd >= 0, "open O_PATH render node failed with errno %d",
         errno);

   char driver_name[32];
   CHECK(query_driver(fd, driver_name, sizeof(driver_name)),
         "DRM_IOCTL_VERSION did not return radeon");
   CHECK(query_device_id(fd),
         "RADEON_INFO_DEVICE_ID did not return 0x5974");
   check_render_fd(fd, false, true);
   if (path_fd >= 0)
      check_render_fd(path_fd, true, true);

   struct stat status;
   memset(&status, 0, sizeof(status));
   int ret = call_stat(render_path, &status);
   CHECK(ret == 0 && is_render_status(&status),
         "render stat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);

   check_readlink_exact(char_path, char_target);
   check_text_file(vendor_path, "0x1002\n");
   check_text_file(device_path, "0x5974\n");
   check_directory_surface();
   check_freopen_surface();
   if (use_legacy_stat && path_fd >= 0)
      check_legacy_stat_surface(fd, path_fd);

   close(fd);
   if (path_fd >= 0)
      close(path_fd);
}

static void
application_atexit(void)
{
   if (!atexit_checks_enabled)
      return;
   unsigned before = failures;
   require_provider("open");
   int fd = open(render_path, O_RDWR | O_CLOEXEC);
   if (fd < 0 || !query_device_id(fd))
      failures++;
   if (fd >= 0)
      close(fd);
   check_text_file(vendor_path, "0x1002\n");
   check_text_file(device_path, "0x5974\n");
   if (failures != before)
      _Exit(EXIT_FAILURE);
}

static int
run_exec_child(int argc, char **argv)
{
   if (argc != 5)
      return 2;
   int fd = atoi(argv[2]);
   int path_fd = atoi(argv[3]);
   int write_only_fd = atoi(argv[4]);
   check_provider_surface(use_legacy_stat);
   check_render_fd(fd, false, false);
   check_render_fd(path_fd, true, false);
   check_render_fd(write_only_fd, false, false);
   int fresh_fd = open(render_path, O_RDWR | O_CLOEXEC);
   CHECK(fresh_fd >= 0 && query_device_id(fresh_fd),
         "fresh post-exec render identity failed with errno %d", errno);
   if (fresh_fd >= 0)
      close(fresh_fd);
   return failures ? 1 : 0;
}

static void
check_inherited_exec(const char *self, const char *child_mode)
{
   int fd = open(render_path, O_RDWR);
   int path_fd = open(render_path, O_PATH);
   int write_only_fd = open(render_path, O_WRONLY);
   CHECK(fd >= 0 && path_fd >= 0 && write_only_fd >= 0,
         "inherited descriptors are %d, %d, and %d errno %d",
         fd, path_fd, write_only_fd, errno);
   if (fd < 0 || path_fd < 0 || write_only_fd < 0)
      goto cleanup;
   CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0 &&
            (fcntl(path_fd, F_GETFD) & FD_CLOEXEC) == 0 &&
            (fcntl(write_only_fd, F_GETFD) & FD_CLOEXEC) == 0,
         "inherited descriptors unexpectedly have FD_CLOEXEC");

   pid_t child = fork();
   CHECK(child >= 0, "inherited exec fork failed with errno %d", errno);
   if (child == 0) {
      char fd_text[32];
      char path_fd_text[32];
      char write_only_fd_text[32];
      snprintf(fd_text, sizeof(fd_text), "%d", fd);
      snprintf(path_fd_text, sizeof(path_fd_text), "%d", path_fd);
      snprintf(write_only_fd_text, sizeof(write_only_fd_text), "%d",
               write_only_fd);
      execl(self, self, child_mode, fd_text, path_fd_text,
            write_only_fd_text, NULL);
      _exit(127);
   }
   if (child > 0) {
      int status;
      pid_t waited;
      do {
         waited = waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      CHECK(waited == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
            "inherited exec child status is 0x%x",
            waited == child ? status : -1);
   }

cleanup:
   if (fd >= 0)
      close(fd);
   if (path_fd >= 0)
      close(path_fd);
   if (write_only_fd >= 0)
      close(write_only_fd);
}

static int
run_wrong_driver_child(int argc, char **argv)
{
   if (argc != 3)
      return 2;
   int inherited_fd = atoi(argv[2]);
   struct stat status;
   int ret = fstat(inherited_fd, &status);
   CHECK(ret == 0 && S_ISREG(status.st_mode),
         "wrong-driver inherited fstat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);
   char driver_name[32];
   errno = 0;
   CHECK(!query_driver(inherited_fd, driver_name, sizeof(driver_name)) &&
            errno == ENOTTY,
         "wrong-driver inherited fd was claimed with errno %d", errno);
   return failures ? 1 : 0;
}

static void
check_cross_driver_exec(const char *self, const char *amdgpu_dso)
{
   int fd = open(render_path, O_RDWR);
   CHECK(fd >= 0, "cross-driver render open failed with errno %d", errno);
   if (fd < 0)
      return;
   pid_t child = fork();
   CHECK(child >= 0, "cross-driver fork failed with errno %d", errno);
   if (child == 0) {
      char fd_text[32];
      snprintf(fd_text, sizeof(fd_text), "%d", fd);
      setenv("LD_PRELOAD", amdgpu_dso, 1);
      setenv("DRM_SHIM_EXPECTED_DSO", amdgpu_dso, 1);
      unsetenv("RADEON_GPU_ID");
      execl(self, self, "wrong-driver-child", fd_text, NULL);
      _exit(127);
   }
   if (child > 0) {
      int status;
      pid_t waited;
      do {
         waited = waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      CHECK(waited == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
            "cross-driver child status is 0x%x",
            waited == child ? status : -1);
   }
   close(fd);
}

static int
run_changed_device_child(int argc, char **argv)
{
   if (argc != 3)
      return 2;

   int inherited_fd = atoi(argv[2]);
   struct stat status;
   int ret = fstat(inherited_fd, &status);
   CHECK(ret == 0 && S_ISREG(status.st_mode),
         "changed-device inherited fstat returned %d mode 0%o errno %d",
         ret, status.st_mode, errno);

   char driver_name[32];
   errno = 0;
   CHECK(!query_driver(inherited_fd, driver_name, sizeof(driver_name)) &&
            errno == ENOTTY,
         "changed-device inherited fd was claimed with errno %d", errno);

   int fresh_fd = open(render_path, O_RDWR | O_CLOEXEC);
   CHECK(fresh_fd >= 0 &&
            query_device_id_matches(fresh_fd, UINT32_C(0x7140)),
         "changed-device fresh render fd did not select 0x7140, errno %d",
         errno);
   if (fresh_fd >= 0)
      close(fresh_fd);
   return failures ? 1 : 0;
}

static int
run_changed_device_exec(const char *self)
{
   int inherited_fd = open(render_path, O_RDWR);
   CHECK(inherited_fd >= 0,
         "changed-device render open failed with errno %d", errno);
   if (inherited_fd < 0)
      return 1;

   pid_t child = fork();
   CHECK(child >= 0, "changed-device fork failed with errno %d", errno);
   if (child == 0) {
      char fd_text[32];
      snprintf(fd_text, sizeof(fd_text), "%d", inherited_fd);
      if (setenv("RADEON_GPU_ID", "0x7140", 1) < 0)
         _exit(126);
      execl(self, self, "changed-device-child", fd_text, NULL);
      _exit(127);
   }
   if (child > 0) {
      int status;
      pid_t waited;
      do {
         waited = waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      CHECK(waited == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0,
            "changed-device child status is 0x%x",
            waited == child ? status : -1);
   }

   close(inherited_fd);
   return failures ? 1 : 0;
}

extern char **environ;

static int
run_malformed_token_exec(const char *self)
{
   int malformed_fd =
      syscall(SYS_memfd_create, "malformed-drm-shim-state-token", 0);
   CHECK(malformed_fd >= 0,
         "malformed-token memfd create failed with errno %d", errno);
   if (malformed_fd < 0)
      return 1;

   long page_size = sysconf(_SC_PAGESIZE);
   int truncate_result =
      page_size > 0 ? ftruncate(malformed_fd, page_size) : -1;
   CHECK(page_size > 0 && truncate_result == 0,
         "malformed-token sizing failed with errno %d", errno);
   CHECK((fcntl(malformed_fd, F_GETFD) & FD_CLOEXEC) == 0,
         "malformed-token fd unexpectedly has FD_CLOEXEC");
   if (page_size <= 0 || truncate_result < 0) {
      close(malformed_fd);
      return 1;
   }

   pid_t child = fork();
   CHECK(child >= 0, "malformed-token fork failed with errno %d", errno);
   if (child == 0) {
      char locator[128];
      int length =
         snprintf(locator, sizeof(locator),
                  "v1i:%d:00000000000000000000000000000000:radeon",
                  malformed_fd);
      if (length < 0 || length >= (int)sizeof(locator) ||
          setenv("MESA_DRM_SHIM_EXEC_LOCATOR", locator, 1) < 0)
         _exit(126);
      char *const child_argv[] = {(char *)self,
                                  "malformed-token-trigger", NULL};
      syscall(SYS_execve, self, child_argv, environ);
      _exit(127);
   }
   if (child > 0) {
      int status;
      pid_t waited;
      do {
         waited = waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      CHECK(waited == child && WIFSIGNALED(status) &&
               WTERMSIG(status) == SIGABRT,
            "malformed-token child status is 0x%x",
            waited == child ? status : -1);
   }

   close(malformed_fd);
   return failures ? 1 : 0;
}

static int
run_unloaded(int argc, char **argv)
{
   if (argc != 3)
      return 2;
   void *symbol = dlsym(RTLD_DEFAULT, "open");
   Dl_info info;
   memset(&info, 0, sizeof(info));
   CHECK(symbol && dladdr(symbol, &info) != 0 && info.dli_fname,
         "unloaded open provider is unavailable");
   if (info.dli_fname)
      CHECK(!same_file(info.dli_fname, argv[2]),
            "unloaded calibration resolved the shim DSO");
   return failures ? 1 : 0;
}

static int
run_reject_selector(int argc, char **argv)
{
   if (argc != 4)
      return 2;
   pid_t child = fork();
   if (child < 0)
      return 1;
   if (child == 0) {
      setenv("LD_PRELOAD", argv[2], 1);
      setenv("DRM_SHIM_EXPECTED_DSO", argv[2], 1);
      setenv("RADEON_GPU_ID", argv[3], 1);
      execl(argv[0], argv[0], "trigger-selector", NULL);
      _exit(127);
   }
   int status;
   if (waitpid(child, &status, 0) != child)
      return 1;
   return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT ? 0 : 1;
}

int
main(int argc, char **argv)
{
   if (argc < 2)
      return 2;
   if (strcmp(argv[1], "unloaded") == 0)
      return run_unloaded(argc, argv);
   if (strcmp(argv[1], "reject-selector") == 0)
      return run_reject_selector(argc, argv);
   if (strcmp(argv[1], "trigger-selector") == 0) {
      int fd = open(render_path, O_RDWR);
      if (fd >= 0)
         close(fd);
      return 1;
   }
   if (strcmp(argv[1], "wrong-driver-child") == 0)
      return run_wrong_driver_child(argc, argv);
   if (strcmp(argv[1], "changed-device-child") == 0)
      return run_changed_device_child(argc, argv);
   if (strcmp(argv[1], "changed-device") == 0)
      return run_changed_device_exec(argv[0]);
   if (strcmp(argv[1], "malformed-token") == 0)
      return run_malformed_token_exec(argv[0]);
   if (strcmp(argv[1], "malformed-token-trigger") == 0) {
      int fd = open(render_path, O_RDWR);
      if (fd >= 0)
         close(fd);
      return 1;
   }

   use_legacy_stat =
      strstr(argv[1], "legacy") != NULL;
   if (use_legacy_stat)
      load_legacy_stat_symbols();

   if (atexit(application_atexit) != 0)
      return 1;
   atexit_checks_enabled = true;

   if (strcmp(argv[1], "exec-modern") == 0 ||
       strcmp(argv[1], "exec-legacy") == 0)
      return run_exec_child(argc, argv);

   if (strcmp(argv[1], "loaded-modern") == 0 ||
       strcmp(argv[1], "loaded-legacy") == 0) {
      check_provider_surface(use_legacy_stat);
      check_loaded_surface();
      check_inherited_exec(
         argv[0], use_legacy_stat ? "exec-legacy" : "exec-modern");
      return failures ? 1 : 0;
   }

   if (strcmp(argv[1], "cross-driver") == 0 && argc == 3) {
      check_cross_driver_exec(argv[0], argv[2]);
      return failures ? 1 : 0;
   }
   return 2;
}
