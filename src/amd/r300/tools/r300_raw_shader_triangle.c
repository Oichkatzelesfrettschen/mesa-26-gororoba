// SPDX-License-Identifier: MIT

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "git_sha1.h"
#include "util/macros.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define R300_TRACE_SCHEMA_VERSION 1
#define R300_TRACE_ENDIAN_MARKER 0x01020304u
#define R300_PACKET2_NOP 0x80000000u

#define RADEON_CP_PACKET0 0x00000000u
#define RADEON_CP_PACKET3 0xc0000000u
#define R300_PACKET3_3D_LOAD_VBPNTR 0x00002f00u
#define R300_PACKET3_3D_DRAW_VBUF_2 0x00003400u

#define R300_RB3D_DSTCACHE_CTLSTAT 0x4e4c
#define R300_ZB_ZCACHE_CTLSTAT 0x4f18
#define RADEON_WAIT_UNTIL 0x1720
#define R300_GB_AA_CONFIG 0x4020
#define R300_RB3D_CCTL 0x4e00
#define R300_RB3D_COLOROFFSET0 0x4e28
#define R300_RB3D_COLORPITCH0 0x4e38
#define R300_ZB_CNTL 0x4f00
#define R300_RB3D_ROPCNTL 0x4e18
#define R300_RB3D_CBLEND 0x4e04
#define R300_RB3D_ABLEND 0x4e08
#define R300_RB3D_COLOR_CHANNEL_MASK 0x4e0c
#define R300_RB3D_DITHER_CTL 0x4e50
#define R300_GA_COLOR_CONTROL 0x4278
#define R300_VAP_VF_MAX_VTX_INDX 0x2134
#define R300_VAP_OUTPUT_VTX_FMT_0 0x2090
#define R300_VAP_OUTPUT_VTX_FMT_1 0x2094
#define R300_TX_INVALTAGS 0x4100
#define R300_TX_ENABLE 0x4104
#define R300_TX_FILTER0_0 0x4400
#define R300_TX_FILTER1_0 0x4440
#define R300_TX_FORMAT0_0 0x4480
#define R300_TX_FORMAT1_0 0x44c0
#define R300_TX_FORMAT2_0 0x4500
#define R300_TX_OFFSET_0 0x4540
#define R300_US_OUT_FMT_0 0x46a4
#define R300_US_CONFIG 0x4600
#define R300_US_PIXSIZE 0x4604
#define R300_US_CODE_OFFSET 0x4608
#define R300_US_CODE_ADDR_0 0x4610
#define R300_US_ALU_RGB_INST_0 0x48c0
#define R300_US_ALU_RGB_ADDR_0 0x46c0
#define R300_US_ALU_ALPHA_INST_0 0x49c0
#define R300_US_ALU_ALPHA_ADDR_0 0x47c0
#define R300_US_W_FMT 0x46b4
#define R300_PFS_PARAM_0_X 0x4c00

#define R300_ALU_SRC0C_CONST (1u << 5)
#define R300_ALU_SRC1C_CONST (1u << 11)
#define R300_ALU_SRC2C_CONST (1u << 17)
#define R300_ALU_SRC0A_CONST (1u << 5)
#define R300_ALU_SRC1A_CONST (1u << 11)
#define R300_ALU_SRC2A_CONST (1u << 17)

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

enum r300_raw_program {
   R300_RAW_PROGRAM_SOLID_TRIANGLE,
   R300_RAW_PROGRAM_FRAGMENT_UNIFORM,
   R300_RAW_PROGRAM_FRAGMENT_VARYING,
   R300_RAW_PROGRAM_FRAGMENT_TEXTURE,
};

struct r300_raw_program_info {
   enum r300_raw_program program;
   const char *name;
   uint8_t expected_rgba[4];
   bool uses_uniform_seed;
   bool uses_varying_seed;
   bool uses_texture_seed;
};

struct r300_raw_options {
   bool submit;
   uint32_t pci_id;
   const char *device_path;
   const char *out_dir;
   const struct r300_raw_program_info *program;
};

struct r300_raw_ib {
   uint32_t dw[256];
   unsigned count;
   bool overflow;
};

static const struct r300_raw_program_info programs[] = {
   {
      .program = R300_RAW_PROGRAM_SOLID_TRIANGLE,
      .name = "solid-triangle",
      .expected_rgba = {32, 159, 223, 255},
   },
   {
      .program = R300_RAW_PROGRAM_FRAGMENT_UNIFORM,
      .name = "fragment-uniform",
      .expected_rgba = {43, 159, 223, 255},
      .uses_uniform_seed = true,
   },
   {
      .program = R300_RAW_PROGRAM_FRAGMENT_VARYING,
      .name = "fragment-varying",
      .expected_rgba = {43, 159, 223, 255},
      .uses_varying_seed = true,
   },
   {
      .program = R300_RAW_PROGRAM_FRAGMENT_TEXTURE,
      .name = "fragment-texture",
      .expected_rgba = {43, 159, 223, 255},
      .uses_texture_seed = true,
   },
};

static void
usage(FILE *file)
{
   fprintf(file,
           "usage: r300_raw_shader_triangle [--no-submit|--submit] "
           "[--program NAME] [--device PATH] [--pci-id HEX] "
           "[--out-dir DIR]\n\n");
   fprintf(file,
           "programs: solid-triangle, fragment-uniform, "
           "fragment-varying, fragment-texture\n");
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

static const struct r300_raw_program_info *
find_program(const char *name)
{
   for (unsigned i = 0; i < ARRAY_SIZE(programs); i++) {
      if (!strcmp(name, programs[i].name))
         return &programs[i];
   }
   return NULL;
}

static bool
parse_args(int argc, char **argv, struct r300_raw_options *opts)
{
   *opts = (struct r300_raw_options) {
      .submit = false,
      .pci_id = 0x5974,
      .device_path = "/dev/dri/renderD128",
      .out_dir = "r300_raw_shader_triangle_out",
      .program = &programs[0],
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
      } else if (!strcmp(argv[i], "--program") && i + 1 < argc) {
         opts->program = find_program(argv[++i]);
         if (!opts->program)
            return false;
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

static uint32_t
packet0(uint32_t reg, uint32_t count)
{
   return RADEON_CP_PACKET0 | ((count - 1) << 16) | (reg >> 2);
}

static uint32_t
packet3(uint32_t op, uint32_t count)
{
   return RADEON_CP_PACKET3 | op | (count << 16);
}

static bool
emit(struct r300_raw_ib *ib, uint32_t value)
{
   if (ib->count >= ARRAY_SIZE(ib->dw)) {
      ib->overflow = true;
      return false;
   }

   ib->dw[ib->count++] = value;
   return true;
}

static bool
emit_reg(struct r300_raw_ib *ib, uint32_t reg, uint32_t value)
{
   return emit(ib, packet0(reg, 1)) &&
          emit(ib, value);
}

static bool
emit_reg_seq4(struct r300_raw_ib *ib, uint32_t reg, uint32_t a,
              uint32_t b, uint32_t c, uint32_t d)
{
   return emit(ib, packet0(reg, 4)) &&
          emit(ib, a) &&
          emit(ib, b) &&
          emit(ib, c) &&
          emit(ib, d);
}

static uint32_t
pack_r300_float24(float value)
{
   union {
      float f;
      uint32_t u;
   } u = { .f = value };
   int exponent = 0;
   float mantissa;
   uint32_t packed = 0;

   if (value == 0.0f)
      return 0;

   mantissa = frexpf(value, &exponent);
   if (mantissa < 0.0f) {
      packed |= 1u << 23;
      mantissa *= -1.0f;
   }

   exponent += 62;
   packed |= (uint32_t)exponent << 16;
   packed |= (u.u & 0x7fffffu) >> 7;
   return packed;
}

static bool
emit_common_render_state(struct r300_raw_ib *ib)
{
   return emit_reg(ib, R300_RB3D_DSTCACHE_CTLSTAT, 0x0000000a) &&
          emit_reg(ib, R300_ZB_ZCACHE_CTLSTAT, 0x00000003) &&
          emit_reg(ib, RADEON_WAIT_UNTIL, 0x00020000) &&
          emit_reg(ib, R300_GB_AA_CONFIG, 0x00000000) &&
          emit_reg(ib, R300_RB3D_CCTL, 0x00000000) &&
          emit_reg(ib, R300_RB3D_COLOROFFSET0, 0x00000000) &&
          emit_reg(ib, R300_RB3D_COLORPITCH0, 0x00c10040) &&
          emit_reg(ib, R300_ZB_CNTL, 0x00000000) &&
          emit_reg(ib, R300_RB3D_ROPCNTL, 0x00000000) &&
          emit_reg(ib, R300_RB3D_CBLEND, 0x00000000) &&
          emit_reg(ib, R300_RB3D_ABLEND, 0x00000000) &&
          emit_reg(ib, R300_RB3D_COLOR_CHANNEL_MASK, 0x0000000f) &&
          emit_reg(ib, R300_RB3D_DITHER_CTL, 0x00000000) &&
          emit_reg(ib, R300_VAP_OUTPUT_VTX_FMT_0, 0x00000001) &&
          emit_reg(ib, R300_VAP_OUTPUT_VTX_FMT_1, 0x00000004);
}

static bool
emit_uniform_seed_state(struct r300_raw_ib *ib)
{
   return emit_reg_seq4(ib, R300_PFS_PARAM_0_X,
                        pack_r300_float24(6.0f),
                        pack_r300_float24(9.0f),
                        pack_r300_float24(11.0f),
                        pack_r300_float24(1.0f));
}

static bool
emit_texture_seed_state(struct r300_raw_ib *ib)
{
   return emit_reg(ib, R300_TX_INVALTAGS, 0x00000000) &&
          emit_reg(ib, R300_TX_ENABLE, 0x00000001) &&
          emit_reg(ib, R300_TX_FILTER0_0, 0x00002a00) &&
          emit_reg(ib, R300_TX_FILTER1_0, 0x00000000) &&
          emit_reg(ib, R300_TX_FORMAT0_0, 0x00000000) &&
          emit_reg(ib, R300_TX_FORMAT1_0, 0x00000000) &&
          emit_reg(ib, R300_TX_FORMAT2_0, 0x00000000) &&
          emit_reg(ib, R300_TX_OFFSET_0, 0x00000000);
}

static bool
emit_us_program(struct r300_raw_ib *ib,
                const struct r300_raw_program_info *program)
{
   if (!emit_reg(ib, R300_US_OUT_FMT_0, 0x00001b00) ||
       !emit_reg(ib, R300_US_CONFIG, 0x00000000) ||
       !emit_reg(ib, R300_US_PIXSIZE, 0x00000000) ||
       !emit_reg(ib, R300_US_CODE_OFFSET, 0x00000000) ||
       !emit_reg_seq4(ib, R300_US_CODE_ADDR_0,
                      0x00000000, 0x00000000, 0x00000000, 0x00400000))
      return false;

   if (program->program == R300_RAW_PROGRAM_SOLID_TRIANGLE) {
      if (!emit_reg(ib, R300_US_ALU_RGB_INST_0, 0x02804000) ||
          !emit_reg(ib, R300_US_ALU_RGB_ADDR_0, 0x1c000020) ||
          !emit_reg(ib, R300_US_ALU_ALPHA_INST_0, 0x01800489) ||
          !emit_reg(ib, R300_US_ALU_ALPHA_ADDR_0, 0x01000020))
         return false;
   } else if (program->uses_uniform_seed) {
      if (!emit_reg(ib, R300_US_ALU_RGB_INST_0, 0x02804000) ||
          !emit_reg(ib, R300_US_ALU_RGB_ADDR_0,
                    0x1c000000 | R300_ALU_SRC0C_CONST |
                    R300_ALU_SRC1C_CONST | R300_ALU_SRC2C_CONST) ||
          !emit_reg(ib, R300_US_ALU_ALPHA_INST_0, 0x01800891) ||
          !emit_reg(ib, R300_US_ALU_ALPHA_ADDR_0,
                    0x01000000 | R300_ALU_SRC0A_CONST |
                    R300_ALU_SRC1A_CONST | R300_ALU_SRC2A_CONST))
         return false;
   } else {
      if (!emit_reg(ib, R300_US_ALU_RGB_INST_0, 0x02804000) ||
          !emit_reg(ib, R300_US_ALU_RGB_ADDR_0, 0x1c000000) ||
          !emit_reg(ib, R300_US_ALU_ALPHA_INST_0, 0x01800891) ||
          !emit_reg(ib, R300_US_ALU_ALPHA_ADDR_0, 0x01000000))
         return false;
   }

   return emit_reg(ib, R300_US_W_FMT, 0x00000000);
}

static bool
emit_draw(struct r300_raw_ib *ib, bool uses_varying_seed)
{
   return emit(ib, packet3(R300_PACKET3_3D_LOAD_VBPNTR, 3)) &&
          emit(ib, 0x00000021) &&
          emit(ib, uses_varying_seed ? 0x00000808 : 0x00000008) &&
          emit(ib, 0x00000000) &&
          emit(ib, 0x00000000) &&
          emit_reg(ib, R300_GA_COLOR_CONTROL,
                   uses_varying_seed ? 0x0003aaaa : 0x00035555) &&
          emit_reg(ib, R300_VAP_VF_MAX_VTX_INDX, 0x00000002) &&
          emit(ib, packet3(R300_PACKET3_3D_DRAW_VBUF_2, 0)) &&
          emit(ib, 0x00030024) &&
          emit(ib, R300_PACKET2_NOP) &&
          emit(ib, R300_PACKET2_NOP);
}

static bool
build_ib(const struct r300_raw_program_info *program, struct r300_raw_ib *ib)
{
   *ib = (struct r300_raw_ib) {0};

   if (!emit_common_render_state(ib))
      return false;
   if (program->uses_uniform_seed && !emit_uniform_seed_state(ib))
      return false;
   if (program->uses_texture_seed && !emit_texture_seed_state(ib))
      return false;
   if (!emit_us_program(ib, program))
      return false;
   if (!emit_draw(ib, program->uses_varying_seed))
      return false;
   return !ib->overflow;
}

static bool
write_ib(const char *path, const struct r300_raw_options *opts,
         const struct r300_raw_ib *ib)
{
   struct utsname uts;
   struct r300_trace_ib_header header = {
      .magic = "R3RKIB1",
      .schema_version = R300_TRACE_SCHEMA_VERSION,
      .header_size = sizeof(header),
      .endian_marker = R300_TRACE_ENDIAN_MARKER,
      .dword_count = ib->count,
      .pci_id = opts->pci_id,
      .family = 8,
      .drm_major = 0,
      .drm_minor = 0,
      .drm_patchlevel = 0,
      .run_id = "r300_raw_shader_triangle",
   };

   if (uname(&uts) == 0) {
      strncpy(header.kernel_release, uts.release,
              sizeof(header.kernel_release) - 1);
      header.kernel_release[sizeof(header.kernel_release) - 1] = '\0';
   }
   snprintf(header.mesa_commit, sizeof(header.mesa_commit), "%s+%s",
            PACKAGE_VERSION, MESA_GIT_SHA1);

   FILE *file = fopen(path, "wb");
   if (!file)
      return false;

   bool ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
             fwrite(ib->dw, sizeof(uint32_t), ib->count, file) == ib->count;
   ok = fclose(file) == 0 && ok;
   return ok;
}

static bool
write_program_json(const char *path, const struct r300_raw_options *opts)
{
   FILE *file = fopen(path, "w");
   if (!file)
      return false;

   fputs("{\n", file);
   fputs("  \"schema\": \"r300-raw-shader-triangle-program-v1\",\n", file);
   fputs("  \"tool\": \"r300_raw_shader_triangle\",\n", file);
   fputs("  \"program\": ", file);
   json_string(file, opts->program->name);
   fputs(",\n", file);
   fprintf(file, "  \"expected_rgba\": [%u, %u, %u, %u],\n",
           opts->program->expected_rgba[0], opts->program->expected_rgba[1],
           opts->program->expected_rgba[2], opts->program->expected_rgba[3]);
   fprintf(file, "  \"uses_uniform_seed\": %s,\n",
           opts->program->uses_uniform_seed ? "true" : "false");
   fprintf(file, "  \"uses_varying_seed\": %s,\n",
           opts->program->uses_varying_seed ? "true" : "false");
   fprintf(file, "  \"uses_texture_seed\": %s,\n",
           opts->program->uses_texture_seed ? "true" : "false");
   fputs("  \"gpu_only_target\": true,\n", file);
   fputs("  \"gl_dependency\": false,\n", file);
   fputs("  \"gallium_state_tracker_dependency\": false\n", file);
   fputs("}\n", file);

   return fclose(file) == 0;
}

static int
submit_shader_triangle(const struct r300_raw_options *opts)
{
   const char *hazard_accepted = getenv("R300_TRACE_HAZARD_ACCEPTED");
   if (!hazard_accepted || strcmp(hazard_accepted, "1") != 0) {
      fprintf(stderr,
              "r300_raw_shader_triangle: --submit requires "
              "R300_TRACE_HAZARD_ACCEPTED=1\n");
      return -EPERM;
   }

   fprintf(stderr,
           "r300_raw_shader_triangle: --submit requires the BO, reloc, "
           "fence, and readback path; this slice is no-submit only\n");
   (void)opts;
   return -ENOSYS;
}

static bool
write_submit_json(const char *path, const struct r300_raw_options *opts,
                  const struct r300_raw_ib *ib, int submit_result)
{
   FILE *file = fopen(path, "w");
   if (!file)
      return false;

   fputs("{\n", file);
   fputs("  \"schema\": \"r300-raw-shader-triangle-submit-v1\",\n", file);
   fputs("  \"tool\": \"r300_raw_shader_triangle\",\n", file);
   fputs("  \"program\": ", file);
   json_string(file, opts->program->name);
   fputs(",\n", file);
   fputs("  \"device_path\": ", file);
   json_string(file, opts->device_path);
   fputs(",\n", file);
   fprintf(file, "  \"pci_id\": \"0x%04" PRIx32 "\",\n", opts->pci_id);
   fprintf(file, "  \"submit_requested\": %s,\n",
           opts->submit ? "true" : "false");
   fprintf(file, "  \"submit_supported\": false,\n");
   fprintf(file, "  \"submit_result\": %d,\n", submit_result);
   fprintf(file, "  \"ib_dwords\": %u,\n", ib->count);
   fputs("  \"submit_blocker\": ", file);
   json_string(file,
               "fresh BO allocation, reloc table, fence, and readback are not implemented");
   fputs("\n}\n", file);

   return fclose(file) == 0;
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
      fprintf(stderr, "r300_raw_shader_triangle: could not create %s: %s\n",
              opts.out_dir, strerror(errno));
      return 1;
   }

   struct r300_raw_ib ib;
   if (!build_ib(opts.program, &ib)) {
      fprintf(stderr,
              "r300_raw_shader_triangle: IB exceeds %u dwords for %s\n",
              (unsigned)ARRAY_SIZE(ib.dw), opts.program->name);
      return 1;
   }
   char ib_path[PATH_MAX];
   char program_path[PATH_MAX];
   char submit_path[PATH_MAX];
   if (!join_path(ib_path, sizeof(ib_path), opts.out_dir, "pre_ib.bin") ||
       !join_path(program_path, sizeof(program_path), opts.out_dir, "program.json") ||
       !join_path(submit_path, sizeof(submit_path), opts.out_dir, "submit.json")) {
      fprintf(stderr, "r300_raw_shader_triangle: output path too long\n");
      return 1;
   }

   if (!write_ib(ib_path, &opts, &ib)) {
      fprintf(stderr, "r300_raw_shader_triangle: could not write %s: %s\n",
              ib_path, strerror(errno));
      return 1;
   }

   if (!write_program_json(program_path, &opts)) {
      fprintf(stderr, "r300_raw_shader_triangle: could not write %s: %s\n",
              program_path, strerror(errno));
      return 1;
   }

   int submit_result = opts.submit ? submit_shader_triangle(&opts) : 0;
   if (!write_submit_json(submit_path, &opts, &ib, submit_result)) {
      fprintf(stderr, "r300_raw_shader_triangle: could not write %s: %s\n",
              submit_path, strerror(errno));
      return 1;
   }

   printf("r300_raw_shader_triangle_out=%s\n", opts.out_dir);
   printf("program=%s\n", opts.program->name);
   printf("submit_result=%d\n", submit_result);
   return submit_result ? 1 : 0;
}
