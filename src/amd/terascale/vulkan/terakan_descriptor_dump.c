/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

/*
 * terakan_descriptor_dump.c -- env-gated descriptor-object capture (Y.3a).
 *
 * See terakan_descriptor_dump.h for the schema + safety contract.  All file
 * I/O is gated by TERAKAN_DEBUG_DUMP_DESCRIPTOR=1 (strict env gate
 * helper from terakan_env.h).  Disabled-path overhead = one cached
 * boolean read + a branch.
 *
 * Concurrency model: append-write under flockfile() to the per-
 * process JSONL file.  Two threads in the same process writing
 * simultaneously will serialize on the libc stream lock; no two
 * threads from different processes contend (each process opens a
 * distinct file path based on getpid()).  The file is created at
 * first call and held open in a static FILE *; closed at exit via
 * stdio's normal teardown.
 */

#include "terakan_descriptor_dump.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "terakan_env.h"
#include "terakan_image.h"
#include "terakan_sampler.h"

static pthread_once_t   a0_init_once    = PTHREAD_ONCE_INIT;
static bool             a0_enabled      = false;
static FILE            *a0_stream       = NULL;
static pthread_mutex_t  a0_open_lock    = PTHREAD_MUTEX_INITIALIZER;

static void
a0_init_cb(void)
{
   a0_enabled = terakan_env_gate_enabled("TERAKAN_DEBUG_DUMP_DESCRIPTOR");
}

bool
terakan_descriptor_dump_active(void)
{
   pthread_once(&a0_init_once, a0_init_cb);
   return a0_enabled;
}

/* Open the JSONL stream on first emit (lazy) so processes that
 * enable the gate but never construct descriptors don't litter
 * /tmp with empty files. */
static FILE *
a0_stream_or_open(void)
{
   FILE *s;
   char  path[64];

   s = __atomic_load_n(&a0_stream, __ATOMIC_ACQUIRE);
   if (s)
      return s;

   pthread_mutex_lock(&a0_open_lock);
   s = a0_stream;
   if (!s) {
      snprintf(path, sizeof(path), "/tmp/terakan_descriptor_%d.jsonl",
               (int)getpid());
      s = fopen(path, "ae"); /* O_APPEND + FD_CLOEXEC */
      if (s)
         setvbuf(s, NULL, _IOLBF, 0); /* line-buffered for jq -c sanity */
      __atomic_store_n(&a0_stream, s, __ATOMIC_RELEASE);
   }
   pthread_mutex_unlock(&a0_open_lock);
   return s;
}

static uint64_t
a0_ts_nsec(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static pid_t
a0_gettid(void)
{
   return (pid_t)syscall(SYS_gettid);
}

static void
a0_print_dword_array(FILE *s, const uint32_t *dw, unsigned n)
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
   s = a0_stream_or_open();
   if (!s)
      return;

   flockfile(s);
   fprintf(s,
           "{\"event\":\"descriptor_image_view\","
           "\"ts_nsec\":%" PRIu64 ","
           "\"pid\":%d,"
           "\"tid\":%d,"
           "\"handle\":\"0x%" PRIx64 "\","
           "\"image_handle\":\"0x%" PRIx64 "\","
           "\"format\":%d,"
           "\"view_type\":%d,"
           "\"resource\":[",
           a0_ts_nsec(),
           (int)getpid(),
           (int)a0_gettid(),
           (uint64_t)(uintptr_t)view_handle,
           (uint64_t)(uintptr_t)create_info->image,
           (int)create_info->format,
           (int)create_info->viewType);
   a0_print_dword_array(s, view->resource, 8);
   fputs("],\"resource_gather\":[", s);
   a0_print_dword_array(s, view->resource_gather, 8);
   fputs("]}\n", s);
   funlockfile(s);
}

void
terakan_descriptor_dump_sampler(VkSampler sampler_handle,
                        struct terakan_sampler const *sampler)
{
   FILE *s;

   if (!terakan_descriptor_dump_active() || !sampler)
      return;
   s = a0_stream_or_open();
   if (!s)
      return;

   flockfile(s);
   fprintf(s,
           "{\"event\":\"descriptor_sampler\","
           "\"ts_nsec\":%" PRIu64 ","
           "\"pid\":%d,"
           "\"tid\":%d,"
           "\"handle\":\"0x%" PRIx64 "\","
           "\"unnormalized\":%s,"
           "\"sampler\":[",
           a0_ts_nsec(),
           (int)getpid(),
           (int)a0_gettid(),
           (uint64_t)(uintptr_t)sampler_handle,
           sampler->unnormalized_coordinates ? "true" : "false");
   a0_print_dword_array(s, sampler->sampler, 3);
   fputs("]}\n", s);
   funlockfile(s);
}
