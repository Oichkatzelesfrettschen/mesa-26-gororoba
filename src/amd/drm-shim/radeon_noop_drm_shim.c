#include <ctype.h>
#ifdef DRM_SHIM_TEST
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#endif
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef DRM_SHIM_TEST
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif
#include "common/amd_family.h"
#include "drm-shim/drm_shim.h"
#include "util/log.h"
#include "util/os_misc.h"
#include <util/u_math.h>
#include <radeon_drm.h>

#define RADEON_SHIM_VA_RESERVED_SIZE (8u << 20)

static enum radeon_family radeon_family = CHIP_RV515;
static uint16_t device_id = 0x7140;

struct radeon_shim_bo {
   struct shim_bo base;
   uint32_t current_domain;
};

static int
radeon_ioctl_noop(int fd, unsigned long request, void *arg)
{
   return 0;
}

static int
radeon_ioctl_cs(int fd, unsigned long request, void *arg)
{
   const char *failure = getenv("R3V_NATIVE_SHIM_CS_REFUSE");
   if (failure != NULL && strcmp(failure, "1") == 0)
      return -EINVAL;
   return 0;
}

static int
radeon_ioctl_gem_wait_idle(int fd, unsigned long request, void *arg)
{
   const char *failure = getenv("R3V_NATIVE_SHIM_COMPLETION_FAIL");
   if (failure != NULL && strcmp(failure, "1") == 0)
      return -EINVAL;
   return 0;
}

static int
radeon_ioctl_info(int fd, unsigned long request, void *arg)
{
   struct drm_radeon_info *info = arg;
   uint32_t *value = (uint32_t *)(intptr_t)info->value;
   uint64_t *value64 = (uint64_t *)(intptr_t)info->value;

   switch (info->request) {
   case RADEON_INFO_DEVICE_ID:
      *value = device_id;
      return 0;

   case RADEON_INFO_RING_WORKING:
   case RADEON_INFO_ACCEL_WORKING2:
   case RADEON_INFO_VA_UNMAP_WORKING:
      *value = true;
      return 0;

   case RADEON_INFO_GPU_RESET_COUNTER:
      *value = 0;
      return 0;

   case RADEON_INFO_NUM_BYTES_MOVED:
   case RADEON_INFO_VRAM_USAGE:
   case RADEON_INFO_GTT_USAGE:
      *value64 = 0;
      return 0;

   case RADEON_INFO_READ_REG:
      /* The noop device models register reads as unsupported.  RS480 GART
       * memory-controller state remains a silicon-only observation. */
      return -EINVAL;

   case RADEON_INFO_IB_VM_MAX_SIZE:
      if (radeon_family < CHIP_CAYMAN)
         return -EINVAL;
      *value = 64 << 10;
      return 0;

   case RADEON_INFO_TILING_CONFIG:
   case RADEON_INFO_BACKEND_MAP:
      if (radeon_family < CHIP_R600)
         return -EINVAL;
      *value = 0; /* dummy */
      return 0;

   case RADEON_INFO_VA_START:
      if (radeon_family < CHIP_CAYMAN)
         return -EINVAL;
      *value = RADEON_SHIM_VA_RESERVED_SIZE;
      return 0;

   case RADEON_INFO_MAX_SCLK:
   case RADEON_INFO_CLOCK_CRYSTAL_FREQ:
   case RADEON_INFO_NUM_GB_PIPES:
   case RADEON_INFO_NUM_Z_PIPES:
   case RADEON_INFO_MAX_SE:
   case RADEON_INFO_ACTIVE_CU_COUNT:
      *value = 1; /* dummy */
      return 0;

   case RADEON_INFO_VCE_FW_VERSION:
      *value = 0;
      return 0;

   case RADEON_INFO_MAX_PIPES:
   case RADEON_INFO_NUM_BACKENDS:
   case RADEON_INFO_NUM_TILE_PIPES:
      if (radeon_family < CHIP_R600)
         return -EINVAL;
      *value = 1; /* dummy */
      return 0;

   case RADEON_INFO_MAX_SH_PER_SE:
      if (radeon_family < CHIP_TAHITI)
         return -EINVAL;
      *value = 1; /* dummy */
      return 0;

   default:
      fprintf(stderr, "Unknown DRM_IOCTL_RADEON_INFO request 0x%02X\n", info->request);
      return -EINVAL;
   }
}

static int
radeon_ioctl_gem_info(int fd, unsigned long request, void *arg)
{
   struct drm_radeon_gem_info *info = arg;

   /* Dummy values. */
   info->vram_size = 256 * 1024 * 1024;
   info->vram_visible = info->vram_size;
   info->gart_size = 512 * 1024 * 1024;

   return 0;
}

static int
radeon_ioctl_gem_create(int fd, unsigned long request, void *arg)
{
   struct drm_radeon_gem_create *create = arg;
   const uint32_t supported_domains =
      RADEON_GEM_DOMAIN_CPU | RADEON_GEM_DOMAIN_GTT |
      RADEON_GEM_DOMAIN_VRAM;

   if (!create->initial_domain ||
       create->initial_domain & ~supported_domains)
      return -EINVAL;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct radeon_shim_bo *radeon_bo = calloc(1, sizeof(*radeon_bo));
   if (!radeon_bo)
      return -ENOMEM;

   size_t size = (size_t)align64(create->size, 4096);
   int ret = drm_shim_bo_init(&radeon_bo->base, size);
   if (ret) {
      free(radeon_bo);
      return ret;
   }

   if (create->initial_domain & RADEON_GEM_DOMAIN_VRAM)
      radeon_bo->current_domain = RADEON_GEM_DOMAIN_VRAM;
   else if (create->initial_domain & RADEON_GEM_DOMAIN_GTT)
      radeon_bo->current_domain = RADEON_GEM_DOMAIN_GTT;
   else
      radeon_bo->current_domain = RADEON_GEM_DOMAIN_CPU;
   create->handle = drm_shim_bo_get_handle(shim_fd, &radeon_bo->base);

   drm_shim_bo_put(&radeon_bo->base);

   return 0;
}

static int
radeon_ioctl_gem_mmap(int fd, unsigned long request, void *arg)
{
   struct drm_radeon_gem_mmap *mmap_bo = arg;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, mmap_bo->handle);
   if (!bo)
      return -ENOENT;

   uint64_t mmap_offset;
   int ret =
      drm_shim_bo_get_mmap_offset(shim_fd, bo, &mmap_offset);
   if (!ret)
      mmap_bo->addr_ptr = mmap_offset;
   drm_shim_bo_put(bo);

   return ret;
}

static int
radeon_ioctl_gem_userptr(int fd, unsigned long request, void *arg)
{
   /* probed at winsys init, just return no support. */
   return -EINVAL;
}

static int
radeon_ioctl_gem_busy(int fd, unsigned long request, void *arg)
{
   struct drm_radeon_gem_busy *busy = arg;
   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, busy->handle);
   if (!bo)
      return -ENOENT;

   struct radeon_shim_bo *radeon_bo =
      container_of(bo, struct radeon_shim_bo, base);
   busy->domain = radeon_bo->current_domain;
   drm_shim_bo_put(bo);
   return 0;
}

static ioctl_fn_t driver_ioctls[] = {
   [DRM_RADEON_CS] = radeon_ioctl_cs,
   [DRM_RADEON_INFO] = radeon_ioctl_info,
   [DRM_RADEON_GEM_SET_DOMAIN] = radeon_ioctl_noop,
   [DRM_RADEON_GEM_SET_TILING] = radeon_ioctl_noop,
   [DRM_RADEON_GEM_WAIT_IDLE] = radeon_ioctl_gem_wait_idle,
   [DRM_RADEON_GEM_INFO] = radeon_ioctl_gem_info,
   [DRM_RADEON_GEM_CREATE] = radeon_ioctl_gem_create,
   [DRM_RADEON_GEM_MMAP] = radeon_ioctl_gem_mmap,
   [DRM_RADEON_GEM_USERPTR] = radeon_ioctl_gem_userptr,
   [DRM_RADEON_GEM_BUSY] = radeon_ioctl_gem_busy,
};

struct radeon_pci_id {
   const char *name;
   const char *family_name;
   enum radeon_family family;
   uint16_t device_id;
};

#define CHIPSET(d, n, f) {.device_id = (d), .name = #n, .family = CHIP_##f, .family_name = #f},
static const struct radeon_pci_id radeon_pci_ids[] = {
#include "pci_ids/r300_pci_ids.h"
#include "pci_ids/r600_pci_ids.h"
#undef CHIPSET
#define CHIPSET(d, f) {.device_id = (d), .name = #f, .family = CHIP_##f, .family_name = #f},
#include "pci_ids/radeonsi_pci_ids.h"
};
#undef CHIPSET

static const struct radeon_pci_id *
radeon_find_device_by_id(uint16_t candidate_device_id)
{
   for (size_t i = 0; i < ARRAY_SIZE(radeon_pci_ids); i++) {
      if (radeon_pci_ids[i].device_id == candidate_device_id)
         return &radeon_pci_ids[i];
   }
   return NULL;
}

static const struct radeon_pci_id *
radeon_find_device_by_name(const char *candidate_name)
{
   for (size_t i = 0; i < ARRAY_SIZE(radeon_pci_ids); i++) {
      if (strcasecmp(candidate_name, radeon_pci_ids[i].name) == 0 ||
          strcasecmp(candidate_name, radeon_pci_ids[i].family_name) == 0)
         return &radeon_pci_ids[i];
   }
   return NULL;
}

static bool
radeon_parse_device_id(const char *candidate, uint16_t *parsed_device_id)
{
   if (strlen(candidate) != 6 || strncmp(candidate, "0x", 2) != 0)
      return false;

   for (size_t i = 2; i < 6; i++) {
      if (!isxdigit((unsigned char)candidate[i]))
         return false;
   }

   char *end = NULL;
   errno = 0;
   unsigned long parsed = strtoul(candidate + 2, &end, 16);
   if (errno || end != candidate + 6 || parsed > UINT16_MAX)
      return false;

   *parsed_device_id = parsed;
   return true;
}

static const struct radeon_pci_id *
radeon_resolve_device(const char *candidate)
{
   if (!candidate)
      return NULL;

   uint16_t parsed_device_id;
   if (radeon_parse_device_id(candidate, &parsed_device_id))
      return radeon_find_device_by_id(parsed_device_id);
   if (strncmp(candidate, "0x", 2) != 0)
      return radeon_find_device_by_name(candidate);
   return NULL;
}

static void
radeon_select_device(void)
{
   const char *gpu_id = os_get_option("RADEON_GPU_ID");
   if (!gpu_id)
      return;

   const struct radeon_pci_id *selected = radeon_resolve_device(gpu_id);
   if (!selected) {
      mesa_loge("Failed to find Radeon GPU identifier \"%s\"\n", gpu_id);
      abort();
   }

   device_id = selected->device_id;
   radeon_family = selected->family;
}

void
drm_shim_driver_init(void)
{
   radeon_select_device();

   shim_device.driver_ioctls = driver_ioctls;
   shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);

   shim_device.version_major = 2;
   shim_device.version_minor = 50;
   shim_device.version_patchlevel = 0;

   if (radeon_family == CHIP_RS480) {
      /* RS480 GART memory-controller state remains a silicon-only
       * observation under the noop device. */
      drm_shim_hide_path(
         "/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs");
      drm_shim_hide_path_component(
         "/sys/kernel/debug/dri/",
         "radeon_rs480_candidate_gart_mc_regs");
   }

   drm_shim_pci_device_setup(0x1002, device_id, "0000:01:00.0", "radeon");
#ifdef DRM_SHIM_TEST
   drm_shim_override_link(
      "/sys/dev/char/226:128/device",
      "/sys/devices/pci0000:00/0000:01:00.0/absolute-device");
#endif
}

#ifdef DRM_SHIM_TEST
static unsigned test_failures;
static int cookie_close_fd = -1;
static bool test_fork_survivor_paths(void);

#define TEST_CHECK(condition, ...)                  \
   do {                                             \
      if (!(condition)) {                           \
         fprintf(stderr, "FAIL: " __VA_ARGS__);     \
         fprintf(stderr, "\n");                     \
         test_failures++;                           \
      }                                             \
   } while (0)

static int
test_cookie_close(void *cookie)
{
   (void)cookie;
   int fd = cookie_close_fd;
   cookie_close_fd = -1;
   return close(fd);
}

static ssize_t
test_read_bytes(int fd, void *buffer, size_t count)
{
   size_t total = 0;
   while (total < count) {
      ssize_t length =
         read(fd, (char *)buffer + total, count - total);
      if (length < 0 && errno == EINTR)
         continue;
      if (length <= 0)
         return total ? (ssize_t)total : length;
      total += (size_t)length;
   }
   return (ssize_t)total;
}

static void
test_proc_fd_readlink(const char *path, bool expected_tracked)
{
   static const char expected_target[] = "/dev/dri/renderD128";
   char target[PATH_MAX];
   memset(target, 0xa5, sizeof(target));
   errno = 0;
   ssize_t length = readlink(path, target, sizeof(target));
   bool tracked =
      length == (ssize_t)strlen(expected_target) &&
      memcmp(target, expected_target, strlen(expected_target)) == 0;
   TEST_CHECK(tracked == expected_tracked,
              "proc-fd path %s tracked state is %d with length %zd errno %d",
              path, tracked, length, errno);
}

struct test_thread_tid_context {
   int ready_fd;
   int release_fd;
};

struct test_unshared_fd_context {
   int shared_fd;
   int ready_fd;
   int release_fd;
   uint32_t worker_domain;
   bool direct_unshare;
   pid_t worker_tid;
   int worker_render_fd;
   int worker_identity_fd;
   int result;
};

struct test_blocking_lock_context {
   int fd;
   int ready_fd;
   int done_fd;
   struct flock lock;
   int result;
   int error;
};

struct test_fd_discovery_context {
   int fd;
   int start_fd;
   int result;
   struct stat status;
};

enum test_path_snapshot_operation {
   TEST_PATH_SNAPSHOT_OPEN,
   TEST_PATH_SNAPSHOT_STAT,
   TEST_PATH_SNAPSHOT_OPENAT,
   TEST_PATH_SNAPSHOT_FSTATAT,
};

struct test_path_snapshot_context {
   enum test_path_snapshot_operation operation;
   char *path;
   int dirfd;
   int start_fd;
   int result;
   int error;
};

static void *
test_thread_tid_wait(void *data)
{
   struct test_thread_tid_context *context = data;
   pid_t tid = syscall(SYS_gettid);
   ssize_t written;
   do {
      written = write(context->ready_fd, &tid, sizeof(tid));
   } while (written < 0 && errno == EINTR);
   char release;
   ssize_t length;
   do {
      length = read(context->release_fd, &release, sizeof(release));
   } while (length < 0 && errno == EINTR);
   return NULL;
}

static void *
test_fd_discovery_worker(void *data)
{
   struct test_fd_discovery_context *context = data;
   char start;
   ssize_t length;
   do {
      length = read(context->start_fd, &start, sizeof(start));
   } while (length < 0 && errno == EINTR);
   if (length != sizeof(start)) {
      context->result = -1;
      return NULL;
   }
   context->result = fstat(context->fd, &context->status);
   return NULL;
}

static void *
test_unshared_fd_table_worker(void *data)
{
   struct test_unshared_fd_context *context = data;
   context->result = 1;
   int worker_fd = -1;
   uint32_t handle = 0;
   if (context->direct_unshare) {
      if (unshare(CLONE_FILES) < 0 || close(context->shared_fd) < 0)
         goto notify;
   } else {
      if (close_range((unsigned)context->shared_fd,
                      (unsigned)context->shared_fd,
                      CLOSE_RANGE_UNSHARE) < 0)
         goto notify;
   }

   worker_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (worker_fd < 0)
      goto notify;
   context->worker_tid = syscall(SYS_gettid);
   context->worker_render_fd = worker_fd;
   struct shim_fd *worker_shim_fd = drm_shim_fd_get(worker_fd);
   if (!worker_shim_fd)
      goto notify;
   context->worker_identity_fd = worker_shim_fd->identity_fd;
   drm_shim_fd_put(worker_shim_fd);
   if (context->worker_identity_fd < 0)
      goto notify;
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = context->worker_domain,
   };
   if (ioctl(worker_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create) < 0)
      goto notify;
   handle = create.handle;
   struct drm_radeon_gem_busy busy = {
      .handle = create.handle,
      .domain = UINT32_MAX,
   };
   if (ioctl(worker_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy) < 0 ||
       busy.domain != context->worker_domain)
      goto notify;

   context->result = 0;

notify:
   ;
   char ready = context->result == 0;
   ssize_t written;
   do {
      written = write(context->ready_fd, &ready, sizeof(ready));
   } while (written < 0 && errno == EINTR);
   if (written != sizeof(ready))
      goto cleanup;
   char release;
   ssize_t length;
   do {
      length = read(context->release_fd, &release, sizeof(release));
   } while (length < 0 && errno == EINTR);
   if (length != sizeof(release))
      context->result = 1;

cleanup:
   if (handle) {
      struct drm_gem_close close_bo = {
         .handle = handle,
      };
      if (ioctl(worker_fd, DRM_IOCTL_GEM_CLOSE, &close_bo) < 0)
         context->result = 1;
   }
   if (worker_fd >= 0 && close(worker_fd) < 0)
      context->result = 1;
   return NULL;
}

static void *
test_blocking_lock_worker(void *data)
{
   struct test_blocking_lock_context *context = data;
   char byte = 1;
   ssize_t length;
   do {
      length = write(context->ready_fd, &byte, sizeof(byte));
   } while (length < 0 && errno == EINTR);
   if (length != sizeof(byte)) {
      context->result = -1;
      context->error = EIO;
      return NULL;
   }

   errno = 0;
   context->result =
      fcntl(context->fd, F_OFD_SETLKW, &context->lock);
   context->error = errno;

   do {
      length = write(context->done_fd, &byte, sizeof(byte));
   } while (length < 0 && errno == EINTR);
   if (length != sizeof(byte) && context->result == 0) {
      context->result = -1;
      context->error = EIO;
   }
   return NULL;
}

static void *
test_path_snapshot_worker(void *data)
{
   struct test_path_snapshot_context *context = data;
   char start;
   if (read(context->start_fd, &start, sizeof(start)) != sizeof(start)) {
      context->result = -1;
      context->error = EIO;
      return NULL;
   }

   struct stat status;
   errno = 0;
   switch (context->operation) {
   case TEST_PATH_SNAPSHOT_OPEN:
      context->result =
         open(context->path, O_RDONLY | O_CLOEXEC);
      break;
   case TEST_PATH_SNAPSHOT_STAT:
      context->result = stat(context->path, &status);
      break;
   case TEST_PATH_SNAPSHOT_OPENAT:
      context->result =
         openat(context->dirfd, context->path,
                O_RDONLY | O_CLOEXEC);
      break;
   case TEST_PATH_SNAPSHOT_FSTATAT:
      context->result =
         fstatat(context->dirfd, context->path, &status, 0);
      break;
   }
   context->error = errno;
   if ((context->operation == TEST_PATH_SNAPSHOT_OPEN ||
        context->operation == TEST_PATH_SNAPSHOT_OPENAT) &&
       context->result >= 0)
      close(context->result);
   return NULL;
}

static void
test_path_snapshot_case(enum test_path_snapshot_operation operation,
                        int dirfd, const char *initial_path,
                        const char *mutated_path)
{
   char shared_path[PATH_MAX];
   snprintf(shared_path, sizeof(shared_path), "%s", initial_path);
   int start_pipe[2];
   int ready_pipe[2];
   int release_pipe[2];
   int start_result = pipe2(start_pipe, O_CLOEXEC);
   int ready_result = pipe2(ready_pipe, O_CLOEXEC);
   int release_result = pipe2(release_pipe, O_CLOEXEC);
   TEST_CHECK(start_result == 0 && ready_result == 0 &&
                 release_result == 0,
              "path-snapshot pipes failed with errno %d", errno);
   if (start_result != 0 || ready_result != 0 ||
       release_result != 0)
      return;

   struct test_path_snapshot_context context = {
      .operation = operation,
      .path = shared_path,
      .dirfd = dirfd,
      .start_fd = start_pipe[0],
      .result = -2,
   };
   pthread_t worker;
   int thread_result =
      pthread_create(&worker, NULL, test_path_snapshot_worker, &context);
   TEST_CHECK(thread_result == 0,
              "path-snapshot pthread_create returned %d", thread_result);
   if (thread_result == 0) {
      drm_shim_test_arm_path_snapshot_barrier(
         ready_pipe[1], release_pipe[0]);
      char byte = 1;
      ssize_t length = write(start_pipe[1], &byte, sizeof(byte));
      TEST_CHECK(length == sizeof(byte),
                 "path-snapshot start returned %zd", length);
      length = read(ready_pipe[0], &byte, sizeof(byte));
      TEST_CHECK(length == sizeof(byte),
                 "path-snapshot readiness returned %zd", length);
      snprintf(shared_path, sizeof(shared_path), "%s", mutated_path);
      length = write(release_pipe[1], &byte, sizeof(byte));
      TEST_CHECK(length == sizeof(byte),
                 "path-snapshot release returned %zd", length);
      thread_result = pthread_join(worker, NULL);
      TEST_CHECK(thread_result == 0 && context.result == -1 &&
                    context.error == ENOENT,
                 "path-snapshot operation %d returned join %d result %d "
                 "errno %d",
                 operation, thread_result, context.result,
                 context.error);
   }

   close(start_pipe[0]);
   close(start_pipe[1]);
   close(ready_pipe[0]);
   close(ready_pipe[1]);
   close(release_pipe[0]);
   close(release_pipe[1]);
}

static void
test_path_snapshot_confinement(void)
{
   char directory[] = "/tmp/mesa-drm-shim-path-snapshot-XXXXXX";
   char *created_directory = mkdtemp(directory);
   TEST_CHECK(created_directory,
              "path-snapshot directory failed with errno %d", errno);
   if (!created_directory)
      return;

   char hidden_path[PATH_MAX];
   char missing_path[PATH_MAX];
   snprintf(hidden_path, sizeof(hidden_path), "%s/hidden",
            created_directory);
   snprintf(missing_path, sizeof(missing_path), "%s/missing",
            created_directory);
   int hidden_fd =
      open(hidden_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   TEST_CHECK(hidden_fd >= 0,
              "path-snapshot hidden file failed with errno %d", errno);
   if (hidden_fd >= 0)
      close(hidden_fd);
   drm_shim_hide_path(hidden_path);

   test_path_snapshot_case(TEST_PATH_SNAPSHOT_OPEN, AT_FDCWD,
                           missing_path, hidden_path);
   test_path_snapshot_case(TEST_PATH_SNAPSHOT_STAT, AT_FDCWD,
                           missing_path, hidden_path);

   int directory_fd =
      open(created_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(directory_fd >= 0,
              "path-snapshot dirfd failed with errno %d", errno);
   if (directory_fd >= 0) {
      test_path_snapshot_case(TEST_PATH_SNAPSHOT_OPENAT, directory_fd,
                              "missing", "hidden");
      test_path_snapshot_case(TEST_PATH_SNAPSHOT_FSTATAT, directory_fd,
                              "missing", "hidden");
      close(directory_fd);
   }

   unlink(hidden_path);
   rmdir(created_directory);
}

static bool
test_read_text(const char *path, char *buffer, size_t buffer_size)
{
   buffer[0] = '\0';
   errno = 0;
   FILE *file = fopen(path, "r");
   if (!file)
      return false;

   size_t length = fread(buffer, 1, buffer_size - 1, file);
   bool success = !ferror(file);
   success = fclose(file) == 0 && success;
   buffer[length] = '\0';
   return success;
}

static void
test_text_equals(const char *path, const char *expected)
{
   char contents[1024];
   TEST_CHECK(test_read_text(path, contents, sizeof(contents)),
              "override is unreadable with errno %d: %s", errno, path);
   TEST_CHECK(strcmp(contents, expected) == 0,
              "override %s contains \"%s\" instead of \"%s\"", path,
              contents, expected);
}

static int
test_radeon_info(int fd, uint32_t request, void *value)
{
   struct drm_radeon_info info = {
      .request = request,
      .value = (uintptr_t)value,
   };
   return ioctl(fd, DRM_IOCTL_RADEON_INFO, &info);
}

static const struct radeon_pci_id *
test_find_device(uint16_t expected_device_id)
{
   return radeon_find_device_by_id(expected_device_id);
}

static void
test_identity(int fd, uint16_t expected_device_id,
              const char *expected_family_name)
{
   const struct radeon_pci_id *expected =
      test_find_device(expected_device_id);
   TEST_CHECK(expected, "expected PCI ID 0x%04x is absent from the Radeon table",
              expected_device_id);
   if (!expected)
      return;

   TEST_CHECK(device_id == expected_device_id,
              "selected PCI ID is 0x%04x instead of 0x%04x", device_id,
              expected_device_id);
   TEST_CHECK(radeon_family == expected->family,
              "selected family does not match PCI table entry %s",
              expected->name);
   TEST_CHECK(strcmp(expected->family_name, expected_family_name) == 0,
              "PCI table family is %s instead of %s", expected->family_name,
              expected_family_name);

   bool rs480_source_hidden = expected->family == CHIP_RS480;
   TEST_CHECK(
      drm_shim_test_path_is_hidden(
         "/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs") ==
         rs480_source_hidden,
      "RS480 fallback debugfs registration differs from selected family %s",
      expected->family_name);
   TEST_CHECK(
      drm_shim_test_path_is_hidden(
         "/sys/kernel/debug/dri/0/"
         "radeon_rs480_candidate_gart_mc_regs") == rs480_source_hidden,
      "RS480 per-card debugfs registration differs from selected family %s",
      expected->family_name);

   uint32_t info_device_id = UINT32_MAX;
   int ret = test_radeon_info(fd, RADEON_INFO_DEVICE_ID, &info_device_id);
   TEST_CHECK(ret == 0, "RADEON_INFO_DEVICE_ID returned %d", ret);
   TEST_CHECK(info_device_id == expected_device_id,
              "RADEON_INFO_DEVICE_ID is 0x%04x instead of 0x%04x",
              info_device_id, expected_device_id);

   char path[PATH_MAX];
   snprintf(path, sizeof(path), "/sys/dev/char/%d:128/device/device",
            DRM_MAJOR);
   char expected_device_text[16];
   snprintf(expected_device_text, sizeof(expected_device_text), "0x%04x\n",
            expected_device_id);
   test_text_equals(path, expected_device_text);
   test_text_equals(
      "/sys/devices/pci0000:00/0000:01:00.0/device",
      expected_device_text);

   snprintf(path, sizeof(path), "/sys/dev/char/%d:128/device/vendor",
            DRM_MAJOR);
   test_text_equals(path, "0x1002\n");
   test_text_equals(
      "/sys/devices/pci0000:00/0000:01:00.0/vendor", "0x1002\n");

   static const struct {
      const char *name;
      const char *contents;
   } fixed_pci_attributes[] = {
      {"subsystem_vendor", "0x1234\n"},
      {"subsystem_device", "0x1234\n"},
      {"revision", "0x00\n"},
   };
   for (size_t i = 0; i < ARRAY_SIZE(fixed_pci_attributes); i++) {
      snprintf(path, sizeof(path),
               "/sys/dev/char/%d:128/device/%s", DRM_MAJOR,
               fixed_pci_attributes[i].name);
      test_text_equals(path, fixed_pci_attributes[i].contents);
      snprintf(path, sizeof(path),
               "/sys/devices/pci0000:00/0000:01:00.0/%s",
               fixed_pci_attributes[i].name);
      test_text_equals(path, fixed_pci_attributes[i].contents);
   }

   snprintf(path, sizeof(path), "/sys/dev/char/%d:128/device/uevent",
            DRM_MAJOR);
   char contents[1024];
   TEST_CHECK(test_read_text(path, contents, sizeof(contents)),
              "uevent sysfs override is unreadable");
   char expected_uevent[512];
   snprintf(expected_uevent, sizeof(expected_uevent),
            "DRIVER=radeon\n"
            "PCI_CLASS=30000\n"
            "PCI_ID=1002:%04X\n"
            "PCI_SUBSYS_ID=1234:1234\n"
            "PCI_SLOT_NAME=0000:01:00.0\n"
            "MODALIAS=pci:v00001002d%08Xsv00001234sd00001234bc03sc00i00\n",
            expected_device_id, expected_device_id);
   TEST_CHECK(strcmp(contents, expected_uevent) == 0,
              "uevent differs from the selected PCI identity");

   snprintf(path, sizeof(path), "/sys/dev/char/%d:128/device/subsystem",
            DRM_MAJOR);
   static const char expected_subsystem_link[] = "../../../bus/pci";
   unsigned char link_buffer[64];
   memset(link_buffer, 0xa5, sizeof(link_buffer));
   ssize_t link_length =
      readlink(path, (char *)link_buffer, sizeof(link_buffer));
   TEST_CHECK(link_length == (ssize_t)strlen(expected_subsystem_link),
              "subsystem readlink returned %zd bytes", link_length);
   TEST_CHECK(link_length > 0 &&
                 memcmp(link_buffer, expected_subsystem_link,
                        strlen(expected_subsystem_link)) == 0,
              "subsystem readlink returned the wrong target");
   TEST_CHECK(link_buffer[strlen(expected_subsystem_link)] == 0xa5,
              "subsystem readlink appended a terminator");

   memset(link_buffer, 0xa5, sizeof(link_buffer));
   link_length = readlink(path, (char *)link_buffer, 5);
   TEST_CHECK(link_length == 5, "truncated readlink returned %zd bytes",
              link_length);
   TEST_CHECK(memcmp(link_buffer, expected_subsystem_link, 5) == 0 &&
                 link_buffer[5] == 0xa5,
              "truncated readlink changed bytes outside its result");

   snprintf(path, sizeof(path), "/sys/dev/char/%d:128/device", DRM_MAJOR);
   static const char expected_device_path[] =
      "/sys/devices/pci0000:00/0000:01:00.0";
   char resolved_path[PATH_MAX];
   char *resolved = realpath(path, resolved_path);
   TEST_CHECK(resolved && strcmp(resolved, expected_device_path) == 0,
              "caller-buffer realpath differs from the fake device path");
   char *allocated_path = realpath(path, NULL);
   TEST_CHECK(allocated_path &&
                 strcmp(allocated_path, expected_device_path) == 0,
              "allocated realpath differs from the fake device path");
   free(allocated_path);

   snprintf(path, sizeof(path), "/sys/dev/char/%d:128/device/subsystem",
            DRM_MAJOR);
   resolved = realpath(path, resolved_path);
   TEST_CHECK(resolved && strcmp(resolved, "/sys/bus/pci") == 0,
              "subsystem realpath differs from /sys/bus/pci");
}

static void
test_info_u32_gate(int fd, uint32_t request,
                   enum radeon_family minimum_family,
                   uint32_t expected_value)
{
   struct {
      uint32_t before;
      uint32_t value;
      uint32_t after;
   } guarded_value = {
      .before = UINT32_C(0x11223344),
      .value = UINT32_MAX,
      .after = UINT32_C(0x55667788),
   };

   errno = EDOM;
   int ret = test_radeon_info(fd, request, &guarded_value.value);
   int saved_errno = errno;
   if (radeon_family < minimum_family) {
      TEST_CHECK(ret == -1 && saved_errno == EINVAL &&
                    guarded_value.value == UINT32_MAX,
                 "gated Radeon INFO request %u returned %d, errno %d, "
                 "and 0x%08x",
                 request, ret, saved_errno, guarded_value.value);
   } else {
      TEST_CHECK(ret == 0 && guarded_value.value == expected_value,
                 "available Radeon INFO request %u returned %d and 0x%08x",
                 request, ret, guarded_value.value);
   }
   TEST_CHECK(guarded_value.before == UINT32_C(0x11223344) &&
                 guarded_value.after == UINT32_C(0x55667788),
              "Radeon INFO request %u changed an adjacent canary", request);

   guarded_value.value = UINT32_MAX;
   struct drm_radeon_info info = {
      .request = request,
      .value = (uintptr_t)&guarded_value.value,
   };
   errno = ERANGE;
   ret = drmCommandWriteRead(fd, DRM_RADEON_INFO, &info, sizeof(info));
   saved_errno = errno;
   if (radeon_family < minimum_family) {
      TEST_CHECK(ret == -EINVAL && saved_errno == EINVAL &&
                    guarded_value.value == UINT32_MAX,
                 "libdrm gated Radeon INFO request %u returned %d, "
                 "errno %d, and 0x%08x",
                 request, ret, saved_errno, guarded_value.value);
   } else {
      TEST_CHECK(ret == 0 && guarded_value.value == expected_value,
                 "libdrm available Radeon INFO request %u returned %d "
                 "and 0x%08x",
                 request, ret, guarded_value.value);
   }
}

static void
test_info_widths(int fd)
{
   const uint32_t requests[] = {
      RADEON_INFO_NUM_BYTES_MOVED,
      RADEON_INFO_VRAM_USAGE,
      RADEON_INFO_GTT_USAGE,
   };

   for (size_t i = 0; i < ARRAY_SIZE(requests); i++) {
      struct {
         uint64_t before;
         uint64_t value;
         uint64_t after;
      } guarded_value = {
         .before = UINT64_C(0x1122334455667788),
         .value = UINT64_MAX,
         .after = UINT64_C(0x8877665544332211),
      };
      int ret =
         test_radeon_info(fd, requests[i], &guarded_value.value);
      TEST_CHECK(ret == 0, "64-bit Radeon INFO request %u returned %d",
                 requests[i], ret);
      TEST_CHECK(guarded_value.value == 0,
                 "64-bit Radeon INFO request %u retained 0x%016llx",
                 requests[i], (unsigned long long)guarded_value.value);
      TEST_CHECK(guarded_value.before == UINT64_C(0x1122334455667788) &&
                    guarded_value.after == UINT64_C(0x8877665544332211),
                 "64-bit Radeon INFO request %u changed an adjacent canary",
                 requests[i]);
   }

   uint32_t register_value[3] = {
      UINT32_C(0x11223344),
      UINT32_C(0x55667788),
      UINT32_C(0x99aabbcc),
   };
   errno = EDOM;
   int ret = test_radeon_info(fd, RADEON_INFO_READ_REG, register_value);
   int saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == EINVAL,
              "RADEON_INFO_READ_REG returned %d with errno %d", ret,
              saved_errno);
   TEST_CHECK(register_value[0] == UINT32_C(0x11223344) &&
                 register_value[1] == UINT32_C(0x55667788) &&
                 register_value[2] == UINT32_C(0x99aabbcc),
              "RADEON_INFO_READ_REG changed its input buffer");

   struct drm_radeon_info register_info = {
      .request = RADEON_INFO_READ_REG,
      .value = (uintptr_t)register_value,
   };
   errno = ERANGE;
   ret = drmCommandWriteRead(fd, DRM_RADEON_INFO, &register_info,
                             sizeof(register_info));
   saved_errno = errno;
   TEST_CHECK(ret == -EINVAL && saved_errno == EINVAL,
              "libdrm RADEON_INFO_READ_REG returned %d with errno %d", ret,
              saved_errno);
   TEST_CHECK(register_value[0] == UINT32_C(0x11223344) &&
                 register_value[1] == UINT32_C(0x55667788) &&
                 register_value[2] == UINT32_C(0x99aabbcc),
              "libdrm RADEON_INFO_READ_REG changed its input buffer");

   static const struct {
      uint32_t request;
      uint32_t value;
   } r600_requests[] = {
      {RADEON_INFO_TILING_CONFIG, 0},
      {RADEON_INFO_BACKEND_MAP, 0},
      {RADEON_INFO_MAX_PIPES, 1},
      {RADEON_INFO_NUM_BACKENDS, 1},
      {RADEON_INFO_NUM_TILE_PIPES, 1},
   };
   for (size_t i = 0; i < ARRAY_SIZE(r600_requests); i++) {
      test_info_u32_gate(fd, r600_requests[i].request, CHIP_R600,
                         r600_requests[i].value);
   }
   test_info_u32_gate(fd, RADEON_INFO_IB_VM_MAX_SIZE, CHIP_CAYMAN,
                      64 << 10);
   test_info_u32_gate(fd, RADEON_INFO_MAX_SH_PER_SE, CHIP_TAHITI, 1);

   uint32_t vce_fw_version = UINT32_MAX;
   int vce_ret =
      test_radeon_info(fd, RADEON_INFO_VCE_FW_VERSION, &vce_fw_version);
   TEST_CHECK(vce_ret == 0 && vce_fw_version == 0,
              "noop Radeon VCE firmware query returned %d and 0x%08x",
              vce_ret, vce_fw_version);

   uint32_t libdrm_device_id = UINT32_MAX;
   struct drm_radeon_info device_info = {
      .request = RADEON_INFO_DEVICE_ID,
      .value = (uintptr_t)&libdrm_device_id,
   };
   errno = EDOM;
   ret = drmCommandWriteRead(fd, DRM_RADEON_INFO, &device_info,
                             sizeof(device_info));
   TEST_CHECK(ret == 0 && libdrm_device_id == device_id,
              "libdrm RADEON_INFO_DEVICE_ID returned %d and 0x%08x", ret,
              libdrm_device_id);

   struct {
      uint32_t before;
      uint32_t value;
      uint32_t after;
   } guarded_va_start = {
      .before = UINT32_C(0x11223344),
      .value = UINT32_MAX,
      .after = UINT32_C(0x55667788),
   };
   errno = EDOM;
   ret = test_radeon_info(fd, RADEON_INFO_VA_START,
                          &guarded_va_start.value);
   saved_errno = errno;
   if (radeon_family < CHIP_CAYMAN) {
      TEST_CHECK(ret == -1 && saved_errno == EINVAL &&
                    guarded_va_start.value == UINT32_MAX,
                 "pre-Cayman RADEON_INFO_VA_START returned %d, errno %d, "
                 "and 0x%08x",
                 ret, saved_errno, guarded_va_start.value);
   } else {
      TEST_CHECK(ret == 0 &&
                    guarded_va_start.value == RADEON_SHIM_VA_RESERVED_SIZE,
                 "Cayman+ RADEON_INFO_VA_START returned %d and 0x%08x", ret,
                 guarded_va_start.value);
   }
   TEST_CHECK(guarded_va_start.before == UINT32_C(0x11223344) &&
                 guarded_va_start.after == UINT32_C(0x55667788),
              "RADEON_INFO_VA_START changed an adjacent canary");

   guarded_va_start.value = UINT32_MAX;
   struct drm_radeon_info va_start_info = {
      .request = RADEON_INFO_VA_START,
      .value = (uintptr_t)&guarded_va_start.value,
   };
   errno = ERANGE;
   ret = drmCommandWriteRead(fd, DRM_RADEON_INFO, &va_start_info,
                             sizeof(va_start_info));
   saved_errno = errno;
   if (radeon_family < CHIP_CAYMAN) {
      TEST_CHECK(ret == -EINVAL && saved_errno == EINVAL &&
                    guarded_va_start.value == UINT32_MAX,
                 "libdrm pre-Cayman RADEON_INFO_VA_START returned %d, "
                 "errno %d, and 0x%08x",
                 ret, saved_errno, guarded_va_start.value);
   } else {
      TEST_CHECK(ret == 0 &&
                    guarded_va_start.value == RADEON_SHIM_VA_RESERVED_SIZE,
                 "libdrm Cayman+ RADEON_INFO_VA_START returned %d and "
                 "0x%08x",
                 ret, guarded_va_start.value);
   }
}

static void
test_gem_busy(int fd)
{
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_VRAM,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0, "DRM_RADEON_GEM_CREATE returned %d", ret);
   TEST_CHECK(create.handle != 0, "DRM_RADEON_GEM_CREATE returned handle zero");
   TEST_CHECK(drm_shim_test_live_bo_backing_files() == 0,
              "primary GEM create created %d premature backing files",
              drm_shim_test_live_bo_backing_files());

   struct drm_radeon_gem_busy busy = {
      .handle = create.handle,
      .domain = UINT32_MAX,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
   TEST_CHECK(ret == 0, "DRM_RADEON_GEM_BUSY returned %d for a valid BO", ret);
   TEST_CHECK(busy.domain == RADEON_GEM_DOMAIN_VRAM,
              "DRM_RADEON_GEM_BUSY returned domain 0x%x", busy.domain);

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *first_lookup =
      drm_shim_bo_lookup(shim_fd, create.handle);
   struct shim_bo *second_lookup =
      drm_shim_bo_lookup(shim_fd, create.handle);
   uint64_t first_mem_addr =
      first_lookup ? first_lookup->mem_addr : 0;
   TEST_CHECK(first_lookup && second_lookup && first_lookup == second_lookup,
              "overlapping BO lookups returned inconsistent objects");
   if (first_lookup)
      drm_shim_bo_put(first_lookup);
   if (second_lookup) {
      struct radeon_shim_bo *looked_up_bo =
         container_of(second_lookup, struct radeon_shim_bo, base);
      TEST_CHECK(looked_up_bo->current_domain == RADEON_GEM_DOMAIN_VRAM,
                 "second BO lookup became invalid after the first put");
      drm_shim_bo_put(second_lookup);
   }

   struct drm_radeon_gem_create combined_domain = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain =
         RADEON_GEM_DOMAIN_VRAM | RADEON_GEM_DOMAIN_GTT,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &combined_domain);
   TEST_CHECK(ret == 0 && combined_domain.handle != 0,
              "combined-domain GEM create returned %d and handle %u", ret,
              combined_domain.handle);
   if (ret == 0) {
      struct drm_radeon_gem_busy combined_busy = {
         .handle = combined_domain.handle,
         .domain = UINT32_MAX,
      };
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_BUSY, &combined_busy);
      TEST_CHECK(ret == 0 &&
                    combined_busy.domain == RADEON_GEM_DOMAIN_VRAM,
                 "combined-domain GEM busy returned %d and domain 0x%x",
                 ret, combined_busy.domain);
      struct drm_gem_close close_combined = {
         .handle = combined_domain.handle,
      };
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_combined);
      TEST_CHECK(ret == 0,
                 "combined-domain GEM close returned %d", ret);
      TEST_CHECK(drm_shim_test_live_bo_backing_files() == 0,
                 "combined-domain close retained %d backing files",
                 drm_shim_test_live_bo_backing_files());
   }

   struct drm_radeon_gem_create unplaceable = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = 0,
      .handle = UINT32_MAX,
   };
   errno = EDOM;
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &unplaceable);
   int unplaceable_errno = errno;
   TEST_CHECK(ret == -1 && unplaceable_errno == EINVAL &&
                 unplaceable.handle == UINT32_MAX,
              "unplaceable GEM create returned %d, errno %d, handle %u",
              ret, unplaceable_errno, unplaceable.handle);

   const uint64_t invalid_sizes[] = {0, UINT64_MAX};
   for (size_t i = 0; i < ARRAY_SIZE(invalid_sizes); i++) {
      struct drm_radeon_gem_create invalid_size = {
         .size = invalid_sizes[i],
         .alignment = 4096,
         .initial_domain = RADEON_GEM_DOMAIN_GTT,
         .handle = UINT32_MAX,
      };
      errno = EDOM;
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &invalid_size);
      int invalid_size_errno = errno;
      TEST_CHECK(ret == -1 && invalid_size_errno == EINVAL &&
                    invalid_size.handle == UINT32_MAX,
                 "invalid-size GEM create %zu returned %d, errno %d, "
                 "handle %u",
                 i, ret, invalid_size_errno, invalid_size.handle);
   }

   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && mmap_bo.addr_ptr != 0,
              "DRM_RADEON_GEM_MMAP returned %d and offset 0x%llx", ret,
              (unsigned long long)mmap_bo.addr_ptr);
   TEST_CHECK(ret != 0 || drm_shim_test_live_bo_backing_files() == 1,
              "GEM mmap-offset query retained %d backing files",
              drm_shim_test_live_bo_backing_files());
   void *retained_mapping = MAP_FAILED;
   if (ret == 0) {
      retained_mapping =
         mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
              (off_t)mmap_bo.addr_ptr);
      TEST_CHECK(retained_mapping != MAP_FAILED,
                 "live BO mmap failed with errno %d", errno);
      if (retained_mapping != MAP_FAILED)
         *(volatile uint32_t *)retained_mapping = UINT32_C(0x11223344);
   }

   busy.handle = UINT32_MAX;
   busy.domain = UINT32_MAX;
   errno = EDOM;
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
   int saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "invalid DRM_RADEON_GEM_BUSY returned %d with errno %d", ret,
              saved_errno);
   TEST_CHECK(busy.domain == UINT32_MAX,
              "invalid DRM_RADEON_GEM_BUSY changed domain to 0x%x",
              busy.domain);

   if (create.handle) {
      struct drm_gem_close close_bo = {.handle = create.handle};
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      TEST_CHECK(ret == 0, "DRM_IOCTL_GEM_CLOSE returned %d", ret);
      TEST_CHECK(drm_shim_test_live_bo_backing_files() == 0,
                 "GEM close retained %d backing files",
                 drm_shim_test_live_bo_backing_files());
   }

   if (mmap_bo.addr_ptr) {
      errno = EDOM;
      void *mapping =
         mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
              (off_t)mmap_bo.addr_ptr);
      saved_errno = errno;
      TEST_CHECK(mapping == MAP_FAILED && saved_errno == EINVAL,
                 "closed BO mmap returned %p with errno %d", mapping,
                 saved_errno);
      if (mapping != MAP_FAILED)
         munmap(mapping, 4096);
   }

   struct drm_radeon_gem_create replacement = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &replacement);
   TEST_CHECK(ret == 0 && replacement.handle != 0,
              "replacement GEM create returned %d and handle %u", ret,
              replacement.handle);
   TEST_CHECK(ret != 0 || drm_shim_test_live_bo_backing_files() == 0,
              "replacement GEM create created %d premature backing files",
              drm_shim_test_live_bo_backing_files());
   if (ret == 0) {
      struct shim_bo *replacement_bo =
         drm_shim_bo_lookup(shim_fd, replacement.handle);
      TEST_CHECK(replacement_bo &&
                    replacement_bo->mem_addr != first_mem_addr,
                 "replacement BO reused GPU address 0x%llx",
                 (unsigned long long)first_mem_addr);
      if (replacement_bo)
         drm_shim_bo_put(replacement_bo);

      struct drm_radeon_gem_mmap replacement_mmap = {
         .handle = replacement.handle,
      };
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &replacement_mmap);
      TEST_CHECK(ret == 0,
                 "replacement GEM mmap-offset query returned %d", ret);
      TEST_CHECK(ret != 0 || drm_shim_test_live_bo_backing_files() == 1,
                 "replacement mmap-offset query retained %d backing files",
                 drm_shim_test_live_bo_backing_files());
      if (ret == 0) {
         TEST_CHECK(replacement_mmap.addr_ptr != mmap_bo.addr_ptr,
                    "replacement BO reused stale mmap token 0x%llx",
                    (unsigned long long)mmap_bo.addr_ptr);

         errno = EDOM;
         void *stale_mapping =
            mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (off_t)mmap_bo.addr_ptr);
         saved_errno = errno;
         TEST_CHECK(stale_mapping == MAP_FAILED && saved_errno == EINVAL,
                    "stale mmap token selected a later BO with result %p "
                    "errno %d",
                    stale_mapping, saved_errno);
         if (stale_mapping != MAP_FAILED)
            munmap(stale_mapping, 4096);

         void *replacement_mapping =
            mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (off_t)replacement_mmap.addr_ptr);
         TEST_CHECK(replacement_mapping != MAP_FAILED,
                    "replacement BO mmap failed with errno %d", errno);
         if (replacement_mapping != MAP_FAILED) {
            *(volatile uint32_t *)replacement_mapping =
               UINT32_C(0x55667788);
            TEST_CHECK(
               retained_mapping == MAP_FAILED ||
                  *(volatile uint32_t *)retained_mapping ==
                     UINT32_C(0x11223344),
               "closed BO mapping aliases replacement BO storage");
            munmap(replacement_mapping, 4096);
         }
      }
      struct drm_gem_close close_replacement = {
         .handle = replacement.handle,
      };
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_replacement);
      TEST_CHECK(ret == 0, "replacement GEM close returned %d", ret);
      TEST_CHECK(drm_shim_test_live_bo_backing_files() == 0,
                 "replacement GEM close retained %d backing files",
                 drm_shim_test_live_bo_backing_files());
   }

   if (retained_mapping != MAP_FAILED)
      munmap(retained_mapping, 4096);
   TEST_CHECK(drm_shim_test_live_bo_backing_files() == 0,
              "mapping release retained %d backing files",
              drm_shim_test_live_bo_backing_files());
}

extern int test_raw_stat(const char *path, struct stat *stat_buffer)
   __asm__("stat");

static void
test_expect_hidden(const char *path)
{
   errno = 0;
   int ret = access(path, F_OK);
   int saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "access exposed hidden path %s with result %d errno %d", path,
              ret, saved_errno);

   errno = 0;
   int fd = open(path, O_RDONLY);
   saved_errno = errno;
   TEST_CHECK(fd == -1 && saved_errno == ENOENT,
              "open exposed hidden path %s with fd %d errno %d", path, fd,
              saved_errno);
   if (fd >= 0)
      close(fd);

   errno = 0;
   FILE *file = fopen(path, "r");
   saved_errno = errno;
   TEST_CHECK(!file && saved_errno == ENOENT,
              "fopen exposed hidden path %s with errno %d", path,
              saved_errno);
   if (file)
      fclose(file);

   struct stat stat_buffer;
   errno = 0;
   ret = stat(path, &stat_buffer);
   saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "stat exposed hidden path %s with result %d errno %d", path,
              ret, saved_errno);

   errno = 0;
   ret = test_raw_stat(path, &stat_buffer);
   saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "raw stat exposed hidden path %s with result %d errno %d",
              path, ret, saved_errno);

   errno = 0;
   ret = lstat(path, &stat_buffer);
   saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "lstat exposed hidden path %s with result %d errno %d", path,
              ret, saved_errno);

   struct statx statx_buffer;
   errno = 0;
   ret = statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &statx_buffer);
   saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "statx exposed hidden path %s with result %d errno %d", path,
              ret, saved_errno);

   char resolved_path[PATH_MAX];
   errno = 0;
   char *resolved = realpath(path, resolved_path);
   saved_errno = errno;
   TEST_CHECK(!resolved && saved_errno == ENOENT,
              "realpath exposed hidden path %s with errno %d", path,
              saved_errno);

   char link_target[PATH_MAX];
   errno = 0;
   ssize_t link_length = readlink(path, link_target, sizeof(link_target));
   saved_errno = errno;
   TEST_CHECK(link_length == -1 && saved_errno == ENOENT,
              "readlink exposed hidden path %s with result %zd errno %d",
              path, link_length, saved_errno);

   errno = 0;
   DIR *directory = opendir(path);
   saved_errno = errno;
   TEST_CHECK(!directory && saved_errno == ENOENT,
              "opendir exposed hidden path %s with errno %d", path,
              saved_errno);
   if (directory)
      closedir(directory);
}

static void
test_expect_hidden_at(int dirfd, const char *path)
{
   errno = 0;
   int ret = faccessat(dirfd, path, F_OK, 0);
   int saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "faccessat exposed hidden path %s with result %d errno %d",
              path, ret, saved_errno);

   errno = 0;
   int fd = openat(dirfd, path, O_RDONLY);
   saved_errno = errno;
   TEST_CHECK(fd == -1 && saved_errno == ENOENT,
              "openat exposed hidden path %s with fd %d errno %d", path, fd,
              saved_errno);
   if (fd >= 0)
      close(fd);

   struct stat stat_buffer;
   errno = 0;
   ret = fstatat(dirfd, path, &stat_buffer, 0);
   saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "fstatat exposed hidden path %s with result %d errno %d", path,
              ret, saved_errno);

   struct statx statx_buffer;
   errno = 0;
   ret = statx(dirfd, path, 0, STATX_BASIC_STATS, &statx_buffer);
   saved_errno = errno;
   TEST_CHECK(ret == -1 && saved_errno == ENOENT,
              "relative statx exposed hidden path %s with result %d errno %d",
              path, ret, saved_errno);

   char link_target[PATH_MAX];
   errno = 0;
   ssize_t link_length =
      readlinkat(dirfd, path, link_target, sizeof(link_target));
   saved_errno = errno;
   TEST_CHECK(link_length == -1 && saved_errno == ENOENT,
              "readlinkat exposed hidden path %s with result %zd errno %d",
              path, link_length, saved_errno);
}

static void
test_hidden_paths(void)
{
   TEST_CHECK(drm_shim_test_path_is_hidden(
                 "/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs"),
              "RS480 fallback debugfs path is not registered");
   TEST_CHECK(drm_shim_test_path_is_hidden(
                 "/sys/kernel/debug/dri/0/"
                 "radeon_rs480_candidate_gart_mc_regs"),
              "RS480 per-card debugfs path is not registered");
   TEST_CHECK(!drm_shim_test_path_is_hidden(
                 "/sys/kernel/debug/dri/0/radeon_rs480_visible_neighbor"),
              "RS480 debugfs neighbor is hidden");

   char exact_path[] = "/tmp/radeon-shim-hidden-exact-XXXXXX";
   int fd = mkstemp(exact_path);
   TEST_CHECK(fd >= 0, "mkstemp failed for exact hidden-path calibration");
   if (fd >= 0) {
      TEST_CHECK(write(fd, "host", 4) == 4,
                 "host exact hidden-path write failed");
      close(fd);
      drm_shim_hide_path(exact_path);
      test_expect_hidden(exact_path);

      const char *basename = strrchr(exact_path, '/');
      TEST_CHECK(basename, "exact hidden path has no basename");
      if (basename) {
         char normalized_alias[PATH_MAX];
         snprintf(normalized_alias, sizeof(normalized_alias),
                  "/tmp//./unused/../%s", basename + 1);
         test_expect_hidden(normalized_alias);
      }
      unlink(exact_path);
   }

   char root_path[] = "/tmp/radeon-shim-hidden-component-XXXXXX";
   char *root = mkdtemp(root_path);
   TEST_CHECK(root, "mkdtemp failed for component hidden-path calibration");
   if (root) {
      char parent_path[PATH_MAX];
      char component_path[PATH_MAX];
      char candidate_path[PATH_MAX];
      char visible_path[PATH_MAX];
      char alias_path[PATH_MAX];
      char component_subdir_path[PATH_MAX];
      char semantic_alias_path[PATH_MAX];
      char semantic_escape_path[PATH_MAX];
      snprintf(parent_path, sizeof(parent_path), "%s/dri/", root);
      snprintf(component_path, sizeof(component_path), "%s/dri/card0", root);
      snprintf(candidate_path, sizeof(candidate_path), "%s/%s",
               component_path, "radeon_rs480_candidate_gart_mc_regs");
      snprintf(visible_path, sizeof(visible_path), "%s/%s", component_path,
               "radeon_rs480_visible_neighbor");
      snprintf(alias_path, sizeof(alias_path), "%s/candidate-alias", root);
      snprintf(component_subdir_path, sizeof(component_subdir_path),
               "%s/subdir", component_path);
      snprintf(semantic_alias_path, sizeof(semantic_alias_path),
               "%s/semantic-alias", root);
      snprintf(semantic_escape_path, sizeof(semantic_escape_path),
               "%s/semantic-alias/../%s", root,
               "radeon_rs480_candidate_gart_mc_regs");
      TEST_CHECK(mkdir(parent_path, 0700) == 0,
                 "component hidden-path parent creation failed");
      TEST_CHECK(mkdir(component_path, 0700) == 0,
                 "component hidden-path directory creation failed");
      TEST_CHECK(mkdir(component_subdir_path, 0700) == 0,
                 "component hidden-path subdirectory creation failed");
      fd = open(candidate_path, O_CREAT | O_WRONLY, 0600);
      TEST_CHECK(fd >= 0,
                 "component hidden-path host file creation failed");
      if (fd >= 0) {
         TEST_CHECK(write(fd, "host", 4) == 4,
                    "host component hidden-path write failed");
         close(fd);
      }
      fd = open(visible_path, O_CREAT | O_WRONLY, 0600);
      TEST_CHECK(fd >= 0, "visible-neighbor creation failed");
      if (fd >= 0)
         close(fd);
      TEST_CHECK(symlink(candidate_path, alias_path) == 0,
                 "hidden-path symlink calibration creation failed");
      TEST_CHECK(symlink(component_subdir_path, semantic_alias_path) == 0,
                 "hidden-path semantic alias creation failed");

      drm_shim_hide_path_component(
         parent_path, "radeon_rs480_candidate_gart_mc_regs");
      test_expect_hidden(candidate_path);
      test_expect_hidden(alias_path);
      test_expect_hidden(semantic_escape_path);

      char lexical_alias[PATH_MAX];
      snprintf(lexical_alias, sizeof(lexical_alias),
               "%s/dri//card0/../card0/./%s", root,
               "radeon_rs480_candidate_gart_mc_regs");
      test_expect_hidden(lexical_alias);

      TEST_CHECK(!drm_shim_test_path_is_hidden(visible_path),
                 "component matcher hides a visible neighbor");
      fd = open(visible_path, O_RDONLY);
      TEST_CHECK(fd >= 0, "visible neighbor became inaccessible");
      if (fd >= 0)
         close(fd);

      int root_fd = open(root, O_RDONLY | O_DIRECTORY);
      TEST_CHECK(root_fd >= 0, "component root directory open failed");
      if (root_fd >= 0) {
         test_expect_hidden_at(
            root_fd,
            "dri/card0/radeon_rs480_candidate_gart_mc_regs");
         test_expect_hidden_at(
            root_fd,
            "dri//card0/../card0/./"
            "radeon_rs480_candidate_gart_mc_regs");
         close(root_fd);
      }

      unlink(alias_path);
      unlink(semantic_alias_path);
      unlink(candidate_path);
      unlink(visible_path);
      rmdir(component_subdir_path);
      rmdir(component_path);
      rmdir(parent_path);
      rmdir(root);
   }

   test_expect_hidden(
      "/sys/kernel/debug/radeon_rs480_candidate_gart_mc_regs");
   test_expect_hidden(
      "/sys/kernel/debug/dri/0/radeon_rs480_candidate_gart_mc_regs");
}

extern int test_openat2(int dirfd, const char *path,
                        const struct open_how *how, size_t size)
   __asm__("openat2");
extern int test_readdir_r(DIR *directory, struct dirent *entry,
                          struct dirent **result)
   __asm__("readdir_r");
extern int test_readdir64_r(DIR *directory, struct dirent64 *entry,
                            struct dirent64 **result)
   __asm__("readdir64_r");

static void
test_synthetic_directory_entries(DIR *directory, const char *label)
{
   unsigned render_count = 0;
   unsigned other_count = 0;
   struct dirent *entry;

   errno = 0;
   while ((entry = readdir(directory))) {
      if (strcmp(entry->d_name, ".") == 0 ||
          strcmp(entry->d_name, "..") == 0)
         continue;
      if (strcmp(entry->d_name, "renderD128") == 0) {
         render_count++;
         TEST_CHECK(entry->d_type == DT_CHR,
                    "%s renderD128 type is %u", label,
                    (unsigned)entry->d_type);
      } else {
         other_count++;
      }
   }
   TEST_CHECK(errno == 0, "%s readdir stopped with errno %d", label, errno);
   TEST_CHECK(render_count == 1,
              "%s contains %u renderD128 entries", label, render_count);
   TEST_CHECK(other_count == 0,
              "%s exposes %u non-shim entries", label, other_count);
}

static void
test_synthetic_directory_reentrant_entries(DIR *directory,
                                           const char *label)
{
   unsigned render_count = 0;
   unsigned other_count = 0;
   struct dirent entry;
   struct dirent *result = NULL;
   int ret;

   while ((ret = test_readdir_r(directory, &entry, &result)) == 0 &&
          result) {
      if (strcmp(result->d_name, ".") == 0 ||
          strcmp(result->d_name, "..") == 0)
         continue;
      if (strcmp(result->d_name, "renderD128") == 0) {
         render_count++;
         TEST_CHECK(result->d_type == DT_CHR,
                    "%s readdir_r renderD128 type is %u", label,
                    (unsigned)result->d_type);
      } else {
         other_count++;
      }
   }
   TEST_CHECK(ret == 0, "%s readdir_r returned %d", label, ret);
   TEST_CHECK(render_count == 1 && other_count == 0,
              "%s readdir_r contains %u render and %u other entries",
              label, render_count, other_count);

   rewinddir(directory);
   render_count = 0;
   other_count = 0;
   struct dirent64 entry64;
   struct dirent64 *result64 = NULL;
   while ((ret =
              test_readdir64_r(directory, &entry64, &result64)) == 0 &&
          result64) {
      if (strcmp(result64->d_name, ".") == 0 ||
          strcmp(result64->d_name, "..") == 0)
         continue;
      if (strcmp(result64->d_name, "renderD128") == 0) {
         render_count++;
         TEST_CHECK(result64->d_type == DT_CHR,
                    "%s readdir64_r renderD128 type is %u", label,
                    (unsigned)result64->d_type);
      } else {
         other_count++;
      }
   }
   TEST_CHECK(ret == 0, "%s readdir64_r returned %d", label, ret);
   TEST_CHECK(render_count == 1 && other_count == 0,
              "%s readdir64_r contains %u render and %u other entries",
              label, render_count, other_count);
}

static void
test_scandir_entries(struct dirent **entries, int entry_count,
                     const char *label)
{
   unsigned render_count = 0;
   unsigned other_count = 0;
   for (int i = 0; i < entry_count; i++) {
      if (strcmp(entries[i]->d_name, ".") != 0 &&
          strcmp(entries[i]->d_name, "..") != 0) {
         if (strcmp(entries[i]->d_name, "renderD128") == 0) {
            render_count++;
            TEST_CHECK(entries[i]->d_type == DT_CHR,
                       "%s renderD128 type is %u", label,
                       (unsigned)entries[i]->d_type);
         } else {
            other_count++;
         }
      }
      free(entries[i]);
   }
   free(entries);
   TEST_CHECK(render_count == 1 && other_count == 0,
              "%s contains %u render and %u other entries", label,
              render_count, other_count);
}

static void
test_scandir64_entries(struct dirent64 **entries, int entry_count,
                       const char *label)
{
   unsigned render_count = 0;
   unsigned other_count = 0;
   for (int i = 0; i < entry_count; i++) {
      if (strcmp(entries[i]->d_name, ".") != 0 &&
          strcmp(entries[i]->d_name, "..") != 0) {
         if (strcmp(entries[i]->d_name, "renderD128") == 0) {
            render_count++;
            TEST_CHECK(entries[i]->d_type == DT_CHR,
                       "%s renderD128 type is %u", label,
                       (unsigned)entries[i]->d_type);
         } else {
            other_count++;
         }
      }
      free(entries[i]);
   }
   free(entries);
   TEST_CHECK(render_count == 1 && other_count == 0,
              "%s contains %u render and %u other entries", label,
              render_count, other_count);
}

static void
test_synthetic_directories(void)
{
   static const char *const paths[] = {
      "/dev/dri",
      "/dev/./dri",
      "/proc/self/root/dev/dri",
      "/proc/thread-self/root/dev/dri",
   };

   for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
      DIR *directory = opendir(paths[i]);
      TEST_CHECK(directory, "opendir failed for %s with errno %d",
                 paths[i], errno);
      if (!directory)
         continue;
      test_synthetic_directory_entries(directory, paths[i]);
      rewinddir(directory);
      test_synthetic_directory_entries(directory, paths[i]);
      rewinddir(directory);
      test_synthetic_directory_reentrant_entries(directory, paths[i]);
      TEST_CHECK(closedir(directory) == 0,
                 "closedir failed for %s with errno %d", paths[i], errno);
   }

   int directory_fd = open("/dev/dri", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(directory_fd >= 0,
              "directory open failed with errno %d", errno);
   if (directory_fd >= 0) {
      DIR *directory = fdopendir(directory_fd);
      TEST_CHECK(directory, "fdopendir failed with errno %d", errno);
      if (directory) {
         test_synthetic_directory_entries(directory, "fdopendir");
         TEST_CHECK(closedir(directory) == 0,
                    "fdopendir stream close failed with errno %d", errno);
      } else {
         close(directory_fd);
      }
   }

   struct dirent **entries = NULL;
   int entry_count = scandir("/dev/dri", &entries, NULL, alphasort);
   TEST_CHECK(entry_count >= 0, "scandir failed with errno %d", errno);
   if (entry_count >= 0)
      test_scandir_entries(entries, entry_count, "scandir");

   int dev_dirfd =
      open("/dev", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(dev_dirfd >= 0,
              "scandirat base directory open failed with errno %d", errno);
   if (dev_dirfd >= 0) {
      entries = NULL;
      entry_count =
         scandirat(dev_dirfd, "dri", &entries, NULL, alphasort);
      TEST_CHECK(entry_count >= 0,
                 "scandirat failed with errno %d", errno);
      if (entry_count >= 0)
         test_scandir_entries(entries, entry_count, "scandirat");

      struct dirent64 **entries64 = NULL;
      entry_count =
         scandirat64(dev_dirfd, "dri", &entries64, NULL, alphasort64);
      TEST_CHECK(entry_count >= 0,
                 "scandirat64 failed with errno %d", errno);
      if (entry_count >= 0)
         test_scandir64_entries(entries64, entry_count, "scandirat64");
      close(dev_dirfd);
   }
}

static void
test_external_synthetic_links(void)
{
   char root[] = "/tmp/radeon-shim-external-links-XXXXXX";
   char *created_root = mkdtemp(root);
   TEST_CHECK(created_root,
              "external-link root creation failed with errno %d", errno);
   if (!created_root)
      return;

   char device_link[PATH_MAX];
   char vendor_link[PATH_MAX];
   char vendor_link_slash[PATH_MAX];
   char missing_link[PATH_MAX];
   char escape_link[PATH_MAX];
   snprintf(device_link, sizeof(device_link), "%s/device", root);
   snprintf(vendor_link, sizeof(vendor_link), "%s/vendor", root);
   snprintf(vendor_link_slash, sizeof(vendor_link_slash), "%s/vendor/",
            root);
   snprintf(missing_link, sizeof(missing_link), "%s/missing", root);
   snprintf(escape_link, sizeof(escape_link), "%s/escape", root);
   TEST_CHECK(symlink("/sys/dev/char/226:128/device", device_link) == 0,
              "external device symlink creation failed with errno %d",
              errno);
   TEST_CHECK(
      symlink("/sys/dev/char/226:128/device/vendor", vendor_link) == 0,
      "external vendor symlink creation failed with errno %d", errno);
   TEST_CHECK(
      symlink("/sys/dev/char/226:128/device/missing", missing_link) == 0,
      "external missing symlink creation failed with errno %d", errno);
   TEST_CHECK(
      symlink("/dev/dri/../../../../etc/passwd", escape_link) == 0,
      "external escape symlink creation failed with errno %d", errno);

   struct stat status;
   int ret = stat(device_link, &status);
   TEST_CHECK(ret == 0 && S_ISDIR(status.st_mode),
              "external synthetic stat returned %d mode 0%o errno %d",
              ret, status.st_mode, errno);
   ret = lstat(device_link, &status);
   TEST_CHECK(ret == 0 && S_ISLNK(status.st_mode),
              "external synthetic lstat returned %d mode 0%o errno %d",
              ret, status.st_mode, errno);

   char target[128] = {0};
   ssize_t target_length =
      readlink(device_link, target, sizeof(target));
   TEST_CHECK(target_length ==
                 (ssize_t)strlen("/sys/dev/char/226:128/device") &&
                 memcmp(target, "/sys/dev/char/226:128/device",
                        strlen("/sys/dev/char/226:128/device")) == 0,
              "external synthetic readlink returned %zd bytes",
              target_length);

   char resolved_path[PATH_MAX];
   char *resolved = realpath(device_link, resolved_path);
   TEST_CHECK(
      resolved &&
         strcmp(resolved,
                "/sys/devices/pci0000:00/0000:01:00.0") == 0,
      "external synthetic realpath returned %s",
      resolved ? resolved : "(null)");

   errno = 0;
   int link_fd = open(device_link, O_RDONLY | O_NOFOLLOW);
   TEST_CHECK(link_fd == -1 && errno == ELOOP,
              "external O_NOFOLLOW open returned %d errno %d",
              link_fd, errno);
   if (link_fd >= 0)
      close(link_fd);

   errno = 0;
   ret = access(vendor_link, R_OK);
   TEST_CHECK(ret == 0,
              "access failed to follow external vendor link: %d errno %d",
              ret, errno);
   errno = 0;
   ret = euidaccess(vendor_link, R_OK);
   TEST_CHECK(ret == 0,
              "euidaccess failed to follow external vendor link: %d "
              "errno %d",
              ret, errno);

   errno = 0;
   link_fd = open(vendor_link_slash, O_RDONLY | O_CLOEXEC);
   TEST_CHECK(link_fd == -1 && errno == ENOTDIR,
              "external trailing-slash open returned %d errno %d",
              link_fd, errno);
   if (link_fd >= 0)
      close(link_fd);
   errno = 0;
   ret = stat(vendor_link_slash, &status);
   TEST_CHECK(ret == -1 && errno == ENOTDIR,
              "external trailing-slash stat returned %d errno %d",
              ret, errno);
   errno = 0;
   ret = access(vendor_link_slash, R_OK);
   TEST_CHECK(ret == -1 && errno == ENOTDIR,
              "external trailing-slash access returned %d errno %d",
              ret, errno);
   errno = 0;
   resolved = realpath(vendor_link_slash, resolved_path);
   TEST_CHECK(!resolved && errno == ENOTDIR,
              "external trailing-slash realpath returned %s errno %d",
              resolved ? resolved : "(null)", errno);

   errno = 0;
   ret = euidaccess(missing_link, F_OK);
   TEST_CHECK(ret == -1 && errno == ENOENT,
              "euidaccess failed to follow external link: %d errno %d",
              ret, errno);
   errno = 0;
   ret = faccessat(AT_FDCWD, missing_link, F_OK, 0);
   TEST_CHECK(ret == -1 && errno == ENOENT,
              "faccessat failed to follow external link: %d errno %d",
              ret, errno);
   errno = 0;
   ret = faccessat(AT_FDCWD, missing_link, F_OK,
                   AT_SYMLINK_NOFOLLOW);
   TEST_CHECK(ret == 0,
              "faccessat nofollow rejected external link: %d errno %d",
              ret, errno);

   errno = 0;
   int escape_fd = open(escape_link, O_RDONLY | O_CLOEXEC);
   TEST_CHECK(escape_fd == -1 && errno == ENOENT,
              "external synthetic path escaped its root: %d errno %d",
              escape_fd, errno);
   if (escape_fd >= 0)
      close(escape_fd);

   int device_fd =
      open("/sys/dev/char/226:128/device", O_PATH | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(device_fd >= 0,
              "synthetic directory O_PATH failed with errno %d", errno);
   if (device_fd >= 0) {
      char proc_fd_path[64];
      snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d",
               device_fd);
      ret = stat(proc_fd_path, &status);
      TEST_CHECK(ret == 0 && S_ISDIR(status.st_mode),
                 "proc-fd synthetic stat returned %d mode 0%o errno %d",
                 ret, status.st_mode, errno);
      ret = lstat(proc_fd_path, &status);
      TEST_CHECK(ret == 0 && S_ISLNK(status.st_mode),
                 "proc-fd lstat returned %d mode 0%o errno %d",
                 ret, status.st_mode, errno);
      close(device_fd);
   }

   unlink(escape_link);
   unlink(missing_link);
   unlink(vendor_link);
   unlink(device_link);
   rmdir(root);
}

static void
test_deep_dirfd_synthetic_link(void)
{
   enum { component_count = 48, component_length = 92 };
   char root[] = "/tmp/radeon-shim-deep-dirfd-XXXXXX";
   char *created_root = mkdtemp(root);
   TEST_CHECK(created_root,
              "deep-dirfd root creation failed with errno %d", errno);
   if (!created_root)
      return;

   int directory_fds[component_count + 1];
   char components[component_count][component_length];
   unsigned created_count = 0;
   directory_fds[0] =
      open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(directory_fds[0] >= 0,
              "deep-dirfd root open failed with errno %d", errno);
   if (directory_fds[0] < 0) {
      rmdir(root);
      return;
   }

   for (unsigned i = 0; i < component_count; i++) {
      int prefix_length =
         snprintf(components[i], sizeof(components[i]), "d%02u_", i);
      memset(components[i] + prefix_length, 'a',
             sizeof(components[i]) - (size_t)prefix_length - 1);
      components[i][sizeof(components[i]) - 1] = '\0';
      if (mkdirat(directory_fds[i], components[i], 0700) < 0)
         break;
      directory_fds[i + 1] =
         openat(directory_fds[i], components[i],
                O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (directory_fds[i + 1] < 0) {
         unlinkat(directory_fds[i], components[i], AT_REMOVEDIR);
         break;
      }
      created_count++;
   }
   TEST_CHECK(created_count == component_count,
              "deep-dirfd created %u of %u components",
              created_count, component_count);

   int deepest_fd = directory_fds[created_count];
   if (created_count == component_count) {
      int ret =
         symlinkat("/sys/dev/char/226:128/device", deepest_fd,
                   "synthetic-device");
      TEST_CHECK(ret == 0,
                 "deep-dirfd symlink creation returned %d errno %d",
                 ret, errno);
      if (ret == 0) {
         int vendor_fd =
            openat(deepest_fd, "synthetic-device/vendor",
                   O_RDONLY | O_CLOEXEC);
         TEST_CHECK(vendor_fd >= 0,
                    "deep-dirfd synthetic open failed with errno %d",
                    errno);
         if (vendor_fd >= 0) {
            char vendor[8] = {0};
            ssize_t length = read(vendor_fd, vendor, sizeof(vendor) - 1);
            TEST_CHECK(length == 7 && strcmp(vendor, "0x1002\n") == 0,
                       "deep-dirfd synthetic read returned %zd bytes: %s",
                       length, vendor);
            close(vendor_fd);
         }
         unlinkat(deepest_fd, "synthetic-device", 0);
      }
   }

   for (unsigned i = created_count; i > 0; i--) {
      close(directory_fds[i]);
      TEST_CHECK(
         unlinkat(directory_fds[i - 1], components[i - 1],
                  AT_REMOVEDIR) == 0,
         "deep-dirfd component %u cleanup failed with errno %d", i - 1,
         errno);
   }
   close(directory_fds[0]);
   TEST_CHECK(rmdir(root) == 0,
              "deep-dirfd root cleanup failed with errno %d", errno);
}

static void
test_bo_fd_limit(int fd)
{
   struct rlimit original_limit;
   int ret = getrlimit(RLIMIT_NOFILE, &original_limit);
   TEST_CHECK(ret == 0, "getrlimit RLIMIT_NOFILE returned %d errno %d",
              ret, errno);
   if (ret)
      return;

   struct rlimit test_limit = original_limit;
   if (test_limit.rlim_cur > 32)
      test_limit.rlim_cur = 32;
   TEST_CHECK(test_limit.rlim_cur >= 16,
              "RLIMIT_NOFILE is too small for calibration: %llu",
              (unsigned long long)test_limit.rlim_cur);
   if (test_limit.rlim_cur < 16)
      return;

   ret = setrlimit(RLIMIT_NOFILE, &test_limit);
   TEST_CHECK(ret == 0, "setrlimit RLIMIT_NOFILE returned %d errno %d",
              ret, errno);
   if (ret)
      return;

   enum { bo_count = 64 };
   uint32_t handles[bo_count] = {0};
   unsigned created_count = 0;
   for (unsigned i = 0; i < bo_count; i++) {
      struct drm_radeon_gem_create create = {
         .size = 4096,
         .alignment = 4096,
         .initial_domain = RADEON_GEM_DOMAIN_GTT,
      };
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
      if (ret)
         break;
      struct drm_radeon_gem_mmap mmap_bo = {
         .handle = create.handle,
      };
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
      if (ret) {
         struct drm_gem_close close_bo = {.handle = create.handle};
         ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         break;
      }
      handles[created_count++] = create.handle;
   }

   int create_errno = errno;
   TEST_CHECK(created_count == bo_count,
              "RLIMIT_NOFILE created %u of %u BOs, errno %d",
              created_count, bo_count, create_errno);

   for (unsigned i = 0; i < created_count; i++) {
      struct drm_gem_close close_bo = {.handle = handles[i]};
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      TEST_CHECK(ret == 0,
                 "RLIMIT_NOFILE BO %u close returned %d errno %d",
                 i, ret, errno);
   }

   ret = setrlimit(RLIMIT_NOFILE, &original_limit);
   TEST_CHECK(ret == 0,
              "RLIMIT_NOFILE restore returned %d errno %d", ret, errno);
}

static void
test_mem_addr_lifetime(int fd)
{
   const size_t page_size = 4096;
   struct drm_radeon_gem_create create = {
      .size = 3 * page_size,
      .alignment = page_size,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle != 0,
              "mapped-address GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   if (ret != 0)
      return;

   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, create.handle);
   uint64_t retained_mem_addr = bo ? bo->mem_addr : 0;
   TEST_CHECK(bo && retained_mem_addr,
              "mapped-address BO lookup returned %p address 0x%llx",
              (void *)bo, (unsigned long long)retained_mem_addr);
   if (bo)
      drm_shim_bo_put(bo);

   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && mmap_bo.addr_ptr,
              "mapped-address offset returned %d offset 0x%llx",
              ret, (unsigned long long)mmap_bo.addr_ptr);
   void *mapping = MAP_FAILED;
   if (ret == 0) {
      mapping =
         mmap(NULL, create.size, PROT_READ | PROT_WRITE,
              MAP_SHARED, fd, (off_t)mmap_bo.addr_ptr);
      TEST_CHECK(mapping != MAP_FAILED,
                 "mapped-address mmap failed with errno %d", errno);
   }

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0,
              "mapped-address GEM close returned %d errno %d", ret,
              errno);

   if (mapping != MAP_FAILED) {
      ret = munmap((char *)mapping + page_size, page_size);
      TEST_CHECK(ret == 0,
                 "mapped-address middle munmap returned %d errno %d",
                 ret, errno);
   }

   struct drm_radeon_gem_create replacement = {
      .size = 3 * page_size,
      .alignment = page_size,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &replacement);
   TEST_CHECK(ret == 0 && replacement.handle != 0,
              "mapped-address replacement create returned %d handle %u",
              ret, replacement.handle);
   if (ret == 0) {
      struct shim_bo *replacement_bo =
         drm_shim_bo_lookup(shim_fd, replacement.handle);
      TEST_CHECK(replacement_bo &&
                    replacement_bo->mem_addr != retained_mem_addr,
                 "partial VMA released GPU address 0x%llx",
                 (unsigned long long)retained_mem_addr);
      if (replacement_bo)
         drm_shim_bo_put(replacement_bo);
      close_bo.handle = replacement.handle;
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      TEST_CHECK(ret == 0,
                 "mapped-address replacement close returned %d", ret);
   }

   if (mapping != MAP_FAILED) {
      ret = munmap(mapping, page_size);
      TEST_CHECK(ret == 0,
                 "mapped-address left munmap returned %d errno %d",
                 ret, errno);
      ret = munmap((char *)mapping + 2 * page_size, page_size);
      TEST_CHECK(ret == 0,
                 "mapped-address right munmap returned %d errno %d",
                 ret, errno);
   }

   struct drm_radeon_gem_create remapped = {
      .size = 2 * page_size,
      .alignment = page_size,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &remapped);
   TEST_CHECK(ret == 0 && remapped.handle,
              "mremap-lifetime GEM create returned %d handle %u errno %d",
              ret, remapped.handle, errno);
   if (ret == 0) {
      struct shim_bo *remapped_bo =
         drm_shim_bo_lookup(shim_fd, remapped.handle);
      uint64_t remapped_mem_addr =
         remapped_bo ? remapped_bo->mem_addr : 0;
      if (remapped_bo)
         drm_shim_bo_put(remapped_bo);
      struct drm_radeon_gem_mmap remapped_mmap = {
         .handle = remapped.handle,
      };
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &remapped_mmap);
      TEST_CHECK(ret == 0 && remapped_mmap.addr_ptr,
                 "mremap-lifetime offset returned %d offset 0x%llx",
                 ret, (unsigned long long)remapped_mmap.addr_ptr);
      void *remapped_mapping = MAP_FAILED;
      if (ret == 0) {
         remapped_mapping =
            mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, (off_t)remapped_mmap.addr_ptr);
         TEST_CHECK(remapped_mapping != MAP_FAILED,
                    "mremap-lifetime mmap failed with errno %d", errno);
      }
      close_bo.handle = remapped.handle;
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      TEST_CHECK(ret == 0,
                 "mremap-lifetime GEM close returned %d errno %d",
                 ret, errno);
      if (remapped_mapping != MAP_FAILED) {
         void *moved =
            mremap(remapped_mapping, 2 * page_size, page_size,
                   MREMAP_MAYMOVE);
         TEST_CHECK(moved != MAP_FAILED,
                    "mremap-lifetime shrink returned %p errno %d",
                    moved, errno);
         if (moved != MAP_FAILED) {
            struct drm_radeon_gem_create while_mapped = {
               .size = 2 * page_size,
               .alignment = page_size,
               .initial_domain = RADEON_GEM_DOMAIN_GTT,
            };
            ret =
               ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &while_mapped);
            TEST_CHECK(ret == 0 && while_mapped.handle,
                       "mremap-lifetime replacement returned %d "
                       "handle %u errno %d",
                       ret, while_mapped.handle, errno);
            if (ret == 0) {
               struct shim_bo *while_mapped_bo =
                  drm_shim_bo_lookup(shim_fd, while_mapped.handle);
               TEST_CHECK(while_mapped_bo &&
                             while_mapped_bo->mem_addr !=
                                remapped_mem_addr,
                          "mremap released GPU address 0x%llx",
                          (unsigned long long)remapped_mem_addr);
               if (while_mapped_bo)
                  drm_shim_bo_put(while_mapped_bo);
               close_bo.handle = while_mapped.handle;
               ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
               TEST_CHECK(ret == 0,
                          "mremap-lifetime replacement close returned %d",
                          ret);
            }
            ret = munmap(moved, page_size);
            TEST_CHECK(ret == 0,
                       "mremap-lifetime munmap returned %d errno %d",
                       ret, errno);
         } else {
            munmap(remapped_mapping, 2 * page_size);
         }
      }
   }

#ifdef MREMAP_DONTUNMAP
   struct drm_radeon_gem_create dontunmap_create = {
      .size = page_size,
      .alignment = page_size,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   ret =
      ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &dontunmap_create);
   TEST_CHECK(ret == 0 && dontunmap_create.handle,
              "mremap DONTUNMAP create returned %d handle %u errno %d",
              ret, dontunmap_create.handle, errno);
   if (ret == 0) {
      struct shim_bo *dontunmap_bo =
         drm_shim_bo_lookup(shim_fd, dontunmap_create.handle);
      TEST_CHECK(dontunmap_bo,
                 "mremap DONTUNMAP BO lookup returned NULL");
      void *source =
         mmap(NULL, page_size, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      TEST_CHECK(source != MAP_FAILED,
                 "mremap DONTUNMAP mmap failed with errno %d", errno);
      bool registered =
         source != MAP_FAILED && dontunmap_bo;
      if (registered)
         drm_shim_mapping_replace(dontunmap_bo, source, page_size);
      if (dontunmap_bo)
         drm_shim_bo_put(dontunmap_bo);
      void *moved = MAP_FAILED;
      if (registered)
         moved =
            mremap(source, page_size, page_size,
                   MREMAP_MAYMOVE | MREMAP_DONTUNMAP);
      int dontunmap_errno = errno;
      TEST_CHECK(moved != MAP_FAILED || dontunmap_errno == EINVAL,
                 "mremap DONTUNMAP calibration returned %p errno %d",
                 moved, dontunmap_errno);
      if (moved != MAP_FAILED) {
         struct shim_bo *source_bo =
            drm_shim_mapping_get(source, page_size);
         struct shim_bo *moved_bo =
            drm_shim_mapping_get(moved, page_size);
         TEST_CHECK(!source_bo && moved_bo,
                    "mremap DONTUNMAP registry source %p moved %p",
                    (void *)source_bo, (void *)moved_bo);
         if (source_bo)
            drm_shim_bo_put(source_bo);
         if (moved_bo)
            drm_shim_bo_put(moved_bo);
      } else if (registered) {
         struct shim_bo *source_bo =
            drm_shim_mapping_get(source, page_size);
         TEST_CHECK(source_bo,
                    "failed mremap DONTUNMAP lost the source registry");
         if (source_bo)
            drm_shim_bo_put(source_bo);
      }
      close_bo.handle = dontunmap_create.handle;
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      TEST_CHECK(ret == 0,
                 "mremap DONTUNMAP GEM close returned %d errno %d",
                 ret, errno);
      if (moved != MAP_FAILED) {
         ret = munmap(moved, page_size);
         TEST_CHECK(ret == 0,
                    "mremap DONTUNMAP moved munmap returned %d errno %d",
                    ret, errno);
      }
      if (source != MAP_FAILED) {
         ret = munmap(source, page_size);
         TEST_CHECK(ret == 0,
                    "mremap DONTUNMAP source munmap returned %d errno %d",
                    ret, errno);
      }
   }
#endif

   for (unsigned i = 0; i < 4097; i++) {
      struct drm_radeon_gem_create recycled = {
         .size = 1u << 20,
         .alignment = page_size,
         .initial_domain = RADEON_GEM_DOMAIN_GTT,
      };
      ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &recycled);
      if (ret != 0 || !recycled.handle) {
         TEST_CHECK(false,
                    "recycled-address create %u returned %d handle %u "
                    "errno %d",
                    i, ret, recycled.handle, errno);
         break;
      }
      close_bo.handle = recycled.handle;
      ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      if (ret != 0) {
         TEST_CHECK(false,
                    "recycled-address close %u returned %d errno %d",
                    i, ret, errno);
         break;
      }
   }
}

static void
test_fork_child_close_preserves_parent_bo(int fd)
{
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle != 0,
              "fork-close GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   if (ret != 0)
      return;

   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && mmap_bo.addr_ptr != 0,
              "fork-close mmap-offset query returned %d offset 0x%llx",
              ret, (unsigned long long)mmap_bo.addr_ptr);
   if (ret == 0) {
      pid_t child = fork();
      TEST_CHECK(child >= 0, "fork-close fork failed with errno %d", errno);
      if (child == 0) {
         int close_result = close(fd);
         _exit(close_result == 0 ? 0 : 1);
      }
      if (child > 0) {
         int child_status;
         pid_t waited;
         do {
            waited = waitpid(child, &child_status, 0);
         } while (waited < 0 && errno == EINTR);
         TEST_CHECK(waited == child && WIFEXITED(child_status) &&
                       WEXITSTATUS(child_status) == 0,
                    "fork-close child status is 0x%x",
                    waited == child ? child_status : -1);

         void *mapping =
            mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (off_t)mmap_bo.addr_ptr);
         TEST_CHECK(mapping != MAP_FAILED,
                    "child close removed parent BO backing with errno %d",
                    errno);
         if (mapping != MAP_FAILED) {
            *(volatile uint32_t *)mapping = UINT32_C(0xdec0de01);
            munmap(mapping, 4096);
         }
      }
   }

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0, "fork-close GEM close returned %d errno %d",
              ret, errno);
}

static void
test_fork_child_identity_repair_preserves_parent_bo(int fd)
{
   int second_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(second_fd >= 0,
              "fork-identity second open failed with errno %d", errno);
   if (second_fd < 0)
      return;

   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle,
              "fork-identity GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   if (ret != 0)
      goto cleanup_second;

   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && mmap_bo.addr_ptr,
              "fork-identity mmap-offset query returned %d offset 0x%llx",
              ret, (unsigned long long)mmap_bo.addr_ptr);
   if (ret == 0) {
      pid_t child = fork();
      TEST_CHECK(child >= 0,
                 "fork-identity fork failed with errno %d", errno);
      if (child == 0) {
         int duplicate_result =
            syscall(SYS_dup2, second_fd, fd);
         struct stat status;
         int stat_result =
            duplicate_result == fd ? fstat(fd, &status) : -1;
         _exit(stat_result == 0 && S_ISCHR(status.st_mode) ? 0 : 1);
      }
      if (child > 0) {
         int child_status;
         pid_t waited;
         do {
            waited = waitpid(child, &child_status, 0);
         } while (waited < 0 && errno == EINTR);
         TEST_CHECK(waited == child && WIFEXITED(child_status) &&
                       WEXITSTATUS(child_status) == 0,
                    "fork-identity child status is 0x%x",
                    waited == child ? child_status : -1);

         void *mapping =
            mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 (off_t)mmap_bo.addr_ptr);
         TEST_CHECK(mapping != MAP_FAILED,
                    "fork-identity child repair removed parent backing "
                    "with errno %d",
                    errno);
         if (mapping != MAP_FAILED)
            munmap(mapping, 4096);
      }
   }

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0,
              "fork-identity GEM close returned %d errno %d", ret,
              errno);

cleanup_second:
   close(second_fd);
}

static ssize_t
test_send_descriptor(int socket_fd, int descriptor)
{
   char payload = 1;
   char control[CMSG_SPACE(sizeof(descriptor))];
   memset(control, 0, sizeof(control));
   struct iovec iov = {
      .iov_base = &payload,
      .iov_len = sizeof(payload),
   };
   struct msghdr message = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
   };
   struct cmsghdr *header = CMSG_FIRSTHDR(&message);
   header->cmsg_level = SOL_SOCKET;
   header->cmsg_type = SCM_RIGHTS;
   header->cmsg_len = CMSG_LEN(sizeof(descriptor));
   memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
   return sendmsg(socket_fd, &message, 0);
}

static int
test_receive_descriptor_flags(int socket_fd, int flags)
{
   char payload = 0;
   char control[CMSG_SPACE(sizeof(int))];
   memset(control, 0, sizeof(control));
   struct iovec iov = {
      .iov_base = &payload,
      .iov_len = sizeof(payload),
   };
   struct msghdr message = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
   };
   ssize_t length =
      recvmsg(socket_fd, &message, flags | MSG_CMSG_CLOEXEC);
   if (length != sizeof(payload) ||
       (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) {
      if (length >= 0)
         errno = EPROTO;
      return -1;
   }

   struct cmsghdr *header = CMSG_FIRSTHDR(&message);
   if (!header || header->cmsg_level != SOL_SOCKET ||
       header->cmsg_type != SCM_RIGHTS ||
       header->cmsg_len != CMSG_LEN(sizeof(int))) {
      errno = EPROTO;
      return -1;
   }

   int descriptor;
   memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
   return descriptor;
}

static int
test_receive_descriptor(int socket_fd)
{
   return test_receive_descriptor_flags(socket_fd, 0);
}

static void
test_state_token_wrong_inode_witness(void)
{
   int witness_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(witness_fd >= 0,
              "wrong-inode witness open failed with errno %d", errno);
   if (witness_fd < 0)
      return;

   drm_shim_test_force_state_token_readable_witness(witness_fd);
   errno = 0;
   int rejected_open =
      open("/dev/dri/renderD128", O_WRONLY | O_CLOEXEC);
   int open_errno = errno;
   int readable_witness_fd =
      drm_shim_test_state_token_readable_witness_fd();
   int rejected_fd = drm_shim_test_rejected_state_token_fd();
   drm_shim_test_force_state_token_readable_witness(-1);

   TEST_CHECK(rejected_open == -1 && open_errno == ENODEV,
              "wrong-inode state token open returned %d errno %d",
              rejected_open, open_errno);
   TEST_CHECK(rejected_fd >= 0,
              "wrong-inode rejection did not record its target fd");
   TEST_CHECK(readable_witness_fd >= 0,
              "wrong-inode rejection did not record its readable witness");
   if (rejected_fd >= 0)
      TEST_CHECK(!drm_shim_test_fd_is_registered(rejected_fd),
                 "wrong-inode target fd %d remained registered",
                 rejected_fd);
   if (rejected_fd >= 0) {
      errno = 0;
      int descriptor_flags = fcntl(rejected_fd, F_GETFD);
      int descriptor_errno = errno;
      TEST_CHECK(descriptor_flags == -1 && descriptor_errno == EBADF,
                 "wrong-inode target fd %d remained open: %d errno %d",
                 rejected_fd, descriptor_flags, descriptor_errno);
   }
   if (readable_witness_fd >= 0) {
      errno = 0;
      int descriptor_flags = fcntl(readable_witness_fd, F_GETFD);
      int descriptor_errno = errno;
      TEST_CHECK(descriptor_flags == -1 && descriptor_errno == EBADF,
                 "wrong-inode readable witness fd %d remained open: %d "
                 "errno %d",
                 readable_witness_fd, descriptor_flags,
                 descriptor_errno);
   }
   if (rejected_open >= 0)
      close(rejected_open);
   close(witness_fd);
}

static void
test_write_only_scm_rights(void)
{
   int bo_baseline = drm_shim_test_live_bos();
   int backing_baseline = drm_shim_test_live_bo_backing_files();
   int source_fd =
      open("/dev/dri/renderD128", O_WRONLY | O_CLOEXEC);
   TEST_CHECK(source_fd >= 0,
              "write-only SCM source open failed with errno %d", errno);
   if (source_fd < 0)
      return;

   int source_status_flags = fcntl(source_fd, F_GETFL);
   TEST_CHECK(source_status_flags >= 0 &&
                 (source_status_flags & O_ACCMODE) == O_WRONLY,
              "write-only SCM source flags are 0x%x",
              source_status_flags);

   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret =
      ioctl(source_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   if (ret == 0)
      ret = ioctl(source_fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && create.handle && mmap_bo.addr_ptr &&
                 drm_shim_test_live_bos() == bo_baseline + 1 &&
                 drm_shim_test_live_bo_backing_files() ==
                    backing_baseline + 1,
              "write-only SCM setup returned %d handle %u offset 0x%llx "
              "BOs %d/%d backing %d/%d errno %d",
              ret, create.handle,
              (unsigned long long)mmap_bo.addr_ptr,
              drm_shim_test_live_bos(), bo_baseline,
              drm_shim_test_live_bo_backing_files(), backing_baseline,
              errno);

   int sockets[2] = {-1, -1};
   int received_fd = -1;
   int received_fd_number = -1;
   int socket_result =
      socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets);
   TEST_CHECK(socket_result == 0,
              "write-only SCM socketpair returned %d errno %d",
              socket_result, errno);
   if (socket_result == 0 && ret == 0) {
      ssize_t length = test_send_descriptor(sockets[0], source_fd);
      TEST_CHECK(length == 1,
                 "write-only SCM send returned %zd errno %d",
                 length, errno);
      if (length == 1) {
         int close_result = close(source_fd);
         source_fd = -1;
         TEST_CHECK(close_result == 0,
                    "write-only SCM source close returned %d errno %d",
                    close_result, errno);

         received_fd = test_receive_descriptor(sockets[1]);
         received_fd_number = received_fd;
         TEST_CHECK(received_fd >= 0,
                    "write-only SCM receive returned %d errno %d",
                    received_fd, errno);
         if (received_fd >= 0) {
            int received_status_flags = fcntl(received_fd, F_GETFL);
            TEST_CHECK(received_status_flags >= 0 &&
                          (received_status_flags & O_ACCMODE) == O_WRONLY,
                       "write-only SCM receiver flags are 0x%x",
                       received_status_flags);
            struct drm_radeon_gem_busy busy = {
               .handle = create.handle,
               .domain = UINT32_MAX,
            };
            ret =
               ioctl(received_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
            TEST_CHECK(ret == 0 &&
                          busy.domain == RADEON_GEM_DOMAIN_GTT,
                       "write-only SCM receiver lost GEM state: %d "
                       "domain 0x%x errno %d",
                       ret, busy.domain, errno);
            struct drm_gem_close close_bo = {
               .handle = create.handle,
            };
            ret = ioctl(received_fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
            TEST_CHECK(ret == 0,
                       "write-only SCM GEM close returned %d errno %d",
                       ret, errno);
            if (ret == 0)
               create.handle = 0;
         }
      }
   }

   if (received_fd >= 0)
      close(received_fd);
   if (received_fd_number >= 0) {
      TEST_CHECK(!drm_shim_test_fd_is_registered(received_fd_number),
                 "write-only SCM receiver fd %d remained registered",
                 received_fd_number);
      errno = 0;
      int descriptor_flags = fcntl(received_fd_number, F_GETFD);
      int descriptor_errno = errno;
      TEST_CHECK(descriptor_flags == -1 && descriptor_errno == EBADF,
                 "write-only SCM receiver fd %d remained open: %d "
                 "errno %d",
                 received_fd_number, descriptor_flags,
                 descriptor_errno);
   }
   if (source_fd >= 0) {
      if (create.handle) {
         struct drm_gem_close close_bo = {
            .handle = create.handle,
         };
         (void)ioctl(source_fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         create.handle = 0;
      }
      close(source_fd);
   }
   if (sockets[0] >= 0)
      close(sockets[0]);
   if (sockets[1] >= 0)
      close(sockets[1]);

   TEST_CHECK(drm_shim_test_live_bos() == bo_baseline &&
                 drm_shim_test_live_bo_backing_files() ==
                    backing_baseline,
              "write-only SCM cleanup retained BOs %d/%d or backing "
              "files %d/%d",
              drm_shim_test_live_bos(), bo_baseline,
              drm_shim_test_live_bo_backing_files(), backing_baseline);
}

static int
test_write_only_state_token_readable_witness(void)
{
   if (setenv("RADEON_GPU_ID", "0x5974", 1) < 0)
      return 1;
   test_state_token_wrong_inode_witness();
   test_write_only_scm_rights();
   return test_failures ? 1 : 0;
}

static void
test_scm_record_fifo(void)
{
   int backing_baseline = drm_shim_test_live_bo_backing_files();
   int source_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(source_fd >= 0,
              "SCM FIFO source open failed with errno %d", errno);
   if (source_fd < 0)
      return;

   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret =
      ioctl(source_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   if (ret == 0)
      ret = ioctl(source_fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && create.handle && mmap_bo.addr_ptr,
              "SCM FIFO source setup returned %d handle %u "
              "offset 0x%llx errno %d",
              ret, create.handle,
              (unsigned long long)mmap_bo.addr_ptr, errno);

   int stream_sockets[2] = {-1, -1};
   int stream_result =
      socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                 stream_sockets);
   if (ret == 0 && stream_result == 0) {
      errno = 0;
      ssize_t stream_length =
         test_send_descriptor(stream_sockets[0], source_fd);
      TEST_CHECK(stream_length == -1 && errno == EOPNOTSUPP,
                 "SCM FIFO untracked stream send returned %zd "
                 "errno %d",
                 stream_length, errno);
   }
   if (stream_sockets[0] >= 0)
      close(stream_sockets[0]);
   if (stream_sockets[1] >= 0)
      close(stream_sockets[1]);

   int ordinary_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
   int sockets[2] = {-1, -1};
   int socket_result =
      socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets);
   TEST_CHECK(ordinary_fd >= 0 && socket_result == 0,
              "SCM FIFO endpoints returned ordinary %d socket %d "
              "errno %d",
              ordinary_fd, socket_result, errno);

   if (ret == 0 && ordinary_fd >= 0 && socket_result == 0) {
      char payload = 1;
      ssize_t length = writev(sockets[0], NULL, 0);
      TEST_CHECK(length == 0,
                 "SCM FIFO empty writev returned %zd errno %d",
                 length, errno);
      length = write(sockets[0], &payload, sizeof(payload));
      TEST_CHECK(length == sizeof(payload),
                 "SCM FIFO plain send returned %zd errno %d",
                 length, errno);

      int descriptors[2] = {ordinary_fd, source_fd};
      char send_control[CMSG_SPACE(sizeof(descriptors))];
      memset(send_control, 0, sizeof(send_control));
      struct iovec send_iov = {
         .iov_base = &payload,
         .iov_len = sizeof(payload),
      };
      struct msghdr send_message = {
         .msg_iov = &send_iov,
         .msg_iovlen = 1,
         .msg_control = send_control,
         .msg_controllen = sizeof(send_control),
      };
      struct cmsghdr *send_header = CMSG_FIRSTHDR(&send_message);
      send_header->cmsg_level = SOL_SOCKET;
      send_header->cmsg_type = SCM_RIGHTS;
      send_header->cmsg_len = CMSG_LEN(sizeof(descriptors));
      memcpy(CMSG_DATA(send_header), descriptors,
             sizeof(descriptors));
      length = sendmsg(sockets[0], &send_message, 0);
      TEST_CHECK(length == sizeof(payload),
                 "SCM FIFO mixed send returned %zd errno %d",
                 length, errno);

      char received_payload = 0;
      length = read(sockets[1], &received_payload, 0);
      TEST_CHECK(length == 0,
                 "SCM FIFO zero-capacity read returned %zd errno %d",
                 length, errno);
      length =
         recv(sockets[1], &received_payload,
              sizeof(received_payload), 0);
      TEST_CHECK(length == sizeof(received_payload),
                 "SCM FIFO plain receive returned %zd errno %d",
                 length, errno);

      int received_descriptors[2] = {-1, -1};
      char receive_control[CMSG_SPACE(sizeof(received_descriptors))];
      memset(receive_control, 0, sizeof(receive_control));
      struct iovec receive_iov = {
         .iov_base = &received_payload,
         .iov_len = sizeof(received_payload),
      };
      struct msghdr receive_message = {
         .msg_iov = &receive_iov,
         .msg_iovlen = 1,
         .msg_control = receive_control,
         .msg_controllen = sizeof(receive_control),
      };
      length =
         recvmsg(sockets[1], &receive_message, MSG_CMSG_CLOEXEC);
      struct cmsghdr *receive_header =
         CMSG_FIRSTHDR(&receive_message);
      if (length == sizeof(received_payload) && receive_header &&
          receive_header->cmsg_level == SOL_SOCKET &&
          receive_header->cmsg_type == SCM_RIGHTS &&
          receive_header->cmsg_len ==
             CMSG_LEN(sizeof(received_descriptors)))
         memcpy(received_descriptors, CMSG_DATA(receive_header),
                sizeof(received_descriptors));
      TEST_CHECK(received_descriptors[0] >= 0 &&
                    received_descriptors[1] >= 0,
                 "SCM FIFO mixed receive returned %zd fds %d/%d "
                 "flags 0x%x errno %d",
                 length, received_descriptors[0],
                 received_descriptors[1],
                 receive_message.msg_flags, errno);
      if (received_descriptors[0] >= 0) {
         struct drm_radeon_gem_busy busy = {
            .handle = create.handle,
            .domain = UINT32_MAX,
         };
         errno = 0;
         int busy_result =
            ioctl(received_descriptors[0],
                  DRM_IOCTL_RADEON_GEM_BUSY, &busy);
         TEST_CHECK(busy_result == -1 && errno == ENOTTY,
                    "SCM FIFO ordinary slot gained shim state: %d "
                    "errno %d",
                    busy_result, errno);
         close(received_descriptors[0]);
      }
      if (received_descriptors[1] >= 0) {
         struct drm_radeon_gem_busy busy = {
            .handle = create.handle,
            .domain = UINT32_MAX,
         };
         int busy_result =
            ioctl(received_descriptors[1],
                  DRM_IOCTL_RADEON_GEM_BUSY, &busy);
         TEST_CHECK(busy_result == 0 &&
                       busy.domain == RADEON_GEM_DOMAIN_GTT,
                    "SCM FIFO shim slot lost state: %d domain 0x%x "
                    "errno %d",
                    busy_result, busy.domain, errno);
         close(received_descriptors[1]);
      }

      length = test_send_descriptor(sockets[0], source_fd);
      TEST_CHECK(length == sizeof(payload),
                 "SCM FIFO peek send returned %zd errno %d",
                 length, errno);
      int peek_fd =
         length == sizeof(payload)
            ? test_receive_descriptor_flags(sockets[1], MSG_PEEK)
            : -1;
      TEST_CHECK(peek_fd >= 0,
                 "SCM FIFO peek receive returned %d errno %d",
                 peek_fd, errno);
      if (peek_fd >= 0) {
         struct drm_radeon_gem_busy busy = {
            .handle = create.handle,
            .domain = UINT32_MAX,
         };
         int busy_result =
            ioctl(peek_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
         TEST_CHECK(busy_result == 0 &&
                       busy.domain == RADEON_GEM_DOMAIN_GTT,
                    "SCM FIFO peek lost state: %d domain 0x%x errno %d",
                    busy_result, busy.domain, errno);
         close(peek_fd);
      }
      int consumed_fd =
         length == sizeof(payload)
            ? test_receive_descriptor(sockets[1])
            : -1;
      TEST_CHECK(consumed_fd >= 0,
                 "SCM FIFO post-peek receive returned %d errno %d",
                 consumed_fd, errno);
      if (consumed_fd >= 0)
         close(consumed_fd);

      length = test_send_descriptor(sockets[0], source_fd);
      TEST_CHECK(length == sizeof(payload),
                 "SCM FIFO truncation send returned %zd errno %d",
                 length, errno);
      struct iovec truncated_iov = {
         .iov_base = &received_payload,
         .iov_len = sizeof(received_payload),
      };
      struct msghdr truncated_message = {
         .msg_iov = &truncated_iov,
         .msg_iovlen = 1,
      };
      if (length == sizeof(payload))
         length = recvmsg(sockets[1], &truncated_message, 0);
      TEST_CHECK(length == sizeof(received_payload) &&
                    (truncated_message.msg_flags & MSG_CTRUNC),
                 "SCM FIFO truncated receive returned %zd flags 0x%x "
                 "errno %d",
                 length, truncated_message.msg_flags, errno);

      length = test_send_descriptor(sockets[0], source_fd);
      TEST_CHECK(length == sizeof(payload),
                 "SCM FIFO discard send returned %zd errno %d",
                 length, errno);
      close(source_fd);
      source_fd = -1;
      if (length == sizeof(payload))
         length =
            read(sockets[1], &received_payload,
                 sizeof(received_payload));
      TEST_CHECK(length == sizeof(received_payload),
                 "SCM FIFO discard read returned %zd errno %d",
                 length, errno);
      TEST_CHECK(drm_shim_test_live_bo_backing_files() ==
                    backing_baseline,
                 "SCM FIFO discard retained %d backing files from "
                 "baseline %d",
                 drm_shim_test_live_bo_backing_files(),
                 backing_baseline);
   }

   if (source_fd >= 0)
      close(source_fd);
   if (ordinary_fd >= 0)
      close(ordinary_fd);
   if (sockets[0] >= 0)
      close(sockets[0]);
   if (sockets[1] >= 0)
      close(sockets[1]);
   TEST_CHECK(drm_shim_test_live_bo_backing_files() ==
                 backing_baseline,
              "SCM FIFO cleanup retained %d backing files from "
              "baseline %d",
              drm_shim_test_live_bo_backing_files(),
              backing_baseline);
}

static void
test_delayed_scm_rights_receive(void)
{
   int backing_baseline = drm_shim_test_live_bo_backing_files();
   int source_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(source_fd >= 0,
              "delayed SCM source open failed with errno %d", errno);
   if (source_fd < 0)
      return;

   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret =
      ioctl(source_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle,
              "delayed SCM GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   if (ret == 0)
      ret = ioctl(source_fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && mmap_bo.addr_ptr &&
                 drm_shim_test_live_bo_backing_files() ==
                    backing_baseline + 1,
              "delayed SCM mmap setup returned %d offset 0x%llx "
              "live %d baseline %d errno %d",
              ret, (unsigned long long)mmap_bo.addr_ptr,
              drm_shim_test_live_bo_backing_files(),
              backing_baseline, errno);

   int sockets[2] = {-1, -1};
   int received_fd = -1;
   int socket_result =
      socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets);
   TEST_CHECK(socket_result == 0,
              "delayed SCM socketpair returned %d errno %d",
              socket_result, errno);
   if (socket_result == 0 && ret == 0) {
      ssize_t length = test_send_descriptor(sockets[0], source_fd);
      TEST_CHECK(length == 1,
                 "delayed SCM send returned %zd errno %d",
                 length, errno);
      if (length == 1) {
         int close_result = close(source_fd);
         source_fd = -1;
         TEST_CHECK(close_result == 0,
                    "delayed SCM source close returned %d errno %d",
                    close_result, errno);

         received_fd = test_receive_descriptor(sockets[1]);
         TEST_CHECK(received_fd >= 0,
                    "delayed SCM receive returned %d errno %d",
                    received_fd, errno);
         if (received_fd >= 0) {
            struct drm_radeon_gem_busy busy = {
               .handle = create.handle,
               .domain = UINT32_MAX,
            };
            ret =
               ioctl(received_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
            TEST_CHECK(ret == 0 &&
                          busy.domain == RADEON_GEM_DOMAIN_GTT,
                       "delayed SCM lost GEM state: %d domain 0x%x "
                       "errno %d",
                       ret, busy.domain, errno);
            struct drm_gem_close close_bo = {
               .handle = create.handle,
            };
            ret =
               ioctl(received_fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
            TEST_CHECK(ret == 0,
                       "delayed SCM GEM close returned %d errno %d",
                       ret, errno);
            create.handle = 0;
         }
      }
   }

   if (received_fd >= 0)
      close(received_fd);
   if (source_fd >= 0) {
      if (create.handle) {
         struct drm_gem_close close_bo = {
            .handle = create.handle,
         };
         (void)ioctl(source_fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
         create.handle = 0;
      }
      close(source_fd);
   }
   if (sockets[0] >= 0)
      close(sockets[0]);
   if (sockets[1] >= 0)
      close(sockets[1]);

   TEST_CHECK(drm_shim_test_live_bo_backing_files() ==
                 backing_baseline,
              "delayed SCM retained %d backing files from baseline %d",
              drm_shim_test_live_bo_backing_files(),
              backing_baseline);
}

static void
test_dropped_scm_rights_release_body(void)
{
   int backing_baseline = drm_shim_test_live_bo_backing_files();
   int source_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(source_fd >= 0,
              "dropped SCM source open failed with errno %d", errno);
   if (source_fd < 0)
      return;

   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret =
      ioctl(source_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   struct drm_radeon_gem_mmap mmap_bo = {
      .handle = create.handle,
   };
   if (ret == 0)
      ret = ioctl(source_fd, DRM_IOCTL_RADEON_GEM_MMAP, &mmap_bo);
   TEST_CHECK(ret == 0 && create.handle && mmap_bo.addr_ptr &&
                 drm_shim_test_live_bo_backing_files() ==
                    backing_baseline + 1,
              "dropped SCM setup returned %d handle %u offset 0x%llx "
              "live %d baseline %d errno %d",
              ret, create.handle,
              (unsigned long long)mmap_bo.addr_ptr,
              drm_shim_test_live_bo_backing_files(),
              backing_baseline, errno);

   int sockets[2] = {-1, -1};
   int socket_result =
      socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets);
   TEST_CHECK(socket_result == 0,
              "dropped SCM socketpair returned %d errno %d",
              socket_result, errno);
   ssize_t length = -1;
   if (socket_result == 0 && ret == 0)
      length = test_send_descriptor(sockets[0], source_fd);
   TEST_CHECK(length == 1,
              "dropped SCM send returned %zd errno %d", length,
              errno);

   if (length == 1) {
      int close_result = close(source_fd);
      source_fd = -1;
      TEST_CHECK(close_result == 0,
                 "dropped SCM source close returned %d errno %d",
                 close_result, errno);
      close(sockets[0]);
      close(sockets[1]);
      sockets[0] = -1;
      sockets[1] = -1;
   }

   if (source_fd >= 0) {
      if (create.handle) {
         struct drm_gem_close close_bo = {
            .handle = create.handle,
         };
         (void)ioctl(source_fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
      }
      close(source_fd);
   }
   if (sockets[0] >= 0)
      close(sockets[0]);
   if (sockets[1] >= 0)
      close(sockets[1]);

   TEST_CHECK(drm_shim_test_live_bo_backing_files() ==
                 backing_baseline,
              "dropped SCM retained %d backing files from baseline %d",
              drm_shim_test_live_bo_backing_files(),
              backing_baseline);
}

static void
test_dropped_scm_rights_release(void)
{
   pid_t child = fork();
   TEST_CHECK(child >= 0,
              "dropped SCM isolation fork failed with errno %d",
              errno);
   if (child == 0) {
      unsigned failures_before = test_failures;
      test_dropped_scm_rights_release_body();
      _exit(test_failures == failures_before ? 0 : 1);
   }
   if (child < 0)
      return;

   int child_status;
   pid_t waited;
   do {
      waited = waitpid(child, &child_status, 0);
   } while (waited < 0 && errno == EINTR);
   TEST_CHECK(waited == child && WIFEXITED(child_status) &&
                 WEXITSTATUS(child_status) == 0,
              "dropped SCM child status is 0x%x",
              waited == child ? child_status : -1);
}

static void
test_shared_drm_file_description(int fd)
{
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle != 0,
              "shared-file GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   if (ret != 0)
      return;

   int duplicates[5] = {-1, -1, -1, -1, -1};
   duplicates[0] = syscall(SYS_dup, fd);
   duplicates[1] = syscall(SYS_fcntl, fd, F_DUPFD, 3);
   duplicates[2] = syscall(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 3);
   int replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);
   if (replacement >= 0) {
#ifdef SYS_dup2
      duplicates[3] = syscall(SYS_dup2, fd, replacement);
#else
      duplicates[3] = dup2(fd, replacement);
#endif
   }
   int replacement3 = open("/dev/null", O_RDONLY | O_CLOEXEC);
   if (replacement3 >= 0) {
#ifdef SYS_dup3
      duplicates[4] = syscall(SYS_dup3, fd, replacement3, O_CLOEXEC);
#else
      duplicates[4] = dup3(fd, replacement3, O_CLOEXEC);
#endif
   }

   for (size_t i = 0; i < ARRAY_SIZE(duplicates); i++) {
      int duplicate = duplicates[i];
      TEST_CHECK(duplicate >= 0,
                 "raw duplicate %zu returned %d errno %d", i,
                 duplicate, errno);
      if (duplicate < 0)
         continue;
      struct stat status;
      ret = fstat(duplicate, &status);
      TEST_CHECK(ret == 0 && S_ISCHR(status.st_mode) &&
                    major(status.st_rdev) == DRM_MAJOR &&
                    minor(status.st_rdev) == 128,
                 "raw duplicate %zu first fstat returned %d mode 0%o",
                 i, ret, status.st_mode);
      struct drm_radeon_gem_busy busy = {
         .handle = create.handle,
         .domain = UINT32_MAX,
      };
      ret = ioctl(duplicate, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
      TEST_CHECK(ret == 0 &&
                    busy.domain == RADEON_GEM_DOMAIN_GTT,
                 "raw duplicate %zu lost GEM handles: %d domain 0x%x",
                 i, ret, busy.domain);
      close(duplicate);
   }

   int sockets[2];
   ret = socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets);
   TEST_CHECK(ret == 0,
              "SCM_RIGHTS socketpair returned %d errno %d", ret, errno);
   if (ret == 0) {
      char payload = 1;
      char control[CMSG_SPACE(sizeof(fd))];
      memset(control, 0, sizeof(control));
      struct iovec send_iov = {
         .iov_base = &payload,
         .iov_len = sizeof(payload),
      };
      struct msghdr send_message = {
         .msg_iov = &send_iov,
         .msg_iovlen = 1,
         .msg_control = control,
         .msg_controllen = sizeof(control),
      };
      struct cmsghdr *send_control = CMSG_FIRSTHDR(&send_message);
      send_control->cmsg_level = SOL_SOCKET;
      send_control->cmsg_type = SCM_RIGHTS;
      send_control->cmsg_len = CMSG_LEN(sizeof(fd));
      memcpy(CMSG_DATA(send_control), &fd, sizeof(fd));
      ssize_t length = sendmsg(sockets[0], &send_message, 0);
      TEST_CHECK(length == sizeof(payload),
                 "SCM_RIGHTS send returned %zd errno %d", length, errno);

      char received_payload;
      char received_control[CMSG_SPACE(sizeof(fd))];
      struct iovec receive_iov = {
         .iov_base = &received_payload,
         .iov_len = sizeof(received_payload),
      };
      struct msghdr receive_message = {
         .msg_iov = &receive_iov,
         .msg_iovlen = 1,
         .msg_control = received_control,
         .msg_controllen = sizeof(received_control),
      };
      length = recvmsg(sockets[1], &receive_message, MSG_CMSG_CLOEXEC);
      TEST_CHECK(length == sizeof(received_payload),
                 "SCM_RIGHTS receive returned %zd errno %d", length,
                 errno);
      int received_fd = -1;
      struct cmsghdr *received_control_header =
         CMSG_FIRSTHDR(&receive_message);
      if (received_control_header &&
          received_control_header->cmsg_level == SOL_SOCKET &&
          received_control_header->cmsg_type == SCM_RIGHTS)
         memcpy(&received_fd, CMSG_DATA(received_control_header),
                sizeof(received_fd));
      TEST_CHECK(received_fd >= 0,
                 "SCM_RIGHTS returned descriptor %d", received_fd);
      if (received_fd >= 0) {
         struct stat status;
         ret = fstat(received_fd, &status);
         TEST_CHECK(ret == 0 && S_ISCHR(status.st_mode),
                    "SCM_RIGHTS first fstat returned %d mode 0%o",
                    ret, status.st_mode);
         struct drm_radeon_gem_busy busy = {
            .handle = create.handle,
            .domain = UINT32_MAX,
         };
         ret = ioctl(received_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
         TEST_CHECK(ret == 0 &&
                       busy.domain == RADEON_GEM_DOMAIN_GTT,
                    "SCM_RIGHTS lost GEM handles: %d domain 0x%x",
                    ret, busy.domain);
         close(received_fd);
      }
      close(sockets[0]);
      close(sockets[1]);
   }

   int separate_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(separate_fd >= 0,
              "separate DRM open failed with errno %d", errno);
   if (separate_fd >= 0) {
      struct drm_radeon_gem_busy busy = {
         .handle = create.handle,
         .domain = UINT32_MAX,
      };
      errno = 0;
      ret = ioctl(separate_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
      TEST_CHECK(ret == -1 && errno == ENOENT &&
                    busy.domain == UINT32_MAX,
                 "separate DRM open shared handles: %d errno %d "
                 "domain 0x%x",
                 ret, errno, busy.domain);
      close(separate_fd);
   }

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0,
              "shared-file GEM close returned %d errno %d", ret, errno);
}

static void
test_delayed_raw_duplicate_discovery(void)
{
   int source_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(source_fd >= 0,
              "delayed raw duplicate source open failed with errno %d",
              errno);
   if (source_fd < 0)
      return;

   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret =
      ioctl(source_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle,
              "delayed raw duplicate GEM create returned %d handle %u",
              ret, create.handle);
   int raw_alias = syscall(SYS_dup, source_fd);
   TEST_CHECK(raw_alias >= 0,
              "delayed raw duplicate returned %d errno %d",
              raw_alias, errno);
   close(source_fd);
   if (ret == 0 && raw_alias >= 0) {
      struct drm_radeon_gem_busy busy = {
         .handle = create.handle,
         .domain = UINT32_MAX,
      };
      ret = ioctl(raw_alias, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
      TEST_CHECK(ret == 0 &&
                    busy.domain == RADEON_GEM_DOMAIN_GTT,
                 "delayed raw duplicate lost GEM state: %d domain 0x%x "
                 "errno %d",
                 ret, busy.domain, errno);
      struct drm_gem_close close_bo = {
         .handle = create.handle,
      };
      ret = ioctl(raw_alias, DRM_IOCTL_GEM_CLOSE, &close_bo);
      TEST_CHECK(ret == 0,
                 "delayed raw duplicate GEM close returned %d errno %d",
                 ret, errno);
   }
   if (raw_alias >= 0)
      close(raw_alias);
}

static void
test_fd_identity_first_discovery(void)
{
   const char *synthetic_root = drm_shim_test_synthetic_root_path();
   char render_path[PATH_MAX];
   int length =
      snprintf(render_path, sizeof(render_path),
               "%s/dev/dri/renderD128", synthetic_root);
   TEST_CHECK(length > 0 && length < (int)sizeof(render_path),
              "first-discovery render path length is %d", length);
   if (length <= 0 || length >= (int)sizeof(render_path))
      return;

   int seed_fd =
      syscall(SYS_openat, AT_FDCWD, render_path,
              O_RDWR | O_CLOEXEC, 0);
   TEST_CHECK(seed_fd >= 0,
              "first-discovery raw open failed with errno %d", errno);
   if (seed_fd < 0)
      return;
   int aliases[2] = {
      syscall(SYS_fcntl, seed_fd, F_DUPFD_CLOEXEC, 3),
      syscall(SYS_fcntl, seed_fd, F_DUPFD_CLOEXEC, 3),
   };
   syscall(SYS_close, seed_fd);
   TEST_CHECK(aliases[0] >= 0 && aliases[1] >= 0,
              "first-discovery raw aliases are %d and %d",
              aliases[0], aliases[1]);
   if (aliases[0] < 0 || aliases[1] < 0)
      goto cleanup_aliases;

   int start_pipe[2];
   int ready_pipe[2];
   int release_pipe[2];
   int start_result = pipe2(start_pipe, O_CLOEXEC);
   int ready_result = pipe2(ready_pipe, O_CLOEXEC);
   int release_result = pipe2(release_pipe, O_CLOEXEC);
   TEST_CHECK(start_result == 0 && ready_result == 0 &&
                 release_result == 0,
              "first-discovery pipes failed with errno %d", errno);
   if (start_result != 0 || ready_result != 0 ||
       release_result != 0)
      goto cleanup_pipes;

   struct test_fd_discovery_context contexts[2] = {
      {.fd = aliases[0], .start_fd = start_pipe[0], .result = -2},
      {.fd = aliases[1], .start_fd = start_pipe[0], .result = -2},
   };
   pthread_t workers[2];
   int created = 0;
   for (; created < 2; created++) {
      int thread_result =
         pthread_create(&workers[created], NULL,
                        test_fd_discovery_worker,
                        &contexts[created]);
      TEST_CHECK(thread_result == 0,
                 "first-discovery pthread_create %d returned %d",
                 created, thread_result);
      if (thread_result != 0)
         break;
   }
   if (created == 2) {
      drm_shim_test_arm_fd_discovery_barrier(
         ready_pipe[1], release_pipe[0]);
      char bytes[2] = {1, 1};
      ssize_t transferred = write(start_pipe[1], bytes, sizeof(bytes));
      TEST_CHECK(transferred == sizeof(bytes),
                 "first-discovery start returned %zd", transferred);
      transferred =
         test_read_bytes(ready_pipe[0], bytes, sizeof(bytes));
      TEST_CHECK(transferred == sizeof(bytes),
                 "first-discovery readiness returned %zd", transferred);
      transferred = write(release_pipe[1], bytes, sizeof(bytes));
      TEST_CHECK(transferred == sizeof(bytes),
                 "first-discovery release returned %zd", transferred);
   } else {
      char bytes[2] = {1, 1};
      (void)write(start_pipe[1], bytes, sizeof(bytes));
   }
   for (int i = 0; i < created; i++) {
      int thread_result = pthread_join(workers[i], NULL);
      TEST_CHECK(thread_result == 0 && contexts[i].result == 0 &&
                    S_ISCHR(contexts[i].status.st_mode),
                 "first-discovery worker %d joined %d with result %d "
                 "mode 0%o",
                 i, thread_result, contexts[i].result,
                 contexts[i].status.st_mode);
   }

   if (created == 2) {
      struct drm_radeon_gem_create create = {
         .size = 4096,
         .alignment = 4096,
         .initial_domain = RADEON_GEM_DOMAIN_GTT,
      };
      int ret =
         ioctl(aliases[0], DRM_IOCTL_RADEON_GEM_CREATE, &create);
      TEST_CHECK(ret == 0 && create.handle,
                 "first-discovery GEM create returned %d handle %u "
                 "errno %d",
                 ret, create.handle, errno);
      if (ret == 0) {
         struct drm_radeon_gem_busy busy = {
            .handle = create.handle,
            .domain = UINT32_MAX,
         };
         ret = ioctl(aliases[1], DRM_IOCTL_RADEON_GEM_BUSY, &busy);
         TEST_CHECK(ret == 0 &&
                       busy.domain == RADEON_GEM_DOMAIN_GTT,
                    "first-discovery aliases split handles: %d "
                    "domain 0x%x errno %d",
                    ret, busy.domain, errno);
         struct drm_gem_close close_bo = {
            .handle = create.handle,
         };
         ret =
            ioctl(aliases[0], DRM_IOCTL_GEM_CLOSE, &close_bo);
         TEST_CHECK(ret == 0,
                    "first-discovery GEM close returned %d errno %d",
                    ret, errno);
      }
   }

cleanup_pipes:
   if (start_result == 0) {
      close(start_pipe[0]);
      close(start_pipe[1]);
   }
   if (ready_result == 0) {
      close(ready_pipe[0]);
      close(ready_pipe[1]);
   }
   if (release_result == 0) {
      close(release_pipe[0]);
      close(release_pipe[1]);
   }

cleanup_aliases:
   if (aliases[0] >= 0)
      close(aliases[0]);
   if (aliases[1] >= 0)
      close(aliases[1]);
}

static void
test_close_range_unshared_fd_table(int fd)
{
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle != 0,
              "UNSHARE GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   if (ret != 0)
      return;

   int ready_pipe[2];
   int release_pipe[2];
   int ready_result = pipe2(ready_pipe, O_CLOEXEC);
   int release_result = pipe2(release_pipe, O_CLOEXEC);
   TEST_CHECK(ready_result == 0 && release_result == 0,
              "UNSHARE pipes failed with errno %d", errno);
   if (ready_result == 0 && release_result == 0) {
      struct test_unshared_fd_context context = {
         .shared_fd = fd,
         .ready_fd = ready_pipe[1],
         .release_fd = release_pipe[0],
         .worker_domain = RADEON_GEM_DOMAIN_VRAM,
         .direct_unshare = false,
         .worker_tid = -1,
         .worker_render_fd = -1,
         .worker_identity_fd = -1,
         .result = 1,
      };
      pthread_t worker;
      int thread_result =
         pthread_create(&worker, NULL, test_unshared_fd_table_worker,
                        &context);
      TEST_CHECK(thread_result == 0,
                 "UNSHARE pthread_create returned %d", thread_result);
      if (thread_result == 0) {
         char ready;
         ssize_t length = read(ready_pipe[0], &ready, sizeof(ready));
         TEST_CHECK(length == sizeof(ready),
                    "UNSHARE worker readiness returned %zd", length);
         if (length == sizeof(ready)) {
            struct drm_radeon_gem_busy busy = {
               .handle = create.handle,
               .domain = UINT32_MAX,
            };
            ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
            TEST_CHECK(ret == 0 &&
                          busy.domain == RADEON_GEM_DOMAIN_GTT,
                       "UNSHARE worker replaced leader handles: %d "
                       "domain 0x%x",
                       ret, busy.domain);

            TEST_CHECK(context.worker_tid > 0 &&
                          context.worker_render_fd >= 0 &&
                          context.worker_identity_fd >= 0,
                       "UNSHARE worker identity is tid %ld render %d "
                       "identity %d",
                       (long)context.worker_tid,
                       context.worker_render_fd,
                       context.worker_identity_fd);
            if (context.worker_tid > 0 &&
                context.worker_render_fd >= 0) {
               char worker_proc_path[128];
               snprintf(worker_proc_path, sizeof(worker_proc_path),
                        "/proc/%ld/fd/%d",
                        (long)context.worker_tid,
                        context.worker_render_fd);
               test_proc_fd_readlink(worker_proc_path, true);
            }

            if (context.worker_identity_fd >= 0) {
               int unrelated_fd =
                  open("/dev/null", O_RDONLY | O_CLOEXEC);
               TEST_CHECK(unrelated_fd >= 0,
                          "UNSHARE collision source open failed with "
                          "errno %d",
                          errno);
               if (unrelated_fd >= 0) {
                  int collision_fd = context.worker_identity_fd;
                  if (unrelated_fd != collision_fd) {
                     int duplicate_result =
                        syscall(SYS_dup3, unrelated_fd, collision_fd,
                                O_CLOEXEC);
                     TEST_CHECK(duplicate_result == collision_fd,
                                "UNSHARE collision dup3 returned %d "
                                "errno %d",
                                duplicate_result, errno);
                  }
                  int close_result = close(collision_fd);
                  TEST_CHECK(close_result == 0,
                             "UNSHARE collision close returned %d "
                             "errno %d",
                             close_result, errno);
                  if (unrelated_fd != collision_fd)
                     close(unrelated_fd);
               }

               unrelated_fd =
                  open("/dev/null", O_RDONLY | O_CLOEXEC);
               if (unrelated_fd >= 0) {
                  int collision_fd = context.worker_identity_fd;
                  if (unrelated_fd != collision_fd) {
                     int duplicate_result =
                        syscall(SYS_dup3, unrelated_fd, collision_fd,
                                O_CLOEXEC);
                     TEST_CHECK(duplicate_result == collision_fd,
                                "UNSHARE close_range collision dup3 "
                                "returned %d errno %d",
                                duplicate_result, errno);
                  }
                  int close_result =
                     close_range((unsigned)collision_fd,
                                 (unsigned)collision_fd, 0);
                  int status_result =
                     syscall(SYS_fcntl, collision_fd, F_GETFD);
                  TEST_CHECK(close_result == 0 &&
                                status_result == -1 &&
                                errno == EBADF,
                             "UNSHARE collision close_range returned %d "
                             "status %d errno %d",
                             close_result, status_result, errno);
                  if (unrelated_fd != collision_fd)
                     close(unrelated_fd);
               }
            }
         }
         char release = 1;
         ssize_t written =
            write(release_pipe[1], &release, sizeof(release));
         TEST_CHECK(written == sizeof(release),
                    "UNSHARE worker release returned %zd", written);
         thread_result = pthread_join(worker, NULL);
         TEST_CHECK(thread_result == 0 && context.result == 0,
                    "UNSHARE worker completed with join %d result %d",
                    thread_result, context.result);
      }
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      close(release_pipe[0]);
      close(release_pipe[1]);
   } else {
      if (ready_result == 0) {
         close(ready_pipe[0]);
         close(ready_pipe[1]);
      }
      if (release_result == 0) {
         close(release_pipe[0]);
         close(release_pipe[1]);
      }
   }

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0,
              "UNSHARE leader GEM close returned %d errno %d", ret,
              errno);

   int backing_baseline = drm_shim_test_live_bo_backing_files();
   for (unsigned iteration = 0; iteration < 64; iteration++) {
      int abandoned_fd =
         open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      TEST_CHECK(abandoned_fd >= 0,
                 "UNSHARE lifetime open %u failed with errno %d",
                 iteration, errno);
      if (abandoned_fd < 0)
         break;
      struct drm_radeon_gem_create abandoned_create = {
         .size = 1024 * 1024,
         .alignment = 4096,
         .initial_domain = RADEON_GEM_DOMAIN_GTT,
      };
      ret =
         ioctl(abandoned_fd, DRM_IOCTL_RADEON_GEM_CREATE,
               &abandoned_create);
      struct drm_radeon_gem_mmap abandoned_mmap = {
         .handle = abandoned_create.handle,
      };
      if (ret == 0)
         ret =
            ioctl(abandoned_fd, DRM_IOCTL_RADEON_GEM_MMAP,
                  &abandoned_mmap);
      TEST_CHECK(ret == 0 && abandoned_mmap.addr_ptr,
                 "UNSHARE lifetime setup %u returned %d offset 0x%llx "
                 "errno %d",
                 iteration, ret,
                 (unsigned long long)abandoned_mmap.addr_ptr, errno);
      ret = close(abandoned_fd);
      TEST_CHECK(ret == 0 &&
                    drm_shim_test_live_bo_backing_files() ==
                       backing_baseline,
                 "UNSHARE lifetime close %u returned %d live %d "
                 "baseline %d errno %d",
                 iteration, ret,
                 drm_shim_test_live_bo_backing_files(),
                 backing_baseline, errno);
      if (ret != 0 ||
          drm_shim_test_live_bo_backing_files() != backing_baseline)
         break;
   }
}

static void
test_direct_unshared_fd_table(int fd)
{
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle,
              "direct UNSHARE GEM create returned %d handle %u errno %d",
              ret, create.handle, errno);
   if (ret != 0)
      return;

   int ready_pipe[2];
   int release_pipe[2];
   int ready_result = pipe2(ready_pipe, O_CLOEXEC);
   int release_result = pipe2(release_pipe, O_CLOEXEC);
   TEST_CHECK(ready_result == 0 && release_result == 0,
              "direct UNSHARE pipes failed with errno %d", errno);
   if (ready_result == 0 && release_result == 0) {
      struct test_unshared_fd_context context = {
         .shared_fd = fd,
         .ready_fd = ready_pipe[1],
         .release_fd = release_pipe[0],
         .worker_domain = RADEON_GEM_DOMAIN_VRAM,
         .direct_unshare = true,
         .worker_tid = -1,
         .worker_render_fd = -1,
         .worker_identity_fd = -1,
         .result = 1,
      };
      pthread_t worker;
      int thread_result =
         pthread_create(&worker, NULL, test_unshared_fd_table_worker,
                        &context);
      TEST_CHECK(thread_result == 0,
                 "direct UNSHARE pthread_create returned %d",
                 thread_result);
      if (thread_result == 0) {
         char ready = 0;
         ssize_t length;
         do {
            length = read(ready_pipe[0], &ready, sizeof(ready));
         } while (length < 0 && errno == EINTR);
         TEST_CHECK(length == sizeof(ready) && ready,
                    "direct UNSHARE worker readiness returned %zd/%d",
                    length, ready);
         if (length == sizeof(ready) && ready) {
            struct drm_radeon_gem_busy busy = {
               .handle = create.handle,
               .domain = UINT32_MAX,
            };
            ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
            TEST_CHECK(ret == 0 &&
                          busy.domain == RADEON_GEM_DOMAIN_GTT,
                       "direct UNSHARE worker replaced leader state: "
                       "%d domain 0x%x errno %d",
                       ret, busy.domain, errno);
         }

         char release = 1;
         ssize_t written;
         do {
            written =
               write(release_pipe[1], &release, sizeof(release));
         } while (written < 0 && errno == EINTR);
         TEST_CHECK(written == sizeof(release),
                    "direct UNSHARE worker release returned %zd",
                    written);
         thread_result = pthread_join(worker, NULL);
         TEST_CHECK(thread_result == 0 && context.result == 0,
                    "direct UNSHARE worker completed with join %d "
                    "result %d",
                    thread_result, context.result);
      }
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      close(release_pipe[0]);
      close(release_pipe[1]);
   } else {
      if (ready_result == 0) {
         close(ready_pipe[0]);
         close(ready_pipe[1]);
      }
      if (release_result == 0) {
         close(release_pipe[0]);
         close(release_pipe[1]);
      }
   }

   struct drm_radeon_gem_busy busy = {
      .handle = create.handle,
      .domain = UINT32_MAX,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
   TEST_CHECK(ret == 0 && busy.domain == RADEON_GEM_DOMAIN_GTT,
              "direct UNSHARE leader recovery returned %d domain 0x%x "
              "errno %d",
              ret, busy.domain, errno);

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0,
              "direct UNSHARE leader GEM close returned %d errno %d",
              ret, errno);
}

static int
test_reopen_descriptor(int fd, int flags)
{
   char path[64];
   int length =
      snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
   if (length <= 0 || length >= (int)sizeof(path)) {
      errno = ENAMETOOLONG;
      return -1;
   }
   return open(path, flags | O_CLOEXEC);
}

static void
test_getlk_access_contract(int owner_fd, const char *label)
{
   int readonly_fd = test_reopen_descriptor(owner_fd, O_RDONLY);
   int writeonly_fd = test_reopen_descriptor(owner_fd, O_WRONLY);
   TEST_CHECK(readonly_fd >= 0 && writeonly_fd >= 0,
              "%s GETLK access-mode reopens are %d and %d errno %d",
              label, readonly_fd, writeonly_fd, errno);
   if (readonly_fd < 0 || writeonly_fd < 0)
      goto cleanup;

   struct flock owner_lock = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = 3072,
      .l_len = 4,
   };
   int ret = fcntl(owner_fd, F_OFD_SETLK, &owner_lock);
   TEST_CHECK(ret == 0,
              "%s GETLK owner lock returned %d errno %d",
              label, ret, errno);
   if (ret != 0)
      goto cleanup;

   struct flock readonly_query = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = owner_lock.l_start,
      .l_len = owner_lock.l_len,
   };
   ret = fcntl(readonly_fd, F_GETLK, &readonly_query);
   TEST_CHECK(ret == 0 && readonly_query.l_type == F_WRLCK,
              "%s read-only GETLK for a write lock returned %d "
              "type %d errno %d",
              label, ret, readonly_query.l_type, errno);

   struct flock writeonly_query = {
      .l_type = F_RDLCK,
      .l_whence = SEEK_SET,
      .l_start = owner_lock.l_start,
      .l_len = owner_lock.l_len,
   };
   ret = fcntl(writeonly_fd, F_GETLK, &writeonly_query);
   TEST_CHECK(ret == 0 && writeonly_query.l_type == F_WRLCK,
              "%s write-only GETLK for a read lock returned %d "
              "type %d errno %d",
              label, ret, writeonly_query.l_type, errno);

   owner_lock.l_type = F_UNLCK;
   ret = fcntl(owner_fd, F_OFD_SETLK, &owner_lock);
   TEST_CHECK(ret == 0,
              "%s GETLK owner unlock returned %d errno %d",
              label, ret, errno);

cleanup:
   if (readonly_fd >= 0)
      close(readonly_fd);
   if (writeonly_fd >= 0)
      close(writeonly_fd);
}

static void
test_expect_ofd_lock(int fd, off_t start, off_t length,
                     short expected_type, const char *label,
                     const char *position_label)
{
   struct flock query = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = start,
      .l_len = length,
   };
   int ret = fcntl(fd, F_OFD_GETLK, &query);
   TEST_CHECK(ret == 0 && query.l_type == expected_type,
              "%s OFD query at %s returned %d type %d expected %d "
              "errno %d",
              label, position_label, ret, query.l_type,
              expected_type, errno);
}

static void
test_ofd_description_contract(int owner_fd, const char *label)
{
   int duplicate_fd = dup(owner_fd);
   int reopened_fd = test_reopen_descriptor(owner_fd, O_RDWR);
   TEST_CHECK(duplicate_fd >= 0 && reopened_fd >= 0,
              "%s OFD descriptions are duplicate %d reopen %d errno %d",
              label, duplicate_fd, reopened_fd, errno);
   if (duplicate_fd < 0 || reopened_fd < 0)
      goto cleanup;

   off64_t position = lseek64(owner_fd, 37, SEEK_SET);
   TEST_CHECK(position == 37,
              "%s SEEK_CUR setup returned %lld errno %d",
              label, (long long)position, errno);
   if (position != 37)
      goto cleanup;

   struct flock owner_lock = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_CUR,
      .l_start = 5,
      .l_len = 7,
   };
   int ret = fcntl(owner_fd, F_OFD_SETLK, &owner_lock);
   off64_t position_after_lock = lseek64(owner_fd, 0, SEEK_CUR);
   TEST_CHECK(ret == 0 && position_after_lock == 37,
              "%s SEEK_CUR OFD lock returned %d offset %lld errno %d",
              label, ret, (long long)position_after_lock, errno);
   if (ret != 0)
      goto cleanup;

   test_expect_ofd_lock(duplicate_fd, 42, 7, F_UNLCK, label,
                        "duplicate ownership");
   test_expect_ofd_lock(reopened_fd, 41, 1, F_UNLCK, label,
                        "byte before range");
   test_expect_ofd_lock(reopened_fd, 42, 1, F_WRLCK, label,
                        "first locked byte");
   test_expect_ofd_lock(reopened_fd, 48, 1, F_WRLCK, label,
                        "last locked byte");
   test_expect_ofd_lock(reopened_fd, 49, 1, F_UNLCK, label,
                        "byte after range");

   struct flock invalid_query = {
      .l_type = F_WRLCK,
      .l_whence = (short)-1,
      .l_start = 0,
      .l_len = 1,
   };
   errno = 0;
   ret = fcntl(reopened_fd, F_OFD_GETLK, &invalid_query);
   TEST_CHECK(ret == -1 && errno == EINVAL,
              "%s invalid lock whence returned %d errno %d",
              label, ret, errno);

   ret = close(duplicate_fd);
   duplicate_fd = -1;
   TEST_CHECK(ret == 0,
              "%s duplicate close returned %d errno %d",
              label, ret, errno);
   test_expect_ofd_lock(reopened_fd, 42, 1, F_WRLCK, label,
                        "surviving owner after alias close");

   struct flock unlock = {
      .l_type = F_UNLCK,
      .l_whence = SEEK_SET,
      .l_start = 42,
      .l_len = 7,
   };
   ret = fcntl(owner_fd, F_OFD_SETLK, &unlock);
   TEST_CHECK(ret == 0,
              "%s OFD unlock returned %d errno %d",
              label, ret, errno);
   test_expect_ofd_lock(reopened_fd, 42, 1, F_UNLCK, label,
                        "released range");

cleanup:
   if (duplicate_fd >= 0)
      close(duplicate_fd);
   if (reopened_fd >= 0)
      close(reopened_fd);
}

static bool
test_child_posix_lock_type(int fd, off_t start, short expected_type)
{
   pid_t child = fork();
   if (child < 0)
      return false;
   if (child == 0) {
      struct flock query = {
         .l_type = F_WRLCK,
         .l_whence = SEEK_SET,
         .l_start = start,
         .l_len = 1,
      };
      int ret = fcntl(fd, F_GETLK, &query);
      _exit(ret == 0 && query.l_type == expected_type ? 0 : 1);
   }

   int child_status;
   pid_t waited;
   do {
      waited = waitpid(child, &child_status, 0);
   } while (waited < 0 && errno == EINTR);
   return waited == child && WIFEXITED(child_status) &&
          WEXITSTATUS(child_status) == 0;
}

static void
test_posix_alias_close_contract(int owner_fd, const char *label)
{
   int duplicate_fd = dup(owner_fd);
   TEST_CHECK(duplicate_fd >= 0,
              "%s POSIX duplicate returned %d errno %d",
              label, duplicate_fd, errno);
   if (duplicate_fd < 0)
      return;

   struct flock owner_lock = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = 6144,
      .l_len = 1,
   };
   int ret = fcntl(owner_fd, F_SETLK, &owner_lock);
   TEST_CHECK(ret == 0,
              "%s POSIX owner lock returned %d errno %d",
              label, ret, errno);
   if (ret == 0) {
      TEST_CHECK(test_child_posix_lock_type(
                    owner_fd, owner_lock.l_start, F_WRLCK),
                 "%s child did not observe the parent POSIX lock",
                 label);
      ret = close(duplicate_fd);
      duplicate_fd = -1;
      TEST_CHECK(ret == 0,
                 "%s POSIX alias close returned %d errno %d",
                 label, ret, errno);
      TEST_CHECK(test_child_posix_lock_type(
                    owner_fd, owner_lock.l_start, F_UNLCK),
                 "%s POSIX lock survived an alias close", label);

      owner_lock.l_type = F_UNLCK;
      (void)fcntl(owner_fd, F_SETLK, &owner_lock);
   }

   if (duplicate_fd >= 0)
      close(duplicate_fd);
}

static void
test_blocking_ofd_contract(int owner_fd, const char *label)
{
   int waiter_fd = test_reopen_descriptor(owner_fd, O_RDWR);
   TEST_CHECK(waiter_fd >= 0,
              "%s blocking OFD reopen returned %d errno %d",
              label, waiter_fd, errno);
   if (waiter_fd < 0)
      return;

   struct flock owner_lock = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = 8192,
      .l_len = 1,
   };
   int ret = fcntl(owner_fd, F_OFD_SETLK, &owner_lock);
   TEST_CHECK(ret == 0,
              "%s blocking OFD owner lock returned %d errno %d",
              label, ret, errno);
   if (ret != 0) {
      close(waiter_fd);
      return;
   }

   int ready_pipe[2];
   int done_pipe[2];
   int ready_result = pipe2(ready_pipe, O_CLOEXEC);
   int done_result = pipe2(done_pipe, O_CLOEXEC);
   TEST_CHECK(ready_result == 0 && done_result == 0,
              "%s blocking OFD pipes failed with errno %d",
              label, errno);
   if (ready_result != 0 || done_result != 0)
      goto unlock_owner;

   struct test_blocking_lock_context context = {
      .fd = waiter_fd,
      .ready_fd = ready_pipe[1],
      .done_fd = done_pipe[1],
      .lock = {
         .l_type = F_WRLCK,
         .l_whence = SEEK_SET,
         .l_start = owner_lock.l_start,
         .l_len = owner_lock.l_len,
      },
      .result = -1,
      .error = 0,
   };
   pthread_t worker;
   int thread_result =
      pthread_create(&worker, NULL, test_blocking_lock_worker,
                     &context);
   TEST_CHECK(thread_result == 0,
              "%s blocking OFD pthread_create returned %d",
              label, thread_result);
   if (thread_result == 0) {
      char byte;
      ssize_t length;
      do {
         length = read(ready_pipe[0], &byte, sizeof(byte));
      } while (length < 0 && errno == EINTR);
      TEST_CHECK(length == sizeof(byte),
                 "%s blocking OFD readiness returned %zd",
                 label, length);

      struct pollfd completion = {
         .fd = done_pipe[0],
         .events = POLLIN,
      };
      int poll_result;
      do {
         poll_result = poll(&completion, 1, 100);
      } while (poll_result < 0 && errno == EINTR);
      TEST_CHECK(poll_result == 0,
                 "%s F_OFD_SETLKW completed before owner unlock: %d "
                 "events 0x%x",
                 label, poll_result, completion.revents);

      owner_lock.l_type = F_UNLCK;
      ret = fcntl(owner_fd, F_OFD_SETLK, &owner_lock);
      TEST_CHECK(ret == 0,
                 "%s blocking OFD owner unlock returned %d errno %d",
                 label, ret, errno);

      completion.revents = 0;
      do {
         poll_result = poll(&completion, 1, 5000);
      } while (poll_result < 0 && errno == EINTR);
      TEST_CHECK(poll_result == 1 &&
                    (completion.revents & POLLIN),
                 "%s F_OFD_SETLKW wake returned %d events 0x%x "
                 "errno %d",
                 label, poll_result, completion.revents, errno);
      if (poll_result == 1 && (completion.revents & POLLIN)) {
         do {
            length = read(done_pipe[0], &byte, sizeof(byte));
         } while (length < 0 && errno == EINTR);
         TEST_CHECK(length == sizeof(byte),
                    "%s blocking OFD completion returned %zd",
                    label, length);
      }
      thread_result = pthread_join(worker, NULL);
      TEST_CHECK(thread_result == 0 && context.result == 0,
                 "%s F_OFD_SETLKW joined %d result %d errno %d",
                 label, thread_result, context.result, context.error);

      if (context.result == 0) {
         context.lock.l_type = F_UNLCK;
         ret = fcntl(waiter_fd, F_OFD_SETLK, &context.lock);
         TEST_CHECK(ret == 0,
                    "%s blocking OFD waiter unlock returned %d errno %d",
                    label, ret, errno);
      }
   }

   close(ready_pipe[0]);
   close(ready_pipe[1]);
   close(done_pipe[0]);
   close(done_pipe[1]);
   close(waiter_fd);
   return;

unlock_owner:
   if (ready_result == 0) {
      close(ready_pipe[0]);
      close(ready_pipe[1]);
   }
   if (done_result == 0) {
      close(done_pipe[0]);
      close(done_pipe[1]);
   }
   owner_lock.l_type = F_UNLCK;
   (void)fcntl(owner_fd, F_OFD_SETLK, &owner_lock);
   close(waiter_fd);
}

static void
test_descriptor_lock_contracts(void)
{
   int control_fd =
      memfd_create("radeon-shim-lock-control",
                   MFD_CLOEXEC | MFD_ALLOW_SEALING);
   TEST_CHECK(control_fd >= 0,
              "regular lock control create failed with errno %d",
              errno);
   if (control_fd >= 0) {
      int ret = ftruncate(control_fd, 16384);
      TEST_CHECK(ret == 0,
                 "regular lock control resize returned %d errno %d",
                 ret, errno);
      if (ret == 0) {
         test_getlk_access_contract(control_fd, "regular control");
         test_ofd_description_contract(control_fd, "regular control");
         test_posix_alias_close_contract(control_fd, "regular control");
         test_blocking_ofd_contract(control_fd, "regular control");
      }
      close(control_fd);
   }

   int render_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(render_fd >= 0,
              "render lock contract open failed with errno %d", errno);
   if (render_fd >= 0) {
      test_getlk_access_contract(render_fd, "render");
      test_ofd_description_contract(render_fd, "render");
      test_posix_alias_close_contract(render_fd, "render");
      test_blocking_ofd_contract(render_fd, "render");
      close(render_fd);
   }
}

static void
test_fd_identity_exact_or_refuse(int fd, uint16_t expected_device_id)
{
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   int ret = ioctl(fd, DRM_IOCTL_RADEON_GEM_CREATE, &create);
   TEST_CHECK(ret == 0 && create.handle,
              "fd exact-identity GEM create returned %d handle %u "
              "errno %d",
              ret, create.handle, errno);
   if (!create.handle)
      return;

   static const int unavailable_errors[][2] = {
      {EINVAL, EPERM},
      {ENOSYS, ENOSYS},
   };
   for (size_t index = 0;
        index < ARRAY_SIZE(unavailable_errors); index++) {
      drm_shim_test_force_fd_identity_errors(
         unavailable_errors[index][0],
         unavailable_errors[index][1]);
      uint32_t device_id = UINT32_MAX;
      struct drm_radeon_info info = {
         .request = RADEON_INFO_DEVICE_ID,
         .value = (uintptr_t)&device_id,
      };
      errno = 0;
      ret = ioctl(fd, DRM_IOCTL_RADEON_INFO, &info);
      int ioctl_errno = errno;
      drm_shim_test_force_fd_identity_errors(0, 0);
      TEST_CHECK(ret == -1 && ioctl_errno == ENOTTY &&
                    device_id == UINT32_MAX,
                 "fd exact-identity refusal %zu returned %d errno %d "
                 "device 0x%x",
                 index, ret, ioctl_errno, device_id);
   }

   drm_shim_test_force_fd_identity_errors(EPERM, 0);
   drm_shim_test_force_kcmp_result(true, 0);
   uint32_t device_id = UINT32_MAX;
   struct drm_radeon_info info = {
      .request = RADEON_INFO_DEVICE_ID,
      .value = (uintptr_t)&device_id,
   };
   ret = ioctl(fd, DRM_IOCTL_RADEON_INFO, &info);
   int ioctl_errno = errno;
   drm_shim_test_force_kcmp_result(false, 0);
   drm_shim_test_force_fd_identity_errors(0, 0);
   TEST_CHECK(ret == 0 && device_id == expected_device_id,
              "fd exact-identity kcmp continuation returned %d errno %d "
              "device 0x%x",
              ret, ioctl_errno, device_id);

   int raw_alias = syscall(SYS_dup, fd);
   TEST_CHECK(raw_alias >= 0,
              "fd exact-identity raw duplicate failed with errno %d",
              errno);
   if (raw_alias >= 0) {
      drm_shim_test_force_fd_identity_errors(EINVAL, EPERM);
      struct drm_radeon_gem_busy busy = {
         .handle = create.handle,
         .domain = UINT32_MAX,
      };
      errno = 0;
      ret = ioctl(raw_alias, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
      ioctl_errno = errno;
      drm_shim_test_force_fd_identity_errors(0, 0);
      TEST_CHECK(ret == -1 && ioctl_errno == ENOTTY &&
                    busy.domain == UINT32_MAX,
                 "fd exact-identity raw duplicate refusal returned %d "
                 "errno %d domain 0x%x",
                 ret, ioctl_errno, busy.domain);

      drm_shim_test_force_fd_identity_errors(EPERM, 0);
      drm_shim_test_force_kcmp_result(true, 0);
      busy.domain = UINT32_MAX;
      ret = ioctl(raw_alias, DRM_IOCTL_RADEON_GEM_BUSY, &busy);
      ioctl_errno = errno;
      drm_shim_test_force_kcmp_result(false, 0);
      drm_shim_test_force_fd_identity_errors(0, 0);
      TEST_CHECK(ret == 0 && busy.domain == RADEON_GEM_DOMAIN_GTT,
                 "fd exact-identity kcmp continuation returned %d "
                 "errno %d domain 0x%x",
                 ret, ioctl_errno, busy.domain);
      close(raw_alias);
   }

   int stale_fd = syscall(SYS_dup, fd);
   TEST_CHECK(stale_fd >= 0,
              "fd exact-identity stale duplicate failed with errno %d",
              errno);
   if (stale_fd >= 0) {
      int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
      TEST_CHECK(null_fd >= 0,
                 "fd exact-identity /dev/null open failed with errno %d",
                 errno);
      if (null_fd >= 0) {
         ret = syscall(SYS_dup3, null_fd, stale_fd, O_CLOEXEC);
         TEST_CHECK(ret == stale_fd,
                    "fd exact-identity raw replacement returned %d "
                    "errno %d",
                    ret, errno);
         drm_shim_test_force_fd_identity_errors(EINVAL, EPERM);
         device_id = UINT32_MAX;
         errno = 0;
         ret = ioctl(stale_fd, DRM_IOCTL_RADEON_INFO, &info);
         ioctl_errno = errno;
         drm_shim_test_force_fd_identity_errors(0, 0);
         TEST_CHECK(ret == -1 && ioctl_errno == ENOTTY &&
                       device_id == UINT32_MAX,
                    "fd exact-identity stale mapping routed ioctl: %d "
                    "errno %d device 0x%x",
                    ret, ioctl_errno, device_id);
         close(null_fd);
      }
      close(stale_fd);
   }

   char reopen_path[64];
   int path_length =
      snprintf(reopen_path, sizeof(reopen_path),
               "/proc/thread-self/fd/%d", fd);
   TEST_CHECK(path_length > 0 && path_length < (int)sizeof(reopen_path),
              "fd exact-identity reopen path length is %d", path_length);
   int independent_fd =
      path_length > 0 && path_length < (int)sizeof(reopen_path)
         ? syscall(SYS_openat, AT_FDCWD, reopen_path,
                   O_RDWR | O_CLOEXEC, 0)
         : -1;
   TEST_CHECK(independent_fd >= 0,
              "fd exact-identity independent reopen failed with errno %d",
              errno);

   struct flock application_lock = {
      .l_type = F_WRLCK,
      .l_whence = SEEK_SET,
      .l_start = INT64_C(0x5000000000000000),
      .l_len = 1,
   };
   bool application_lock_set =
      syscall(SYS_fcntl, fd, F_OFD_SETLK, &application_lock) == 0;
   TEST_CHECK(application_lock_set,
              "fd exact-identity OFD lock setup failed with errno %d",
              errno);

   if (independent_fd >= 0) {
      struct stat source_status;
      struct stat independent_status;
      ret = syscall(SYS_fstat, fd, &source_status);
      int independent_stat_result =
         syscall(SYS_fstat, independent_fd, &independent_status);
      TEST_CHECK(ret == 0 && independent_stat_result == 0 &&
                    source_status.st_dev == independent_status.st_dev &&
                    source_status.st_ino == independent_status.st_ino,
                 "fd exact-identity metadata calibration returned %d/%d "
                 "device %llu/%llu inode %llu/%llu",
                 ret, independent_stat_result,
                 (unsigned long long)source_status.st_dev,
                 (unsigned long long)independent_status.st_dev,
                 (unsigned long long)source_status.st_ino,
                 (unsigned long long)independent_status.st_ino);

      struct flock lock_before = application_lock;
      int lock_query_result =
         syscall(SYS_fcntl, independent_fd, F_OFD_GETLK, &lock_before);
      TEST_CHECK(!application_lock_set ||
                    (lock_query_result == 0 &&
                     lock_before.l_type == F_WRLCK &&
                     lock_before.l_whence == SEEK_SET &&
                     lock_before.l_start == application_lock.l_start &&
                     lock_before.l_len == application_lock.l_len &&
                     lock_before.l_pid == -1),
                 "fd exact-identity separate-OFD calibration returned %d "
                 "type %d start %lld length %lld pid %d errno %d",
                 lock_query_result, lock_before.l_type,
                 (long long)lock_before.l_start,
                 (long long)lock_before.l_len, lock_before.l_pid, errno);

      drm_shim_test_force_fd_identity_errors(EINVAL, EPERM);
      struct drm_radeon_gem_busy independent_busy = {
         .handle = create.handle,
         .domain = UINT32_MAX,
      };
      errno = 0;
      ret =
         ioctl(independent_fd, DRM_IOCTL_RADEON_GEM_BUSY,
               &independent_busy);
      ioctl_errno = errno;
      drm_shim_test_force_fd_identity_errors(0, 0);
      TEST_CHECK(ret == -1 && ioctl_errno == ENOTTY &&
                    independent_busy.domain == UINT32_MAX,
                 "fd exact-identity independent reopen refusal returned "
                 "%d errno %d domain 0x%x",
                 ret, ioctl_errno, independent_busy.domain);

      struct flock lock_after = application_lock;
      lock_query_result =
         syscall(SYS_fcntl, independent_fd, F_OFD_GETLK, &lock_after);
      TEST_CHECK(!application_lock_set ||
                    (lock_query_result == 0 &&
                     lock_after.l_type == lock_before.l_type &&
                     lock_after.l_whence == lock_before.l_whence &&
                     lock_after.l_start == lock_before.l_start &&
                     lock_after.l_len == lock_before.l_len &&
                     lock_after.l_pid == lock_before.l_pid),
                 "fd exact-identity refusal changed OFD lock: %d type %d "
                 "start %lld length %lld pid %d errno %d",
                 lock_query_result, lock_after.l_type,
                 (long long)lock_after.l_start,
                 (long long)lock_after.l_len, lock_after.l_pid, errno);
      close(independent_fd);
   }

   if (application_lock_set) {
      application_lock.l_type = F_UNLCK;
      ret = syscall(SYS_fcntl, fd, F_OFD_SETLK, &application_lock);
      TEST_CHECK(ret == 0,
                 "fd exact-identity OFD lock cleanup returned %d errno %d",
                 ret, errno);
   }

   struct drm_gem_close close_bo = {
      .handle = create.handle,
   };
   ret = ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
   TEST_CHECK(ret == 0,
              "fd exact-identity GEM close returned %d errno %d",
              ret, errno);
}

static void
test_fd_identity_contract(int fd, uint16_t expected_device_id)
{
   int original_descriptor_flags = fcntl(fd, F_GETFD);
   TEST_CHECK(original_descriptor_flags >= 0,
              "descriptor flag query failed with errno %d", errno);
   int descriptor_result = ioctl(fd, FIOCLEX);
   int descriptor_flags = fcntl(fd, F_GETFD);
   TEST_CHECK(descriptor_result == 0 &&
                 descriptor_flags >= 0 &&
                 (descriptor_flags & FD_CLOEXEC),
              "FIOCLEX returned %d flags 0x%x errno %d",
              descriptor_result, descriptor_flags, errno);
   descriptor_result = ioctl(fd, FIONCLEX);
   descriptor_flags = fcntl(fd, F_GETFD);
   TEST_CHECK(descriptor_result == 0 &&
                 descriptor_flags >= 0 &&
                 !(descriptor_flags & FD_CLOEXEC),
              "FIONCLEX returned %d flags 0x%x errno %d",
              descriptor_result, descriptor_flags, errno);
   if (original_descriptor_flags & FD_CLOEXEC)
      (void)ioctl(fd, FIOCLEX);

   int ret;
   off64_t seek_result = lseek64(fd, 17, SEEK_SET);
   TEST_CHECK(seek_result == 17,
              "fd identity lseek returned %lld errno %d",
              (long long)seek_result, errno);
   char marker_byte = (char)0xa5;
   ssize_t read_length = read(fd, &marker_byte, sizeof(marker_byte));
   off64_t current_offset = lseek64(fd, 0, SEEK_CUR);
   TEST_CHECK(read_length == 0 && marker_byte == (char)0xa5 &&
                 current_offset == 17,
              "fd identity exposed payload: read %zd byte 0x%x "
              "offset %lld errno %d",
              read_length, (unsigned char)marker_byte,
              (long long)current_offset, errno);
   marker_byte = (char)0x5a;
   read_length = pread64(fd, &marker_byte, sizeof(marker_byte), 0);
   current_offset = lseek64(fd, 0, SEEK_CUR);
   TEST_CHECK(read_length == 0 && marker_byte == (char)0x5a &&
                 current_offset == 17,
              "fd identity exposed pread payload: read %zd byte 0x%x "
              "offset %lld errno %d",
              read_length, (unsigned char)marker_byte,
              (long long)current_offset, errno);

   struct flock unlock = {
      .l_type = F_UNLCK,
      .l_whence = SEEK_SET,
      .l_start = 0,
      .l_len = 0,
   };
   ret = fcntl(fd, F_OFD_SETLK, &unlock);
   TEST_CHECK(ret == 0,
              "fd identity application unlock returned %d errno %d",
              ret, errno);

   /* lockf and F_SETLK key a record lock on (process, device, inode), and
    * F_OFD_SETLK keys one on (open file description, device, inode); a
    * lock of either kind still conflicts with a lock of the other kind
    * over the same inode range, per fcntl(2).  A lockf64 call through
    * /dev/null therefore keyed this calibration to the one inode every
    * process on the host shares, and concurrent shim-test processes raced
    * that real lock: the loser's non-blocking acquire observed EAGAIN
    * instead of the exclusivity the calibration means to prove.
    * O_TMPFILE opens an unnamed inode private to this file description,
    * so no concurrent process can ever name it and no lock request can
    * ever contend with this one.
    */
   int null_fd = open("/tmp", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
   TEST_CHECK(null_fd >= 0,
              "lockf character calibration open failed with errno %d",
              errno);
   if (null_fd >= 0) {
      TEST_CHECK(lseek64(null_fd, 0, SEEK_SET) == 0,
                 "lockf character calibration lseek failed with errno %d",
                 errno);
      ret = lockf64(null_fd, F_TLOCK, 0);
      TEST_CHECK(ret == 0,
                 "lockf character calibration lock returned %d errno %d",
                 ret, errno);
      if (ret == 0) {
         ret = lockf64(null_fd, F_ULOCK, 0);
         TEST_CHECK(ret == 0,
                    "lockf character calibration unlock returned %d "
                    "errno %d",
                    ret, errno);
      }
      close(null_fd);
   }
   TEST_CHECK(lseek64(fd, 0, SEEK_SET) == 0,
              "lockf render lseek failed with errno %d", errno);
   ret = lockf64(fd, F_TLOCK, 0);
   TEST_CHECK(ret == 0,
              "lockf render lock returned %d errno %d", ret, errno);
   if (ret == 0) {
      ret = lockf64(fd, F_ULOCK, 0);
      TEST_CHECK(ret == 0,
                 "lockf render unlock returned %d errno %d",
                 ret, errno);
   }
   int readonly_render_fd =
      open("/dev/dri/renderD128", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(readonly_render_fd >= 0,
              "lockf read-only render open failed with errno %d", errno);
   if (readonly_render_fd >= 0) {
      errno = 0;
      ret = lockf64(readonly_render_fd, F_TLOCK, 0);
      TEST_CHECK(ret == -1 && errno == EBADF,
                 "lockf read-only render returned %d errno %d",
                 ret, errno);
      close(readonly_render_fd);
   }

   test_fd_identity_exact_or_refuse(fd, expected_device_id);

   int backing_baseline = drm_shim_test_live_bo_backing_files();
   int abandoned_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(abandoned_fd >= 0,
              "fd identity abandoned open failed with errno %d", errno);
   if (abandoned_fd >= 0) {
      struct drm_radeon_gem_create abandoned_create = {
         .size = 4096,
         .alignment = 4096,
         .initial_domain = RADEON_GEM_DOMAIN_GTT,
      };
      ret =
         ioctl(abandoned_fd, DRM_IOCTL_RADEON_GEM_CREATE,
               &abandoned_create);
      struct drm_radeon_gem_mmap abandoned_mmap = {
         .handle = abandoned_create.handle,
      };
      if (ret == 0)
         ret =
            ioctl(abandoned_fd, DRM_IOCTL_RADEON_GEM_MMAP,
                  &abandoned_mmap);
      TEST_CHECK(ret == 0 && abandoned_mmap.addr_ptr &&
                    drm_shim_test_live_bo_backing_files() ==
                       backing_baseline + 1,
                 "fd identity abandoned backing setup returned %d "
                 "offset 0x%llx live %d",
                 ret, (unsigned long long)abandoned_mmap.addr_ptr,
                 drm_shim_test_live_bo_backing_files());
      syscall(SYS_close, abandoned_fd);
      struct stat status;
      errno = 0;
      ret = fstat(abandoned_fd, &status);
      TEST_CHECK(ret == -1 && errno == EBADF &&
                    drm_shim_test_live_bo_backing_files() ==
                       backing_baseline,
                 "fd identity abandoned cleanup returned %d errno %d "
                 "live %d baseline %d",
                 ret, errno, drm_shim_test_live_bo_backing_files(),
                 backing_baseline);
   }
}

static bool
test_read_render_identity_name(int fd, char *name, size_t capacity)
{
   char path[64];
   int path_length =
      snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
   if (path_length <= 0 || path_length >= (int)sizeof(path))
      return false;

   char target[DRM_SHIM_RENDER_IDENTITY_CAPACITY + 32];
   ssize_t target_length =
      syscall(SYS_readlinkat, AT_FDCWD, path, target,
              sizeof(target));
   static const char prefix[] = "/memfd:";
   static const char suffix[] = " (deleted)";
   if (target_length <=
          (ssize_t)(sizeof(prefix) - 1 + sizeof(suffix) - 1) ||
       target_length > (ssize_t)sizeof(target) ||
       memcmp(target, prefix, sizeof(prefix) - 1) != 0 ||
       memcmp(target + target_length - (sizeof(suffix) - 1),
              suffix, sizeof(suffix) - 1) != 0)
      return false;

   size_t name_length =
      (size_t)target_length - (sizeof(prefix) - 1) -
      (sizeof(suffix) - 1);
   if (name_length + 1 > capacity)
      return false;
   memcpy(name, target + sizeof(prefix) - 1, name_length);
   name[name_length] = '\0';
   return true;
}

static int
test_create_sealed_identity(const char *name,
                            const void *marker, size_t marker_length)
{
   int fd =
      memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (fd < 0)
      return -1;
   if (fsetxattr(fd, DRM_SHIM_RENDER_MARKER_XATTR,
                 marker, marker_length, 0) < 0 ||
       fcntl(fd, F_ADD_SEALS,
             F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK |
                F_SEAL_SEAL) < 0) {
      int saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return -1;
   }
   return fd;
}

static void
test_expect_regular_identity(int fd, const char *label)
{
   TEST_CHECK(fd >= 0,
              "%s adversarial identity create failed with errno %d",
              label, errno);
   if (fd < 0)
      return;

   struct stat status;
   int ret = fstat(fd, &status);
   TEST_CHECK(ret == 0 && S_ISREG(status.st_mode),
              "%s adversarial identity fstat returned %d mode 0%o "
              "errno %d",
              label, ret, status.st_mode, errno);

   struct drm_version version = {0};
   errno = 0;
   ret = ioctl(fd, DRM_IOCTL_VERSION, &version);
   TEST_CHECK(ret == -1 && errno == ENOTTY,
              "%s adversarial identity ioctl returned %d errno %d",
              label, ret, errno);
   close(fd);
}

static void
test_sealed_identity_exactness(int render_fd)
{
   char identity_name[DRM_SHIM_RENDER_IDENTITY_CAPACITY];
   bool name_result =
      test_read_render_identity_name(
         render_fd, identity_name, sizeof(identity_name));
   TEST_CHECK(name_result,
              "sealed identity name discovery failed with errno %d",
              errno);
   if (!name_result)
      return;

   static const char suffix[] = "-suffix";
   char suffixed_name[DRM_SHIM_RENDER_IDENTITY_CAPACITY];
   int name_length =
      snprintf(suffixed_name, sizeof(suffixed_name), "%s%s",
               identity_name, suffix);
   TEST_CHECK(name_length > 0 &&
                 name_length < (int)sizeof(suffixed_name),
              "sealed identity suffix name length is %d", name_length);
   if (name_length > 0 &&
       name_length < (int)sizeof(suffixed_name)) {
      int fd =
         test_create_sealed_identity(
            suffixed_name, shim_device.render_marker,
            shim_device.render_marker_length);
      test_expect_regular_identity(fd, "suffixed name");
   }

   char suffixed_marker[DRM_SHIM_RENDER_MARKER_CAPACITY + sizeof(suffix)];
   TEST_CHECK(shim_device.render_marker_length + sizeof(suffix) - 1 <=
                 sizeof(suffixed_marker),
              "sealed identity suffix marker length is %zu",
              shim_device.render_marker_length + sizeof(suffix) - 1);
   if (shim_device.render_marker_length + sizeof(suffix) - 1 <=
       sizeof(suffixed_marker)) {
      memcpy(suffixed_marker, shim_device.render_marker,
             shim_device.render_marker_length);
      memcpy(suffixed_marker + shim_device.render_marker_length,
             suffix, sizeof(suffix) - 1);
      int fd =
         test_create_sealed_identity(
            identity_name, suffixed_marker,
            shim_device.render_marker_length + sizeof(suffix) - 1);
      test_expect_regular_identity(fd, "suffixed marker");
   }
}

static void
test_fcntl_lock_pointer_validation(int fd)
{
   pid_t child = fork();
   TEST_CHECK(child >= 0,
              "fcntl lock pointer fork failed with errno %d", errno);
   if (child == 0) {
      size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
      void *protected_page =
         mmap(NULL, page_size, PROT_NONE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (protected_page == MAP_FAILED)
         _exit(2);
      static const int commands[] = {
         F_GETLK,
         F_SETLK,
         F_SETLKW,
#ifdef F_OFD_GETLK
         F_OFD_GETLK,
#endif
#ifdef F_OFD_SETLK
         F_OFD_SETLK,
#endif
#ifdef F_OFD_SETLKW
         F_OFD_SETLKW,
#endif
      };
      for (size_t i = 0; i < ARRAY_SIZE(commands); i++) {
         errno = 0;
         int ret = fcntl(-1, commands[i], protected_page);
         if (ret != -1 || errno != EBADF)
            _exit(5);
         errno = 0;
         ret = fcntl(fd, commands[i], protected_page);
         if (ret != -1 || errno != EFAULT)
            _exit(3);
         errno = 0;
         ret = fcntl(fd, commands[i], (void *)(uintptr_t)1);
         if (ret != -1 || errno != EFAULT)
            _exit(4);
      }
      munmap(protected_page, page_size);
      _exit(0);
   }
   if (child > 0) {
      int status;
      pid_t waited;
      do {
         waited = waitpid(child, &status, 0);
      } while (waited < 0 && errno == EINTR);
      TEST_CHECK(waited == child && WIFEXITED(status) &&
                    WEXITSTATUS(status) == 0,
                 "fcntl lock pointer child status is 0x%x",
                 waited == child ? status : -1);
   }
}

static void
test_synthetic_filesystem(int fd, uint16_t expected_device_id)
{
   char char_path[128];
   char device_path[160];
   char vendor_path[192];
   char device_id_path[192];
   char modalias_path[192];
   snprintf(char_path, sizeof(char_path), "/sys/dev/char/%d:128",
            DRM_MAJOR);
   snprintf(device_path, sizeof(device_path), "%s/device", char_path);
   snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", device_path);
   snprintf(device_id_path, sizeof(device_id_path), "%s/device",
            device_path);
   snprintf(modalias_path, sizeof(modalias_path), "%s/modalias",
            device_path);

   int cookie_pipe[2];
   int cookie_pipe_result = pipe2(cookie_pipe, O_CLOEXEC);
   TEST_CHECK(cookie_pipe_result == 0,
              "cookie callback pipe failed with errno %d", errno);
   if (cookie_pipe_result == 0) {
      cookie_close_fd = cookie_pipe[1];
      cookie_io_functions_t cookie_functions = {
         .close = test_cookie_close,
      };
      FILE *cookie_stream =
         fopencookie(NULL, "w", cookie_functions);
      TEST_CHECK(cookie_stream,
                 "cookie stream creation failed with errno %d", errno);
      if (cookie_stream) {
         alarm(5);
         int cookie_result = fclose(cookie_stream);
         alarm(0);
         TEST_CHECK(cookie_result == 0 && cookie_close_fd == -1,
                    "cookie close callback returned %d state %d errno %d",
                    cookie_result, cookie_close_fd, errno);
      } else {
         close(cookie_pipe[1]);
         cookie_close_fd = -1;
      }
      close(cookie_pipe[0]);
   }

   FILE *render_stream = fopen("/dev/dri/renderD128", "r+");
   TEST_CHECK(render_stream,
              "render stream open failed with errno %d", errno);
   if (render_stream) {
      FILE *plain_stream = freopen("/dev/null", "r", render_stream);
      TEST_CHECK(plain_stream,
                 "render-to-plain freopen failed with errno %d", errno);
      if (plain_stream) {
         struct drm_version version = {0};
         errno = 0;
         int stream_ret =
            ioctl(fileno(plain_stream), DRM_IOCTL_VERSION, &version);
         TEST_CHECK(stream_ret == -1 && errno == ENOTTY,
                    "freopen retained stale shim dispatch: %d errno %d",
                    stream_ret, errno);
         fclose(plain_stream);
      }
   }

   FILE *plain_stream = fopen("/dev/null", "r");
   TEST_CHECK(plain_stream,
              "plain stream open failed with errno %d", errno);
   if (plain_stream) {
      render_stream =
         freopen("/dev/dri/renderD128", "r+", plain_stream);
      TEST_CHECK(render_stream,
                 "plain-to-render freopen failed with errno %d", errno);
      if (render_stream) {
         uint32_t stream_device_id = UINT32_MAX;
         struct drm_radeon_info info = {
            .request = RADEON_INFO_DEVICE_ID,
            .value = (uintptr_t)&stream_device_id,
         };
         int stream_ret =
            ioctl(fileno(render_stream), DRM_IOCTL_RADEON_INFO, &info);
         TEST_CHECK(stream_ret == 0 &&
                       stream_device_id == expected_device_id,
                    "freopen failed to install shim dispatch: %d value "
                    "0x%x errno %d",
                    stream_ret, stream_device_id, errno);
         fclose(render_stream);
      }
   }

   struct stat status;
   int ret = stat("/dev/dri/renderD128", &status);
   TEST_CHECK(ret == 0 && S_ISCHR(status.st_mode) &&
                 major(status.st_rdev) == DRM_MAJOR &&
                 minor(status.st_rdev) == 128,
              "render-node stat returned %d mode 0%o device %u:%u",
              ret, status.st_mode, major(status.st_rdev),
              minor(status.st_rdev));

   ret = lstat(char_path, &status);
   TEST_CHECK(ret == 0 && S_ISLNK(status.st_mode),
              "char-device alias lstat returned %d mode 0%o", ret,
              status.st_mode);
   ret = stat(char_path, &status);
   TEST_CHECK(ret == 0 && S_ISDIR(status.st_mode),
              "char-device alias stat returned %d mode 0%o", ret,
              status.st_mode);
   ret = lstat(device_path, &status);
   TEST_CHECK(ret == 0 && S_ISLNK(status.st_mode),
              "device alias lstat returned %d mode 0%o", ret,
              status.st_mode);
   ret = stat(vendor_path, &status);
   TEST_CHECK(ret == 0 && S_ISREG(status.st_mode) && status.st_size == 7,
              "vendor stat returned %d mode 0%o size %lld", ret,
              status.st_mode, (long long)status.st_size);

   struct statx statusx;
   memset(&statusx, 0, sizeof(statusx));
   ret = statx(AT_FDCWD, vendor_path, 0, STATX_BASIC_STATS, &statusx);
   TEST_CHECK(ret == 0 && S_ISREG(statusx.stx_mode) &&
                 statusx.stx_size == 7,
              "vendor statx returned %d mode 0%o size %llu", ret,
              statusx.stx_mode, (unsigned long long)statusx.stx_size);

   char expected_modalias[128];
   snprintf(expected_modalias, sizeof(expected_modalias),
            "pci:v00001002d%08Xsv00001234sd00001234bc03sc00i00\n",
            expected_device_id);
   test_text_equals(modalias_path, expected_modalias);

   static const char absolute_link_path[] =
      "/sys/devices/pci0000:00/0000:01:00.0/absolute-device";
   unsigned char absolute_link_target[128];
   memset(absolute_link_target, 0xa5, sizeof(absolute_link_target));
   ssize_t absolute_link_length =
      readlink(absolute_link_path, (char *)absolute_link_target,
               sizeof(absolute_link_target));
   static const char expected_absolute_link_target[] =
      "/sys/dev/char/226:128/device";
   TEST_CHECK(
      absolute_link_length ==
         (ssize_t)strlen(expected_absolute_link_target) &&
         memcmp(absolute_link_target, expected_absolute_link_target,
                strlen(expected_absolute_link_target)) == 0,
      "absolute override readlink returned %zd bytes",
      absolute_link_length);
   char absolute_link_vendor[256];
   snprintf(absolute_link_vendor, sizeof(absolute_link_vendor),
            "%s/vendor", absolute_link_path);
   test_text_equals(absolute_link_vendor, "0x1002\n");

   const char *synthetic_root = drm_shim_test_synthetic_root_path();
   int internal_root_fd;
   int internal_lease_fd;
   drm_shim_test_internal_fds(&internal_root_fd, &internal_lease_fd);
   TEST_CHECK(internal_root_fd >= 0 && internal_lease_fd >= 0 &&
                 internal_root_fd != internal_lease_fd,
              "internal descriptors are root %d lease %d",
              internal_root_fd, internal_lease_fd);
   int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(null_fd >= 0,
              "internal descriptor test open failed with errno %d", errno);
   if (null_fd >= 0) {
      int internal_fds[] = {internal_root_fd, internal_lease_fd};
      for (size_t i = 0; i < ARRAY_SIZE(internal_fds); i++) {
         int internal_fd = internal_fds[i];
         errno = 0;
         ret = close(internal_fd);
         TEST_CHECK(ret == -1 && errno == EBUSY,
                    "internal close %d returned %d errno %d",
                    internal_fd, ret, errno);
         errno = 0;
         ret = dup(internal_fd);
         TEST_CHECK(ret == -1 && errno == EBUSY,
                    "internal dup %d returned %d errno %d",
                    internal_fd, ret, errno);
         errno = 0;
         ret = fcntl(internal_fd, F_GETFD);
         TEST_CHECK(ret == -1 && errno == EBUSY,
                    "internal fcntl %d returned %d errno %d",
                    internal_fd, ret, errno);
         errno = 0;
         ret = dup2(null_fd, internal_fd);
         TEST_CHECK(ret == -1 && errno == EBUSY,
                    "internal dup2 %d returned %d errno %d",
                    internal_fd, ret, errno);
         errno = 0;
         ret = dup3(null_fd, internal_fd, O_CLOEXEC);
         TEST_CHECK(ret == -1 && errno == EBUSY,
                    "internal dup3 %d returned %d errno %d",
                    internal_fd, ret, errno);
         ret = close_range((unsigned)internal_fd,
                           (unsigned)internal_fd, 0);
         TEST_CHECK(ret == 0 &&
                       syscall(SYS_fcntl, internal_fd, F_GETFD) >= 0,
                    "internal close_range %d returned %d errno %d",
                    internal_fd, ret, errno);
         errno = 0;
         DIR *internal_directory = fdopendir(internal_fd);
         TEST_CHECK(!internal_directory && errno == EBUSY,
                    "internal fdopendir %d returned %p errno %d",
                    internal_fd, (void *)internal_directory, errno);
         if (internal_directory)
            closedir(internal_directory);
         TEST_CHECK(syscall(SYS_fcntl, internal_fd, F_GETFD) >= 0,
                    "internal descriptor %d did not survive fdopendir",
                    internal_fd);
      }
      close(null_fd);
   }

   struct stat mirror_status;
   ret = stat(synthetic_root, &mirror_status);
   TEST_CHECK(ret == 0 &&
                 (mirror_status.st_mode & 0777) == 0500,
              "synthetic root mode is 0%o after result %d",
              mirror_status.st_mode & 0777, ret);
   char physical_device_path[PATH_MAX];
   snprintf(physical_device_path, sizeof(physical_device_path), "%s%s",
            synthetic_root,
            "/sys/devices/pci0000:00/0000:01:00.0");
   ret = stat(physical_device_path, &mirror_status);
   TEST_CHECK(ret == 0 && S_ISDIR(mirror_status.st_mode) &&
                 (mirror_status.st_mode & 0777) == 0500,
              "synthetic device directory mode is 0%o after result %d",
              mirror_status.st_mode & 0777, ret);

   char physical_absolute_link[PATH_MAX];
   snprintf(physical_absolute_link, sizeof(physical_absolute_link), "%s%s",
            synthetic_root, absolute_link_path);
   unsigned char physical_link_target[PATH_MAX];
   ssize_t physical_link_length =
      syscall(SYS_readlinkat, AT_FDCWD, physical_absolute_link,
              physical_link_target, sizeof(physical_link_target));
   TEST_CHECK(physical_link_length > 0 &&
                 physical_link_target[0] != '/',
              "physical absolute override target escaped with length %zd",
              physical_link_length);

   const char *unknown_create_path =
      "/sys/devices/pci0000:00/0000:01:00.0/shim-created";
   errno = 0;
   int unknown_fd =
      open(unknown_create_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
   TEST_CHECK(unknown_fd == -1 && errno == ENOENT,
              "sealed synthetic create returned %d errno %d", unknown_fd,
              errno);
   if (unknown_fd >= 0)
      close(unknown_fd);
   char physical_unknown_path[PATH_MAX];
   snprintf(physical_unknown_path, sizeof(physical_unknown_path), "%s%s",
            synthetic_root, unknown_create_path);
   errno = 0;
   ret = syscall(SYS_newfstatat, AT_FDCWD, physical_unknown_path,
                 &mirror_status, AT_SYMLINK_NOFOLLOW);
   TEST_CHECK(ret == -1 && errno == ENOENT,
              "sealed synthetic create left physical residue: %d errno %d",
              ret, errno);

   errno = 0;
   ret = stat(
      "/sys/devices/pci0000:00/0000:01:00.0/host-only-sentinel",
      &status);
   TEST_CHECK(ret == -1 && errno == ENOENT,
              "private PCI subtree exposed an unknown child with result %d "
              "errno %d",
              ret, errno);
   errno = 0;
   ret = stat("/dev/dri/../null", &status);
   TEST_CHECK(ret == -1 && errno == ENOENT,
              "private DRI namespace escaped to /dev/null with result %d "
              "errno %d",
              ret, errno);
   errno = 0;
   int escape_fd =
      open("/dev/dri/../../../../etc/passwd", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(escape_fd == -1 && errno == ENOENT,
              "private DRI namespace escaped to /etc/passwd with result %d "
              "errno %d",
              escape_fd, errno);
   if (escape_fd >= 0)
      close(escape_fd);
   errno = 0;
   escape_fd = open(
      "/sys/dev/char/226:128/../../../../../../etc/passwd",
      O_RDONLY | O_CLOEXEC);
   TEST_CHECK(escape_fd == -1 && errno == ENOENT,
              "synthetic sysfs symlink escaped to /etc/passwd with result "
              "%d errno %d",
              escape_fd, errno);
   if (escape_fd >= 0)
      close(escape_fd);

   drm_shim_test_force_openat2_resolver_enosys(true);
   int fallback_fd =
      open("/dev/dri/renderD128", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(fallback_fd >= 0,
              "bounded resolver fallback render open failed with errno %d",
              errno);
   if (fallback_fd >= 0)
      close(fallback_fd);
   test_text_equals(vendor_path, "0x1002\n");
   errno = 0;
   fallback_fd =
      open("/dev/dri/../../../../etc/passwd", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(fallback_fd == -1 && errno == ENOENT,
              "bounded resolver fallback escaped its root: %d errno %d",
              fallback_fd, errno);
   if (fallback_fd >= 0)
      close(fallback_fd);
   drm_shim_test_force_openat2_resolver_enosys(false);

   drm_shim_test_force_process_vm_readv_error(ENOSYS);
   fallback_fd =
      open("/dev/dri/renderD128", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(fallback_fd >= 0,
              "path-copy fallback render open failed with errno %d",
              errno);
   if (fallback_fd >= 0)
      close(fallback_fd);
   test_text_equals(vendor_path, "0x1002\n");
   errno = 0;
   ret = stat((const char *)(uintptr_t)1, &status);
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "path-copy fallback invalid stat returned %d errno %d",
              ret, errno);
   drm_shim_test_force_process_vm_readv_error(0);

   drm_shim_test_force_process_vm_readv_error(EPERM);
   drm_shim_test_force_proc_mem_error(ENOENT);
   fallback_fd = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
   TEST_CHECK(fallback_fd >= 0,
              "direct path-copy fallback open failed with errno %d",
              errno);
   if (fallback_fd >= 0)
      close(fallback_fd);
   ret = stat(vendor_path, &status);
   TEST_CHECK(ret == 0 && S_ISREG(status.st_mode),
              "direct path-copy fallback stat returned %d errno %d",
              ret, errno);
   errno = 0;
   ret = stat((const char *)(uintptr_t)1, &status);
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "pipe path-copy fallback invalid stat returned %d errno %d",
              ret, errno);
   drm_shim_test_force_proc_mem_error(0);
   drm_shim_test_force_process_vm_readv_error(0);

   char regular_parent_path[224];
   snprintf(regular_parent_path, sizeof(regular_parent_path), "%s/..",
            vendor_path);
   errno = 0;
   ret = open(regular_parent_path, O_RDONLY);
   TEST_CHECK(ret == -1 && errno == ENOTDIR,
              "regular-file parent traversal returned %d errno %d",
              ret, errno);
   if (ret >= 0)
      close(ret);

   char symlink_parent_path[256];
   snprintf(symlink_parent_path, sizeof(symlink_parent_path),
            "%s/../renderD128/device/vendor", char_path);
   test_text_equals(symlink_parent_path, "0x1002\n");

   errno = 0;
   int link_fd = open(char_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   TEST_CHECK(link_fd == -1 && errno == ELOOP,
              "O_NOFOLLOW symlink open returned %d errno %d",
              link_fd, errno);
   if (link_fd >= 0)
      close(link_fd);

   link_fd = open(char_path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
   TEST_CHECK(link_fd >= 0,
              "O_PATH symlink open failed with errno %d", errno);
   if (link_fd >= 0) {
      ret = fstat(link_fd, &status);
      TEST_CHECK(ret == 0 && S_ISLNK(status.st_mode),
                 "O_PATH symlink fstat returned %d mode 0%o", ret,
                 status.st_mode);
      close(link_fd);
   }

   errno = 0;
   int regular_fd = open(vendor_path, O_RDONLY | O_DIRECTORY);
   TEST_CHECK(regular_fd == -1 && errno == ENOTDIR,
              "O_DIRECTORY regular-file open returned %d errno %d",
              regular_fd, errno);
   if (regular_fd >= 0)
      close(regular_fd);

   regular_fd = open(vendor_path, O_RDONLY | O_CLOEXEC);
   TEST_CHECK(regular_fd >= 0,
              "vendor open failed with errno %d", errno);
   if (regular_fd >= 0) {
      int status_flags = fcntl(regular_fd, F_GETFL);
      int descriptor_flags = fcntl(regular_fd, F_GETFD);
      TEST_CHECK(status_flags >= 0 &&
                    (status_flags & O_ACCMODE) == O_RDONLY,
                 "vendor F_GETFL returned 0x%x", status_flags);
      TEST_CHECK(descriptor_flags >= 0 &&
                    (descriptor_flags & FD_CLOEXEC),
                 "vendor F_GETFD returned 0x%x", descriptor_flags);
      close(regular_fd);
   }

   char link_buffer[256];
   errno = 0;
   ssize_t link_length = readlink(char_path, link_buffer, 0);
   TEST_CHECK(link_length == -1 && errno == EINVAL,
              "zero-size readlink returned %zd errno %d",
              link_length, errno);
   errno = 0;
   link_length =
      readlink(char_path, (char *)(uintptr_t)1, 1);
   TEST_CHECK(link_length == -1 && errno == EFAULT,
              "invalid-buffer readlink returned %zd errno %d",
              link_length, errno);

   const char *invalid_path = (const char *)(uintptr_t)1;
   errno = 0;
   ret = open(invalid_path, O_RDONLY);
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "invalid-path open returned %d errno %d", ret, errno);
   errno = 0;
   ret = stat(invalid_path, &status);
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "invalid-path stat returned %d errno %d", ret, errno);
   errno = 0;
   ret = fstatat(AT_FDCWD, invalid_path, &status, 0);
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "invalid-path fstatat returned %d errno %d", ret, errno);
   errno = 0;
   ret = statx(AT_FDCWD, invalid_path, 0, STATX_BASIC_STATS, &statusx);
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "invalid-path statx returned %d errno %d", ret, errno);
   errno = 0;
   link_length = readlink(invalid_path, link_buffer, sizeof(link_buffer));
   TEST_CHECK(link_length == -1 && errno == EFAULT,
              "invalid-path readlink returned %zd errno %d",
              link_length, errno);
   errno = 0;
   FILE *invalid_file = fopen(invalid_path, "r");
   TEST_CHECK(!invalid_file && errno == EFAULT,
              "invalid-path fopen returned %p errno %d",
              (void *)invalid_file, errno);
   if (invalid_file)
      fclose(invalid_file);
   errno = 0;
   DIR *invalid_directory = opendir(invalid_path);
   TEST_CHECK(!invalid_directory && errno == EFAULT,
              "invalid-path opendir returned %p errno %d",
              (void *)invalid_directory, errno);
   if (invalid_directory)
      closedir(invalid_directory);

   char alias_path[256];
   snprintf(alias_path, sizeof(alias_path),
            "/proc/self/root%s", vendor_path);
   test_text_equals(alias_path, "0x1002\n");
   snprintf(alias_path, sizeof(alias_path),
            "/proc/thread-self/root%s", vendor_path);
   test_text_equals(alias_path, "0x1002\n");
   snprintf(alias_path, sizeof(alias_path), "/proc/%ld/root%s",
            (long)getpid(), vendor_path);
   test_text_equals(alias_path, "0x1002\n");
   snprintf(alias_path, sizeof(alias_path), "/proc/%ld/root%s",
            (long)syscall(SYS_gettid), vendor_path);
   test_text_equals(alias_path, "0x1002\n");

   int vendor_dirfd = open(vendor_path, O_RDONLY | O_CLOEXEC);
   TEST_CHECK(vendor_dirfd >= 0,
              "vendor dirfd calibration open failed with errno %d", errno);
   if (vendor_dirfd >= 0) {
      errno = 0;
      int relative_fd = openat(vendor_dirfd, "child", O_RDONLY);
      TEST_CHECK(relative_fd == -1 && errno == ENOTDIR,
                 "regular-file dirfd openat returned %d errno %d",
                 relative_fd, errno);
      if (relative_fd >= 0)
         close(relative_fd);
      close(vendor_dirfd);
   }

   int path_fd =
      open("/dev/dri/renderD128", O_PATH | O_CLOEXEC);
   TEST_CHECK(path_fd >= 0,
              "render O_PATH open failed with errno %d", errno);
   if (path_fd >= 0) {
      int status_flags = fcntl(path_fd, F_GETFL);
      int descriptor_flags = fcntl(path_fd, F_GETFD);
      TEST_CHECK(status_flags >= 0 &&
                    (status_flags & O_PATH) == O_PATH,
                 "render O_PATH F_GETFL returned 0x%x", status_flags);
      TEST_CHECK(descriptor_flags >= 0 &&
                    (descriptor_flags & FD_CLOEXEC),
                 "render O_PATH F_GETFD returned 0x%x", descriptor_flags);
      ret = fstat(path_fd, &status);
      TEST_CHECK(ret == 0 && S_ISCHR(status.st_mode) &&
                    major(status.st_rdev) == DRM_MAJOR &&
                    minor(status.st_rdev) == 128,
                 "render O_PATH fstat returned %d mode 0%o device %u:%u",
                 ret, status.st_mode, major(status.st_rdev),
                 minor(status.st_rdev));
      ret = fstatat(path_fd, "", &status, AT_EMPTY_PATH);
      TEST_CHECK(ret == 0 && S_ISCHR(status.st_mode),
                 "render AT_EMPTY_PATH fstatat returned %d mode 0%o",
                 ret, status.st_mode);
      int (*fstatat_call)(int, const char *, struct stat *, int) = fstatat;
      ret = fstatat_call(path_fd, NULL, &status, AT_EMPTY_PATH);
      TEST_CHECK(ret == 0 && S_ISCHR(status.st_mode),
                 "render NULL AT_EMPTY_PATH fstatat returned %d errno %d",
                 ret, errno);
      memset(&statusx, 0, sizeof(statusx));
      ret = statx(path_fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &statusx);
      TEST_CHECK(ret == 0 && S_ISCHR(statusx.stx_mode) &&
                    statusx.stx_rdev_major == DRM_MAJOR &&
                    statusx.stx_rdev_minor == 128,
                 "render AT_EMPTY_PATH statx returned %d mode 0%o "
                 "device %u:%u",
                 ret, statusx.stx_mode, statusx.stx_rdev_major,
                 statusx.stx_rdev_minor);

      drm_shim_test_force_statx_symbol_absent(true);
      memset(&statusx, 0, sizeof(statusx));
      ret = statx(path_fd, "", AT_EMPTY_PATH, STATX_BASIC_STATS, &statusx);
      TEST_CHECK(ret == 0 && S_ISCHR(statusx.stx_mode) &&
                    statusx.stx_rdev_major == DRM_MAJOR &&
                    statusx.stx_rdev_minor == 128,
                 "syscall-backed render statx returned %d mode 0%o "
                 "device %u:%u",
                 ret, statusx.stx_mode, statusx.stx_rdev_major,
                 statusx.stx_rdev_minor);
      drm_shim_test_force_statx_symbol_absent(false);

      uint32_t info_device_id = UINT32_MAX;
      struct drm_radeon_info info = {
         .request = RADEON_INFO_DEVICE_ID,
         .value = (uintptr_t)&info_device_id,
      };
      errno = 0;
      ret = ioctl(path_fd, DRM_IOCTL_RADEON_INFO, &info);
      TEST_CHECK(ret == -1 && errno == EBADF &&
                    info_device_id == UINT32_MAX,
                 "render O_PATH ioctl returned %d errno %d value 0x%x",
                 ret, errno, info_device_id);

      char proc_fd_path[64];
      snprintf(proc_fd_path, sizeof(proc_fd_path), "/proc/self/fd/%d",
               path_fd);
      memset(link_buffer, 0xa5, sizeof(link_buffer));
      link_length =
         readlink(proc_fd_path, link_buffer, sizeof(link_buffer));
      TEST_CHECK(link_length ==
                    (ssize_t)strlen("/dev/dri/renderD128") &&
                    memcmp(link_buffer, "/dev/dri/renderD128",
                           strlen("/dev/dri/renderD128")) == 0,
                 "render proc-fd readlink leaked %zd bytes", link_length);
      drm_shim_test_force_process_vm_writev_error(EPERM);
      memset(link_buffer, 0xa5, sizeof(link_buffer));
      link_length =
         readlink(proc_fd_path, link_buffer, sizeof(link_buffer));
      TEST_CHECK(link_length ==
                    (ssize_t)strlen("/dev/dri/renderD128") &&
                    memcmp(link_buffer, "/dev/dri/renderD128",
                           strlen("/dev/dri/renderD128")) == 0,
                 "proc-memory readlink fallback returned %zd bytes",
                 link_length);
      drm_shim_test_force_process_vm_writev_error(0);
      drm_shim_test_force_process_vm_writev_error(EPERM);
      drm_shim_test_force_proc_mem_error(ENOENT);
      memset(link_buffer, 0xa5, sizeof(link_buffer));
      link_length =
         readlink(proc_fd_path, link_buffer, sizeof(link_buffer));
      TEST_CHECK(link_length ==
                    (ssize_t)strlen("/dev/dri/renderD128") &&
                    memcmp(link_buffer, "/dev/dri/renderD128",
                           strlen("/dev/dri/renderD128")) == 0,
                 "direct readlink fallback returned %zd bytes",
                 link_length);
      errno = 0;
      link_length =
         readlink(proc_fd_path, (char *)(uintptr_t)1,
                  sizeof(link_buffer));
      TEST_CHECK(link_length == -1 && errno == EFAULT,
                 "pipe readlink fallback invalid buffer returned %zd "
                 "errno %d",
                 link_length, errno);
      drm_shim_test_force_proc_mem_error(0);
      drm_shim_test_force_process_vm_writev_error(0);
      int proc_fd_directory =
         open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      TEST_CHECK(proc_fd_directory >= 0,
                 "proc-fd directory open failed with errno %d", errno);
      if (proc_fd_directory >= 0) {
         char descriptor_name[32];
         snprintf(descriptor_name, sizeof(descriptor_name), "%d",
                  path_fd);
         memset(link_buffer, 0xa5, sizeof(link_buffer));
         link_length =
            readlinkat(proc_fd_directory, descriptor_name, link_buffer,
                       sizeof(link_buffer));
         TEST_CHECK(link_length ==
                       (ssize_t)strlen("/dev/dri/renderD128") &&
                       memcmp(link_buffer, "/dev/dri/renderD128",
                              strlen("/dev/dri/renderD128")) == 0,
                    "render proc-fd readlinkat leaked %zd bytes",
                    link_length);
         close(proc_fd_directory);
      }

      char proc_variant[PATH_MAX];
      snprintf(proc_variant, sizeof(proc_variant),
               "/proc/%ld/fd/%d", (long)getpid(), path_fd);
      test_proc_fd_readlink(proc_variant, true);
      snprintf(proc_variant, sizeof(proc_variant),
               "/proc/thread-self/fd/%d", path_fd);
      test_proc_fd_readlink(proc_variant, true);
      snprintf(proc_variant, sizeof(proc_variant),
               "/proc/self/task/%ld/fd/%d",
               (long)syscall(SYS_gettid), path_fd);
      test_proc_fd_readlink(proc_variant, true);
      snprintf(proc_variant, sizeof(proc_variant),
               "/dev/fd/%d", path_fd);
      test_proc_fd_readlink(proc_variant, true);

      int ready_pipe[2];
      int release_pipe[2];
      int ready_result = pipe2(ready_pipe, O_CLOEXEC);
      int release_result = pipe2(release_pipe, O_CLOEXEC);
      TEST_CHECK(ready_result == 0 && release_result == 0,
                 "thread-tid pipes failed with errno %d", errno);
      if (ready_result == 0 && release_result == 0) {
         struct test_thread_tid_context context = {
            .ready_fd = ready_pipe[1],
            .release_fd = release_pipe[0],
         };
         pthread_t thread;
         int thread_result =
            pthread_create(&thread, NULL, test_thread_tid_wait, &context);
         TEST_CHECK(thread_result == 0,
                    "thread-tid pthread_create returned %d", thread_result);
         if (thread_result == 0) {
            pid_t other_tid = -1;
            ssize_t length;
            do {
               length =
                  read(ready_pipe[0], &other_tid, sizeof(other_tid));
            } while (length < 0 && errno == EINTR);
            TEST_CHECK(length == sizeof(other_tid) && other_tid > 0,
                       "thread-tid report returned %zd and %ld", length,
                       (long)other_tid);
            if (length == sizeof(other_tid) && other_tid > 0) {
               snprintf(proc_variant, sizeof(proc_variant),
                        "/proc/%ld/fd/%d", (long)other_tid, path_fd);
               test_proc_fd_readlink(proc_variant, true);
               snprintf(proc_variant, sizeof(proc_variant),
                        "/proc/self/task/%ld/fd/%d",
                        (long)other_tid, path_fd);
               test_proc_fd_readlink(proc_variant, true);
            }
            char release = 1;
            ssize_t written;
            do {
               written =
                  write(release_pipe[1], &release, sizeof(release));
            } while (written < 0 && errno == EINTR);
            TEST_CHECK(written == sizeof(release),
                       "thread-tid release returned %zd", written);
            thread_result = pthread_join(thread, NULL);
            TEST_CHECK(thread_result == 0,
                       "thread-tid pthread_join returned %d",
                       thread_result);
         }
         close(ready_pipe[0]);
         close(ready_pipe[1]);
         close(release_pipe[0]);
         close(release_pipe[1]);
      } else {
         if (ready_result == 0) {
            close(ready_pipe[0]);
            close(ready_pipe[1]);
         }
         if (release_result == 0) {
            close(release_pipe[0]);
            close(release_pipe[1]);
         }
      }

      char alias_directory[] = "/tmp/mesa-drm-shim-fd-alias-XXXXXX";
      char *created_alias_directory = mkdtemp(alias_directory);
      TEST_CHECK(created_alias_directory,
                 "proc-fd alias directory failed with errno %d", errno);
      if (created_alias_directory) {
         char alias_link[PATH_MAX];
         snprintf(alias_link, sizeof(alias_link), "%s/fd",
                  created_alias_directory);
         ret = symlink("/proc/self/fd", alias_link);
         TEST_CHECK(ret == 0,
                    "proc-fd alias symlink returned %d errno %d", ret,
                    errno);
         if (ret == 0) {
            snprintf(proc_variant, sizeof(proc_variant), "%s/%d",
                     alias_link, path_fd);
            test_proc_fd_readlink(proc_variant, true);
            unlink(alias_link);
         }
         rmdir(created_alias_directory);
      }

      static const char *const invalid_proc_formats[] = {
         "/proc/self/fd/+%d",
         "/proc/self/fd/0%d",
         "/proc/self/fd/ %d",
         "/proc/self/fd/-%d",
         "/proc/self/fd/%d/",
         "/proc/self/fd/%d/.",
         "/proc/self/fd/%d/../%d",
      };
      for (size_t i = 0; i < ARRAY_SIZE(invalid_proc_formats); i++) {
         snprintf(proc_variant, sizeof(proc_variant),
                  invalid_proc_formats[i], path_fd, path_fd);
         test_proc_fd_readlink(proc_variant, false);
      }

      int untracked_fd =
         syscall(SYS_openat, AT_FDCWD, proc_fd_path,
                 O_RDWR | O_CLOEXEC, 0);
      TEST_CHECK(untracked_fd >= 0,
                 "untracked render duplicate failed with errno %d", errno);
      if (untracked_fd >= 0) {
         alarm(5);
         int discovered_fd = fcntl(untracked_fd, F_DUPFD_CLOEXEC, 3);
         alarm(0);
         TEST_CHECK(discovered_fd >= 0,
                    "untracked render fcntl discovery failed with errno %d",
                    errno);
         if (discovered_fd >= 0) {
            uint32_t discovered_device = UINT32_MAX;
            struct drm_radeon_info discovered_info = {
               .request = RADEON_INFO_DEVICE_ID,
               .value = (uintptr_t)&discovered_device,
            };
            ret = ioctl(discovered_fd, DRM_IOCTL_RADEON_INFO,
                        &discovered_info);
            TEST_CHECK(ret == 0 &&
                          discovered_device == expected_device_id,
                       "untracked render discovery returned %d value 0x%x",
                       ret, discovered_device);
            close(discovered_fd);
         }
         (void)syscall(SYS_close, untracked_fd);
      }
      close(path_fd);
   }

#ifdef SYS_statx
   int root_path_fd = open("/", O_PATH | O_CLOEXEC);
   TEST_CHECK(root_path_fd >= 0,
              "root O_PATH open failed with errno %d", errno);
   if (root_path_fd >= 0) {
      struct statx expected_statusx;
      struct statx actual_statusx;
      memset(&expected_statusx, 0, sizeof(expected_statusx));
      errno = 0;
      int expected_ret =
         syscall(SYS_statx, root_path_fd, NULL, AT_EMPTY_PATH,
                 STATX_BASIC_STATS, &expected_statusx);
      int expected_errno = errno;

      drm_shim_test_force_statx_symbol_absent(true);
      memset(&actual_statusx, 0, sizeof(actual_statusx));
      errno = 0;
      ret = statx(root_path_fd, NULL, AT_EMPTY_PATH, STATX_BASIC_STATS,
                  &actual_statusx);
      int actual_errno = errno;
      drm_shim_test_force_statx_symbol_absent(false);

      TEST_CHECK(ret == expected_ret &&
                    (ret == 0 || actual_errno == expected_errno),
                 "syscall-backed NULL statx returned %d errno %d; "
                 "raw syscall returned %d errno %d",
                 ret, actual_errno, expected_ret, expected_errno);
      if (ret == 0 && expected_ret == 0) {
         TEST_CHECK(actual_statusx.stx_ino == expected_statusx.stx_ino &&
                       actual_statusx.stx_mode ==
                          expected_statusx.stx_mode,
                    "syscall-backed NULL statx changed inode or mode");
      }
      close(root_path_fd);
   }
#endif

   struct open_how how = {
      .flags = O_RDONLY | O_CLOEXEC,
      .resolve = RESOLVE_NO_MAGICLINKS,
   };
   int openat2_fd =
      test_openat2(AT_FDCWD, vendor_path, &how, sizeof(how));
   TEST_CHECK(openat2_fd >= 0,
              "openat2 synthetic file failed with errno %d", errno);
   if (openat2_fd >= 0)
      close(openat2_fd);
   errno = 0;
   ret = test_openat2(
      AT_FDCWD, vendor_path,
      (const struct open_how *)(uintptr_t)1, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "pipe openat2 fallback invalid how returned %d errno %d",
              ret, errno);

   drm_shim_test_force_process_vm_readv_error(EPERM);
   drm_shim_test_force_proc_mem_error(ENOENT);
   openat2_fd =
      test_openat2(AT_FDCWD, vendor_path, &how, sizeof(how));
   TEST_CHECK(openat2_fd >= 0,
              "direct openat2 argument fallback failed with errno %d",
              errno);
   if (openat2_fd >= 0)
      close(openat2_fd);
   errno = 0;
   ret = test_openat2(
      AT_FDCWD, vendor_path,
      (const struct open_how *)(uintptr_t)1, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "pipe openat2 fallback invalid how returned %d errno %d",
              ret, errno);
   drm_shim_test_force_proc_mem_error(0);
   drm_shim_test_force_process_vm_readv_error(0);

   errno = 0;
   ret = test_openat2(AT_FDCWD, vendor_path, NULL, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "openat2 NULL how returned %d errno %d", ret, errno);
   errno = 0;
   ret = test_openat2(AT_FDCWD, vendor_path, &how, sizeof(how) - 1);
   TEST_CHECK(ret == -1 && errno == EINVAL,
              "openat2 short how returned %d errno %d", ret, errno);
   errno = 0;
   ret = test_openat2(
      AT_FDCWD, vendor_path,
      (const struct open_how *)(uintptr_t)1, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EFAULT,
              "openat2 invalid how returned %d errno %d", ret, errno);

   struct {
      struct open_how how;
      uint64_t extension;
   } extended_how = {
      .how = how,
      .extension = 0,
   };
   openat2_fd =
      test_openat2(AT_FDCWD, vendor_path, &extended_how.how,
                   sizeof(extended_how));
   TEST_CHECK(openat2_fd >= 0,
              "openat2 zero extension failed with errno %d", errno);
   if (openat2_fd >= 0)
      close(openat2_fd);
   extended_how.extension = 1;
   errno = 0;
   ret = test_openat2(AT_FDCWD, vendor_path, &extended_how.how,
                      sizeof(extended_how));
   TEST_CHECK(ret == -1 && errno == E2BIG,
              "openat2 nonzero extension returned %d errno %d", ret,
              errno);

   how.flags = O_RDONLY;
   how.mode = 0600;
   how.resolve = 0;
   errno = 0;
   ret = test_openat2(AT_FDCWD, vendor_path, &how, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EINVAL,
              "openat2 invalid mode returned %d errno %d", ret, errno);
   how.mode = 0;
   how.resolve = UINT64_C(1) << 63;
   errno = 0;
   ret = test_openat2(AT_FDCWD, vendor_path, &how, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EINVAL,
              "openat2 invalid resolve returned %d errno %d", ret, errno);

   how.flags = O_RDONLY | O_CLOEXEC;
   how.mode = 0;
   how.resolve = RESOLVE_BENEATH;
   int synthetic_device_dirfd =
      open(device_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(synthetic_device_dirfd >= 0,
              "openat2 synthetic base open failed with errno %d", errno);
   if (synthetic_device_dirfd >= 0) {
      openat2_fd =
         test_openat2(synthetic_device_dirfd, "vendor", &how, sizeof(how));
      TEST_CHECK(openat2_fd >= 0,
                 "openat2 BENEATH relative file failed with errno %d",
                 errno);
      if (openat2_fd >= 0)
         close(openat2_fd);

      openat2_fd =
         test_openat2(synthetic_device_dirfd, "drm/../vendor", &how,
                      sizeof(how));
      TEST_CHECK(openat2_fd >= 0,
                 "openat2 BENEATH bounded parent traversal failed with "
                 "errno %d",
                 errno);
      if (openat2_fd >= 0)
         close(openat2_fd);

      errno = 0;
      ret = test_openat2(synthetic_device_dirfd, "../device/vendor", &how,
                         sizeof(how));
      TEST_CHECK(ret == -1 && errno == EXDEV,
                 "openat2 BENEATH parent traversal returned %d errno %d",
                 ret, errno);

      how.resolve = RESOLVE_IN_ROOT;
      openat2_fd =
         test_openat2(synthetic_device_dirfd, "/vendor", &how,
                      sizeof(how));
      TEST_CHECK(openat2_fd >= 0,
                 "openat2 IN_ROOT file failed with errno %d", errno);
      if (openat2_fd >= 0)
         close(openat2_fd);
      close(synthetic_device_dirfd);
   }

   int host_pci_root_fd =
      syscall(SYS_openat, AT_FDCWD, "/sys/devices/pci0000:00",
              O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
   TEST_CHECK(host_pci_root_fd >= 0,
              "openat2 host PCI root open failed with errno %d",
              errno);
   if (host_pci_root_fd >= 0) {
      how.flags = O_PATH | O_CLOEXEC;
      how.mode = 0;
      how.resolve = RESOLVE_BENEATH;
      errno = 0;
      ret = test_openat2(
         host_pci_root_fd,
         "0000:01:00.0/subsystem", &how, sizeof(how));
      TEST_CHECK(ret == -1 && errno == EXDEV,
                 "openat2 synthetic BENEATH symlink escape returned %d "
                 "errno %d",
                 ret, errno);
      if (ret >= 0)
         close(ret);
      close(host_pci_root_fd);
   }

   int host_root_fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(host_root_fd >= 0,
              "openat2 host root open failed with errno %d", errno);
   if (host_root_fd >= 0) {
      how.flags = O_RDONLY | O_CLOEXEC;
      how.resolve = RESOLVE_IN_ROOT;
      openat2_fd =
         test_openat2(host_root_fd, device_id_path, &how, sizeof(how));
      TEST_CHECK(openat2_fd >= 0,
                 "openat2 host-root overlay failed with errno %d", errno);
      if (openat2_fd >= 0) {
         char selected_device[16] = {0};
         ssize_t selected_length =
            read(openat2_fd, selected_device,
                 sizeof(selected_device) - 1);
         char expected_selected_device[16];
         snprintf(expected_selected_device,
                  sizeof(expected_selected_device), "0x%04x\n",
                  expected_device_id);
         TEST_CHECK(
            selected_length ==
               (ssize_t)strlen(expected_selected_device) &&
               memcmp(selected_device, expected_selected_device,
                      (size_t)selected_length) == 0,
            "openat2 host-root overlay returned %zd bytes '%s'",
            selected_length, selected_device);
         close(openat2_fd);
      }
      close(host_root_fd);
   }

   int tmp_root_fd = open("/tmp", O_PATH | O_DIRECTORY | O_CLOEXEC);
   TEST_CHECK(tmp_root_fd >= 0,
              "openat2 temporary root open failed with errno %d", errno);
   if (tmp_root_fd >= 0) {
      how.flags = O_RDONLY | O_CLOEXEC;
      how.resolve = RESOLVE_IN_ROOT;
      errno = 0;
      ret =
         test_openat2(tmp_root_fd, device_id_path, &how, sizeof(how));
      TEST_CHECK(ret == -1 && errno == ENOENT,
                 "openat2 non-root IN_ROOT returned %d errno %d",
                 ret, errno);
      if (ret >= 0)
         close(ret);
      close(tmp_root_fd);
   }

   how.resolve = RESOLVE_BENEATH;
   errno = 0;
   ret = test_openat2(AT_FDCWD, vendor_path, &how, sizeof(how));
   TEST_CHECK(ret == -1 && errno == EXDEV,
              "openat2 BENEATH absolute path returned %d errno %d", ret,
              errno);

   how.resolve = RESOLVE_NO_SYMLINKS;
   errno = 0;
   ret = test_openat2(AT_FDCWD, char_path, &how, sizeof(how));
   TEST_CHECK(ret == -1 && errno == ELOOP,
              "openat2 NO_SYMLINKS returned %d errno %d", ret, errno);

#ifdef SYS_openat2
   char openat2_root[] = "/tmp/mesa-drm-shim-openat2-XXXXXX";
   char *created_openat2_root = mkdtemp(openat2_root);
   TEST_CHECK(created_openat2_root,
              "openat2 parity root creation failed with errno %d", errno);
   if (created_openat2_root) {
      char alias_path[PATH_MAX];
      snprintf(alias_path, sizeof(alias_path), "%s/vendor-alias",
               created_openat2_root);
      ret = symlink(vendor_path, alias_path);
      TEST_CHECK(ret == 0,
                 "openat2 parity alias creation failed with errno %d",
                 errno);
      if (ret == 0) {
         struct open_how parity_how = {
            .flags = O_RDONLY | O_CLOEXEC,
            .resolve = RESOLVE_NO_SYMLINKS,
         };
         errno = 0;
         int raw_ret =
            syscall(SYS_openat2, AT_FDCWD, alias_path, &parity_how,
                    sizeof(parity_how));
         int raw_errno = errno;
         if (raw_ret >= 0)
            syscall(SYS_close, raw_ret);

         errno = 0;
         int shim_ret =
            test_openat2(AT_FDCWD, alias_path, &parity_how,
                         sizeof(parity_how));
         int shim_errno = errno;
         if (shim_ret >= 0)
            close(shim_ret);
         TEST_CHECK(raw_ret == -1 && raw_errno == ELOOP &&
                       shim_ret == raw_ret && shim_errno == raw_errno,
                    "openat2 NO_SYMLINKS parity returned raw %d/%d "
                    "shim %d/%d",
                    raw_ret, raw_errno, shim_ret, shim_errno);

         parity_how.flags = O_PATH | O_NOFOLLOW | O_CLOEXEC;
         errno = 0;
         raw_ret =
            syscall(SYS_openat2, AT_FDCWD, alias_path, &parity_how,
                    sizeof(parity_how));
         raw_errno = errno;
         struct stat raw_status = {0};
         if (raw_ret >= 0)
            syscall(SYS_fstat, raw_ret, &raw_status);

         errno = 0;
         shim_ret =
            test_openat2(AT_FDCWD, alias_path, &parity_how,
                         sizeof(parity_how));
         shim_errno = errno;
         struct stat shim_status = {0};
         if (shim_ret >= 0)
            fstat(shim_ret, &shim_status);
         TEST_CHECK(raw_ret >= 0 && shim_ret >= 0 &&
                       S_ISLNK(raw_status.st_mode) &&
                       S_ISLNK(shim_status.st_mode),
                    "openat2 final NO_SYMLINKS parity returned raw %d/%d "
                    "mode 0%o shim %d/%d mode 0%o",
                    raw_ret, raw_errno, raw_status.st_mode,
                    shim_ret, shim_errno, shim_status.st_mode);
         if (raw_ret >= 0)
            syscall(SYS_close, raw_ret);
         if (shim_ret >= 0)
            close(shim_ret);
         unlink(alias_path);
      }

      char escape_path[PATH_MAX];
      snprintf(escape_path, sizeof(escape_path), "%s/escape",
               created_openat2_root);
      ret = symlink(vendor_path, escape_path);
      TEST_CHECK(ret == 0,
                 "openat2 BENEATH escape creation failed with errno %d",
                 errno);
      int openat2_root_fd =
         open(created_openat2_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      TEST_CHECK(openat2_root_fd >= 0,
                 "openat2 parity root open failed with errno %d", errno);
      if (ret == 0 && openat2_root_fd >= 0) {
         struct open_how parity_how = {
            .flags = O_RDONLY | O_CLOEXEC,
            .resolve = RESOLVE_BENEATH,
         };
         errno = 0;
         int raw_ret =
            syscall(SYS_openat2, openat2_root_fd, "escape", &parity_how,
                    sizeof(parity_how));
         int raw_errno = errno;
         if (raw_ret >= 0)
            syscall(SYS_close, raw_ret);

         errno = 0;
         int shim_ret =
            test_openat2(openat2_root_fd, "escape", &parity_how,
                         sizeof(parity_how));
         int shim_errno = errno;
         if (shim_ret >= 0)
            close(shim_ret);
         TEST_CHECK(raw_ret == -1 && raw_errno == EXDEV &&
                       shim_ret == raw_ret && shim_errno == raw_errno,
                    "openat2 BENEATH parity returned raw %d/%d "
                    "shim %d/%d",
                    raw_ret, raw_errno, shim_ret, shim_errno);
      }
      if (openat2_root_fd >= 0)
         close(openat2_root_fd);
      if (ret == 0)
         unlink(escape_path);
      rmdir(created_openat2_root);
   }

   int magic_target_fd = open(vendor_path, O_RDONLY | O_CLOEXEC);
   TEST_CHECK(magic_target_fd >= 0,
              "openat2 magic-link target open failed with errno %d",
              errno);
   if (magic_target_fd >= 0) {
      char magic_path[64];
      snprintf(magic_path, sizeof(magic_path), "/proc/self/fd/%d",
               magic_target_fd);
      struct open_how parity_how = {
         .flags = O_RDONLY | O_CLOEXEC,
         .resolve = RESOLVE_NO_MAGICLINKS,
      };
      errno = 0;
      int raw_ret =
         syscall(SYS_openat2, AT_FDCWD, magic_path, &parity_how,
                 sizeof(parity_how));
      int raw_errno = errno;
      if (raw_ret >= 0)
         syscall(SYS_close, raw_ret);

      errno = 0;
      int shim_ret =
         test_openat2(AT_FDCWD, magic_path, &parity_how,
                      sizeof(parity_how));
      int shim_errno = errno;
      if (shim_ret >= 0)
         close(shim_ret);
      TEST_CHECK(raw_ret == -1 && raw_errno == ELOOP &&
                    shim_ret == raw_ret && shim_errno == raw_errno,
                 "openat2 NO_MAGICLINKS parity returned raw %d/%d "
                 "shim %d/%d",
                 raw_ret, raw_errno, shim_ret, shim_errno);
      close(magic_target_fd);
   }
#endif

   how.flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
   how.resolve = 0;
   errno = 0;
   ret = test_openat2(AT_FDCWD, char_path, &how, sizeof(how));
   TEST_CHECK(ret == -1 && errno == ELOOP,
              "openat2 O_NOFOLLOW symlink returned %d errno %d", ret,
              errno);

   how.flags = O_PATH | O_NOFOLLOW | O_CLOEXEC;
   how.resolve = RESOLVE_NO_SYMLINKS;
   openat2_fd =
      test_openat2(AT_FDCWD, char_path, &how, sizeof(how));
   TEST_CHECK(openat2_fd >= 0,
              "openat2 O_PATH symlink failed with errno %d", errno);
   if (openat2_fd >= 0) {
      ret = fstat(openat2_fd, &status);
      TEST_CHECK(ret == 0 && S_ISLNK(status.st_mode),
                 "openat2 O_PATH symlink fstat returned %d mode 0%o",
                 ret, status.st_mode);
      close(openat2_fd);
   }

#ifdef SYS_openat2
   static const char *const traversed_link_suffixes[] = {
      "subsystem/.",
      "subsystem/..",
   };
   for (size_t i = 0;
        i < ARRAY_SIZE(traversed_link_suffixes); i++) {
      char logical_path[PATH_MAX];
      char physical_path[PATH_MAX];
      snprintf(logical_path, sizeof(logical_path),
               "/sys/devices/pci0000:00/0000:01:00.0/%s",
               traversed_link_suffixes[i]);
      snprintf(physical_path, sizeof(physical_path), "%s%s",
               drm_shim_test_synthetic_root_path(), logical_path);
      struct open_how parity_how = {
         .flags = O_PATH | O_NOFOLLOW | O_CLOEXEC,
         .resolve = RESOLVE_NO_SYMLINKS,
      };
      errno = 0;
      int raw_ret =
         syscall(SYS_openat2, AT_FDCWD, physical_path, &parity_how,
                 sizeof(parity_how));
      int raw_errno = errno;
      if (raw_ret >= 0)
         syscall(SYS_close, raw_ret);
      errno = 0;
      int shim_ret =
         test_openat2(AT_FDCWD, logical_path, &parity_how,
                      sizeof(parity_how));
      int shim_errno = errno;
      if (shim_ret >= 0)
         close(shim_ret);
      TEST_CHECK(raw_ret == -1 && raw_errno == ELOOP &&
                    shim_ret == raw_ret && shim_errno == raw_errno,
                 "openat2 traversed-link parity %zu returned raw %d/%d "
                 "shim %d/%d",
                 i, raw_ret, raw_errno, shim_ret, shim_errno);
   }
#endif

   test_synthetic_directories();
   test_external_synthetic_links();
   test_deep_dirfd_synthetic_link();
   test_path_snapshot_confinement();
   test_fcntl_lock_pointer_validation(fd);
   test_fd_identity_contract(fd, expected_device_id);
   test_sealed_identity_exactness(fd);
   test_descriptor_lock_contracts();
   test_fd_identity_first_discovery();
   test_bo_fd_limit(fd);
   test_mem_addr_lifetime(fd);
   test_shared_drm_file_description(fd);
   test_scm_record_fifo();
   test_delayed_scm_rights_receive();
   test_delayed_raw_duplicate_discovery();
   test_close_range_unshared_fd_table(fd);
   test_direct_unshared_fd_table(fd);
   test_fork_child_close_preserves_parent_bo(fd);
   test_fork_child_identity_repair_preserves_parent_bo(fd);

   pid_t child = fork();
   TEST_CHECK(child >= 0, "fork child-first case failed with errno %d",
              errno);
   if (child == 0)
      exit(test_fork_survivor_paths() ? 0 : 1);
   if (child > 0) {
      int child_status;
      pid_t waited;
      do {
         waited = waitpid(child, &child_status, 0);
      } while (waited < 0 && errno == EINTR);
      TEST_CHECK(waited == child && WIFEXITED(child_status) &&
                    WEXITSTATUS(child_status) == 0,
                 "fork child-first child status is 0x%x",
                 waited == child ? child_status : -1);
      TEST_CHECK(test_fork_survivor_paths(),
                 "fork child-first parent lost synthetic paths");
   }
   test_dropped_scm_rights_release();
}

static int
test_rejected_selector(const char *selector)
{
   if (!radeon_resolve_device(selector))
      return 0;

   fprintf(stderr, "FAIL: rejected selector %s resolved to a device\n",
           selector);
   return 1;
}

static bool
test_fork_survivor_paths(void)
{
   int render_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (render_fd < 0)
      return false;
   char driver_name[16] = {0};
   struct drm_version version = {
      .name_len = sizeof(driver_name),
      .name = driver_name,
   };
   bool correct_driver =
      ioctl(render_fd, DRM_IOCTL_VERSION, &version) == 0 &&
      version.name_len == strlen("radeon") &&
      memcmp(driver_name, "radeon", strlen("radeon")) == 0;
   struct drm_radeon_gem_create create = {
      .size = 4096,
      .alignment = 4096,
      .initial_domain = RADEON_GEM_DOMAIN_GTT,
   };
   bool fresh_file_state =
      correct_driver &&
      ioctl(render_fd, DRM_IOCTL_RADEON_GEM_CREATE, &create) == 0 &&
      create.handle != 0;
   if (fresh_file_state) {
      struct drm_radeon_gem_busy busy = {
         .handle = create.handle,
         .domain = UINT32_MAX,
      };
      fresh_file_state =
         ioctl(render_fd, DRM_IOCTL_RADEON_GEM_BUSY, &busy) == 0 &&
         busy.domain == RADEON_GEM_DOMAIN_GTT;
      struct drm_gem_close close_bo = {
         .handle = create.handle,
      };
      fresh_file_state =
         ioctl(render_fd, DRM_IOCTL_GEM_CLOSE, &close_bo) == 0 &&
         fresh_file_state;
   }
   close(render_fd);

   char vendor[16];
   char device[16];
   char expected_device[16];
   snprintf(expected_device, sizeof(expected_device), "0x%04x\n",
            (unsigned)device_id);
   return correct_driver && fresh_file_state &&
          test_read_text(
             "/sys/dev/char/226:128/device/vendor",
             vendor, sizeof(vendor)) &&
          strcmp(vendor, "0x1002\n") == 0 &&
          test_read_text(
             "/sys/dev/char/226:128/device/device",
             device, sizeof(device)) &&
          strcmp(device, expected_device) == 0;
}

static int
test_fork_parent_exit_first(void)
{
   int result_pipe[2];
   int lifetime_pipe[2];
   if (pipe2(result_pipe, O_CLOEXEC) < 0 ||
       pipe2(lifetime_pipe, O_CLOEXEC) < 0)
      return 1;

   pid_t owner = fork();
   if (owner < 0)
      return 1;
   if (owner == 0) {
      if (setenv("RADEON_GPU_ID", "0x5974", 1) < 0)
         _exit(1);
      (void)syscall(SYS_close, result_pipe[0]);
      int render_fd =
         open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      if (render_fd < 0)
         _exit(1);
      close(render_fd);

      pid_t survivor = fork();
      if (survivor < 0)
         _exit(1);
      if (survivor == 0) {
         (void)syscall(SYS_close, lifetime_pipe[1]);
         char byte;
         while (read(lifetime_pipe[0], &byte, 1) < 0 &&
                errno == EINTR)
            ;
         close(lifetime_pipe[0]);
         byte = test_fork_survivor_paths() ? 'P' : 'F';
         (void)write(result_pipe[1], &byte, 1);
         close(result_pipe[1]);
         exit(byte == 'P' ? 0 : 1);
      }

      (void)syscall(SYS_close, lifetime_pipe[0]);
      (void)syscall(SYS_close, result_pipe[1]);
      (void)syscall(SYS_close, lifetime_pipe[1]);
      exit(0);
   }

   (void)syscall(SYS_close, result_pipe[1]);
   (void)syscall(SYS_close, lifetime_pipe[0]);
   (void)syscall(SYS_close, lifetime_pipe[1]);
   int owner_status;
   if (waitpid(owner, &owner_status, 0) != owner)
      return 1;
   char result = 'F';
   ssize_t length;
   do {
      length = read(result_pipe[0], &result, 1);
   } while (length < 0 && errno == EINTR);
   (void)syscall(SYS_close, result_pipe[0]);
   return WIFEXITED(owner_status) && WEXITSTATUS(owner_status) == 0 &&
          length == 1 && result == 'P' ? 0 : 1;
}

static int
test_reaper_close_sweep(void)
{
   int inherited_pipe[2];
   if (pipe2(inherited_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
      return 1;
   drm_shim_test_force_reaper_close_range_error(ENOSYS);
   drm_shim_test_force_reaper_getdents_eintr_once(true);
   if (setenv("RADEON_GPU_ID", "0x5974", 1) < 0)
      return 1;

   int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   if (render_fd < 0)
      return 1;
   if (close(inherited_pipe[1]) < 0)
      return 1;

   struct pollfd poll_fd = {
      .fd = inherited_pipe[0],
      .events = POLLIN | POLLHUP,
   };
   int poll_result;
   do {
      poll_result = poll(&poll_fd, 1, 5000);
   } while (poll_result < 0 && errno == EINTR);
   char byte;
   ssize_t length = read(inherited_pipe[0], &byte, sizeof(byte));
   int result =
      poll_result > 0 && (poll_fd.revents & POLLHUP) && length == 0 ? 0 : 1;
   close(inherited_pipe[0]);
   close(render_fd);
   return result;
}

/* A mapping attempt that loses its allocation or its descriptor knows nothing
 * about whether the path belongs to the shim, and /sys/dev/char/226:128 also
 * names the host's own DRM node. The claimed namespace therefore answers
 * ENOENT while the resolver is failing, and an unclaimed path keeps reaching
 * the real filesystem. Forcing the failure pins both halves without fd or
 * memory pressure.
 */
static int
test_claimed_namespace_map_miss(void)
{
   if (setenv("RADEON_GPU_ID", "0x5974", 1) < 0)
      return 1;

   int render_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(render_fd >= 0, "fake render node open failed with errno %d",
              errno);
   if (render_fd < 0)
      return 1;

   FILE *vendor =
      fopen("/sys/dev/char/226:128/device/vendor", "r");
   TEST_CHECK(vendor != NULL,
              "claimed vendor file failed to open with errno %d", errno);
   if (vendor)
      fclose(vendor);

   drm_shim_test_force_absolute_path_error(ENOMEM);

   errno = 0;
   FILE *forced =
      fopen("/sys/dev/char/226:128/device/vendor", "r");
   TEST_CHECK(forced == NULL && errno == ENOMEM,
              "claimed vendor file fell through to the real filesystem: "
              "file %p errno %d", (void *)forced, errno);
   if (forced)
      fclose(forced);

   errno = 0;
   FILE *unclaimed = fopen("/proc/self/cmdline", "r");
   TEST_CHECK(unclaimed != NULL,
              "unclaimed path lost its passthrough with errno %d", errno);
   if (unclaimed)
      fclose(unclaimed);

   drm_shim_test_force_absolute_path_error(0);
   close(render_fd);
   return test_failures ? 1 : 0;
}

static int
test_fd_exact_identity_or_refuse(void)
{
   if (setenv("RADEON_GPU_ID", "0x5974", 1) < 0)
      return 1;

   int render_fd =
      open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   TEST_CHECK(render_fd >= 0,
              "fd exact-identity render open failed with errno %d",
              errno);
   if (render_fd < 0)
      return 1;

   test_fd_identity_exact_or_refuse(render_fd, 0x5974);
   close(render_fd);
   return test_failures ? 1 : 0;
}

int
main(int argc, char **argv)
{
   if (argc == 2 &&
       strcmp(argv[1], "write-only-state-token-readable-witness") == 0)
      return test_write_only_state_token_readable_witness();
   if (argc == 2 &&
       strcmp(argv[1], "fd-exact-identity-or-refuse") == 0)
      return test_fd_exact_identity_or_refuse();
   if (argc == 2 && strcmp(argv[1], "claimed-namespace-map-miss") == 0)
      return test_claimed_namespace_map_miss();
   if (argc == 2 && strcmp(argv[1], "fork-parent-exit-first") == 0)
      return test_fork_parent_exit_first();
   if (argc == 2 && strcmp(argv[1], "reaper-close-sweep") == 0)
      return test_reaper_close_sweep();
   if (argc == 2 && strcmp(argv[1], "scm-record-fifo") == 0) {
      if (setenv("RADEON_GPU_ID", "0x5974", 1) < 0)
         return 1;
      test_scm_record_fifo();
      return test_failures ? 1 : 0;
   }

   bool rejection = argc == 3 && strcmp(argv[2], "reject") == 0;
   if ((!rejection && argc != 4 && argc != 5) || (rejection && argc != 3)) {
      fprintf(stderr,
              "usage: %s SELECTOR reject | SELECTOR DEVICE FAMILY [full]\n",
              argv[0]);
      return 2;
   }

   if (rejection)
      return test_rejected_selector(argv[1]);

   if (strcmp(argv[1], "DEFAULT") == 0)
      unsetenv("RADEON_GPU_ID");
   else
      setenv("RADEON_GPU_ID", argv[1], 1);

   int fd = open("/dev/dri/renderD128", O_RDWR);
   TEST_CHECK(fd >= 0, "fake render node open failed with errno %d", errno);
   if (fd < 0)
      return 1;

   char *end = NULL;
   unsigned long parsed_device_id = strtoul(argv[2], &end, 0);
   TEST_CHECK(end && !*end && parsed_device_id <= UINT16_MAX,
              "expected device argument is invalid: %s", argv[2]);
   test_identity(fd, parsed_device_id, argv[3]);

   if (argc == 5 && strcmp(argv[4], "full") == 0) {
      test_info_widths(fd);
      test_gem_busy(fd);
      test_synthetic_filesystem(fd, parsed_device_id);
      if (radeon_family == CHIP_RS480)
         test_hidden_paths();
   }

   close(fd);
   return test_failures ? 1 : 0;
}
#endif
