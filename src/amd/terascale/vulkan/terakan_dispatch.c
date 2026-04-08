/*
 * Copyright © 2026 steinmarder project
 * SPDX-License-Identifier: MIT
 *
 * Vulkan compute dispatch for Terakan (TeraScale-2/Evergreen).
 *
 * Emits the PM4 packet sequence for compute shader dispatch:
 *   SQ_PGM_START_CS → SQ_PGM_RESOURCES_CS → SPI_COMPUTE_NUM_THREAD_X/Y/Z
 *   → VGT_COMPUTE_THREAD_GROUP_SIZE → SQ_LDS_ALLOC → PKT3_DISPATCH_DIRECT
 */

#include "terakan_pipeline_compute.h"

#include "terakan_command_buffer.h"
#include "terakan_buffer.h"
#include "terakan_barrier.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"

#include "amd/terascale/common/terascale_eg_sq.h"
#include "gallium/drivers/r600/r600_opcodes.h"

#include <assert.h>
#include "util/macros.h"

/* Register offsets for Evergreen compute dispatch.
 * Reference: AMD Evergreen Family ISA, Section 10 (Compute Setup). */
#ifndef R_028D9C_SQ_PGM_START_CS
#define R_028D9C_SQ_PGM_START_CS             0x28D9C
#endif
#ifndef R_028DA0_SQ_PGM_RESOURCES_CS
#define R_028DA0_SQ_PGM_RESOURCES_CS         0x28DA0
#endif
#ifndef R_028DA4_SQ_PGM_RESOURCES_CS_2
#define R_028DA4_SQ_PGM_RESOURCES_CS_2       0x28DA4
#endif
#ifndef R_0286EC_SPI_COMPUTE_NUM_THREAD_X
#define R_0286EC_SPI_COMPUTE_NUM_THREAD_X    0x286EC
#endif
#ifndef R_0286F0_SPI_COMPUTE_NUM_THREAD_Y
#define R_0286F0_SPI_COMPUTE_NUM_THREAD_Y    0x286F0
#endif
#ifndef R_0286F4_SPI_COMPUTE_NUM_THREAD_Z
#define R_0286F4_SPI_COMPUTE_NUM_THREAD_Z    0x286F4
#endif
#ifndef R_0089AC_VGT_COMPUTE_THREAD_GROUP_SZ
#define R_0089AC_VGT_COMPUTE_THREAD_GROUP_SZ 0x89AC
#endif
#ifndef R_008970_VGT_NUM_INDICES
#define R_008970_VGT_NUM_INDICES             0x8970
#endif
#ifndef R_00899C_VGT_COMPUTE_START_X
#define R_00899C_VGT_COMPUTE_START_X         0x899C
#endif
#ifndef R_0288E8_SQ_LDS_ALLOC
#define R_0288E8_SQ_LDS_ALLOC                0x288E8
#endif

/* PM4 packet helpers (matching r600 driver conventions) */
#ifndef PKT3
#define PKT3(op, count, predicate) \
   (((unsigned)(op) << 8) | (((count) - 1) << 16) | ((predicate) << 0) | (3u << 30))
#endif
#ifndef PKT3C
#define PKT3C PKT3
#endif

/* SET_CONTEXT_REG: for registers in the 0x28000-0x29FFF range */
#ifndef PKT3_SET_CONTEXT_REG
#define PKT3_SET_CONTEXT_REG 0x69
#endif
#define CONTEXT_REG_OFFSET(reg) (((reg) - 0x28000) >> 2)

/* SET_CONFIG_REG: for registers in the 0x08000-0x0BFFF range */
#ifndef PKT3_SET_CONFIG_REG
#define PKT3_SET_CONFIG_REG 0x68
#endif
#define CONFIG_REG_OFFSET(reg) (((reg) - 0x08000) >> 2)

/* DISPATCH_DIRECT */
#ifndef PKT3_DISPATCH_DIRECT
#define PKT3_DISPATCH_DIRECT 0x15
#endif

/* RADEON_CP_PACKET3_COMPUTE_MODE: bit 1 in PM4 header.
 * On Evergreen, all compute context register writes MUST have this bit set
 * or the kernel CS validator will reject them. */
#define COMPUTE_MODE_BIT 0x00000002

/* PKT3 with compute mode flag */
#define PKT3_COMPUTE(op, count, predicate) \
   (PKT3(op, count, predicate) | COMPUTE_MODE_BIT)

/* Evergreen compute uses the Local Shader (LS) stage, not a dedicated CS.
 * Program registers: R_0288D0_SQ_PGM_START_LS, R_0288D4_SQ_PGM_RESOURCES_LS */
#ifndef R_0288D0_SQ_PGM_START_LS
#define R_0288D0_SQ_PGM_START_LS        0x288D0
#endif
#ifndef R_0288D4_SQ_PGM_RESOURCES_LS
#define R_0288D4_SQ_PGM_RESOURCES_LS    0x288D4
#endif
#ifndef R_0288D8_SQ_PGM_RESOURCES_LS_2
#define R_0288D8_SQ_PGM_RESOURCES_LS_2  0x288D8
#endif

/* S_0288D4 field macros (from r600d_common.h) */
#ifndef S_0288D4_NUM_GPRS
#define S_0288D4_NUM_GPRS(x)     (((unsigned)(x) & 0xFF) << 0)
#define S_0288D4_STACK_SIZE(x)   (((unsigned)(x) & 0xFF) << 8)
#define S_0288D4_DX10_CLAMP(x)   (((unsigned)(x) & 0x1) << 21)
#endif


/* Emit bound compute resources (SSBOs/UBOs) to hardware.
 * Compute resources are stored in hw_state_sqc.resource_descriptors.fs[]
 * (because Evergreen compute shares the FS resource namespace).
 * We emit PKT3_SET_RESOURCE for each bound resource. */

/* Emit the RAT (CB_COLOR0) binding for compute SSBO writes.
 *
 * On TeraScale-2, compute SSBO writes use MEM_RAT WRITE_IND which routes
 * through the Render Output Units (ROPs) to CB_COLOR0. This is NOT a
 * texture resource — it's a render target in disguise.
 *
 * The Gallium pattern from evergreen_compute.c:
 *   1. CB_TARGET_MASK = 0xF (enable all channels for RAT0)
 *   2. CB_COLOR0_BASE through CB_COLOR0_CLEAR_WORD1 (13 sequential regs)
 *   3. 4 NOP relocations for BASE, ATTRIB, CMASK, FMASK
 *
 * Register values derived from evergreen_init_color_surface_rat +
 * evergreen_set_color_surface_buffer for PIPE_FORMAT_R32_UINT.
 */


/* Register addresses for compute KCACHE (constant buffer for LS stage) */
#ifndef R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0
#define R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0  0x28FC0
#endif
#ifndef R_028F40_ALU_CONST_CACHE_LS_0
#define R_028F40_ALU_CONST_CACHE_LS_0        0x28F40
#endif
#define EG_FETCH_CONSTANTS_OFFSET_CS         816

/* Emit the KCACHE constant buffer setup for compute.
 *
 * The Gallium/LLVM pattern passes the SSBO base address via KC0[0].x
 * (Constant Buffer 0, slot 0, component X). The shader reads this as
 * LSHR_INT R6.x, KC0[0].x, 2 to get the dword offset for MEM_RAT.
 *
 * We set up:
 *   1. ALU_CONST_BUFFER_SIZE_LS_0 = size in 256-byte units
 *   2. ALU_CONST_CACHE_LS_0 = base address (va >> 8) + NOP reloc
 *   3. SET_RESOURCE at EG_FETCH_CONSTANTS_OFFSET_CS for constant fetch
 *
 * The constant buffer contains a single vec4 with the SSBO's
 * pool-relative byte offset in .x component.
 */
static void
terakan_emit_compute_kcache(struct terakan_gfx_command_writer *command_writer,
                            struct terakan_bo const *ssbo_bo)
{
   /* For now, the constant buffer contains just the offset (0) since
    * the SSBO starts at offset 0 within its BO. In the Gallium path,
    * KC0[0].x = pool-relative byte offset of the buffer. Since we
    * bind the SSBO directly (not through a pool), the offset is 0.
    *
    * The shader reads KC0[0].x and uses it as the byte address
    * for the MEM_RAT write (after >> 2 for dword conversion).
    *
    * NOTE: The current Terakan shader does NOT read from KC0 —
    * it uses VFETCH RID:1 instead. This KCACHE setup prepares
    * the hardware for when we fix the shader compilation to
    * match the Gallium/LLVM pattern. For now, it ensures the
    * KCACHE hardware is in a valid state. */

   /* Register the SSBO BO for the constant cache relocation */
   uint32_t bo_ref = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer,
      ssbo_bo, true, false, TERAKAN_BO_PRIORITY_UNIFORM_BUFFER);

   fprintf(stderr, "TERAKAN_COMPUTE: KCACHE bo_ref=%u for SSBO\n", bo_ref);

   /* 1. ALU_CONST_BUFFER_SIZE_LS_0 = size in 256-byte units
    *    For a 16-byte buffer: ceil(16/256) = 1 */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0 - 0x28000) >> 2;
      *p++ = 1;  /* 1 × 256 bytes = 256 bytes (minimum) */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 2. ALU_CONST_CACHE_LS_0 = base address (va >> 8) + NOP reloc
    *    This is a context register that needs COMPUTE_MODE + relocation */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028F40_ALU_CONST_CACHE_LS_0 - 0x28000) >> 2;
      *p++ = 0;  /* Base address (relocated by kernel) */
      /* NOP relocation for the constant cache address */
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 3. SET_RESOURCE for the constant fetch hardware
    *    This creates a SQ_TEX_RESOURCE at the CS constant offset
    *    so the shader can read via KC0[n].x */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 12);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_RESOURCE, 8, 0) | COMPUTE_MODE_BIT;
      *p++ = EG_FETCH_CONSTANTS_OFFSET_CS * 8;  /* Resource index */
      *p++ = 0;        /* WORD0: base address (relocated) */
      *p++ = 256 - 1;  /* WORD1: size = 256 bytes - 1 */
      *p++ = (2 << 0) |   /* WORD2: STRIDE=16 (vec4), ENDIAN=NONE */
             (0x35 << 20); /* DATA_FORMAT = FMT_32_32_32_32_FLOAT */
      *p++ = (0 << 0) | (1 << 3) | (2 << 6) | (3 << 9); /* WORD3: DST_SEL XYZW */
      *p++ = 0;        /* WORD4 */
      *p++ = 0;        /* WORD5 */
      *p++ = 0;        /* WORD6 */
      *p++ = 3 << 30;  /* WORD7: TYPE = SQ_TEX_VTX_VALID_BUFFER */
      /* NOP relocation */
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }
}

static void
terakan_emit_compute_resources(struct terakan_gfx_command_writer *command_writer)
{
   struct terakan_hw_state_sqc *state = &command_writer->hw_state_sqc;

   /* Find the first bound FS resource (our SSBO) */
   int ssbo_idx = -1;
   for (uint32_t i = 0; i < TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE; i++) {
      if (BITSET_TEST(state->resources_not_null.fs, i) && state->resource_bos.fs[i]) {
         ssbo_idx = (int)i;
         break;
      }
   }

   if (ssbo_idx < 0) {
      fprintf(stderr, "TERAKAN_COMPUTE: No SSBO bound in FS resources\n");
      return;
   }


   struct terakan_bo const *bo = state->resource_bos.fs[ssbo_idx];
   uint32_t const *desc = state->resource_descriptors.fs[ssbo_idx];

   fprintf(stderr, "TERAKAN_COMPUTE: Emitting RAT for SSBO idx=%d bo=%p buf_size=%u bo_va=0x%llx\n", ssbo_idx, (void*)bo, desc[1]+1, (unsigned long long)bo->va);
   fprintf(stderr, "TERAKAN_COMPUTE: desc[0]=0x%08x desc[1]=0x%08x desc[2]=0x%08x desc[7]=0x%08x\n", desc[0], desc[1], desc[2], desc[7]);
   /* Register the SSBO BO for relocation */
   fprintf(stderr, "TERAKAN_COMPUTE: Registering BO for relocation...\n");
   uint32_t bo_ref = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer,
      bo, true, true, TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);
   fprintf(stderr, "TERAKAN_COMPUTE: bo_ref=%u (NOP payload will be %u)\n", bo_ref, 4*bo_ref);

   /* Extract buffer size from the descriptor (Word 1 = size in bytes - 1) */
   uint32_t buf_size = desc[1] + 1;
   uint32_t width_elements = buf_size / 4; /* R32_UINT = 4 bytes per element */

   /* Compute pitch: align to 64, divide by 8, minus 1 */
   uint32_t pitch_aligned = (width_elements + 63) & ~63u;
   uint32_t pitch_tile_max = (pitch_aligned / 8) - 1;

   /* Compute dim (width for linear buffer) */
   uint32_t dim = width_elements > 0 ? width_elements - 1 : 0;

   /* CB_COLOR0_INFO for R32_UINT linear RAT buffer */
   uint32_t cb_color_info =
      S_028C70_FORMAT(V_028C70_COLOR_32) |
      S_028C70_ARRAY_MODE(V_028C70_ARRAY_LINEAR_ALIGNED) |
      S_028C70_NUMBER_TYPE(V_028C70_NUMBER_UINT) |
      S_028C70_COMP_SWAP(0) |
      S_028C70_BLEND_BYPASS(1) |
      S_028C70_RAT(1);  /* THIS IS A RAT, NOT A REGULAR RT */

   /* 1. CB_TARGET_MASK = 0xF (enable RAT0 writes) with COMPUTE_MODE */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028238_CB_TARGET_MASK - 0x28000) >> 2;
      *p++ = 0xF; /* Enable all 4 channels for CB0 */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 2. CB_COLOR0_BASE through CB_COLOR0_CLEAR_WORD1 (13 regs) with COMPUTE_MODE
    *    + 4 NOP relocations for BASE, ATTRIB, CMASK, FMASK */
   {
      /* 13 regs = count 13 in SET_CONTEXT_REG, total = 1+1+13 = 15 header+body
       * Plus 4 NOP relocs = 4*2 = 8 dwords. Total = 15 + 8 = 23. */
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 23);
      if (unlikely(p == NULL)) return;

      /* EXPERIMENT: Remove COMPUTE_MODE from CB_COLOR — kernel may not
       * process CB_COLOR relocations in compute-mode packets */
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 13, 0);  /* NO COMPUTE_MODE! */
      *p++ = (R_028C60_CB_COLOR0_BASE - 0x28000) >> 2;

      *p++ = 0;                  /* CB_COLOR0_BASE (relocated by kernel) */
      fprintf(stderr, "TERAKAN_COMPUTE: CB_COLOR0 emitted at p=%p, pitch=%u dim=%u info=0x%x\n", (void*)p, pitch_tile_max, dim, cb_color_info);
      *p++ = pitch_tile_max;     /* CB_COLOR0_PITCH */
      *p++ = 0;                  /* CB_COLOR0_SLICE */
      *p++ = 0;                  /* CB_COLOR0_VIEW */
      *p++ = cb_color_info;      /* CB_COLOR0_INFO */
      *p++ = S_028C74_NON_DISP_TILING_ORDER(1); /* CB_COLOR0_ATTRIB */
      *p++ = S_028C78_WIDTH_MAX(dim);      /* CB_COLOR0_DIM */
      *p++ = 0;                  /* CB_COLOR0_CMASK (= BASE for no separate cmask) */
      *p++ = 0;                  /* CB_COLOR0_CMASK_SLICE */
      *p++ = 0;                  /* CB_COLOR0_FMASK (= BASE) */
      *p++ = 0;                  /* CB_COLOR0_FMASK_SLICE */
      *p++ = 0;                  /* CB_COLOR0_CLEAR_WORD0 */
      *p++ = 0;                  /* CB_COLOR0_CLEAR_WORD1 */

      /* 4 NOP relocations: BASE, ATTRIB, CMASK, FMASK
       * The kernel patches CB_COLOR0_BASE with gpu_address >> 8 */
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;  /* Reloc for CB_COLOR0_BASE */
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;  /* Reloc for CB_COLOR0_ATTRIB */
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;  /* Reloc for CB_COLOR0_CMASK */
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;  /* Reloc for CB_COLOR0_FMASK */

      terakan_gfx_command_writer_emit_done(command_writer, p);
   }
}

static void
terakan_emit_compute_state(struct terakan_gfx_command_writer *command_writer,
                           struct terakan_pipeline_compute const *pipeline)
{
   /* Manual PM4 emission matching the exact Gallium evergreen_emit_cs_shader
    * byte sequence. We bypass Terakan's emit_with_bo + add_relocation because
    * the relocation system places the NOP at an offset the kernel CS validator
    * cannot find after COMPUTE_MODE SET_CONTEXT_REG packets.
    *
    * The kernel's radeon_cs_packet_next_reloc expects the NOP IMMEDIATELY
    * after the SET_CONTEXT_REG packet containing SQ_PGM_START_LS. */

   fprintf(stderr, "TERAKAN_COMPUTE: BO refs before compute: %u\n", command_writer->base.bo_reference_writer.reference_count);
   /* Register the shader BO for the CS submission relocation table */
   uint32_t bo_ref = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer,
      pipeline->shader.static_state.program_bo,
      true, false, TERAKAN_BO_PRIORITY_SHADER_BINARY);
   fprintf(stderr, "TERAKAN_COMPUTE: SHADER bo_ref=%u program_bo=%p\n", bo_ref, (void*)pipeline->shader.static_state.program_bo);

   /* -1. Minimal DB state: set depth/stencil surface array mode to LINEAR_GENERAL
    * to prevent the kernel CS validator from seeing uninitialized garbage.
    * DB_Z_INFO = 0x28040, DB_STENCIL_INFO = 0x28044.
    * ARRAY_MODE = 0 (LINEAR_GENERAL) satisfies the validator. */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 4);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0);
      *p++ = (0x28040 - 0x28000) >> 2; /* DB_Z_INFO offset */
      *p++ = 0;  /* ARRAY_MODE = LINEAR_GENERAL */
      *p++ = 0;  /* DB_STENCIL_INFO: ARRAY_MODE = LINEAR_GENERAL */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 0. CS_PARTIAL_FLUSH: reset the pipeline before compute dispatch.
    * This ensures prior graphics state (stencil, depth, CB) does not
    * contaminate the kernel CS validator surface tracking. */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 2);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
      *p++ = EVENT_TYPE_CS_PARTIAL_FLUSH | (4 << 8);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 1. SQ_PGM_START_LS + RESOURCES_LS + RESOURCES_LS_2 (with COMPUTE_MODE) */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 3, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_0288D0_SQ_PGM_START_LS);
      *p++ = pipeline->sq_pgm_start_cs;       /* SQ_PGM_START_LS (va >> 8) */
      *p++ = pipeline->sq_pgm_resources_cs[0] |
             S_0288D4_DX10_CLAMP(1);           /* SQ_PGM_RESOURCES_LS */
      *p++ = 0;                                /* SQ_PGM_RESOURCES_LS_2 */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* NOP relocation for SQ_PGM_START_LS — MUST be a separate packet
    * because the kernel CS validator calls radeon_cs_packet_next_reloc
    * AFTER advancing p->idx past the entire SET_CONTEXT_REG packet. */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 2);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_NOP, 0, 0);
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 2. SPI_COMPUTE_NUM_THREAD_X/Y/Z (COMPUTE_MODE) */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 3, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_0286EC_SPI_COMPUTE_NUM_THREAD_X);
      *p++ = pipeline->local_size[0];
      *p++ = pipeline->local_size[1];
      *p++ = pipeline->local_size[2];
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 3. VGT_NUM_INDICES = group_size */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = CONFIG_REG_OFFSET(R_008970_VGT_NUM_INDICES);
      *p++ = pipeline->group_size;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 4. VGT_COMPUTE_THREAD_GROUP_SIZE */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = CONFIG_REG_OFFSET(R_0089AC_VGT_COMPUTE_THREAD_GROUP_SZ);
      *p++ = pipeline->group_size;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 5. SQ_LDS_ALLOC (COMPUTE_MODE) */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_0288E8_SQ_LDS_ALLOC);
      *p++ = pipeline->sq_lds_alloc;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 6. VGT_COMPUTE_START_X/Y/Z = 0 */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 3, 0);
      *p++ = CONFIG_REG_OFFSET(R_00899C_VGT_COMPUTE_START_X);
      *p++ = 0;
      *p++ = 0;
      *p++ = 0;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }
}

/* ---- Vulkan dispatch entrypoints ---- */

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatch(VkCommandBuffer const commandBuffer,
                    uint32_t const groupCountX,
                    uint32_t const groupCountY,
                    uint32_t const groupCountZ)
{
   if (unlikely(groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)) {
      return;
   }

   struct terakan_command_buffer *cmd =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer *command_writer = cmd->command_writer.gfx;

   struct terakan_pipeline_compute const *pipeline =
      command_writer->bound_compute_pipeline;
   if (unlikely(pipeline == NULL)) {
      assert(!"vkCmdDispatch called without a bound compute pipeline");
      return;
   }

   /* Emit compute pipeline state if dirty (first bind or pipeline change) */
   if (command_writer->compute_pipeline_dirty) {
      terakan_emit_compute_state(command_writer, pipeline);
      terakan_emit_compute_resources(command_writer);
      /* Wire KCACHE (KC0) with SSBO address for compute addressing */
      if (command_writer->hw_state_sqc.resource_bos.fs[2])
         terakan_emit_compute_kcache(command_writer, command_writer->hw_state_sqc.resource_bos.fs[2]);
      command_writer->compute_pipeline_dirty = false;
   }

   /* Emit PKT3_DISPATCH_DIRECT with the grid dimensions */
   uint32_t *p = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
   if (unlikely(p == NULL)) {
      return;
   }
   *p++ = PKT3C(PKT3_DISPATCH_DIRECT, 3, 0);
   *p++ = groupCountX;
   *p++ = groupCountY;
   *p++ = groupCountZ;
   *p++ = 1; /* VGT_DISPATCH_INITIATOR = COMPUTE_SHADER_EN */
   terakan_gfx_command_writer_emit_done(command_writer, p);

#if 0 /* SURFACE_SYNC removed — MEM_RAT_CACHELESS bypasses CB cache */
   /* SURFACE_SYNC: flush CB (RAT write) caches so CPU can read the results.
    * CB_ACTION_ENA flushes the render target (RAT) write cache.
    * TC_ACTION_ENA flushes the texture cache (for any UAV reads).
    * SH_ACTION_ENA flushes shader exports. */
   {
      uint32_t *sync = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(sync == NULL)) return;
      *sync++ = PKT3(PKT3_SURFACE_SYNC, 3, 0);
      *sync++ = S_0085F0_CB_ACTION_ENA(1) | S_0085F0_TC_ACTION_ENA(1) |
                S_0085F0_SH_ACTION_ENA(1) |
                TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
      *sync++ = 0xFFFFFFFF; /* size = entire address space */
      *sync++ = 0;          /* base = 0 */
      *sync++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
      terakan_gfx_command_writer_emit_done(command_writer, sync);
   }
#endif
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatchBase(UNUSED VkCommandBuffer const commandBuffer,
                        UNUSED uint32_t const baseGroupX,
                        UNUSED uint32_t const baseGroupY,
                        UNUSED uint32_t const baseGroupZ,
                        UNUSED uint32_t const groupCountX,
                        UNUSED uint32_t const groupCountY,
                        UNUSED uint32_t const groupCountZ)
{
   /* TODO: Implement base group offset dispatch.
    * TeraScale-2 supports VGT_COMPUTE_START_X/Y/Z for base offsets. */
   assert(!"terakan_CmdDispatchBase not yet implemented");
}

VKAPI_ATTR void VKAPI_CALL
terakan_CmdDispatchIndirect(VkCommandBuffer const commandBuffer,
                            VkBuffer const bufferHandle,
                            VkDeviceSize const offset)
{
   struct terakan_command_buffer * const cmd =
      terakan_command_buffer_from_handle(commandBuffer);
   struct terakan_gfx_command_writer * const command_writer = cmd->command_writer.gfx;
   struct terakan_buffer const * const buffer = terakan_buffer_from_handle(bufferHandle);

   uint64_t const buffer_va = buffer->va + offset;
   struct terakan_bo * const bo = (struct terakan_bo *)buffer->bo;

   /* Compute-to-dispatch surface sync: flush TC (compute UAV writes),
    * invalidate VC + SH (CP reads dispatch params via vertex/shader cache).
    * This prevents the CP from reading stale pre-compute data. */
   {
      uint32_t * sync = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(sync == NULL)) return;
      *sync++ = PKT3(PKT3_SURFACE_SYNC, 4 - 1, 0);
      *sync++ = S_0085F0_TC_ACTION_ENA(1) | S_0085F0_VC_ACTION_ENA(1) |
                S_0085F0_SH_ACTION_ENA(1) | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
      *sync++ = UINT32_MAX; /* size (entire address space) */
      *sync++ = 0;          /* base address */
      *sync++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
      terakan_gfx_command_writer_emit_done(command_writer, sync);
   }

   /* PKT3_DISPATCH_INDIRECT: CP fetches (X, Y, Z) group counts from buffer_va.
    * The buffer must contain three uint32_t values at the given offset. */
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   *packet++ = PKT3(PKT3_DISPATCH_INDIRECT, 2 - 1, 0);
   uint32_t const * const packet_addr = packet;
   *packet++ = (uint32_t)buffer_va;
   *packet++ = ((buffer_va >> 32) & 0xFF);

   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_addr, packet_addr + 1,
      0, 0,
      terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer,
         bo, true, false, TERAKAN_BO_PRIORITY_DRAW_INDIRECT));

   terakan_gfx_command_writer_emit_done(command_writer, packet);
}
