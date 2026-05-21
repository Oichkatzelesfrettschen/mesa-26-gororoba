/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Env-gated descriptor JSONL dump.  The disabled path is one cached
 * boolean read and one branch; file I/O starts only after
 * TERAKAN_DEBUG_DUMP_DESCRIPTOR=1 is accepted by the strict env gate.
 */

#include "terakan_descriptor_dump.h"

#include "c11/threads.h"
#include "util/detect_os.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"

#if DETECT_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#if DETECT_OS_LINUX || DETECT_OS_FREEBSD
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#include <inttypes.h>
#include <stdio.h>

#include "terakan_env.h"
#include "terakan_image.h"
#include "terakan_sampler.h"

static once_flag descriptor_dump_init_once = ONCE_FLAG_INIT;
static bool descriptor_dump_enabled = false;
static FILE *descriptor_dump_stream = NULL;
static simple_mtx_t descriptor_dump_open_lock = SIMPLE_MTX_INITIALIZER;
static simple_mtx_t descriptor_dump_write_lock = SIMPLE_MTX_INITIALIZER;

static void
descriptor_dump_init_cb(void)
{
   descriptor_dump_enabled = terakan_env_gate_enabled("TERAKAN_DEBUG_DUMP_DESCRIPTOR");
}

bool
terakan_descriptor_dump_active(void)
{
   call_once(&descriptor_dump_init_once, descriptor_dump_init_cb);
   return descriptor_dump_enabled;
}

static int
descriptor_dump_getpid(void)
{
#if DETECT_OS_WINDOWS
   return _getpid();
#else
   return (int)getpid();
#endif
}

static uintptr_t
descriptor_dump_gettid(void)
{
#if DETECT_OS_WINDOWS
   return (uintptr_t)GetCurrentThreadId();
#elif DETECT_OS_ANDROID
   return (uintptr_t)gettid();
#elif DETECT_OS_FREEBSD
   long tid = 0;
   if (syscall(SYS_thr_self, &tid) == 0)
      return (uintptr_t)tid;
   return 0;
#elif DETECT_OS_LINUX
   return (uintptr_t)syscall(SYS_gettid);
#else
   return 0;
#endif
}

static const char *
descriptor_dump_tmp_dir(void)
{
#if DETECT_OS_WINDOWS
   const char *tmp_dir = os_get_option("TEMP");
   if (!tmp_dir || !tmp_dir[0])
      tmp_dir = os_get_option("TMP");
   return tmp_dir && tmp_dir[0] ? tmp_dir : ".";
#else
   const char *tmp_dir = os_get_option("TMPDIR");
   return tmp_dir && tmp_dir[0] ? tmp_dir : "/tmp";
#endif
}

static FILE *
descriptor_dump_fopen_append_cloexec(const char *path)
{
#if DETECT_OS_WINDOWS
   int fd = _open(path, _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY | _O_NOINHERIT,
                  _S_IREAD | _S_IWRITE);
   if (fd < 0)
      return NULL;

   FILE *stream = _fdopen(fd, "ab");
   if (!stream)
      _close(fd);
   return stream;
#else
   int flags = O_WRONLY | O_CREAT | O_APPEND
#ifdef O_CLOEXEC
               | O_CLOEXEC
#endif
               ;
   int fd = open(path, flags, 0600);
   if (fd < 0)
      return NULL;

#ifndef O_CLOEXEC
   if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
      close(fd);
      return NULL;
   }
#endif

   FILE *stream = fdopen(fd, "a");
   if (!stream)
      close(fd);
   return stream;
#endif
}

/* Open the JSONL stream on first emit (lazy) so processes that
 * enable the gate but never construct descriptors don't litter
 * the temporary directory with empty files. */
static FILE *
descriptor_dump_stream_or_open(void)
{
   FILE *s;
   char path[512];

   s = __atomic_load_n(&descriptor_dump_stream, __ATOMIC_ACQUIRE);
   if (s)
      return s;

   simple_mtx_lock(&descriptor_dump_open_lock);
   s = __atomic_load_n(&descriptor_dump_stream, __ATOMIC_RELAXED);
   if (!s) {
      snprintf(path, sizeof(path), "%s/terakan_descriptor_%d.jsonl",
               descriptor_dump_tmp_dir(), descriptor_dump_getpid());
      s = descriptor_dump_fopen_append_cloexec(path);
      if (s)
         setvbuf(s, NULL, _IOLBF, 0);
      __atomic_store_n(&descriptor_dump_stream, s, __ATOMIC_RELEASE);
   }
   simple_mtx_unlock(&descriptor_dump_open_lock);
   return s;
}

static uint64_t
descriptor_dump_ts_nsec(void)
{
   return (uint64_t)os_time_get_nano();
}

static void
descriptor_dump_print_dword_array(FILE *s, const uint32_t *dw, unsigned n)
{
   for (unsigned i = 0; i < n; i++)
      fprintf(s, i + 1 < n ? "\"0x%08" PRIx32 "\"," : "\"0x%08" PRIx32 "\"",
              dw[i]);
}

void
terakan_descriptor_dump_image_view(VkImageView view_handle,
                           struct terakan_image_view const *view,
                           VkImageViewCreateInfo const *create_info)
{
   FILE *s;

   if (!terakan_descriptor_dump_active() || !view || !create_info)
      return;
   s = descriptor_dump_stream_or_open();
   if (!s)
      return;

   simple_mtx_lock(&descriptor_dump_write_lock);
   fprintf(s,
           "{\"event\":\"descriptor_image_view\","
           "\"ts_nsec\":%" PRIu64 ","
           "\"pid\":%d,"
           "\"tid\":%" PRIuPTR ","
           "\"handle\":\"0x%" PRIx64 "\","
           "\"image_handle\":\"0x%" PRIx64 "\","
           "\"format\":%d,"
           "\"view_type\":%d,"
           "\"resource\":[",
           descriptor_dump_ts_nsec(),
           descriptor_dump_getpid(),
           descriptor_dump_gettid(),
           (uint64_t)(uintptr_t)view_handle,
           (uint64_t)(uintptr_t)create_info->image,
           (int)create_info->format,
           (int)create_info->viewType);
   descriptor_dump_print_dword_array(s, view->resource, 8);
   fputs("],\"resource_gather\":[", s);
   descriptor_dump_print_dword_array(s, view->resource_gather, 8);
   fputs("]}\n", s);
   simple_mtx_unlock(&descriptor_dump_write_lock);
}

void
terakan_descriptor_dump_sampler(VkSampler sampler_handle,
                        struct terakan_sampler const *sampler)
{
   FILE *s;

   if (!terakan_descriptor_dump_active() || !sampler)
      return;
   s = descriptor_dump_stream_or_open();
   if (!s)
      return;

   simple_mtx_lock(&descriptor_dump_write_lock);
   fprintf(s,
           "{\"event\":\"descriptor_sampler\","
           "\"ts_nsec\":%" PRIu64 ","
           "\"pid\":%d,"
           "\"tid\":%" PRIuPTR ","
           "\"handle\":\"0x%" PRIx64 "\","
           "\"unnormalized\":%s,"
           "\"sampler\":[",
           descriptor_dump_ts_nsec(),
           descriptor_dump_getpid(),
           descriptor_dump_gettid(),
           (uint64_t)(uintptr_t)sampler_handle,
           sampler->unnormalized_coordinates ? "true" : "false");
   descriptor_dump_print_dword_array(s, sampler->sampler, 3);
   fputs("]}\n", s);
   simple_mtx_unlock(&descriptor_dump_write_lock);
}
