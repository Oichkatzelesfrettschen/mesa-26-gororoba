/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * Env-gated PM4 IB JSONL dump.  The disabled path costs one
 * call_once + one atomic-relaxed load of the cached
 * pm4_ib_dump_enabled flag + one branch on the false return;
 * file I/O starts only after TERAKAN_DEBUG_DUMP_IB=1 is
 * accepted by util/u_debug.h.  After the first call the
 * call_once is a no-op on the C11 once-flag (per c11/threads.h
 * contract).
 */

#include "terakan_pm4_ib_dump.h"

#include "c11/threads.h"
#include "util/detect_os.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include "util/u_debug.h"

#if DETECT_OS_WINDOWS
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#if DETECT_OS_FREEBSD && defined(HAVE_PTHREAD_NP_H)
#include <pthread_np.h>
#endif
#if DETECT_OS_LINUX
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "terakan_env.h"

static once_flag pm4_ib_dump_init_once = ONCE_FLAG_INIT;
static bool pm4_ib_dump_enabled = false;
static FILE *pm4_ib_dump_stream = NULL;
static simple_mtx_t pm4_ib_dump_open_lock = SIMPLE_MTX_INITIALIZER;
static simple_mtx_t pm4_ib_dump_write_lock = SIMPLE_MTX_INITIALIZER;

static void
pm4_ib_dump_init_cb(void)
{
   if (!debug_get_bool_option("TERAKAN_DEBUG_DUMP_IB", false)) {
      pm4_ib_dump_enabled = false;
      return;
   }

   pm4_ib_dump_enabled =
      !terakan_env_gate_enabled("TERAKAN_DEBUG_DUMP_IB_JSONL_DISABLE");
}

bool
terakan_pm4_ib_dump_active(void)
{
   call_once(&pm4_ib_dump_init_once, pm4_ib_dump_init_cb);
   return pm4_ib_dump_enabled;
}

static int
pm4_ib_dump_getpid(void)
{
#if DETECT_OS_WINDOWS
   return _getpid();
#else
   return (int)getpid();
#endif
}

#if !DETECT_OS_WINDOWS
static uintptr_t
pm4_ib_dump_pthread_key(pthread_t thread)
{
   unsigned char bytes[sizeof(thread)];
   /* FNV-1a 64-bit offset basis (FNV reference: http://www.isthe.com/chongo/tech/comp/fnv/) */
   uint64_t key = 14695981039346656037ull;

   memcpy(bytes, &thread, sizeof(bytes));

   for (size_t i = 0; i < sizeof(thread); i++) {
      key ^= bytes[i];
      key *= 1099511628211ull;
   }

   uintptr_t folded = (uintptr_t)(key ^ (key >> 32));

   return folded ? folded : 1;
}
#endif /* !DETECT_OS_WINDOWS */

/* Per-thread identifier for the JSONL "tid" field.  The chosen
 * value differs across platforms (kernel TID on Linux/Android,
 * GetCurrentThreadId on Windows, pthread_getthreadid_np on FreeBSD,
 * pthread_t-derived key on other POSIX); within a process the value
 * is stable per thread, which is the property the byte-path decoder
 * relies on for per-thread correlation.  The fallback never returns 0.
 */
static uintptr_t
pm4_ib_dump_gettid(void)
{
#if DETECT_OS_WINDOWS
   return (uintptr_t)GetCurrentThreadId();
#elif DETECT_OS_ANDROID
   return (uintptr_t)gettid();
#elif DETECT_OS_FREEBSD
#ifdef HAVE_PTHREAD_NP_H
   return (uintptr_t)pthread_getthreadid_np();
#else
   return pm4_ib_dump_pthread_key(pthread_self());
#endif
#elif DETECT_OS_LINUX
   return (uintptr_t)syscall(SYS_gettid);
#else
   return pm4_ib_dump_pthread_key(pthread_self());
#endif
}

static const char *
pm4_ib_dump_tmp_dir(void)
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
pm4_ib_dump_fopen_append_cloexec(const char *path)
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

static FILE *
pm4_ib_dump_stream_or_open(void)
{
   FILE *stream;
   char path[512];

   stream = __atomic_load_n(&pm4_ib_dump_stream, __ATOMIC_ACQUIRE);
   if (stream)
      return stream;

   simple_mtx_lock(&pm4_ib_dump_open_lock);
   stream = __atomic_load_n(&pm4_ib_dump_stream, __ATOMIC_RELAXED);
   if (!stream) {
      snprintf(path, sizeof(path), "%s/terakan_pm4_ib_%d.jsonl",
               pm4_ib_dump_tmp_dir(), pm4_ib_dump_getpid());
      stream = pm4_ib_dump_fopen_append_cloexec(path);
      if (stream)
         setvbuf(stream, NULL, _IOLBF, 0);
      __atomic_store_n(&pm4_ib_dump_stream, stream, __ATOMIC_RELEASE);
   }
   simple_mtx_unlock(&pm4_ib_dump_open_lock);
   return stream;
}

static uint64_t
pm4_ib_dump_ts_nsec(void)
{
   return (uint64_t)os_time_get_nano();
}

/* CRC32 with the little-endian Ethernet polynomial.  The kernel-side
 * observer uses the same polynomial, so diagnostic tooling can compare
 * submitted PM4 IB bytes against post-validator captures without
 * retaining every dword in memory.
 */
static uint32_t
pm4_ib_dump_crc32_le(uint32_t crc, const uint8_t *bytes, size_t size)
{
   for (size_t i = 0; i < size; i++) {
      crc ^= bytes[i];
      for (int bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
   }
   return crc;
}

void
terakan_pm4_ib_dump_cs_submission(unsigned ring, const uint32_t *ib_dwords,
                                  uint32_t ib_length_dwords)
{
   FILE *stream;

   if (!terakan_pm4_ib_dump_active() || !ib_dwords || ib_length_dwords == 0)
      return;

   stream = pm4_ib_dump_stream_or_open();
   if (!stream)
      return;

   uint32_t crc = pm4_ib_dump_crc32_le(
      0, (const uint8_t *)ib_dwords,
      (size_t)ib_length_dwords * sizeof(uint32_t));

   simple_mtx_lock(&pm4_ib_dump_write_lock);
   fprintf(stream,
           "{\"event\":\"pm4_ib_cs_submission\","
           "\"ts_nsec\":%" PRIu64 ","
           "\"pid\":%d,"
           "\"tid\":%" PRIuPTR ","
           "\"ring\":%u,"
           "\"ib_length_dw\":%u,"
           "\"ib_crc32\":\"0x%08" PRIx32 "\","
           "\"ib_dwords\":[",
           pm4_ib_dump_ts_nsec(), pm4_ib_dump_getpid(), pm4_ib_dump_gettid(),
           ring, ib_length_dwords, crc);
   for (uint32_t i = 0; i < ib_length_dwords; i++)
      fprintf(stream,
              i + 1 < ib_length_dwords
                 ? "\"0x%08" PRIx32 "\","
                 : "\"0x%08" PRIx32 "\"",
              ib_dwords[i]);
   fputs("]}\n", stream);
   simple_mtx_unlock(&pm4_ib_dump_write_lock);
}
