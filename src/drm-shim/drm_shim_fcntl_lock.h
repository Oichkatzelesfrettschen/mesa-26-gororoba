/* SPDX-License-Identifier: MIT */

#ifndef DRM_SHIM_FCNTL_LOCK_H
#define DRM_SHIM_FCNTL_LOCK_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

union drm_shim_fcntl_lock {
   struct flock native;
   struct flock64 large;
};

enum drm_shim_fcntl_lock_layout {
   DRM_SHIM_FCNTL_LOCK_NATIVE,
   DRM_SHIM_FCNTL_LOCK_FLOCK64,
};

static inline enum drm_shim_fcntl_lock_layout
drm_shim_fcntl_command_lock_layout(int command)
{
#if defined(F_GETLK64) && F_GETLK64 != F_GETLK
   if (command == F_GETLK64)
      return DRM_SHIM_FCNTL_LOCK_FLOCK64;
#endif
#if defined(F_SETLK64) && F_SETLK64 != F_SETLK
   if (command == F_SETLK64)
      return DRM_SHIM_FCNTL_LOCK_FLOCK64;
#endif
#if defined(F_SETLKW64) && F_SETLKW64 != F_SETLKW
   if (command == F_SETLKW64)
      return DRM_SHIM_FCNTL_LOCK_FLOCK64;
#endif
   return DRM_SHIM_FCNTL_LOCK_NATIVE;
}

static inline size_t
drm_shim_fcntl_lock_size(enum drm_shim_fcntl_lock_layout layout)
{
   return layout == DRM_SHIM_FCNTL_LOCK_FLOCK64
             ? sizeof(struct flock64)
             : sizeof(struct flock);
}

static inline short
drm_shim_fcntl_lock_type(const union drm_shim_fcntl_lock *lock,
                         enum drm_shim_fcntl_lock_layout layout)
{
   return layout == DRM_SHIM_FCNTL_LOCK_FLOCK64
             ? lock->large.l_type
             : lock->native.l_type;
}

static inline short
drm_shim_fcntl_lock_whence(const union drm_shim_fcntl_lock *lock,
                           enum drm_shim_fcntl_lock_layout layout)
{
   return layout == DRM_SHIM_FCNTL_LOCK_FLOCK64
             ? lock->large.l_whence
             : lock->native.l_whence;
}

static inline bool
drm_shim_fcntl_normalize_lock(union drm_shim_fcntl_lock *lock,
                              enum drm_shim_fcntl_lock_layout layout,
                              off64_t base)
{
   bool uses_flock64 = layout == DRM_SHIM_FCNTL_LOCK_FLOCK64;
   off64_t relative_start =
      uses_flock64 ? lock->large.l_start : lock->native.l_start;
   off64_t length =
      uses_flock64 ? lock->large.l_len : lock->native.l_len;
   off64_t normalized_start;
   if (__builtin_add_overflow(base, relative_start,
                              &normalized_start) ||
       normalized_start < 0) {
      errno = EINVAL;
      return false;
   }
   if (length < 0) {
      off64_t lower_bound;
      if (__builtin_add_overflow(normalized_start, length,
                                 &lower_bound) ||
          lower_bound < 0) {
         errno = EINVAL;
         return false;
      }
   }

   if (uses_flock64) {
      lock->large.l_whence = SEEK_SET;
      lock->large.l_start = normalized_start;
      return true;
   }
   if ((off64_t)(off_t)normalized_start != normalized_start) {
      errno = EOVERFLOW;
      return false;
   }
   lock->native.l_whence = SEEK_SET;
   lock->native.l_start = (off_t)normalized_start;
   return true;
}

#endif
