/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_pm4_ib_dump.c -- env-gated final PM4 IB capture.
 *
 * Companion to terakan_descriptor_dump.c (descriptor-object capture).  Same
 * lazy-open + per-pid file + flockfile concurrency model; different
 * gate (reuses the existing TERAKAN_DEBUG_DUMP_IB env var so the
 * stderr-side and JSONL-file outputs activate together by default).
 */

#include "terakan_pm4_ib_dump.h"

#include "terakan_env.h"

#include "util/u_debug.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static pthread_once_t   a1_init_once    = PTHREAD_ONCE_INIT;
static bool             a1_enabled      = false;
static FILE            *a1_stream       = NULL;
static pthread_mutex_t  a1_open_lock    = PTHREAD_MUTEX_INITIALIZER;

static void
a1_init_cb(void)
{
   /* Active iff TERAKAN_DEBUG_DUMP_IB is truthy AND the explicit
    * file-output disable knob is NOT set to "1".  The disable knob
    * exists for operators who want stderr text only (e.g. /tmp
    * is on a read-only filesystem).  Default behaviour: when
    * TERAKAN_DEBUG_DUMP_IB activates the stderr-side dump, the
    * JSONL file output activates as well. */
   if (!debug_get_bool_option("TERAKAN_DEBUG_DUMP_IB", false)) {
      a1_enabled = false;
      return;
   }
   if (terakan_env_gate_enabled("TERAKAN_DEBUG_DUMP_IB_JSONL_DISABLE")) {
      a1_enabled = false;
      return;
   }
   a1_enabled = true;
}

bool
terakan_pm4_ib_dump_active(void)
{
   pthread_once(&a1_init_once, a1_init_cb);
   return a1_enabled;
}

static FILE *
a1_stream_or_open(void)
{
   FILE *s;
   char  path[64];

   s = __atomic_load_n(&a1_stream, __ATOMIC_ACQUIRE);
   if (s)
      return s;

   pthread_mutex_lock(&a1_open_lock);
   s = a1_stream;
   if (!s) {
      snprintf(path, sizeof(path), "/tmp/terakan_pm4_ib_%d.jsonl",
               (int)getpid());
      s = fopen(path, "ae");
      if (s)
         setvbuf(s, NULL, _IOLBF, 0);
      __atomic_store_n(&a1_stream, s, __ATOMIC_RELEASE);
   }
   pthread_mutex_unlock(&a1_open_lock);
   return s;
}

static uint64_t
a1_ts_nsec(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static pid_t
a1_gettid(void)
{
   return (pid_t)syscall(SYS_gettid);
}

/* Trivial CRC32 (poly 0xEDB88320, little-endian).  Matches the
 * Linux kernel's crc32_le() output byte-for-byte so the steinmarder
 * Y.3 decoder can compare A1's CRC against the Y.2 observer's
 * ib_post_validate.ib_crc32_le directly.  Cheap: 8 ops/byte
 * via per-byte table-free implementation.  Could be sped up with
 * a table but the IB lengths here are small (~kilobytes per
 * submission). */
static uint32_t
a1_crc32_le(uint32_t crc, uint8_t const *p, size_t n)
{
   for (size_t i = 0; i < n; i++) {
      crc ^= p[i];
      for (int k = 0; k < 8; k++)
         crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
   }
   return crc;
}

void
terakan_pm4_ib_dump_cs_submission(unsigned ring,
                              uint32_t const *ib_dwords,
                              uint32_t ib_length_dwords)
{
   FILE *s;
   uint32_t crc;
   uint32_t i;

   if (!terakan_pm4_ib_dump_active() || !ib_dwords || ib_length_dwords == 0)
      return;
   s = a1_stream_or_open();
   if (!s)
      return;

   crc = a1_crc32_le(0, (uint8_t const *)ib_dwords,
                    (size_t)ib_length_dwords * sizeof(uint32_t));

   flockfile(s);
   fprintf(s,
           "{\"event\":\"pm4_ib_cs_submission\","
           "\"ts_nsec\":%" PRIu64 ","
           "\"pid\":%d,"
           "\"tid\":%d,"
           "\"ring\":%u,"
           "\"ib_length_dw\":%u,"
           "\"ib_crc32\":\"0x%08" PRIx32 "\","
           "\"ib_dwords\":[",
           a1_ts_nsec(),
           (int)getpid(),
           (int)a1_gettid(),
           ring,
           ib_length_dwords,
           crc);
   for (i = 0; i < ib_length_dwords; i++)
      fprintf(s,
              i + 1 < ib_length_dwords
                ? "\"0x%08" PRIx32 "\","
                : "\"0x%08" PRIx32 "\"",
              ib_dwords[i]);
   fputs("]}\n", s);
   funlockfile(s);
}
