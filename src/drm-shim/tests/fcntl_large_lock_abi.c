/* SPDX-License-Identifier: MIT */

#undef _FILE_OFFSET_BITS
#undef _TIME_BITS
#define _LARGEFILE64_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "drm_shim_fcntl_lock.h"

static unsigned failures;

#define CHECK(condition, message)                  \
   do {                                            \
      if (!(condition)) {                          \
         fprintf(stderr, "FAIL: %s\n", message); \
         failures++;                               \
      }                                            \
   } while (0)

int
main(void)
{
   _Static_assert(sizeof(off_t) == 4,
                  "i686 native off_t must be 32 bits");
   _Static_assert(sizeof(off64_t) == 8,
                  "i686 off64_t must be 64 bits");
   _Static_assert(sizeof(struct flock) != sizeof(struct flock64),
                  "the ABI test requires distinct lock layouts");

   static const int large_commands[] = {
      F_GETLK64,
      F_SETLK64,
      F_SETLKW64,
   };
   for (size_t index = 0;
        index < sizeof(large_commands) / sizeof(large_commands[0]);
        index++) {
      CHECK(drm_shim_fcntl_command_lock_layout(
               large_commands[index]) == DRM_SHIM_FCNTL_LOCK_FLOCK64,
            "large command selected the native lock layout");
   }
   CHECK(drm_shim_fcntl_command_lock_layout(F_SETLK) ==
            DRM_SHIM_FCNTL_LOCK_NATIVE,
         "native command selected the large lock layout");

   union drm_shim_fcntl_lock large_lock = {
      .large = {
         .l_type = F_WRLCK,
         .l_whence = SEEK_CUR,
         .l_start = 4096,
         .l_len = 17,
      },
   };
   enum drm_shim_fcntl_lock_layout large_layout =
      drm_shim_fcntl_command_lock_layout(F_SETLK64);
   CHECK(drm_shim_fcntl_lock_size(large_layout) ==
            sizeof(struct flock64),
         "large command selected the native copy size");
   CHECK(drm_shim_fcntl_lock_type(&large_lock,
                                  large_layout) == F_WRLCK,
         "large command read the lock type through the native member");
   CHECK(drm_shim_fcntl_lock_whence(&large_lock,
                                    large_layout) == SEEK_CUR,
         "large command read the whence through the native member");

   off64_t base = INT64_C(1) << 33;
   errno = 0;
   CHECK(drm_shim_fcntl_normalize_lock(
            &large_lock, large_layout, base),
         "large lock normalization failed");
   CHECK(large_lock.large.l_whence == SEEK_SET,
         "large lock normalization retained relative whence");
   CHECK(large_lock.large.l_start == base + 4096,
         "large lock normalization truncated the 64-bit start");
   CHECK(large_lock.large.l_len == 17,
         "large lock normalization changed the length");

   union drm_shim_fcntl_lock native_lock = {
      .native = {
         .l_type = F_WRLCK,
         .l_whence = SEEK_CUR,
         .l_start = INT32_MAX,
         .l_len = 1,
      },
   };
   errno = 0;
   CHECK(!drm_shim_fcntl_normalize_lock(
            &native_lock, DRM_SHIM_FCNTL_LOCK_NATIVE, 1) &&
            errno == EOVERFLOW,
         "native lock accepted a start beyond off_t");

   return failures ? 1 : 0;
}
