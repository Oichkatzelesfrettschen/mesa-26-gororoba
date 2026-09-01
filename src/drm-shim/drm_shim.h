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

#include <c11/threads.h>
#include <stdint.h>

#include "util/macros.h"
#include "util/hash_table.h"
#include "util/vma.h"
#include <xf86drm.h>

#ifdef __has_include
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#else
struct open_how {
   uint64_t flags;
   uint64_t mode;
   uint64_t resolve;
};
#define RESOLVE_NO_XDEV 0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS 0x04
#define RESOLVE_BENEATH 0x08
#define RESOLVE_IN_ROOT 0x10
#define RESOLVE_CACHED 0x20
#endif
#else
#include <linux/openat2.h>
#endif

#ifdef __linux__
#define DRM_MAJOR 226
#endif

#define DRM_SHIM_RENDER_MARKER_CAPACITY 256
#define DRM_SHIM_RENDER_IDENTITY_CAPACITY 512
#define DRM_SHIM_RENDER_MARKER_VERSION "mesa-drm-shim-render-v2"
#define DRM_SHIM_RENDER_MARKER_XATTR "user.mesa_drm_shim_render"
#define DRM_SHIM_RENDER_IDENTITY_NAME_VERSION "mesa-drm-shim-render-v4"
#define DRM_SHIM_EXEC_LOCATOR_ENV "MESA_DRM_SHIM_EXEC_LOCATOR"

typedef int (*ioctl_fn_t)(int fd, unsigned long request, void *arg);

struct shim_bo;
struct shim_fd;
struct shim_mapping;
struct drm_shim_scm_pin;

struct shim_device {
   /* Mapping from int fd to struct shim_fd *. */
   struct hash_table *fd_map;
   struct hash_table *non_cloexec_fd_map;
   /* Objects remain here while their private identity fd is open. */
   struct shim_fd *identity_witness_objects;
   struct shim_fd *diverged_fd_objects;
   struct drm_shim_scm_pin *scm_pins;
   bool fd_tables_diverged;
   unsigned non_cloexec_aliases;
   int lock_backing_fd;
   /* F_DUPFD_CLOEXEC hint for the next identity vault socket.  The floor
    * holds the vault descriptors clear of stdin, stdout, and stderr; the
    * hint rises as vaults open and falls back to the lowest freed vault
    * number as they close, so the range stays bounded by the live vault
    * count rather than climbing to RLIMIT_NOFILE.
    */
#define DRM_SHIM_IDENTITY_VAULT_FD_FLOOR 3
   int next_identity_vault_fd;
   dev_t lock_backing_dev;
   ino_t lock_backing_ino;
   pid_t render_owner_pid;

   /* Mapping from mmap offset to shim_bo */
   struct hash_table_u64 *offset_map;

   /* Monotonic mmap token allocator.  Process-lifetime uniqueness keeps
    * stale tokens unbound from later BOs. */
   uint64_t next_mmap_offset;

   /* IOMEM region */
   struct {
      off64_t start;
      size_t size;
      void *(*mmap)(size_t length, int prot, int flags, off64_t offset);
   } iomem_region;

   /* Lock for fd_map, mappings, and address allocators. */
   mtx_t lock;

   struct shim_mapping *mappings;
   struct util_vma_heap mem_heap;

   int (**driver_ioctls)(int fd, unsigned long request, void *arg);
   int driver_ioctl_count;

   void (*driver_bo_free)(struct shim_bo *bo);

   /* Returned by drmGetVersion(). */
   const char *driver_name;

   /* Returned by drmGetBusid(). */
   const char *unique;

   int version_major, version_minor, version_patchlevel;
   int bus_type;
   char render_marker[DRM_SHIM_RENDER_MARKER_CAPACITY];
   size_t render_marker_length;
   char render_instance[33];
   int exec_locator_fd;
   bool exec_locator_enables_state;
   char lock_domain_marker[DRM_SHIM_RENDER_IDENTITY_CAPACITY];
   size_t lock_domain_marker_length;
};

extern struct shim_device shim_device;

struct shim_fd {
   int fd;
   int identity_vault_fd;
   int identity_witness_fd;
   int lock_proxy_fd;
   int lock_proxy_anchor_fd;
   int refcount;
   unsigned non_cloexec_aliases;
   pid_t owner_pid;
   bool path_only;
   bool state_available;
   bool independent_reopen_candidate;
   bool owns_lock_proxy_anchor;
   bool has_identity_lock;
   off64_t identity_lock_offset;
   dev_t backing_dev;
   ino_t backing_ino;
   dev_t identity_vault_dev;
   ino_t identity_vault_ino;
   mtx_t handle_lock;
   bool diverged_pinned;
   struct shim_fd *next_identity_witness;
   struct shim_fd *next_diverged;
   /* mapping from int gem handle to struct shim_bo *. */
   struct hash_table *handles;
};

struct shim_bo {
   uint64_t mem_addr;
   uint64_t mmap_offset;
   void *map;
   int refcount;
   int handle_count;
   pid_t owner_pid;
   uint32_t size;
};

/* Core support. */
extern const int render_node_minor;
bool drm_shim_inited(void);
bool drm_shim_fd_is_internal(int fd);
bool drm_shim_fd_is_reserved(int fd);
bool drm_shim_fd_reports_selected_device(int fd);
bool drm_shim_fd_names_render_backing(int fd);
int drm_shim_render_node_open(int flags);
int drm_shim_render_node_path(char *path, size_t capacity);
void drm_shim_fd_scan_inherited(void);
void drm_shim_fd_tables_unshared(void);
void drm_shim_fd_reap_diverged(void);
void drm_shim_fd_adopt_raw_aliases(int fd);
int drm_shim_fd_adopt_raw_aliases_range(unsigned first_fd,
                                        unsigned last_fd);
int drm_shim_file_pin_scm(struct shim_fd *shim_fd,
                          uint64_t receiver_cookie);
void drm_shim_file_unpin_scm(struct shim_fd *shim_fd,
                             uint64_t receiver_cookie);
void drm_shim_scm_drop_receiver(uint64_t receiver_cookie);
void drm_shim_device_init(void);
void drm_shim_device_atfork_prepare(void);
void drm_shim_device_atfork_parent(void);
void drm_shim_device_atfork_child(void);
void drm_shim_pci_device_setup(uint16_t vendor_id, uint16_t device_id,
                               const char *pci_slot, const char *driver_name);
void drm_shim_platform_device_setup(const char *driver_name, const char *fullname, const char *compatible);
void drm_shim_override_file(const char *contents,
                            const char *path_format, ...) PRINTFLIKE(2, 3);
void drm_shim_override_link(const char *contents,
                            const char *path_format, ...) PRINTFLIKE(2, 3);
void drm_shim_hide_path(const char *path);
void drm_shim_hide_path_component(const char *parent, const char *basename);
#ifdef DRM_SHIM_TEST
bool drm_shim_test_path_is_hidden(const char *path);
const char *drm_shim_test_synthetic_root_path(void);
void drm_shim_test_force_openat2_resolver_enosys(bool force);
void drm_shim_test_force_openat2_eagain(unsigned attempts);
void drm_shim_test_force_process_vm_readv_error(int error);
void drm_shim_test_force_process_vm_writev_error(int error);
void drm_shim_test_force_proc_mem_error(int error);
void drm_shim_test_force_statx_symbol_absent(bool force);
void drm_shim_test_force_reaper_close_range_error(int error);
/* Forces absolute_path_at_alloc() to fail with the given errno, so the
 * claimed-namespace fallthrough can be exercised without fd or memory
 * pressure. Zero restores resolution.
 */
void drm_shim_test_force_absolute_path_error(int error);
void drm_shim_test_force_path_base_error(int error);
void drm_shim_test_force_reaper_getdents_eintr_once(bool force);
void drm_shim_test_force_fd_identity_errors(int duplicate_query_error,
                                            int kcmp_error);
void drm_shim_test_force_kcmp_result(bool force, int result);
void drm_shim_test_force_state_token_readable_witness(int witness_fd);
int drm_shim_test_state_token_readable_witness_fd(void);
int drm_shim_test_rejected_state_token_fd(void);
bool drm_shim_test_fd_is_registered(int fd);
void drm_shim_test_arm_fd_discovery_barrier(int ready_fd,
                                            int release_fd);
void drm_shim_test_internal_fds(int *root_fd, int *lease_fd);
void drm_shim_test_arm_path_snapshot_barrier(int ready_fd,
                                             int release_fd);
#endif
/* Exported from every shim build so a preloading harness can dlsym() the
 * live BO backing-file and GEM-object censuses.
 */
int drm_shim_test_live_bo_backing_files(void);
int drm_shim_test_live_bos(void);
int drm_shim_test_live_identity_files(void);
int drm_shim_fd_register(int fd, struct shim_fd *shim_fd);
void drm_shim_fd_unregister(int fd);
void drm_shim_fd_unregister_range(unsigned first_fd, unsigned last_fd);
void drm_shim_fd_release_posix_locks(int fd);
void drm_shim_file_release_posix_locks(struct shim_fd *shim_fd);
void drm_shim_fd_update_cloexec(int fd);
void drm_shim_fd_update_cloexec_range(unsigned first_fd,
                                      unsigned last_fd);
int drm_shim_fd_collect_internal(int **fds, size_t *count);
int drm_shim_fd_prepare_exec(int **fds, int **descriptor_flags,
                             size_t *count,
                             char **environment_entry);
int drm_shim_exec_locator_environment(int fd, bool enable_state,
                                      char **environment_entry);
void drm_shim_fd_restore_exec(const int *fds,
                              const int *descriptor_flags,
                              size_t count);
struct shim_fd *drm_shim_fd_lookup(int fd);
struct shim_fd *drm_shim_fd_get(int fd);
void drm_shim_fd_put(struct shim_fd *shim_fd);
int drm_shim_ioctl(struct shim_fd *shim_fd, int fd, unsigned long request,
                   void *arg);
void *drm_shim_mmap(struct shim_fd *shim_fd, void *addr, size_t length,
                    int prot, int flags, int fd, off64_t offset);
void *drm_shim_mmap_real(void *addr, size_t length, int prot, int flags,
                         int fd, off64_t offset);
int drm_shim_backing_create(uint64_t token, size_t size);
void *drm_shim_backing_map(uint64_t token, void *addr, size_t length,
                           int prot, int flags);
void drm_shim_backing_destroy(uint64_t token);

int drm_shim_bo_init(struct shim_bo *bo, size_t size);
void drm_shim_bo_get(struct shim_bo *bo);
void drm_shim_bo_put(struct shim_bo *bo);
struct shim_bo *drm_shim_bo_lookup(struct shim_fd *shim_fd, uint32_t handle);
int drm_shim_bo_get_handle(struct shim_fd *shim_fd, struct shim_bo *bo);
int drm_shim_bo_get_mmap_offset(struct shim_fd *shim_fd,
                                struct shim_bo *bo, uint64_t *offset);
void drm_shim_init_iomem_region(off64_t offset, size_t size,
                                void *(*mmap_handler)(size_t, int, int, off64_t));
void drm_shim_set_mem_addr_range(uint64_t start, uint64_t end);
void drm_shim_mapping_remove(void *address, size_t length);
struct shim_bo *drm_shim_mapping_get(void *address, size_t length);
void drm_shim_mapping_replace(struct shim_bo *bo, void *address,
                              size_t length);

/* driver-specific hooks. */
void drm_shim_driver_init(void);
extern bool drm_shim_driver_prefers_new_render_node;
