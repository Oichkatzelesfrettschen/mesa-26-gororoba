/*
 * SPDX-License-Identifier: MIT
 *
 * Explicit arming provider for the in-process radeon drm-shim harnesses.
 */

#ifndef R3V_NATIVE_SHIM_ARMING_H
#define R3V_NATIVE_SHIM_ARMING_H

#include "r3v_native_arming.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#define R3V_NATIVE_SHIM_MODULE_SRCVERSION "R3V_DRM_SHIM_SRCVERSION"

static const char *
r3v_native_shim_read_env(void *ctx, const char *name)
{
   (void)ctx;
   return getenv(name);
}

static void
r3v_native_shim_read_kernel_release(void *ctx, char *out, size_t size)
{
   (void)ctx;
   struct utsname host;
   out[0] = '\0';
   if (uname(&host) == 0)
      snprintf(out, size, "%s", host.release);
}

static void
r3v_native_shim_read_module_srcversion(void *ctx, char *out, size_t size)
{
   (void)ctx;
   snprintf(out, size, "%s", R3V_NATIVE_SHIM_MODULE_SRCVERSION);
}

static bool
r3v_native_shim_directory_present(void *ctx, const char *path)
{
   (void)ctx;
   struct stat status;
   return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static bool
r3v_native_shim_file_present(void *ctx, const char *path)
{
   (void)ctx;
   struct stat status;
   return stat(path, &status) == 0;
}

static const struct r3v_native_arming_provider
   r3v_native_shim_arming_provider = {
      .read_env = r3v_native_shim_read_env,
      .read_kernel_release = r3v_native_shim_read_kernel_release,
      .read_module_srcversion = r3v_native_shim_read_module_srcversion,
      .directory_present = r3v_native_shim_directory_present,
      .file_present = r3v_native_shim_file_present,
      .ctx = NULL,
   };

#endif /* R3V_NATIVE_SHIM_ARMING_H */
