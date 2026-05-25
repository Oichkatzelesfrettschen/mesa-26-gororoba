/*
 * Copyright © 2008 Jérôme Glisse
 * Copyright © 2010 Marek Olšák <maraeo@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */

/*
    This file replaces libdrm's radeon_cs_gem with our own implemention.
    It's optimized specifically for Radeon DRM.
    Adding buffers and space checking are faster and simpler than their
    counterparts in libdrm (the time complexity of all the functions
    is O(1) in nearly all scenarios, thanks to hashing).

    It works like this:

    cs_add_buffer(cs, buf, read_domain, write_domain) adds a new relocation and
    also adds the size of 'buf' to the used_gart and used_vram winsys variables
    based on the domains, which are simply or'd for the accounting purposes.
    The adding is skipped if the reloc is already present in the list, but it
    accounts any newly-referenced domains.

    cs_validate is then called, which just checks:
        used_vram/gart < vram/gart_size * 0.8
    The 0.8 number allows for some memory fragmentation. If the validation
    fails, the pipe driver flushes CS and tries do the validation again,
    i.e. it validates only that one operation. If it fails again, it drops
    the operation on the floor and prints some nasty message to stderr.
    (done in the pipe driver)

    cs_write_reloc(cs, buf) just writes a reloc that has been added using
    cs_add_buffer. The read_domain and write_domain parameters have been removed,
    because we already specify them in cs_add_buffer.
*/

#include "radeon_drm_cs.h"

#include "git_sha1.h"
#include "util/os_file.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/os_time.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>


#define RELOC_DWORDS (sizeof(struct drm_radeon_cs_reloc) / sizeof(uint32_t))
#define R300_TRACE_SCHEMA_VERSION 1
#define R300_TRACE_ENDIAN_MARKER 0x01020304u
#define R300_TRACE_SHA256_HEX_LEN 64

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct r300_trace_ib_header {
   char magic[8];
   uint32_t schema_version;
   uint32_t header_size;
   uint32_t endian_marker;
   uint32_t dword_count;
   uint32_t pci_id;
   uint32_t family;
   uint32_t drm_major;
   uint32_t drm_minor;
   uint32_t drm_patchlevel;
   char mesa_commit[64];
   char kernel_release[64];
   char run_id[64];
   uint32_t reserved[16];
};

struct r300_trace {
   bool enabled;
   char run_id[64];
   char dir[PATH_MAX];
   char kernel_release[64];
};

static int r300_trace_counter;

struct r300_trace_sha256 {
   uint32_t h[8];
   uint64_t bit_count;
   uint8_t block[64];
   size_t block_len;
};

static FILE *
r300_trace_fopen(const struct r300_trace *trace, const char *name,
                 const char *mode);

static struct pipe_fence_handle *radeon_cs_create_fence(struct radeon_cmdbuf *rcs);
static void radeon_fence_reference(struct radeon_winsys *ws,
                                   struct pipe_fence_handle **dst,
                                   struct pipe_fence_handle *src);

static const char *
r300_trace_option(const char *name, const char *default_value)
{
   return debug_get_option(name, default_value);
}

static bool
r300_trace_bool_option(const char *name, bool default_value)
{
   return debug_get_bool_option(name, default_value);
}

static bool
r300_trace_mask_has(const char *name)
{
   const char *mask = r300_trace_option("R300_TRACE_MASK", NULL);
   size_t name_len = strlen(name);
   const char *token;

   if (!mask || !mask[0])
      return true;

   token = mask;
   while (*token) {
      const char *end;
      while (*token == ',' || *token == ' ' || *token == '\t' ||
             *token == '\n' || *token == '\r')
         ++token;

      end = token;
      while (*end && *end != ',' && *end != ' ' && *end != '\t' &&
             *end != '\n' && *end != '\r')
         ++end;

      if ((size_t)(end - token) == 3 && !strncmp(token, "all", 3))
         return true;
      if ((size_t)(end - token) == name_len &&
          !strncmp(token, name, name_len))
         return true;

      token = end;
   }

   return false;
}

static void
r300_trace_json_string(FILE *file, const char *value)
{
   fputc('"', file);
   for (const unsigned char *p = (const unsigned char *)value; p && *p; ++p) {
      switch (*p) {
      case '"':
         fputs("\\\"", file);
         break;
      case '\\':
         fputs("\\\\", file);
         break;
      case '\b':
         fputs("\\b", file);
         break;
      case '\f':
         fputs("\\f", file);
         break;
      case '\n':
         fputs("\\n", file);
         break;
      case '\r':
         fputs("\\r", file);
         break;
      case '\t':
         fputs("\\t", file);
         break;
      default:
         if (*p < 0x20)
            fprintf(file, "\\u%04x", *p);
         else
            fputc(*p, file);
         break;
      }
   }
   fputc('"', file);
}

static uint32_t
r300_trace_rotr32(uint32_t value, unsigned bits)
{
   return (value >> bits) | (value << (32 - bits));
}

static void
r300_trace_sha256_transform(struct r300_trace_sha256 *ctx,
                            const uint8_t block[64])
{
   static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
      0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
      0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
      0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
   };
   uint32_t w[64];
   uint32_t a, b, c, d, e, f, g, h;

   for (unsigned i = 0; i < 16; ++i) {
      w[i] = ((uint32_t)block[i * 4] << 24) |
             ((uint32_t)block[i * 4 + 1] << 16) |
             ((uint32_t)block[i * 4 + 2] << 8) |
             block[i * 4 + 3];
   }

   for (unsigned i = 16; i < 64; ++i) {
      uint32_t s0 = r300_trace_rotr32(w[i - 15], 7) ^
                    r300_trace_rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = r300_trace_rotr32(w[i - 2], 17) ^
                    r300_trace_rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }

   a = ctx->h[0];
   b = ctx->h[1];
   c = ctx->h[2];
   d = ctx->h[3];
   e = ctx->h[4];
   f = ctx->h[5];
   g = ctx->h[6];
   h = ctx->h[7];

   for (unsigned i = 0; i < 64; ++i) {
      uint32_t s1 = r300_trace_rotr32(e, 6) ^ r300_trace_rotr32(e, 11) ^
                    r300_trace_rotr32(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = r300_trace_rotr32(a, 2) ^ r300_trace_rotr32(a, 13) ^
                    r300_trace_rotr32(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
   }

   ctx->h[0] += a;
   ctx->h[1] += b;
   ctx->h[2] += c;
   ctx->h[3] += d;
   ctx->h[4] += e;
   ctx->h[5] += f;
   ctx->h[6] += g;
   ctx->h[7] += h;
}

static void
r300_trace_sha256_init(struct r300_trace_sha256 *ctx)
{
   static const uint32_t initial[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
   };

   memcpy(ctx->h, initial, sizeof(ctx->h));
   ctx->bit_count = 0;
   ctx->block_len = 0;
}

static void
r300_trace_sha256_update(struct r300_trace_sha256 *ctx, const void *data,
                         size_t size)
{
   const uint8_t *bytes = data;

   ctx->bit_count += (uint64_t)size * 8;

   while (size) {
      size_t copy = MIN2(size, sizeof(ctx->block) - ctx->block_len);
      memcpy(ctx->block + ctx->block_len, bytes, copy);
      ctx->block_len += copy;
      bytes += copy;
      size -= copy;

      if (ctx->block_len == sizeof(ctx->block)) {
         r300_trace_sha256_transform(ctx, ctx->block);
         ctx->block_len = 0;
      }
   }
}

static void
r300_trace_sha256_final(struct r300_trace_sha256 *ctx,
                        uint8_t digest[32])
{
   uint64_t bit_count = ctx->bit_count;

   ctx->block[ctx->block_len++] = 0x80;
   if (ctx->block_len > 56) {
      memset(ctx->block + ctx->block_len, 0, sizeof(ctx->block) - ctx->block_len);
      r300_trace_sha256_transform(ctx, ctx->block);
      ctx->block_len = 0;
   }

   memset(ctx->block + ctx->block_len, 0, 56 - ctx->block_len);
   for (unsigned i = 0; i < 8; ++i)
      ctx->block[56 + i] = (uint8_t)(bit_count >> (56 - i * 8));
   r300_trace_sha256_transform(ctx, ctx->block);

   for (unsigned i = 0; i < 8; ++i) {
      digest[i * 4] = (uint8_t)(ctx->h[i] >> 24);
      digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
      digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
      digest[i * 4 + 3] = (uint8_t)ctx->h[i];
   }
}

static bool
r300_trace_sha256_file(const struct r300_trace *trace, const char *name,
                       char hex[R300_TRACE_SHA256_HEX_LEN + 1])
{
   struct r300_trace_sha256 ctx;
   uint8_t digest[32];
   uint8_t buf[4096];
   FILE *file = r300_trace_fopen(trace, name, "rb");
   static const char hexdigits[] = "0123456789abcdef";

   if (!file)
      return false;

   r300_trace_sha256_init(&ctx);
   for (;;) {
      size_t read_size = fread(buf, 1, sizeof(buf), file);
      if (read_size)
         r300_trace_sha256_update(&ctx, buf, read_size);
      if (read_size != sizeof(buf))
         break;
   }

   if (ferror(file)) {
      fclose(file);
      return false;
   }

   fclose(file);
   r300_trace_sha256_final(&ctx, digest);

   for (unsigned i = 0; i < sizeof(digest); ++i) {
      hex[i * 2] = hexdigits[digest[i] >> 4];
      hex[i * 2 + 1] = hexdigits[digest[i] & 0xf];
   }
   hex[R300_TRACE_SHA256_HEX_LEN] = 0;
   return true;
}

static bool
r300_trace_join_path(char *out, size_t out_size, const char *dir,
                     const char *name)
{
   int written = snprintf(out, out_size, "%s/%s", dir, name);
   return written > 0 && (size_t)written < out_size;
}

static bool
r300_trace_mkdir_one(const char *path)
{
   if (os_mkdir(path, 0775) && errno != EEXIST) {
      fprintf(stderr, "r300 trace: cannot create trace dir %s: %s\n",
              path, strerror(errno));
      return false;
   }

   return true;
}

static bool
r300_trace_mkdir_p(const char *path)
{
   char tmp[PATH_MAX];
   size_t len;

   if (!path || !path[0])
      return false;

   len = strlen(path);
   if (len >= sizeof(tmp)) {
      fprintf(stderr, "r300 trace: trace path too long\n");
      return false;
   }

   memcpy(tmp, path, len + 1);

   while (len > 1 && tmp[len - 1] == '/')
      tmp[--len] = 0;

   for (char *slash = tmp + 1; *slash; ++slash) {
      if (*slash != '/')
         continue;

      *slash = 0;
      if (!r300_trace_mkdir_one(tmp))
         return false;
      *slash = '/';
   }

   return r300_trace_mkdir_one(tmp);
}

static void
r300_trace_mesa_string(char *out, size_t out_size)
{
   snprintf(out, out_size, "%s+%s", PACKAGE_VERSION, MESA_GIT_SHA1);
}

static FILE *
r300_trace_fopen(const struct r300_trace *trace, const char *name,
                 const char *mode)
{
   char path[PATH_MAX];

   if (!r300_trace_join_path(path, sizeof(path), trace->dir, name))
      return NULL;

   return fopen(path, mode);
}

static void
r300_trace_fill_ib_header(struct radeon_drm_cs *cs,
                          const struct radeon_cs_context *csc,
                          const struct r300_trace *trace,
                          struct r300_trace_ib_header *header)
{
   memset(header, 0, sizeof(*header));
   memcpy(header->magic, "R3RKIB1", 7);
   header->schema_version = R300_TRACE_SCHEMA_VERSION;
   header->header_size = sizeof(*header);
   header->endian_marker = R300_TRACE_ENDIAN_MARKER;
   header->dword_count = csc->chunks[0].length_dw;
   header->pci_id = cs->ws->info.pci_id;
   header->family = cs->ws->info.family;
   header->drm_major = cs->ws->info.drm_major;
   header->drm_minor = cs->ws->info.drm_minor;
   header->drm_patchlevel = cs->ws->info.drm_patchlevel;
   r300_trace_mesa_string(header->mesa_commit, sizeof(header->mesa_commit));
   snprintf(header->kernel_release, sizeof(header->kernel_release), "%s",
            trace->kernel_release);
   snprintf(header->run_id, sizeof(header->run_id), "%s", trace->run_id);
}

static void
r300_trace_write_ib(struct radeon_drm_cs *cs,
                    const struct radeon_cs_context *csc,
                    const struct r300_trace *trace,
                    const char *name)
{
   struct r300_trace_ib_header header;
   FILE *file = r300_trace_fopen(trace, name, "wb");

   if (!file)
      return;

   r300_trace_fill_ib_header(cs, csc, trace, &header);
   fwrite(&header, sizeof(header), 1, file);
   fwrite(csc->buf, sizeof(uint32_t), csc->chunks[0].length_dw, file);
   fclose(file);
}

static void
r300_trace_write_bo_table(const struct radeon_cs_context *csc,
                          const struct r300_trace *trace)
{
   FILE *file = r300_trace_fopen(trace, "bo_table.json", "w");

   if (!file)
      return;

   fputs("[\n", file);
   for (unsigned i = 0; i < csc->num_relocs; ++i) {
      const struct radeon_bo *bo = csc->relocs_bo[i].bo;
      fprintf(file,
              "%s{\"reloc_index\":%u,\"handle\":%u,\"size\":%" PRIu64
              ",\"va\":%" PRIu64 ",\"priority_usage\":%u,"
              "\"initial_domain\":%u}",
              i ? ",\n" : "", i, bo ? bo->handle : 0,
              bo ? (uint64_t)bo->base.size : 0,
              bo ? bo->va : 0,
              csc->relocs_bo[i].u.real.priority_usage,
              bo ? bo->initial_domain : 0);
   }
   fputs("\n]\n", file);
   fclose(file);
}

static void
r300_trace_write_relocs(const struct radeon_cs_context *csc,
                        const struct r300_trace *trace)
{
   FILE *file = r300_trace_fopen(trace, "relocs.json", "w");

   if (!file)
      return;

   fputs("[\n", file);
   for (unsigned i = 0; i < csc->num_relocs; ++i) {
      const struct drm_radeon_cs_reloc *reloc = &csc->relocs[i];
      fprintf(file,
              "%s{\"reloc_index\":%u,\"handle\":%u,"
              "\"read_domains\":%u,\"write_domain\":%u,\"flags\":%u}",
              i ? ",\n" : "", i, reloc->handle, reloc->read_domains,
              reloc->write_domain, reloc->flags);
   }
   fputs("\n]\n", file);
   fclose(file);
}

static void
r300_trace_write_submit(struct radeon_drm_cs *cs,
                        const struct radeon_cs_context *csc,
                        const struct r300_trace *trace,
                        int ioctl_result)
{
   FILE *file = r300_trace_fopen(trace, "submit.json", "w");

   if (!file)
      return;

   fputs("{\n", file);
   fputs("  \"run_id\":", file);
   r300_trace_json_string(file, trace->run_id);
   fprintf(file,
           ",\n  \"schema_version\":%u,\n  \"ip_type\":%u,\n"
           "  \"flags0\":%u,\n  \"flags1\":%u,\n"
           "  \"num_chunks\":%u,\n  \"ib_dwords\":%u,\n"
           "  \"num_relocs\":%u,\n  \"ioctl_result\":%d\n}\n",
           R300_TRACE_SCHEMA_VERSION, cs->ip_type, csc->flags[0],
           csc->flags[1], csc->cs.num_chunks,
           csc->chunks[0].length_dw, csc->num_relocs, ioctl_result);
   fclose(file);
}

static void
r300_trace_manifest_artifact(FILE *file, const char *name, bool *first)
{
   if (!*first)
      fputc(',', file);
   r300_trace_json_string(file, name);
   *first = false;
}

static void
r300_trace_write_manifest(struct radeon_drm_cs *cs,
                          const struct radeon_cs_context *csc,
                          const struct r300_trace *trace,
                          bool ioctl_result_valid,
                          int ioctl_result,
                          bool patched_ib_available)
{
   FILE *file = r300_trace_fopen(trace, "manifest.json", "w");
   const char *mask = r300_trace_option("R300_TRACE_MASK", "all");
   char pre_hash[R300_TRACE_SHA256_HEX_LEN + 1] = {0};
   char patched_hash[R300_TRACE_SHA256_HEX_LEN + 1] = {0};
   bool pre_hash_valid = r300_trace_sha256_file(trace, "pre_ib.bin", pre_hash);
   bool patched_hash_valid =
      patched_ib_available &&
      r300_trace_sha256_file(trace, "patched_ib.bin", patched_hash);
   char mesa_string[128];
   bool first_artifact = true;

   if (!file)
      return;

   r300_trace_mesa_string(mesa_string, sizeof(mesa_string));

   fputs("{\n", file);
   fputs("  \"run_id\":", file);
   r300_trace_json_string(file, trace->run_id);
   fputs(",\n  \"mesa\":", file);
   r300_trace_json_string(file, mesa_string);
   fputs(",\n  \"mesa_version\":", file);
   r300_trace_json_string(file, PACKAGE_VERSION);
   fputs(",\n  \"mesa_git_sha1\":", file);
   r300_trace_json_string(file, MESA_GIT_SHA1);
   fputs(",\n  \"kernel_release\":", file);
   r300_trace_json_string(file, trace->kernel_release);
   fputs(",\n  \"trace_mask\":", file);
   r300_trace_json_string(file, mask ? mask : "all");
   fprintf(file,
           ",\n  \"schema_version\":%u,\n  \"pci_id\":\"0x%04x\",\n"
           "  \"family\":%u,\n  \"drm_version\":\"%u.%u.%u\",\n"
           "  \"ib_dwords\":%u,\n  \"num_relocs\":%u,\n"
           "  \"ioctl_result_valid\":%s,\n"
           "  \"ioctl_result\":%d,\n"
           "  \"patched_ib_available\":%s,\n",
           R300_TRACE_SCHEMA_VERSION, cs->ws->info.pci_id,
           cs->ws->info.family, cs->ws->info.drm_major,
           cs->ws->info.drm_minor, cs->ws->info.drm_patchlevel,
           csc->chunks[0].length_dw, csc->num_relocs,
           ioctl_result_valid ? "true" : "false",
           ioctl_result_valid ? ioctl_result : 0,
           patched_ib_available ? "true" : "false");
   fputs("  \"artifact_sha256\":{\n", file);
   fprintf(file, "    \"pre_ib.bin\":%s", pre_hash_valid ? "\"" : "null");
   if (pre_hash_valid) {
      fputs(pre_hash, file);
      fputc('"', file);
   }
   fputs(",\n", file);
   fprintf(file, "    \"patched_ib.bin\":%s", patched_hash_valid ? "\"" : "null");
   if (patched_hash_valid) {
      fputs(patched_hash, file);
      fputc('"', file);
   }
   fputs("\n  },\n", file);
   fputs("  \"artifacts\":[", file);
   if (r300_trace_mask_has("cs")) {
      r300_trace_manifest_artifact(file, "pre_ib.bin", &first_artifact);
      if (patched_ib_available)
         r300_trace_manifest_artifact(file, "patched_ib.bin", &first_artifact);
   }
   if (r300_trace_mask_has("reloc"))
      r300_trace_manifest_artifact(file, "relocs.json", &first_artifact);
   if (r300_trace_mask_has("bo"))
      r300_trace_manifest_artifact(file, "bo_table.json", &first_artifact);
   if (ioctl_result_valid && r300_trace_mask_has("submit"))
      r300_trace_manifest_artifact(file, "submit.json", &first_artifact);
   r300_trace_manifest_artifact(file, "stderr.txt", &first_artifact);
   fputs("]\n}\n", file);
   fclose(file);
}

static void
r300_trace_write_stderr_note(const struct r300_trace *trace)
{
   FILE *file = r300_trace_fopen(trace, "stderr.txt", "w");

   if (!file)
      return;

   fprintf(file, "r300 trace run %s\n", trace->run_id);
   fputs("Process stderr is captured by the outer runner.\n", file);
   fclose(file);
}

static bool
r300_trace_begin_trace(struct radeon_drm_cs *cs,
                       const struct radeon_cs_context *csc,
                       struct r300_trace *trace)
{
   static bool warned_missing_dir;
   const char *root;
   struct utsname uts;
   int counter;
   int written;

   memset(trace, 0, sizeof(*trace));

   if (cs->ws->gen != DRV_R300 ||
       !r300_trace_bool_option("R300_TRACE", false))
      return false;

   root = r300_trace_option("R300_TRACE_DIR", NULL);
   if (!root || !root[0]) {
      if (!warned_missing_dir) {
         fprintf(stderr,
                 "r300 trace: R300_TRACE_DIR is required; tracing disabled\n");
         warned_missing_dir = true;
      }
      return false;
   }

   if (!r300_trace_mkdir_p(root))
      return false;

   if (uname(&uts) == 0)
      snprintf(trace->kernel_release, sizeof(trace->kernel_release), "%s",
               uts.release);
   else
      snprintf(trace->kernel_release, sizeof(trace->kernel_release), "unknown");

   counter = p_atomic_inc_return(&r300_trace_counter);
   snprintf(trace->run_id, sizeof(trace->run_id), "%lld-%ld-%d",
            (long long)time(NULL), (long)getpid(), counter);

   written = snprintf(trace->dir, sizeof(trace->dir), "%s/%s", root,
                      trace->run_id);
   if (written <= 0 || (size_t)written >= sizeof(trace->dir)) {
      fprintf(stderr, "r300 trace: trace path too long\n");
      return false;
   }

   if (!r300_trace_mkdir_p(trace->dir))
      return false;

   trace->enabled = true;

   if (r300_trace_mask_has("cs"))
      r300_trace_write_ib(cs, csc, trace, "pre_ib.bin");
   if (r300_trace_mask_has("bo"))
      r300_trace_write_bo_table(csc, trace);
   if (r300_trace_mask_has("reloc"))
      r300_trace_write_relocs(csc, trace);
   if (r300_trace_mask_has("submit"))
      r300_trace_write_manifest(cs, csc, trace, false, 0, false);
   r300_trace_write_stderr_note(trace);

   return true;
}

static void
r300_trace_finish_trace(struct radeon_drm_cs *cs,
                        const struct radeon_cs_context *csc,
                        const struct r300_trace *trace,
                        int ioctl_result)
{
   if (!trace->enabled)
      return;

   if (!ioctl_result && r300_trace_mask_has("cs"))
      r300_trace_write_ib(cs, csc, trace, "patched_ib.bin");
   if (r300_trace_mask_has("submit"))
      r300_trace_write_submit(cs, csc, trace, ioctl_result);
   if (r300_trace_mask_has("submit"))
      r300_trace_write_manifest(cs, csc, trace, true, ioctl_result,
                                ioctl_result == 0 &&
                                r300_trace_mask_has("cs"));
}

static struct radeon_winsys_ctx *radeon_drm_ctx_create(struct radeon_winsys *ws,
                                                       unsigned flags)
{
   struct radeon_ctx *ctx = CALLOC_STRUCT(radeon_ctx);
   if (!ctx)
      return NULL;

   ctx->ws = (struct radeon_drm_winsys*)ws;
   ctx->gpu_reset_counter = radeon_drm_get_gpu_reset_counter(ctx->ws);
   return (struct radeon_winsys_ctx*)ctx;
}

static void radeon_drm_ctx_destroy(struct radeon_winsys_ctx *ctx)
{
   FREE(ctx);
}

static void
radeon_drm_ctx_set_sw_reset_status(struct radeon_winsys_ctx *rwctx, enum pipe_reset_status status,
                                   const char *format, ...)
{
   /* TODO: we should do something better here */
   va_list args;

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);
}

static enum pipe_reset_status
radeon_drm_ctx_query_reset_status(struct radeon_winsys_ctx *rctx, bool full_reset_only,
                                  bool *needs_reset, bool *reset_completed)
{
   struct radeon_ctx *ctx = (struct radeon_ctx*)rctx;

   unsigned latest = radeon_drm_get_gpu_reset_counter(ctx->ws);

   if (ctx->gpu_reset_counter == latest) {
      if (needs_reset)
         *needs_reset = false;
      if (reset_completed)
         *reset_completed = false;
      return PIPE_NO_RESET;
   }

   if (needs_reset)
      *needs_reset = true;
   if (reset_completed)
      *reset_completed = true;

   ctx->gpu_reset_counter = latest;
   return PIPE_UNKNOWN_CONTEXT_RESET;
}

static bool radeon_init_cs_context(struct radeon_cs_context *csc,
                                   struct radeon_drm_winsys *ws)
{

   csc->fd = ws->fd;

   csc->chunks[0].chunk_id = RADEON_CHUNK_ID_IB;
   csc->chunks[0].length_dw = 0;
   csc->chunks[0].chunk_data = (uint64_t)(uintptr_t)csc->buf;
   csc->chunks[1].chunk_id = RADEON_CHUNK_ID_RELOCS;
   csc->chunks[1].length_dw = 0;
   csc->chunks[1].chunk_data = (uint64_t)(uintptr_t)csc->relocs;
   csc->chunks[2].chunk_id = RADEON_CHUNK_ID_FLAGS;
   csc->chunks[2].length_dw = 2;
   csc->chunks[2].chunk_data = (uint64_t)(uintptr_t)&csc->flags;

   csc->chunk_array[0] = (uint64_t)(uintptr_t)&csc->chunks[0];
   csc->chunk_array[1] = (uint64_t)(uintptr_t)&csc->chunks[1];
   csc->chunk_array[2] = (uint64_t)(uintptr_t)&csc->chunks[2];

   csc->cs.chunks = (uint64_t)(uintptr_t)csc->chunk_array;

   /* Use memset for bulk init */
   memset(csc->reloc_indices_hashlist, 0xff, sizeof(csc->reloc_indices_hashlist));
   return true;
}

static void radeon_cs_context_cleanup(struct radeon_winsys *rws,
                                      struct radeon_cs_context *csc)
{
   unsigned i;

   for (i = 0; i < csc->num_relocs; i++) {
      p_atomic_dec(&csc->relocs_bo[i].bo->num_cs_references);
      radeon_ws_bo_reference(rws, &csc->relocs_bo[i].bo, NULL);
   }
   for (i = 0; i < csc->num_slab_buffers; ++i) {
      p_atomic_dec(&csc->slab_buffers[i].bo->num_cs_references);
      radeon_ws_bo_reference(rws, &csc->slab_buffers[i].bo, NULL);
   }

   csc->num_relocs = 0;
   csc->num_validated_relocs = 0;
   csc->num_slab_buffers = 0;
   csc->chunks[0].length_dw = 0;
   csc->chunks[1].length_dw = 0;

   /* Selective hash clear: only reset entries that were actually used.
    * For typical frames with 3-10 BOs, this clears ~10 entries instead of 4096. */
   for (i = 0; i < csc->num_used_hash_slots; i++) {
      csc->reloc_indices_hashlist[csc->used_hash_slots[i]] = -1;
   }
   csc->num_used_hash_slots = 0;
}

static void radeon_destroy_cs_context(struct radeon_winsys *rws, struct radeon_cs_context *csc)
{
   radeon_cs_context_cleanup(rws, csc);
   FREE(csc->slab_buffers);
   FREE(csc->relocs_bo);
   FREE(csc->relocs);
}

static bool
radeon_drm_cs_create(struct radeon_cmdbuf *rcs,
                     struct radeon_winsys_ctx *ctx,
                     enum amd_ip_type ip_type,
                     void (*flush)(void *ctx, unsigned flags,
                                   struct pipe_fence_handle **fence),
                     void *flush_ctx)
{
   struct radeon_drm_winsys *ws = ((struct radeon_ctx*)ctx)->ws;
   struct radeon_drm_cs *cs;

   cs = CALLOC_STRUCT(radeon_drm_cs);
   if (!cs) {
      return false;
   }
   for (int i = 0; i < 3; i++)
      util_queue_fence_init(&cs->flush_completed[i]);

   cs->ws = ws;
   cs->flush_cs = flush;
   cs->flush_data = flush_ctx;

   for (int i = 0; i < 3; i++) {
      if (!radeon_init_cs_context(&cs->csc[i], cs->ws)) {
         for (int j = 0; j < i; j++)
            radeon_destroy_cs_context(&ws->base, &cs->csc[j]);
         FREE(cs);
         return false;
      }
   }

   /* Triple-buffer: start filling csc[0], cst points to csc[1].
    * csc[2] is the extra buffer that eliminates the fence stall. */
   cs->csc_fill_idx = 0;
   cs->csc_cur = &cs->csc[0];
   cs->cst = &cs->csc[1];
   cs->ip_type = ip_type;

   memset(rcs, 0, sizeof(*rcs));
   rcs->current.buf = cs->csc_cur->buf;
   rcs->current.max_dw = ARRAY_SIZE(cs->csc_cur->buf);
   rcs->priv = cs;

   p_atomic_inc(&ws->num_cs);
   return true;
}

int radeon_lookup_buffer(struct radeon_winsys *rws, struct radeon_cs_context *csc,
                         struct radeon_bo *bo)
{
   unsigned hash = bo->hash & (ARRAY_SIZE(csc->reloc_indices_hashlist)-1);
   struct radeon_bo_item *buffers;
   unsigned num_buffers;
   int i = csc->reloc_indices_hashlist[hash];

   if (bo->handle) {
      buffers = csc->relocs_bo;
      num_buffers = csc->num_relocs;
   } else {
      buffers = csc->slab_buffers;
      num_buffers = csc->num_slab_buffers;
   }

   /* not found or found */
   if (i == -1 || (i < num_buffers && buffers[i].bo == bo))
      return i;

   /* Hash collision, look for the BO in the list of relocs linearly. */
   for (i = num_buffers - 1; i >= 0; i--) {
      if (buffers[i].bo == bo) {
         /* Put this reloc in the hash list.
          * This will prevent additional hash collisions if there are
          * several consecutive lookup_buffer calls for the same buffer.
          *
          * Example: Assuming buffers A,B,C collide in the hash list,
          * the following sequence of relocs:
          *         AAAAAAAAAAABBBBBBBBBBBBBBCCCCCCCC
          * will collide here: ^ and here:   ^,
          * meaning that we should get very few collisions in the end. */
         csc->reloc_indices_hashlist[hash] = i;
         return i;
      }
   }
   return -1;
}

static unsigned radeon_lookup_or_add_real_buffer(struct radeon_drm_cs *cs,
                                                 struct radeon_bo *bo)
{
   struct radeon_cs_context *csc = cs->csc_cur;
   struct drm_radeon_cs_reloc *reloc;
   unsigned hash = bo->hash & (ARRAY_SIZE(csc->reloc_indices_hashlist)-1);
   int i = -1;

   i = radeon_lookup_buffer(&cs->ws->base, csc, bo);

   if (i >= 0) {
      /* For async DMA, every add_buffer call must add a buffer to the list
       * no matter how many duplicates there are. This is due to the fact
       * the DMA CS checker doesn't use NOP packets for offset patching,
       * but always uses the i-th buffer from the list to patch the i-th
       * offset. If there are N offsets in a DMA CS, there must also be N
       * buffers in the relocation list.
       *
       * This doesn't have to be done if virtual memory is enabled,
       * because there is no offset patching with virtual memory.
       */
      if (cs->ip_type != AMD_IP_SDMA || cs->ws->info.r600_has_virtual_memory) {
         return i;
      }
   }

   /* New relocation, check if the backing array is large enough. */
   if (csc->num_relocs >= csc->max_relocs) {
      uint32_t size;
      csc->max_relocs = MAX2(csc->max_relocs + 16, (unsigned)(csc->max_relocs * 1.3));

      size = csc->max_relocs * sizeof(csc->relocs_bo[0]);
      csc->relocs_bo = realloc(csc->relocs_bo, size);

      size = csc->max_relocs * sizeof(struct drm_radeon_cs_reloc);
      csc->relocs = realloc(csc->relocs, size);

      csc->chunks[1].chunk_data = (uint64_t)(uintptr_t)csc->relocs;
   }

   /* Initialize the new relocation. */
   csc->relocs_bo[csc->num_relocs].bo = NULL;
   csc->relocs_bo[csc->num_relocs].u.real.priority_usage = 0;
   radeon_ws_bo_reference(&cs->ws->base, &csc->relocs_bo[csc->num_relocs].bo, bo);
   p_atomic_inc(&bo->num_cs_references);
   reloc = &csc->relocs[csc->num_relocs];
   reloc->handle = bo->handle;
   reloc->read_domains = 0;
   reloc->write_domain = 0;
   reloc->flags = 0;

   csc->reloc_indices_hashlist[hash] = csc->num_relocs;
   if (csc->num_used_hash_slots < ARRAY_SIZE(csc->used_hash_slots))
      csc->used_hash_slots[csc->num_used_hash_slots++] = hash;

   csc->chunks[1].length_dw += RELOC_DWORDS;

   return csc->num_relocs++;
}

static int radeon_lookup_or_add_slab_buffer(struct radeon_drm_cs *cs,
                                            struct radeon_bo *bo)
{
   struct radeon_cs_context *csc = cs->csc_cur;
   unsigned hash;
   struct radeon_bo_item *item;
   int idx;
   int real_idx;

   idx = radeon_lookup_buffer(&cs->ws->base, csc, bo);
   if (idx >= 0)
      return idx;

   real_idx = radeon_lookup_or_add_real_buffer(cs, bo->u.slab.real);

   /* Check if the backing array is large enough. */
   if (csc->num_slab_buffers >= csc->max_slab_buffers) {
      unsigned new_max = MAX2(csc->max_slab_buffers + 16,
                              (unsigned)(csc->max_slab_buffers * 1.3));
      struct radeon_bo_item *new_buffers =
            REALLOC(csc->slab_buffers,
                    csc->max_slab_buffers * sizeof(*new_buffers),
                    new_max * sizeof(*new_buffers));
      if (!new_buffers) {
         fprintf(stderr, "radeon_lookup_or_add_slab_buffer: allocation failure\n");
         return -1;
      }

      csc->max_slab_buffers = new_max;
      csc->slab_buffers = new_buffers;
   }

   /* Initialize the new relocation. */
   idx = csc->num_slab_buffers++;
   item = &csc->slab_buffers[idx];

   item->bo = NULL;
   item->u.slab.real_idx = real_idx;
   radeon_ws_bo_reference(&cs->ws->base, &item->bo, bo);
   p_atomic_inc(&bo->num_cs_references);

   hash = bo->hash & (ARRAY_SIZE(csc->reloc_indices_hashlist)-1);
   csc->reloc_indices_hashlist[hash] = idx;

   return idx;
}

static unsigned radeon_drm_cs_add_buffer(struct radeon_cmdbuf *rcs,
                                         struct pb_buffer_lean *buf,
                                         unsigned usage,
                                         enum radeon_bo_domain domains)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);
   struct radeon_bo *bo = (struct radeon_bo*)buf;
   enum radeon_bo_domain added_domains;

   /* If VRAM is just stolen system memory, allow both VRAM and
    * GTT, whichever has free space. If a buffer is evicted from
    * VRAM to GTT, it will stay there.
    */
   if (!cs->ws->info.has_dedicated_vram)
      domains |= RADEON_DOMAIN_GTT;

   enum radeon_bo_domain rd = usage & RADEON_USAGE_READ ? domains : 0;
   enum radeon_bo_domain wd = usage & RADEON_USAGE_WRITE ? domains : 0;
   struct drm_radeon_cs_reloc *reloc;
   int index;

   if (!bo->handle) {
      index = radeon_lookup_or_add_slab_buffer(cs, bo);
      if (index < 0)
         return 0;

      index = cs->csc_cur->slab_buffers[index].u.slab.real_idx;
   } else {
      index = radeon_lookup_or_add_real_buffer(cs, bo);
   }

   reloc = &cs->csc_cur->relocs[index];
   added_domains = (rd | wd) & ~(reloc->read_domains | reloc->write_domain);
   reloc->read_domains |= rd;
   reloc->write_domain |= wd;

   /* The priority must be in [0, 15]. It's used by the kernel memory management. */
   unsigned priority = usage & RADEON_ALL_PRIORITIES;
   unsigned bo_priority = util_last_bit(priority) / 2;
   reloc->flags = MAX2(reloc->flags, bo_priority);
   cs->csc_cur->relocs_bo[index].u.real.priority_usage |= priority;

   if (added_domains & RADEON_DOMAIN_VRAM)
      rcs->used_vram_kb += bo->base.size / 1024;
   else if (added_domains & RADEON_DOMAIN_GTT)
      rcs->used_gart_kb += bo->base.size / 1024;

   return index;
}

static int radeon_drm_cs_lookup_buffer(struct radeon_cmdbuf *rcs,
                                       struct pb_buffer_lean *buf)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);

   return radeon_lookup_buffer(&cs->ws->base, cs->csc_cur, (struct radeon_bo*)buf);
}

static bool radeon_drm_cs_validate(struct radeon_cmdbuf *rcs)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);
   bool status =
         rcs->used_gart_kb < cs->ws->info.gart_size_kb * 0.8 &&
         rcs->used_vram_kb < cs->ws->info.vram_size_kb * 0.8;

   if (status) {
      cs->csc_cur->num_validated_relocs = cs->csc_cur->num_relocs;
   } else {
      /* Remove lately-added buffers. The validation failed with them
       * and the CS is about to be flushed because of that. Keep only
       * the already-validated buffers. */
      unsigned i;

      for (i = cs->csc_cur->num_validated_relocs; i < cs->csc_cur->num_relocs; i++) {
         p_atomic_dec(&cs->csc_cur->relocs_bo[i].bo->num_cs_references);
         radeon_ws_bo_reference(&cs->ws->base, &cs->csc_cur->relocs_bo[i].bo, NULL);
      }
      cs->csc_cur->num_relocs = cs->csc_cur->num_validated_relocs;

      /* Flush if there are any relocs. Clean up otherwise. */
      if (cs->csc_cur->num_relocs) {
         cs->flush_cs(cs->flush_data,
                      RADEON_FLUSH_ASYNC_START_NEXT_GFX_IB_NOW, NULL);
      } else {
         radeon_cs_context_cleanup(&cs->ws->base, cs->csc_cur);
         rcs->used_vram_kb = 0;
         rcs->used_gart_kb = 0;

         assert(rcs->current.cdw == 0);
         if (rcs->current.cdw != 0) {
            fprintf(stderr, "radeon: Unexpected error in %s.\n", __func__);
         }
      }
   }
   return status;
}

static bool radeon_drm_cs_check_space(struct radeon_cmdbuf *rcs, unsigned dw)
{
   assert(rcs->current.cdw <= rcs->current.max_dw);
   return rcs->current.max_dw - rcs->current.cdw >= dw;
}

static unsigned radeon_drm_cs_get_buffer_list(struct radeon_cmdbuf *rcs,
                                              struct radeon_bo_list_item *list)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);
   int i;

   if (list) {
      for (i = 0; i < cs->csc_cur->num_relocs; i++) {
         list[i].bo_size = cs->csc_cur->relocs_bo[i].bo->base.size;
         list[i].vm_address = cs->csc_cur->relocs_bo[i].bo->va;
         list[i].priority_usage = cs->csc_cur->relocs_bo[i].u.real.priority_usage;
      }
   }
   return cs->csc_cur->num_relocs;
}

void radeon_drm_cs_emit_ioctl_oneshot(void *job, void *gdata, int thread_index)
{
   struct radeon_drm_cs *cs = (struct radeon_drm_cs*)job;
   struct radeon_cs_context *csc = cs->cst;
   struct r300_trace trace;
   unsigned i;
   int r;

   /* Pre-submit IB dump for compute debug */
   if (debug_get_bool_option("RADEON_DUMP_PRE_IB", false)) {
      fprintf(stderr, "PRE_IB cdw=%u relocs=%u ring=%u\n",
              csc->chunks[0].length_dw, csc->num_relocs, csc->flags[1]);
      for (unsigned _j = 0; _j < csc->chunks[0].length_dw; _j++)
         fprintf(stderr, "IB[%u] 0x%08x\n", _j, csc->buf[_j]);
      fflush(stderr);
   }

   r300_trace_begin_trace(cs, csc, &trace);

   r = drmCommandWriteRead(csc->fd, DRM_RADEON_CS,
                           &csc->cs, sizeof(struct drm_radeon_cs));

   r300_trace_finish_trace(cs, csc, &trace, r);

   /* Post-reloc IB dump: after the kernel patches relocations,
    * csc->buf contains the actual GPU-visible register values.
    * Enable with RADEON_DUMP_PATCHED_IB=1 */
   if (!r && debug_get_bool_option("RADEON_DUMP_PATCHED_IB", false)) {
      unsigned _j;
      fprintf(stderr, "PATCHED_IB cdw=%u relocs=%u ring=%u\n",
              csc->chunks[0].length_dw, csc->num_relocs, csc->flags[1]);
      for (_j = 0; _j < csc->chunks[0].length_dw; _j++)
         fprintf(stderr, "PIB %u 0x%08x\n", _j, csc->buf[_j]);
   }

   if (r) {
      if (r == -ENOMEM)
         fprintf(stderr, "radeon: Not enough memory for command submission.\n");
      else if (debug_get_bool_option("RADEON_DUMP_CS", false)) {
         unsigned i;

         fprintf(stderr, "radeon: The kernel rejected CS, dumping...\n");
         for (i = 0; i < csc->chunks[0].length_dw; i++) {
            fprintf(stderr, "0x%08X\n", csc->buf[i]);
         }
      } else {
         fprintf(stderr, "radeon: The kernel rejected CS, "
                         "see dmesg for more information (%i).\n", r);
      }
   }

   for (i = 0; i < csc->num_relocs; i++)
      p_atomic_dec(&csc->relocs_bo[i].bo->num_active_ioctls);
   for (i = 0; i < csc->num_slab_buffers; i++)
      p_atomic_dec(&csc->slab_buffers[i].bo->num_active_ioctls);

   radeon_cs_context_cleanup(&cs->ws->base, csc);
}

/*
 * Drain all in-flight ioctls across the triple-buffer ring.
 */
void radeon_drm_cs_sync_flush(struct radeon_cmdbuf *rcs)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);

   if (util_queue_is_initialized(&cs->ws->cs_queue)) {
      for (int i = 0; i < 3; i++)
         util_queue_fence_wait(&cs->flush_completed[i]);
   }
}

/* Add the given fence to a slab buffer fence list.
 *
 * There is a potential race condition when bo participates in submissions on
 * two or more threads simultaneously. Since we do not know which of the
 * submissions will be sent to the GPU first, we have to keep the fences
 * of all submissions.
 *
 * However, fences that belong to submissions that have already returned from
 * their respective ioctl do not have to be kept, because we know that they
 * will signal earlier.
 */
static void radeon_bo_slab_fence(struct radeon_winsys *rws, struct radeon_bo *bo,
                                 struct radeon_bo *fence)
{
   unsigned dst;

   assert(fence->num_cs_references);

   /* Cleanup older fences */
   dst = 0;
   for (unsigned src = 0; src < bo->u.slab.num_fences; ++src) {
      if (bo->u.slab.fences[src]->num_cs_references) {
         bo->u.slab.fences[dst] = bo->u.slab.fences[src];
         dst++;
      } else {
         radeon_ws_bo_reference(rws, &bo->u.slab.fences[src], NULL);
      }
   }
   bo->u.slab.num_fences = dst;

   /* Check available space for the new fence */
   if (bo->u.slab.num_fences >= bo->u.slab.max_fences) {
      unsigned new_max_fences = bo->u.slab.max_fences + 1;
      struct radeon_bo **new_fences = REALLOC(bo->u.slab.fences,
                                              bo->u.slab.max_fences * sizeof(*new_fences),
                                              new_max_fences * sizeof(*new_fences));
      if (!new_fences) {
         fprintf(stderr, "radeon_bo_slab_fence: allocation failure, dropping fence\n");
         return;
      }

      bo->u.slab.fences = new_fences;
      bo->u.slab.max_fences = new_max_fences;
   }

   /* Add the new fence */
   bo->u.slab.fences[bo->u.slab.num_fences] = NULL;
   radeon_ws_bo_reference(rws, &bo->u.slab.fences[bo->u.slab.num_fences], fence);
   bo->u.slab.num_fences++;
}

static int radeon_drm_cs_flush(struct radeon_cmdbuf *rcs,
                               unsigned flags,
                               struct pipe_fence_handle **pfence)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);

   switch (cs->ip_type) {
   case AMD_IP_SDMA:
      /* pad DMA ring to 8 DWs */
      if (cs->ws->info.gfx_level <= GFX6) {
         while (rcs->current.cdw & 7)
            radeon_emit(rcs, 0xf0000000); /* NOP packet */
      } else {
         while (rcs->current.cdw & 7)
            radeon_emit(rcs, 0x00000000); /* NOP packet */
      }
      break;
   case AMD_IP_GFX:
      /* pad GFX ring to 8 DWs to meet CP fetch alignment requirements
       * r6xx, requires at least 4 dw alignment to avoid a hw bug.
       */
      if (cs->ws->info.gfx_ib_pad_with_type2) {
         while (rcs->current.cdw & 7)
            radeon_emit(rcs, 0x80000000); /* type2 nop packet */
      } else {
         while (rcs->current.cdw & 7)
            radeon_emit(rcs, 0xffff1000); /* type3 nop packet */
      }
      break;
   case AMD_IP_UVD:
      while (rcs->current.cdw & 15)
         radeon_emit(rcs, 0x80000000); /* type2 nop packet */
      break;
   default:
      break;
   }

   if (rcs->current.cdw > rcs->current.max_dw) {
      fprintf(stderr, "radeon: command stream overflowed\n");
   }

   if (pfence || cs->csc_cur->num_slab_buffers) {
      struct pipe_fence_handle *fence;

      if (cs->next_fence) {
         fence = cs->next_fence;
         cs->next_fence = NULL;
      } else {
         fence = radeon_cs_create_fence(rcs);
      }

      if (fence) {
         if (pfence)
            radeon_fence_reference(&cs->ws->base, pfence, fence);

         mtx_lock(&cs->ws->bo_fence_lock);
         for (unsigned i = 0; i < cs->csc_cur->num_slab_buffers; ++i) {
            struct radeon_bo *bo = cs->csc_cur->slab_buffers[i].bo;
            p_atomic_inc(&bo->num_active_ioctls);
            radeon_bo_slab_fence(&cs->ws->base, bo, (struct radeon_bo *)fence);
         }
         mtx_unlock(&cs->ws->bo_fence_lock);

         radeon_fence_reference(&cs->ws->base, &fence, NULL);
      }
   } else {
      radeon_fence_reference(&cs->ws->base, &cs->next_fence, NULL);
   }

   /* Triple-buffer rotation: wait ONLY on the buffer we are about to
    * reuse (from 2 submissions ago), NOT the one we just filled.
    * This is the key to actual pipeline overlap. */
   uint8_t submit_idx = cs->csc_fill_idx;
   {
      uint8_t next_fill = (submit_idx + 1) % 3;

      /* Wait for csc[next_fill]'s ioctl to complete -- it may still
       * be in-flight from two submissions ago.  We do NOT wait for
       * csc[submit_idx] (just filled), allowing the GPU to execute
       * it concurrently while we build the next IB. */
      if (util_queue_is_initialized(&cs->ws->cs_queue))
         util_queue_fence_wait(&cs->flush_completed[next_fill]);

      cs->cst = &cs->csc[submit_idx];
      cs->csc_fill_idx = next_fill;
      cs->csc_cur = &cs->csc[next_fill];
   }

   /* If the CS is not empty or overflowed, emit it in a separate thread. */
   if (rcs->current.cdw && rcs->current.cdw <= rcs->current.max_dw &&
       !cs->ws->noop_cs && !(flags & RADEON_FLUSH_NOOP)) {
      unsigned i, num_relocs;

      num_relocs = cs->cst->num_relocs;

      cs->cst->chunks[0].length_dw = rcs->current.cdw;

      for (i = 0; i < num_relocs; i++) {
         /* Update the number of active asynchronous CS ioctls for the buffer. */
         p_atomic_inc(&cs->cst->relocs_bo[i].bo->num_active_ioctls);
      }

      switch (cs->ip_type) {
      case AMD_IP_SDMA:
         cs->cst->flags[0] = 0;
         cs->cst->flags[1] = RADEON_CS_RING_DMA;
         cs->cst->cs.num_chunks = 3;
         if (cs->ws->info.r600_has_virtual_memory) {
            cs->cst->flags[0] |= RADEON_CS_USE_VM;
         }
         break;

      case AMD_IP_UVD:
         cs->cst->flags[0] = 0;
         cs->cst->flags[1] = RADEON_CS_RING_UVD;
         cs->cst->cs.num_chunks = 3;
         break;

      case AMD_IP_VCE:
         cs->cst->flags[0] = 0;
         cs->cst->flags[1] = RADEON_CS_RING_VCE;
         cs->cst->cs.num_chunks = 3;
         break;

      default:
      case AMD_IP_GFX:
      case AMD_IP_COMPUTE:
         cs->cst->flags[0] = RADEON_CS_KEEP_TILING_FLAGS;
         cs->cst->flags[1] = RADEON_CS_RING_GFX;
         cs->cst->cs.num_chunks = 3;

         if (cs->ws->info.r600_has_virtual_memory) {
            cs->cst->flags[0] |= RADEON_CS_USE_VM;
            cs->cst->cs.num_chunks = 3;
         }
         if (flags & PIPE_FLUSH_END_OF_FRAME) {
            cs->cst->flags[0] |= RADEON_CS_END_OF_FRAME;
            cs->cst->cs.num_chunks = 3;
         }
         if (cs->ip_type == AMD_IP_COMPUTE) {
            cs->cst->flags[1] = RADEON_CS_RING_COMPUTE;
            cs->cst->cs.num_chunks = 3;
         }
         break;
      }

      if (util_queue_is_initialized(&cs->ws->cs_queue)) {
         util_queue_add_job(&cs->ws->cs_queue, cs, &cs->flush_completed[submit_idx],
                            radeon_drm_cs_emit_ioctl_oneshot, NULL, 0);
         if (!(flags & PIPE_FLUSH_ASYNC))
            radeon_drm_cs_sync_flush(rcs);
      } else {
         radeon_drm_cs_emit_ioctl_oneshot(cs, NULL, 0);
      }
   } else {
      /* A valid RADEON_NOOP or RADEON_FLUSH_NOOP stream never reaches
       * DRM_RADEON_CS, so capture its pre-ioctl IB here for decoder-shape
       * preflight.  The submit path sets chunks[0].length_dw before emit; this
       * path must do the same before r300_trace_write_ib copies the buffer.
       * Overflowed command streams are excluded by the submit-path bounds check
       * so trace capture never reads past the IB buffer.  Without a real ioctl,
       * the manifest keeps ioctl_result_valid=false and
       * patched_ib_available=false, and omits submit.json.  begin_trace
       * self-gates on DRV_R300 + R300_TRACE, so this is a no-op for every other
       * winsys and when tracing is off. */
      if (rcs->current.cdw && rcs->current.cdw <= rcs->current.max_dw) {
         struct r300_trace trace;
         cs->cst->chunks[0].length_dw = rcs->current.cdw;
         r300_trace_begin_trace(cs, cs->cst, &trace);
      }
      radeon_cs_context_cleanup(&cs->ws->base, cs->cst);
   }

   /* Prepare a new CS. */
   rcs->current.buf = cs->csc_cur->buf;
   rcs->current.cdw = 0;
   rcs->used_vram_kb = 0;
   rcs->used_gart_kb = 0;

   if (cs->ip_type == AMD_IP_GFX)
      cs->ws->num_gfx_IBs++;
   else if (cs->ip_type == AMD_IP_SDMA)
      cs->ws->num_sdma_IBs++;
   return 0;
}

static void radeon_drm_cs_destroy(struct radeon_cmdbuf *rcs)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);

   if (!cs)
      return;

   radeon_drm_cs_sync_flush(rcs);
   for (int i = 0; i < 3; i++)
      util_queue_fence_destroy(&cs->flush_completed[i]);
   for (int i = 0; i < 3; i++)
      radeon_cs_context_cleanup(&cs->ws->base, &cs->csc[i]);
   p_atomic_dec(&cs->ws->num_cs);
   for (int i = 0; i < 3; i++)
      radeon_destroy_cs_context(&cs->ws->base, &cs->csc[i]);
   radeon_fence_reference(&cs->ws->base, &cs->next_fence, NULL);
   FREE(cs);
}

static bool radeon_bo_is_referenced(struct radeon_cmdbuf *rcs,
                                    struct pb_buffer_lean *_buf,
                                    unsigned usage)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);
   struct radeon_bo *bo = (struct radeon_bo*)_buf;
   int index;

   if (!bo->num_cs_references)
      return false;

   index = radeon_lookup_buffer(&cs->ws->base, cs->csc_cur, bo);
   if (index == -1)
      return false;

   if (!bo->handle)
      index = cs->csc_cur->slab_buffers[index].u.slab.real_idx;

   if ((usage & RADEON_USAGE_WRITE) && cs->csc_cur->relocs[index].write_domain)
      return true;
   if ((usage & RADEON_USAGE_READ) && cs->csc_cur->relocs[index].read_domains)
      return true;

   return false;
}

/* FENCES */

static struct pipe_fence_handle *radeon_cs_create_fence(struct radeon_cmdbuf *rcs)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);
   struct pb_buffer_lean *fence;

   /* Create a fence, which is a dummy BO. */
   fence = cs->ws->base.buffer_create(&cs->ws->base, 1, 1,
                                      RADEON_DOMAIN_GTT,
                                      RADEON_FLAG_NO_SUBALLOC
                                      | RADEON_FLAG_NO_INTERPROCESS_SHARING);
   if (!fence)
      return NULL;

   /* Add the fence as a dummy relocation. */
   cs->ws->base.cs_add_buffer(rcs, fence,
                              RADEON_USAGE_READWRITE | RADEON_PRIO_FENCE_TRACE, RADEON_DOMAIN_GTT);
   return (struct pipe_fence_handle*)fence;
}

static bool radeon_fence_wait(struct radeon_winsys *ws,
                              struct pipe_fence_handle *fence,
                              uint64_t timeout)
{
   return ws->buffer_wait(ws, (struct pb_buffer_lean*)fence, timeout,
                          RADEON_USAGE_READWRITE);
}

static void radeon_fence_reference(struct radeon_winsys *ws,
                                   struct pipe_fence_handle **dst,
                                   struct pipe_fence_handle *src)
{
   radeon_bo_reference(ws, (struct pb_buffer_lean**)dst, (struct pb_buffer_lean*)src);
}

static struct pipe_fence_handle *radeon_drm_cs_get_next_fence(struct radeon_cmdbuf *rcs)
{
   struct radeon_drm_cs *cs = radeon_drm_cs(rcs);
   struct pipe_fence_handle *fence = NULL;

   if (cs->next_fence) {
      radeon_fence_reference(&cs->ws->base, &fence, cs->next_fence);
      return fence;
   }

   fence = radeon_cs_create_fence(rcs);
   if (!fence)
      return NULL;

   radeon_fence_reference(&cs->ws->base, &cs->next_fence, fence);
   return fence;
}

static void
radeon_drm_cs_add_fence_dependency(struct radeon_cmdbuf *rcs,
                                   struct pipe_fence_handle *fence,
                                   uint64_t timeline_point)
{
   /* TODO: Handle the following unlikely multi-threaded scenario:
    *
    *  Thread 1 / Context 1                   Thread 2 / Context 2
    *  --------------------                   --------------------
    *  f = cs_get_next_fence()
    *                                         cs_add_fence_dependency(f)
    *                                         cs_flush()
    *  cs_flush()
    *
    * We currently assume that this does not happen because we don't support
    * asynchronous flushes on Radeon.
    */
}

void radeon_drm_cs_init_functions(struct radeon_drm_winsys *ws)
{
   ws->base.ctx_create = radeon_drm_ctx_create;
   ws->base.ctx_destroy = radeon_drm_ctx_destroy;
   ws->base.ctx_set_sw_reset_status = radeon_drm_ctx_set_sw_reset_status;
   ws->base.ctx_query_reset_status = radeon_drm_ctx_query_reset_status;
   ws->base.cs_create = radeon_drm_cs_create;
   ws->base.cs_destroy = radeon_drm_cs_destroy;
   ws->base.cs_add_buffer = radeon_drm_cs_add_buffer;
   ws->base.cs_lookup_buffer = radeon_drm_cs_lookup_buffer;
   ws->base.cs_validate = radeon_drm_cs_validate;
   ws->base.cs_check_space = radeon_drm_cs_check_space;
   ws->base.cs_get_buffer_list = radeon_drm_cs_get_buffer_list;
   ws->base.cs_flush = radeon_drm_cs_flush;
   ws->base.cs_get_next_fence = radeon_drm_cs_get_next_fence;
   ws->base.cs_is_buffer_referenced = radeon_bo_is_referenced;
   ws->base.cs_sync_flush = radeon_drm_cs_sync_flush;
   ws->base.cs_add_fence_dependency = radeon_drm_cs_add_fence_dependency;
   ws->base.fence_wait = radeon_fence_wait;
   ws->base.fence_reference = radeon_fence_reference;
}
