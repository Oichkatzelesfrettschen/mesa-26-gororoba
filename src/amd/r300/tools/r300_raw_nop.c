// SPDX-License-Identifier: MIT

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <xf86drm.h>

#include "git_sha1.h"
#include "radeon_drm.h"
#include "util/macros.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define R300_TRACE_SCHEMA_VERSION 1
#define R300_TRACE_ENDIAN_MARKER 0x01020304u
#define R300_PACKET2_NOP 0x80000000u

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

struct r300_raw_options {
   bool submit;
   uint32_t pci_id;
   const char *device_path;
   const char *out_dir;
};

static void
usage(FILE *file)
{
   fprintf(file,
           "usage: r300_raw_nop [--no-submit|--submit] [--device PATH] "
           "[--pci-id HEX] [--out-dir DIR]\n");
}

static bool
parse_u32(const char *text, uint32_t *out)
{
   char *end = NULL;
   unsigned long value = strtoul(text, &end, 0);

   if (!text[0] || (end && *end) || value > UINT32_MAX)
      return false;

   *out = (uint32_t)value;
   return true;
}

static bool
parse_args(int argc, char **argv, struct r300_raw_options *opts)
{
   *opts = (struct r300_raw_options) {
      .submit = false,
      .pci_id = 0x5974,
      .device_path = "/dev/dri/renderD128",
      .out_dir = "r300_raw_nop_out",
   };

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--no-submit")) {
         opts->submit = false;
      } else if (!strcmp(argv[i], "--submit")) {
         opts->submit = true;
      } else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
         opts->device_path = argv[++i];
      } else if (!strcmp(argv[i], "--out-dir") && i + 1 < argc) {
         opts->out_dir = argv[++i];
      } else if (!strcmp(argv[i], "--pci-id") && i + 1 < argc) {
         if (!parse_u32(argv[++i], &opts->pci_id))
            return false;
      } else if (!strcmp(argv[i], "--help")) {
         usage(stdout);
         exit(0);
      } else {
         return false;
      }
   }

   return true;
}

static void
json_string(FILE *file, const char *text)
{
   fputc('"', file);
   for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
      switch (*p) {
      case '\\':
      case '"':
         fputc('\\', file);
         fputc(*p, file);
         break;
      case '\n':
         fputs("\\n", file);
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

static bool
join_path(char *out, size_t out_size, const char *dir, const char *name)
{
   int written = snprintf(out, out_size, "%s/%s", dir, name);
   return written > 0 && (size_t)written < out_size;
}

static bool
write_ib(const char *path, const struct r300_raw_options *opts,
         const uint32_t *ib, unsigned ib_dwords)
{
   struct utsname uts;
   struct r300_trace_ib_header header = {
      .magic = "R3RKIB1",
      .schema_version = R300_TRACE_SCHEMA_VERSION,
      .header_size = sizeof(header),
      .endian_marker = R300_TRACE_ENDIAN_MARKER,
      .dword_count = ib_dwords,
      .pci_id = opts->pci_id,
      .family = 8,
      .drm_major = 0,
      .drm_minor = 0,
      .drm_patchlevel = 0,
      .run_id = "r300_raw_nop",
   };

   if (uname(&uts) == 0)
      snprintf(header.kernel_release, sizeof(header.kernel_release), "%s", uts.release);
   snprintf(header.mesa_commit, sizeof(header.mesa_commit), "%s+%s",
            PACKAGE_VERSION, MESA_GIT_SHA1);

   FILE *file = fopen(path, "wb");
   if (!file)
      return false;

   bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
             fwrite(ib, sizeof(uint32_t), ib_dwords, file) == ib_dwords;
   ok = fclose(file) == 0 && ok;
   return ok;
}

static int
submit_nop(const struct r300_raw_options *opts, const uint32_t *ib,
           unsigned ib_dwords)
{
   if (!getenv("R300_TRACE_HAZARD_ACCEPTED")) {
      fprintf(stderr, "r300_raw_nop: --submit requires R300_TRACE_HAZARD_ACCEPTED=1\n");
      return -EPERM;
   }

   int fd = open(opts->device_path, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return -errno;

   uint32_t flags[2] = {0, RADEON_CS_RING_GFX};
   struct drm_radeon_cs_chunk chunks[3] = {
      {
         .chunk_id = RADEON_CHUNK_ID_IB,
         .length_dw = ib_dwords,
         .chunk_data = (uint64_t)(uintptr_t)ib,
      },
      {
         .chunk_id = RADEON_CHUNK_ID_RELOCS,
         .length_dw = 0,
         .chunk_data = 0,
      },
      {
         .chunk_id = RADEON_CHUNK_ID_FLAGS,
         .length_dw = 2,
         .chunk_data = (uint64_t)(uintptr_t)flags,
      },
   };
   uint64_t chunk_array[3] = {
      (uint64_t)(uintptr_t)&chunks[0],
      (uint64_t)(uintptr_t)&chunks[1],
      (uint64_t)(uintptr_t)&chunks[2],
   };
   struct drm_radeon_cs cs = {
      .num_chunks = 3,
      .chunks = (uint64_t)(uintptr_t)chunk_array,
   };

   int ret = drmCommandWriteRead(fd, DRM_RADEON_CS, &cs, sizeof(cs));
   int saved_errno = errno;
   close(fd);

   if (ret)
      return -saved_errno;
   return 0;
}

static bool
write_submit_json(const char *path, const struct r300_raw_options *opts,
                  unsigned ib_dwords, int submit_result)
{
   FILE *file = fopen(path, "w");
   if (!file)
      return false;

   fputs("{\n", file);
   fputs("  \"schema\": \"r300-raw-nop-v1\",\n", file);
   fputs("  \"tool\": \"r300_raw_nop\",\n", file);
   fputs("  \"device_path\": ", file);
   json_string(file, opts->device_path);
   fputs(",\n", file);
   fprintf(file, "  \"pci_id\": \"0x%04" PRIx32 "\",\n", opts->pci_id);
   fprintf(file, "  \"submit_requested\": %s,\n", opts->submit ? "true" : "false");
   fprintf(file, "  \"submit_result\": %d,\n", submit_result);
   fprintf(file, "  \"ib_dwords\": %u,\n", ib_dwords);
   fprintf(file, "  \"ib_packet2\": \"0x%08" PRIx32 "\"\n", R300_PACKET2_NOP);
   fputs("}\n", file);

   bool ok = fclose(file) == 0;
   return ok;
}

int
main(int argc, char **argv)
{
   struct r300_raw_options opts;
   if (!parse_args(argc, argv, &opts)) {
      usage(stderr);
      return 2;
   }

   if (mkdir(opts.out_dir, 0777) && errno != EEXIST) {
      fprintf(stderr, "r300_raw_nop: could not create %s: %s\n",
              opts.out_dir, strerror(errno));
      return 1;
   }

   const uint32_t ib[] = {R300_PACKET2_NOP};
   char ib_path[PATH_MAX];
   char submit_path[PATH_MAX];
   if (!join_path(ib_path, sizeof(ib_path), opts.out_dir, "pre_ib.bin") ||
       !join_path(submit_path, sizeof(submit_path), opts.out_dir, "submit.json")) {
      fprintf(stderr, "r300_raw_nop: output path too long\n");
      return 1;
   }

   if (!write_ib(ib_path, &opts, ib, ARRAY_SIZE(ib))) {
      fprintf(stderr, "r300_raw_nop: could not write %s: %s\n",
              ib_path, strerror(errno));
      return 1;
   }

   int submit_result = opts.submit ? submit_nop(&opts, ib, ARRAY_SIZE(ib)) : 0;
   if (!write_submit_json(submit_path, &opts, ARRAY_SIZE(ib), submit_result)) {
      fprintf(stderr, "r300_raw_nop: could not write %s: %s\n",
              submit_path, strerror(errno));
      return 1;
   }

   printf("r300_raw_nop_out=%s\n", opts.out_dir);
   printf("submit_result=%d\n", submit_result);
   return submit_result ? 1 : 0;
}
