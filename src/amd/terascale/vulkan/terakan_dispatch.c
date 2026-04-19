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
#include "terakan_push_constants.h"
#include "terakan_device.h"
#include "terakan_entrypoints.h"
#include "terakan_physical_device.h"
#include "terakan_state.h"

#include "amd/terascale/common/terascale_eg_sq.h"
#include "amd/terascale/common/terascale_evergreend.h"
#include "amd/terascale/common/terascale_wddm.h"
#include "gallium/drivers/r600/r600_opcodes.h"
#include "gallium/drivers/r600/r600_formats.h"

#include <assert.h>
#include <stdio.h>
#include "util/macros.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"

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
   (PKT_TYPE_S(3) | PKT_COUNT_S(count) | PKT3_IT_OPCODE_S(op) | PKT3_PREDICATE(predicate))
#endif
#ifndef PKT3C
#define PKT3C(op, count, predicate) (PKT3(op, count, predicate) | COMPUTE_MODE_BIT)
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


/* ===================================================================
 *  CONTEXT MULTIPLEXER — Single-Ring Compute on TeraScale 1/2/3
 * ===================================================================
 *
 * EVERY TeraScale generation under the Linux radeon kernel runs compute
 * on the shared GFX ring:
 *   TS1: R600, RV6xx, RV7xx, R700             (HD 2000–4000)
 *   TS2: Cedar, Palm, Juniper, Cypress,       (HD 5000–6300,
 *        Redwood, Caicos, Turks, Barts              incl. E-300 Palm)
 *   TS3: Cayman, Northern Islands              (HD 6800–6900, aka R9xx)
 *
 * The Linux radeon kernel driver only creates dedicated CP1/CP2 compute
 * rings for CHIP_TAHITI and later (SI+).  Every TeraScale family —
 * including R9xx/Cayman which has the hardware CP1/CP2 — has its
 * RADEON_CS_RING_COMPUTE requests silently redirected to the GFX ring by
 * drivers/gpu/drm/radeon/radeon_cs.c (around line 216).  This means
 * every compute dispatch lands mid-stream inside the graphics command
 * buffer, sharing the SQ, the VLIW ALUs, the CB_COLOR surfaces, and the
 * shader caches with in-flight draws.
 *
 * Without explicit boundary packets a compute dispatch will:
 *   1. race with in-flight pixel / vertex waves (PS still running when we
 *      touch SQ_GPR_RESOURCE_MGMT, causing an SQ config hazard),
 *   2. read stale texture / vertex / shader caches from the previous draw,
 *   3. commandeer CB_COLOR0 as a RAT without flushing the prior render
 *      target write path, corrupting framebuffer contents,
 *   4. leave SQ GPR allocation biased toward LS on the way out, starving
 *      the next draw's VS/PS stages,
 *   5. leave dirty TC / CB caches that the next draw reads back wrong.
 *
 * The Context Multiplexer injects mandatory boundary packets around each
 * compute dispatch:
 *
 *   BEGIN  (terakan_compute_multiplex_begin)
 *     • PS_PARTIAL_FLUSH        — drain all in-flight pixel/vertex waves
 *     • SURFACE_SYNC TC+VC+SH   — invalidate shader / vertex / texture
 *                                 caches so the compute kernel reads
 *                                 freshly-written uniforms and UBOs
 *     • SQ_CONFIG               — raise LS/CS priority, keep VC/EXPORT_SRC
 *     • SQ_GPR_RESOURCE_MGMT_3  — allocate GPRs to the LS (compute) stage
 *     • SQ_THREAD_RESOURCE_MGMT_2 — allocate thread slots to LS
 *
 *   DISPATCH
 *     • (existing: DB stub, compute state, SSBO RAT, KCACHE, push const,
 *       robustness, DISPATCH_DIRECT / DISPATCH_INDIRECT)
 *
 *   END    (terakan_compute_multiplex_end)
 *     • CS_PARTIAL_FLUSH        — wait for every compute wavefront to
 *                                 drain BEFORE the next graphics state
 *                                 overwrites SQ_PGM_START_LS — otherwise
 *                                 a still-executing wave would switch
 *                                 program mid-clause and hang the SQ
 *     • SURFACE_SYNC CB+TC+SH   — flush MEM_RAT writes out of the CB
 *                                 write cache, invalidate the texture /
 *                                 shader cache so the next draw re-reads
 *                                 descriptors that now point to compute
 *                                 output buffers
 *     • Mark 3D state pending   — the multiplexer has stomped every SQ
 *                                 program slot, every CB_COLOR surface,
 *                                 DB_Z_INFO, and the resource tables for
 *                                 FS.  Force the next draw path to fully
 *                                 re-emit them instead of trusting its
 *                                 cached-as-clean state.
 *
 * This is the ONLY correct sequence for Evergreen.  Skipping CS_PARTIAL_FLUSH
 * causes intermittent SQ hangs.  Skipping the post-dispatch dirty marking
 * causes the next draw to render with compute-biased GPR allocation and
 * corrupt shader output.  Skipping PS_PARTIAL_FLUSH at the begin boundary
 * causes a hardware hazard when SQ_GPR_RESOURCE_MGMT is written while a
 * pixel wave is still using those GPRs — documented in the Evergreen ISA
 * spec section 10.1 ("Switching Between Graphics and Compute Mode").
 */

/* Emit one PM4 EVENT_WRITE packet for an ME event (CS_PARTIAL_FLUSH,
 * PS_PARTIAL_FLUSH, etc.).  EVENT_INDEX(4) is the ME (micro-engine) index
 * which is the path the CP uses to wait for shader engine draining. */
static inline void
terakan_compute_multiplex_emit_event(
   struct terakan_gfx_command_writer * const command_writer,
   uint32_t const event_type)
{
   uint32_t * p = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 2);
   if (unlikely(p == NULL)) return;
   *p++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
   *p++ = EVENT_TYPE(event_type) | EVENT_INDEX(4);
   terakan_gfx_command_writer_emit_done(command_writer, p);
}

/* Emit a global SURFACE_SYNC that covers the entire address space.
 * cp_coher_cntl is an OR of S_0085F0_*_ACTION_ENA bits.  Poll interval 10
 * matches the convention in terakan_barrier.c and terakan_queue.c. */
static inline void
terakan_compute_multiplex_emit_surface_sync(
   struct terakan_gfx_command_writer * const command_writer,
   uint32_t const cp_coher_cntl)
{
   uint32_t * p = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
   if (unlikely(p == NULL)) return;
   *p++ = PKT3(PKT3_SURFACE_SYNC, 3, 0);
   *p++ = cp_coher_cntl | TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME;
   *p++ = 0xFFFFFFFF; /* size: entire address space */
   *p++ = 0;          /* base address */
   *p++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;
   terakan_gfx_command_writer_emit_done(command_writer, p);
}

static uint32_t
terakan_w1_max_barrier_cp_coher_cntl(void)
{
   return S_0085F0_CB_ACTION_ENA(1) |
          S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
          S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
          S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
          S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1) |
          S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
          S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1) |
          S_0085F0_DB_ACTION_ENA(1) | S_0085F0_DB_DEST_BASE_ENA(1) |
          S_0085F0_SMX_ACTION_ENA(1) | S_0085F0_TC_ACTION_ENA(1) |
          S_0085F0_SH_ACTION_ENA(1) | S_0085F0_VC_ACTION_ENA(1);
}

static uint32_t
terakan_w6_pre_dispatch_invalidate_cp_coher_cntl(void)
{
   return S_0085F0_SMX_ACTION_ENA(1) |
          S_0085F0_TC_ACTION_ENA(1) |
          S_0085F0_SH_ACTION_ENA(1) |
          S_0085F0_VC_ACTION_ENA(1);
}

static int32_t terakan_debug_w1_first_image_store_emitted = 0;
static int32_t terakan_debug_w4_warmup_dispatch_emitted = 0;

static void
terakan_emit_w4_warmup_dispatch_once(struct terakan_gfx_command_writer * const command_writer)
{
   if (!debug_get_bool_option("TERAKAN_W4_WARMUP_DISPATCH", false)) {
      return;
   }
   if (p_atomic_cmpxchg(&terakan_debug_w4_warmup_dispatch_emitted, 0, 1) != 0) {
      return;
   }

   uint32_t * p = terakan_gfx_command_writer_emit(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
   if (unlikely(p == NULL)) {
      return;
   }
   *p++ = PKT3C(PKT3_DISPATCH_DIRECT, 3, 0);
   *p++ = 0;
   *p++ = 0;
   *p++ = 0;
   *p++ = 1;
   terakan_gfx_command_writer_emit_done(command_writer, p);

   terakan_compute_multiplex_emit_event(command_writer, EVENT_TYPE_CS_PARTIAL_FLUSH);
   uint32_t const cp_coher_cntl = terakan_w6_pre_dispatch_invalidate_cp_coher_cntl();
   terakan_compute_multiplex_emit_surface_sync(command_writer, cp_coher_cntl);
   fprintf(stderr,
           "terakan/dispatch: W4 warmup dispatch emitted (0,0,0) cp_coher_cntl=0x%08x\n",
           cp_coher_cntl);
}

/* BEGIN boundary: prepare the shared ring for compute execution.
 *
 * Must be called before any compute state emission (shader, resources,
 * RAT, KCACHE, push constants).  Emits the fence-drain + cache-invalidate
 * + SQ-reconfig sequence that isolates the compute dispatch from any
 * prior graphics work.
 *
 * Runs on EVERY TeraScale generation under the Linux radeon kernel:
 *   - TeraScale 1 (R600/R700, HD 2000–4000)       → GFX ring forced
 *   - TeraScale 2 (Evergreen, HD 5000–6300 incl.  → GFX ring forced
 *                  Palm / Cedar / Juniper / etc.)
 *   - TeraScale 3 (Northern Islands / Cayman,     → GFX ring forced
 *                  HD 6800/6900 aka R9xx)
 * None of these expose dedicated compute rings via the Linux radeon
 * kernel driver — it only creates CP1/CP2 rings for CHIP_TAHITI and
 * later (see drivers/gpu/drm/radeon/radeon_cs.c RADEON_CS_RING_COMPUTE
 * handler, which silently redirects to GFX for family < TAHITI).  So
 * every TeraScale chip needs the same fence-boundary treatment.
 *
 * Where the generations DIFFER is in how the SQ allocates registers to
 * the LS (compute) stage:
 *   - TS1/TS2: static allocation via SQ_GPR_RESOURCE_MGMT_3.NUM_LS_GPRS
 *              and SQ_THREAD_RESOURCE_MGMT_2.NUM_LS_THREADS.
 *   - TS3/R9xx: dynamic GPR mode (SQ_DYN_GPR_ENABLE=1, set once at
 *               command-buffer init in terakan_command_buffer.c).  The
 *               SQ allocates GPRs per wave from SQ_PGM_RESOURCES_LS.
 *               No static allocation is possible or needed.
 *
 * Steps 1–3 run on ALL TS generations.
 * Steps 4–5 run only on TS1/TS2 (gated by !is_r9xx) because R9xx uses
 * the dynamic GPR path and would reject these register writes or
 * interpret the bit layout differently.
 */
static void
terakan_compute_multiplex_begin(
   struct terakan_gfx_command_writer * const command_writer)
{
   struct terakan_device const * const device =
      terakan_gfx_command_writer_device(command_writer);
   struct terakan_physical_device const * const phys_dev =
      terakan_device_physical_device(device);
   struct terakan_physical_device_chip_info const * const chip_info =
      &phys_dev->chip_info;
   bool const is_r9xx = chip_info->is_r9xx;
   bool const skip_begin_sync =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_BEGIN_SYNC", false);
   bool const skip_begin_resource_mgmt =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_BEGIN_RESOURCE_MGMT", false);
   bool const skip_begin_stage_enable =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_BEGIN_STAGE_ENABLE", false);
   bool const skip_begin_sq_config =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_BEGIN_SQ_CONFIG", false);
   bool const skip_begin_primitive =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_BEGIN_PRIMITIVE", false);

   /* (1) Canonical compute-boundary flush.
    *
    * Match Gallium's evergreen_init_atom_start_compute_cs(): emit only
    * CS_PARTIAL_FLUSH at the begin boundary.  The previous expanded
    * sequence (PS_PARTIAL_FLUSH + WAIT_UNTIL + CACHE_FLUSH_AND_INV_EVENT +
    * SURFACE_SYNC) was not part of the proven Evergreen compute preamble
    * and can wedge this hardware path.
    *
    * CACHE EVENT INVARIANTS on TeraScale-2 / Sumo (learned the hard way):
    *
    *   - EVENT_TYPE_CACHE_FLUSH_AND_INV_EVENT (0x16, EVENT_INDEX=0):
    *     SAFE on GFX ring.  Drains per-SIMD L1 texture / shader instruction
    *     caches.  Used by terakan_barrier.c:497-501 for HOST_WRITE ->
    *     SHADER_READ coherency and by r600g r600_hw_context.c:144-147.
    *
    *   - EVENT_TYPE_CACHE_FLUSH_AND_INV_TS_EVENT (0x14, EVENT_INDEX=5):
    *     *** HANGS Sumo *** when emitted on the GFX ring with compute
    *     work in flight.  Writes a timestamp at EOP, but the Sumo micro-
    *     engine cannot reconcile this with compute queue state and wedges.
    *     NEVER emit on GFX.  (Historically we tried this in the compute
    *     preamble and lost a debug session to it.)
    *
    *   - EVENT_TYPE_FLUSH_AND_INV_CB_DATA_TS (0x38, EVENT_INDEX=5):
    *     SAFE on GFX ring.  CB-specific EOP fence that only drains the
    *     CB write queue; used by terakan_compute_multiplex_end to ensure
    *     MEM_RAT writes retire before a subsequent graphics render pass
    *     rebinds CB_COLOR0_BASE.
    *
    *   - EVENT_TYPE_{PS,CS,VS}_PARTIAL_FLUSH (0x0F/0x13/0x0A, EVENT_INDEX=4):
    *     SAFE.  Drains a specific pipeline stage.  CS_PARTIAL_FLUSH is
    *     required on the compute END boundary before touching SQ_PGM_*
    *     (or a mid-clause wave can be stomped and hang the SQ).
    *
    * Host -> compute coherency is the BARRIER path's responsibility.
    * Vulkan spec requires the application to issue a vkCmdPipelineBarrier
    * with VK_ACCESS_HOST_WRITE_BIT -> VK_ACCESS_SHADER_READ_BIT; that
    * barrier flows through terakan_barrier.c and emits 0x16.  The compute
    * preamble does NOT need to redundantly invalidate TC here. */
   if (!skip_begin_sync) {
      uint32_t const begin_sync_event =
         debug_get_bool_option("TERAKAN_BEGIN_SYNC_PS_FLUSH", false)
            ? EVENT_TYPE_PS_PARTIAL_FLUSH
            : EVENT_TYPE_CS_PARTIAL_FLUSH;
      terakan_compute_multiplex_emit_event(command_writer, begin_sync_event);
   }

   if (debug_get_bool_option("TERAKAN_W6_PRE_DISPATCH_INVALIDATE", false)) {
      uint32_t const cp_coher_cntl = terakan_w6_pre_dispatch_invalidate_cp_coher_cntl();
      terakan_compute_multiplex_emit_surface_sync(command_writer, cp_coher_cntl);
      fprintf(stderr,
              "terakan/dispatch: W6 pre-dispatch invalidate cp_coher_cntl=0x%08x\n",
              cp_coher_cntl);
   }

   /* (3) Re-emit SQ_CONFIG with compute-biased priorities.  The draw path
    * may have written SQ_CONFIG with PS_PRIO > CS_PRIO; compute runs on
    * the LS stage so we want LS_PRIO and CS_PRIO at maximum.  We keep
    * VC_ENABLE and EXPORT_SRC_C intact because compute still uses the
    * vertex cache for KCACHE fetches and the export path for MEM_RAT.
    *
    * Runs on: TS1, TS2, TS3.  The SQ_CONFIG priority field layout
    * (LS_PRIO, CS_PRIO, etc.) is identical on Evergreen and Cayman —
    * verified against terascale_evergreend.h definitions at lines 319–347.
    * Our command-buffer-init code at terakan_command_buffer.c:904 skipped
    * the priority setup on R9xx with the comment "doesn't expose the
    * compute rings at all" — but under the Linux radeon kernel, R9xx
    * IS forced onto the GFX ring, so it needs these priorities too. */
   if (!skip_begin_sq_config) {
      uint32_t const sq_config =
         S_008C00_VC_ENABLE(chip_info->has_vertex_cache) |
         S_008C00_EXPORT_SRC_C(1) |
         S_008C00_LS_PRIO(3) | S_008C00_HS_PRIO(0) |
         S_008C00_ES_PRIO(0) | S_008C00_GS_PRIO(0) |
         S_008C00_VS_PRIO(0) | S_008C00_PS_PRIO(0) |
         S_008C00_CS_PRIO(3);
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = (R_008C00_SQ_CONFIG - 0x8000) >> 2;
      *p++ = sq_config;
      terakan_gfx_command_writer_emit_done(command_writer, p);
      /* Remember that SQ_CONFIG is now in compute-biased mode so
       * terakan_pipeline_graphics_bind() can restore graphics priorities. */
      command_writer->sq_config_is_compute_mode = true;
   }

   /* (3a) Set primitive type to POINTLIST for compute.
    *
    * Without this, the VGT interprets compute threads using the previous
    * draw call's primitive topology (typically TRILIST), causing wavefront
    * scheduling deadlocks.  Gallium writes this in
    * evergreen_init_atom_start_compute_cs() at line ~665.
    *
    * CONFIG register — no COMPUTE_MODE_BIT (global, not shadowed). */
   if (!skip_begin_primitive) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = CONFIG_REG_OFFSET(R_008958_VGT_PRIMITIVE_TYPE);
      *p++ = V_008958_DI_PT_POINTLIST;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (4) Zero ALL static GPR reservations for compute — TS1/TS2 ONLY.
    *
    * On R600/R700/Evergreen, the SQ partitions the 256-entry GPR file
    * statically across stages via SQ_GPR_RESOURCE_MGMT_{1,2,3}.  For
    * compute dispatch, ALL graphics stage reservations must be zeroed
    * so the entire pool is available to the dynamic allocator.  The SQ
    * then allocates per-wave based on SQ_PGM_RESOURCES_LS.NUM_GPRS.
    *
    * BUG FIX: Previously we only wrote MGMT_3 (LS=0x70) and left
    * MGMT_1 and MGMT_2 at their command-buffer-init values.  Although
    * init sets PS=VS=GS=ES=0, a static LS=112 reservation conflicts
    * with the dynamic allocator — Gallium's evergreen_compute.c:362
    * zeros ALL three MGMT registers and relies purely on dynamic mode.
    * Match that proven pattern exactly.
    *
    * On Cayman (R9xx), GPR allocation uses the DYNAMIC GPR path and
    * these registers have different semantics, so we skip them. */
   if (!is_r9xx && !skip_begin_resource_mgmt) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 3, 0);
      *p++ = (R_008C04_SQ_GPR_RESOURCE_MGMT_1 - 0x8000) >> 2;
      *p++ = S_008C04_NUM_PS_GPRS(0) | S_008C04_NUM_VS_GPRS(0) |
             S_008C04_NUM_CLAUSE_TEMP_GPRS(4);
      *p++ = S_008C08_NUM_GS_GPRS(0) | S_008C08_NUM_ES_GPRS(0);
      *p++ = S_008C0C_NUM_HS_GPRS(0) | S_008C0C_NUM_LS_GPRS(0);
      terakan_gfx_command_writer_emit_done(command_writer, p);

      /* Re-emit DYN_GPR_ENABLE after writing MGMT registers.
       * Gallium does this on every compute dispatch (evergreen_compute.c:365).
       * Even though command-buffer init already enables it, re-emitting
       * ensures consistency if a draw path ever toggled it off. */
      p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = (R_008D8C_SQ_DYN_GPR_CNTL_PS_FLUSH_REQ - 0x8000) >> 2;
      *p++ = S_008D8C_DYN_GPR_ENABLE(1);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (5) Static LS thread slot allocation — TS1/TS2 ONLY.
    *
    * Same reasoning as step 4: TS1/TS2 partitions thread slots
    * statically via SQ_THREAD_RESOURCE_MGMT_2, while R9xx uses dynamic
    * per-wave allocation.
    *
    * IMPORTANT: Evergreen compute uses NUM_LS_THREADS=128 (not 0xFF).
    * Gallium programs 128 for all Evergreen families in
    * evergreen_init_atom_start_compute_cs(); over-allocating this field
    * can block wave launch on Palm and wedge the queue at fence wait. */
   if (!is_r9xx && !skip_begin_resource_mgmt) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = (R_008C1C_SQ_THREAD_RESOURCE_MGMT_2 - 0x8000) >> 2;
      *p++ = S_008C1C_NUM_HS_THREADS(0) | S_008C1C_NUM_LS_THREADS(128);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (6) Zero out PS/VS/GS/ES thread slots — TS1/TS2 ONLY.
    *
    * Gallium's evergreen_init_atom_start_compute_cs() zeroes
    * SQ_THREAD_RESOURCE_MGMT_1 to prevent PS/VS/GS/ES waves from
    * stealing thread slots that LS needs for compute. */
   if (!is_r9xx && !skip_begin_resource_mgmt) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = (R_008C18_SQ_THREAD_RESOURCE_MGMT_1 - 0x8000) >> 2;
      *p++ = 0;  /* PS=0, VS=0, GS=0, ES=0 */
      terakan_gfx_command_writer_emit_done(command_writer, p);

      /* Invalidate the sq_tmp mirror so terakan_pipeline_graphics_bind()
       * detects the mismatch and re-emits the graphics thread allocations.
       *
       * WHY: the mirror tracks the EXPECTED hardware value for
       * SQ_THREAD_RESOURCE_MGMT_1/2.  By writing 0 here we force the
       * memcmp in pipeline_graphics_bind to fail, which triggers the
       * existing PS_PARTIAL_FLUSH + SET_CONFIG_REG re-emission path that
       * restores the correct VS/PS thread counts before the next draw.
       * Without this, the mirror stays at the graphics value, pipeline_bind
       * sees a false match, skips the re-emit, and VS/PS wavefronts have
       * zero thread slots -- the SQ deadlocks at the first draw. */
      command_writer->state_draw.sq_tmp.sq_thread_resource_mgmt[0] = 0;
      command_writer->state_draw.sq_tmp.sq_thread_resource_mgmt[1] =
         S_008C1C_NUM_HS_THREADS(0) | S_008C1C_NUM_LS_THREADS(128);
   }

   /* (7) Zero out stack entries for graphics stages, allocate for LS.
    * Matches Gallium: STACK_1=0, _2=0, _3=NUM_LS_STACK_ENTRIES(256). */
   if (!is_r9xx && !skip_begin_resource_mgmt) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 3, 0);
      *p++ = (R_008C20_SQ_STACK_RESOURCE_MGMT_1 - 0x8000) >> 2;
      *p++ = 0;  /* SQ_STACK_RESOURCE_MGMT_1: PS=0, VS=0 */
      *p++ = 0;  /* SQ_STACK_RESOURCE_MGMT_2: GS=0, ES=0 */
      *p++ = S_008C28_NUM_HS_STACK_ENTRIES(0) |
             S_008C28_NUM_LS_STACK_ENTRIES(0x100);  /* 256 for LS */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (8) LDS resource management — give all LDS to LS stage.
    * Matches Gallium: NUM_PS_LDS=0, NUM_LS_LDS=8192. */
   if (!is_r9xx && !skip_begin_resource_mgmt) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = (R_008E2C_SQ_LDS_RESOURCE_MGMT - 0x8000) >> 2;
      *p++ = S_008E2C_NUM_PS_LDS(0) | S_008E2C_NUM_LS_LDS(8192);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

/* (8a) SQ_DYN_GPR_RESOURCE_LIMIT_1 — pre-Cayman silicon erratum.    *    * Gallium writes this in evergreen_init_atom_start_compute_cs() via    * r600_store_context_reg(), which uses cb->pkt_flags to OR in    * RADEON_CP_PACKET3_COMPUTE_MODE.  This is a CONTEXT register    * (0x028838 >= 0x28000) so COMPUTE_MODE_BIT is REQUIRED to route    * the write to the compute shadow register bank.  Without it, the    * write lands in the 3D shadow and DISPATCH_DIRECT reads stale/zero    * GPR limits from the compute shadow — SQ deadlock, GPU hang.    *    * Each field clamped to 0x1e (30 GPRs) per stage, matching Gallium. */
   if (!is_r9xx && !skip_begin_resource_mgmt) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_028838_SQ_DYN_GPR_RESOURCE_LIMIT_1);
      *p++ = S_028838_PS_GPRS(0x1e) | S_028838_VS_GPRS(0x1e) |
             S_028838_GS_GPRS(0x1e) | S_028838_ES_GPRS(0x1e) |
             S_028838_HS_GPRS(0x1e) | S_028838_LS_GPRS(0x1e);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (9) VGT_GS_MODE: switch to compute mode.
    * Without COMPUTE_MODE, VGT remains in graphics mode and interferes
    * with compute wavefront scheduling.  PARTIAL_THD_AT_EOI handles
    * partial wavefronts at the end of a workgroup. */
   if (!skip_begin_stage_enable) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_028A40_VGT_GS_MODE);
      *p++ = S_028A40_COMPUTE_MODE(1) | S_028A40_PARTIAL_THD_AT_EOI(1);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (10) VGT_SHADER_STAGES_EN: enable compute shader stage.
    * LS_EN=2 tells SQ to route LS-stage waves as compute.
    * All other stages disabled. */
   if (!skip_begin_stage_enable) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_028B54_VGT_SHADER_STAGES_EN);
      *p++ = S_028B54_LS_EN(2);  /* CS_ON */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* (11) Initialize LS-stage loop constant for hardware loop counters.
    *
    * Gallium writes SQ_LOOP_CONST at offset 160 (= LS/CS stage base)
    * via eg_store_loop_const() in evergreen_init_atom_start_compute_cs().
    * Value 0x01000FFF encodes: STEP=1, START=0, MAX=4095.  Without this,
    * any shader using LOOP_START_DX10 will execute with garbage loop
    * bounds, causing infinite ALU loops and a GPU soft-reset.
    *
    * PKT3_SET_LOOP_CONST offset is a dword index from the loop constant
    * base (EVERGREEN_LOOP_CONST_OFFSET = 0x3A200).  WITH COMPUTE_MODE_BIT
    * to route to the LS/CS stage shadow, matching eg_store_loop_const(). */
   if (!skip_begin_stage_enable) {
      uint32_t * p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_LOOP_CONST, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = 160;  /* LS/CS stage loop const base offset */
      *p++ = 0x01000FFF;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }
}

/* END boundary: restore the shared ring to a safe post-compute state.
 *
 * Must be called AFTER the DISPATCH_DIRECT / DISPATCH_INDIRECT packet.
 * Drains compute waves, flushes compute outputs, and marks every 3D state
 * index that the dispatch sequence stomped so the next draw call will
 * re-emit its own shader programs, resources, and CB surfaces.
 *
 * Failure to mark state pending here is the #1 cause of post-compute draw
 * hangs and is exactly the silent-corruption case the Context Multiplexer
 * exists to defeat. */

/* TERAKAN_DEBUG_FLUSH_WATERMARKS helper.  Emits a PKT3_EVENT_WRITE_EOP
 * (evergreend.h:83 PKT3_EVENT_WRITE_EOP=0x47) that stores a 32-bit
 * `seq` value into the device's scratch BO at (slot_index*8) bytes
 * offset when the event fires.  Pattern mirrors terakan_query.c:669-685
 * (timestamp emission) but with DATA_SEL=VALUE_32BIT instead of
 * TIMESTAMP so we write our diagnostic seq rather than the GPU clock.
 *
 * Why EVENT_WRITE_EOP and not PKT3_MEM_WRITE (0x3D):
 *   - MEM_WRITE is validated by the radeon CS parser with strict
 *     security constraints; early live experiment with MEM_WRITE
 *     stalled ring 0 for 30+ seconds.
 *   - EVENT_WRITE_EOP is the battle-tested path every Vulkan/OpenGL
 *     queue submit uses for fence emissions.  The kernel CS parser
 *     has an exhaustive fast-path for it (see
 *     drivers/gpu/drm/radeon/evergreen_cs.c validator).
 *   - Uses terakan_gfx_command_writer_add_relocation_for_40_bits
 *     for the 40-bit address reloc, matching what the query sample
 *     emitter already does successfully.
 *
 * Packet layout (5 body dwords per PKT3(count=4)):
 *   body[0]: EVENT_TYPE(BOTTOM_OF_PIPE_TS) | EVENT_INDEX(5)
 *   body[1]: address_lo (patched by kernel reloc)
 *   body[2]: (address_hi & 0xff) | EOP_INT_SEL(NONE) | EOP_DATA_SEL(VALUE_32BIT)
 *   body[3]: seq (data_lo)
 *   body[4]: 0 (data_hi, unused for 32-bit)
 */

/* EOP field encoders from r600d_common.h (not pulled into terakan
 * headers -- defined here local to the watermark helper). */
#ifndef EOP_INT_SEL
#define EOP_INT_SEL(x)        ((x) << 24)
#define EOP_INT_SEL_NONE      0
#endif
#ifndef EOP_DATA_SEL
#define EOP_DATA_SEL(x)       ((x) << 29)
#define EOP_DATA_SEL_VALUE_32BIT  1
#endif

void
terakan_emit_flush_watermark(struct terakan_gfx_command_writer * const command_writer,
                             unsigned const slot_index, uint32_t const seq)
{
   struct terakan_device const * const device =
      terakan_gfx_command_writer_device(command_writer);
   if (!device->debug_watermarks_enabled || device->debug_watermark_bo == NULL) {
      return;
   }

   /* emit_with_bo: 6 body dwords (1 header + 5 body), 1 BO reference,
    * 0 standalone relocs (the 40-bit helper writes the 2 reloc NOPs
    * through the emit cursor).  Pattern copied verbatim from
    * terakan_query.c:664-685. */
   uint32_t * packet = terakan_gfx_command_writer_emit_with_bo(
      command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 6, 1, 0, 1);
   if (unlikely(packet == NULL)) {
      return;
   }
   uint64_t const slot_offset = (uint64_t)(slot_index * 8u);
   *packet++ = PKT3(PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   *packet++ = EVENT_TYPE(EVENT_TYPE_BOTTOM_OF_PIPE_TS) | EVENT_INDEX(5);
   uint32_t const * const packet_address = packet;
   *packet++ = (uint32_t)slot_offset;
   *packet++ = ((uint32_t)(slot_offset >> 32) & 0xffu) |
               EOP_INT_SEL(EOP_INT_SEL_NONE) |
               EOP_DATA_SEL(EOP_DATA_SEL_VALUE_32BIT);
   *packet++ = seq;                                  /* data_lo */
   *packet++ = 0;                                    /* data_hi (unused) */
   terakan_gfx_command_writer_add_relocation_for_40_bits(
      command_writer, &packet, packet_address, packet_address + 1,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_LO,
      TERASCALE_WDDM_PATCH_IDS_EVENT_WRITE_EOP_HI,
      terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer,
         device->debug_watermark_bo, false, true, TERAKAN_BO_PRIORITY_QUERY));
   terakan_gfx_command_writer_emit_done(command_writer, packet);
}

static void
terakan_compute_multiplex_end(
   struct terakan_gfx_command_writer * const command_writer)
{
   /* Watermark checkpoint 2: POST-dispatch, PRE-EOP.  Now via
    * EVENT_WRITE_EOP (battle-tested CS parser path). */
   {
      struct terakan_device const * const dev =
         terakan_gfx_command_writer_device(command_writer);
      if (dev->debug_watermarks_enabled) {
         terakan_emit_flush_watermark(command_writer, 1, 0xBBBB2222u);
      }
   }

   /* (1) Wait for every compute wavefront to drain.  Without this the
    * next graphics SET_CONTEXT_REG that touches SQ_PGM_START_LS can
    * overwrite the program pointer while a wave is mid-clause, which
    * hangs the SQ immediately and takes the whole ring down with it. */
   terakan_compute_multiplex_emit_event(
      command_writer, EVENT_TYPE_CS_PARTIAL_FLUSH);

   /* (2) EOP fence: force CB data/UAV write queue to drain to memory.
    *
    * Compute MEM_RAT writes go CB_COLOR{M} exporter -> CB write queue ->
    * memory.  A subsequent graphics CmdBeginRenderPass that binds a
    * different buffer to CB_COLOR0 can cause the still-pending compute
    * writes to target the new (render pass) base, corrupting the
    * framebuffer or losing the compute output entirely (res=0 symptom
    * in dEQP-VK.draw.renderpass.concurrent.compute_and_triangle_list).
    *
    * FLUSH_AND_INV_CB_DATA_TS (event 0x38, EVENT_INDEX=5 EOP) generates
    * an end-of-pipe timestamp that fires only after ALL CB-data writes
    * retire to memory.  Unlike CACHE_FLUSH_AND_INV_TS_EVENT (0x14) which
    * hangs Sumo on the GFX ring with compute in flight, the CB_DATA_TS
    * variant is the safe CB-specific drain.  Same wrapper that
    * terakan_barrier.c:414 uses for UAV writes.
    *
    * The no-TS CACHE_FLUSH_AND_INV_EVENT (0x16) would drain per-SIMD L1
    * caches but it is NOT an EOP event — it does not guarantee CB write
    * queue retirement.  Use 0x38 here, 0x16 only if host->shader_read
    * coherency is also needed (it is not in the post-compute path). */
   terakan_gfx_command_writer_emit_event_write_eop_discarding_data(
      command_writer, EVENT_TYPE(EVENT_TYPE_FLUSH_AND_INV_CB_DATA_TS) | EVENT_INDEX(5));

   /* Watermark checkpoint 3: POST-EOP, PRE-SURFACE_SYNC. */
   {
      struct terakan_device const * const dev =
         terakan_gfx_command_writer_device(command_writer);
      if (dev->debug_watermarks_enabled) {
         terakan_emit_flush_watermark(command_writer, 2, 0xCCCC3333u);
      }
   }

   /* (3) Flush MEM_RAT writes out of the CB write cache and invalidate
    * read caches.  Compute SSBO writes on Evergreen go CB_COLOR{M} ->
    * CB write queue -> memory; without CB_ACTION_ENA the next draw (or
    * the CPU after a buffer->host barrier) can read stale pre-compute
    * data.  TC_ACTION_ENA + SH_ACTION_ENA cover the case where the next
    * draw samples from a buffer that compute just wrote.
    *
    * LI-2026-04-17-02: CB_ACTION_ENA alone only drains the CB control
    * cache; to also drain the per-slot CB_COLOR{M} DEST_BASE write
    * queues we must set each CB{M}_DEST_BASE_ENA bit that the dispatch
    * could have written to.  SFN currently routes every compute store
    * to RAT0, but once per-store RAT ids land (or multi-image binding
    * goes live with the Phase-3 CS+168 REAL emission) slots 1..7 come
    * into play.  Setting all 12 bits defensively covers every RAT slot
    * Evergreen exposes; the cost is just OR'ing a few immediate bits
    * into the already-emitted SURFACE_SYNC, no extra packet. */
   /* LI-2026-04-18-01 (new): SMX_ACTION_ENA pairs canonically with
    * CB_ACTION_ENA per r600 gallium (r600_hw_context.c:173-192).
    * SMX is the shader memory exporter's coalescing write buffer
    * sitting ahead of the CB exporter in the write path; MEM_RAT
    * STORE_TYPED flows shader -> SMX -> CB -> memory.  Without
    * S_0085F0_SMX_ACTION_ENA(1) the SMX-level cache keeps tag lines
    * live even after CB drains to memory, so a subsequent dispatch
    * that reuses (or is near) the same BO VA can pick up stale
    * SMX-held state and read-modify-write against ghost bytes --
    * the observed cross-test failure pattern in
    * dEQP-VK.image.store.*.single_layer sweeps (first of the format
    * family passes, second -- the same-format single_layer variant
    * -- fails).  See steinmarder findings
    * 2026-04-17-single-layer-cross-test-leak.md + Evergreen oracle
    * pairing. */
   terakan_compute_multiplex_emit_surface_sync(
      command_writer,
      S_0085F0_CB_ACTION_ENA(1) |
      S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
      S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
      S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
      S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1) |
      S_0085F0_CB8_DEST_BASE_ENA(1) | S_0085F0_CB9_DEST_BASE_ENA(1) |
      S_0085F0_CB10_DEST_BASE_ENA(1) | S_0085F0_CB11_DEST_BASE_ENA(1) |
      S_0085F0_SMX_ACTION_ENA(1) |
      S_0085F0_TC_ACTION_ENA(1) |
      S_0085F0_SH_ACTION_ENA(1) |
      S_0085F0_VC_ACTION_ENA(1));

   /* Watermark checkpoint 4: POST-SURFACE_SYNC. */
   {
      struct terakan_device const * const dev =
         terakan_gfx_command_writer_device(command_writer);
      if (dev->debug_watermarks_enabled) {
         terakan_emit_flush_watermark(command_writer, 3, 0xDDDD4444u);
      }
   }

   /* (3) Mark every draw-state index the compute path stomped as pending.
    * The next draw call will hit terakan_state_draw_apply_pending() and
    * re-emit the real graphics versions of these registers, overwriting
    * whatever compute left behind.
    *
    * This list is the INTERSECTION of:
    *   (a) everything terakan_emit_compute_state() writes,
    *   (b) everything terakan_emit_compute_resources() writes,
    *   (c) everything terakan_emit_compute_kcache() writes,
    *   (d) everything terakan_compute_multiplex_begin() writes (SQ config
    *       + GPR / thread resource management).
    *
    * Rather than listing ~15 individual indices and drifting out of date
    * every time the compute emitters change, we mark the whole SQ / CB /
    * DB family.  The cost is a few extra register writes on the very next
    * draw, which is dwarfed by the dispatch + flush we just did. */
   struct terakan_state_draw * const state = &command_writer->state_draw;
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_SQ_PGM_LS_HS_ES_GS_VS);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_SQ_PGM_FS);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_SQ_RESOURCES_FS);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_SQ_PGM_PS);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_SQ_TMP_LS_HS_ES_GS_VS);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_SQ_TMP_PS);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_DB_DEPTH_STENCIL_BUFFER);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_CB_COLOR_RTV);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_CB_COLOR_UAV);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_CB_TARGET_MASK);
   terakan_state_draw_set_pending(state,
      TERAKAN_STATE_DRAW_INDEX_CB_COLOR_CONTROL);

   /* Same-cmdb compute->draw does not naturally cross an indirect-buffer
    * boundary, so explicitly re-arm draw re-emission as boundary emulation.
    * Two-cmdb submissions already get this at command-writer IB begin. */
   terakan_hw_state_draw_indirect_buffer_begun(&command_writer->hw_state_draw);
}

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
#define R600_IMAGE_IMMED_RESOURCE_OFFSET     160
#define R600_IMAGE_REAL_RESOURCE_OFFSET      168
#define TERAKAN_MAX_COMPUTE_STORAGE_IMAGES   8

/* Invariant LI-2026-04-17-04: The IMMED and REAL resource slot windows must
 * not overlap.  A storage image m occupies slots {IMMED+m, REAL+m}, so the
 * separation must be at least TERAKAN_MAX_COMPUTE_STORAGE_IMAGES.  Violating
 * this lets image m's REAL overwrite image (m - delta)'s IMMED and hang the
 * CB exporter. */
static_assert(R600_IMAGE_REAL_RESOURCE_OFFSET - R600_IMAGE_IMMED_RESOURCE_OFFSET >=
                 TERAKAN_MAX_COMPUTE_STORAGE_IMAGES,
              "IMMED/REAL compute storage image slot windows must not overlap");

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
   /* The SFN compiler generates KCACHE reads from KC0[0].x to obtain the
    * SSBO's byte offset within the RAT buffer.  In Gallium, SSBOs live in a
    * shared "compute_memory_pool" BO and the driver writes each SSBO's
    * pool-relative byte offset into a constant buffer bound to KC0.  The
    * shader does: MEM_RAT_INDEX = (KC0[0].x + tid * stride) >> 2.
    *
    * In Terakan/Vulkan each SSBO is its own BO, mapped 1:1 to CB_COLOR0.
    * The byte offset is therefore 0.  We allocate a small push-buffer region
    * and write 0 so KC0[0].x reads the correct value.
    *
    * BUG FIX: Previously we pointed ALU_CONST_CACHE_LS_0 at the SSBO data
    * BO itself.  KC0[0].x then read the first dword of user data (typically
    * 0xdeadbeef from the fill pattern), producing a wildly wrong RAT index
    * that wrote far beyond the buffer — making the output appear untouched. */

   /* Allocate a 256-byte push-buffer region for KC0 and zero it */
   struct terakan_bo const *kc0_bo = NULL;
   uint32_t kc0_va_lines = 0;
   uint32_t *kc0_map = (uint32_t *)terakan_push_buffer_allocate_kcache(
      command_writer->base.command_buffer, 256, &kc0_bo, &kc0_va_lines);
   if (unlikely(kc0_map == NULL)) {
      return;
   }
   memset(kc0_map, 0, 256);
   /* KC0[0].x = 0 (byte offset of SSBO within its BO) */


   /* Register the KC0 BO for relocation */
   uint32_t bo_ref = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer,
      kc0_bo, true, false, TERAKAN_BO_PRIORITY_UNIFORM_BUFFER);
   if (debug_get_bool_option("TERAKAN_DEBUG_COMPUTE_DESC", false)) {
      fprintf(stderr,
              "terakan/compute_kcache: kc0_bo=%p va_lines=0x%08x bo_ref=%u\n",
              (void *)kc0_bo, kc0_va_lines, bo_ref);
   }

   /* 1. ALU_CONST_BUFFER_SIZE_LS_0 = 1 (one 256-byte KCACHE line) */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0 - 0x28000) >> 2;
      *p++ = 1;  /* 1 × 256 bytes */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 2. ALU_CONST_CACHE_LS_0 = KC0 BO base address + NOP reloc */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028F40_ALU_CONST_CACHE_LS_0 - 0x28000) >> 2;
      *p++ = 0;  /* Base address (relocated by kernel via NOP) */
      *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 3. SET_RESOURCE for the constant fetch at KC0 slot.
    *    The SQ fetches constants via this VTX descriptor.  Point it at
    *    the same zero-filled push-buffer BO. */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 12);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_RESOURCE, 8, 0) | COMPUTE_MODE_BIT;
      *p++ = EG_FETCH_CONSTANTS_OFFSET_CS * 8;
      *p++ = 0;                            /* WORD0: base (relocated) */
      *p++ = 256 - 1;                      /* WORD1: size - 1 */
      *p++ = S_030008_STRIDE(16) |                           /* WORD2: 16 bytes per vec4 */
             S_030008_DATA_FORMAT(FMT_32_32_32_32_FLOAT);   /* Gallium-matching format */
      *p++ = S_03000C_DST_SEL_X(V_03000C_SQ_SEL_X) |
             S_03000C_DST_SEL_Y(V_03000C_SQ_SEL_Y) |
             S_03000C_DST_SEL_Z(V_03000C_SQ_SEL_Z) |
             S_03000C_DST_SEL_W(V_03000C_SQ_SEL_W);
      *p++ = 0;                            /* WORD4 */
      *p++ = 0;                            /* WORD5 */
      *p++ = 0;                            /* WORD6 */
      *p++ = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER);
      *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }
}

/* Emit a KCACHE bank binding to LS registers for compute dispatch.
 *
 * The deferred hw_state KCACHE system emits to PS registers (3D shadow) and
 * never includes COMPUTE_MODE_BIT.  Compute reads from LS registers in the
 * compute shadow.  This function directly emits PM4 to
 * ALU_CONST_BUFFER_SIZE_LS_<bank> and ALU_CONST_CACHE_LS_<bank> with
 * COMPUTE_MODE_BIT, matching the proven pattern in terakan_emit_compute_kcache.
 */
/* FIX-G (C-2026-04-19-03): exported so terakan_pipeline_layout.c can call this
 * for compute UBO descriptor-set bindings.  Without this, application UBOs at
 * bindings > 0 never reach the compute shader's LS-side KCACHE banks -- only
 * PS-side (which the compute shader doesn't read).  Gated caller-side on
 * TERAKAN_FIX_G_UBO_WIRING=1 during validation. */
void
terakan_emit_compute_kcache_bank(struct terakan_gfx_command_writer *command_writer,
                                 uint32_t bank,
                                 struct terakan_bo const *bo,
                                 uint32_t va_kcache_lines,
                                 uint32_t size_lines);

void
terakan_emit_compute_kcache_bank(struct terakan_gfx_command_writer *command_writer,
                                 uint32_t bank,
                                 struct terakan_bo const *bo,
                                 uint32_t va_kcache_lines,
                                 uint32_t size_lines)
{
   if (bo == NULL || size_lines == 0)
      return;

   uint32_t bo_ref = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer,
      bo, true, false, TERAKAN_BO_PRIORITY_UNIFORM_BUFFER);

   /* ALU_CONST_BUFFER_SIZE_LS_<bank> = size in 256-byte KCACHE lines */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028FC0_ALU_CONST_BUFFER_SIZE_LS_0 + bank * 4 - 0x28000) >> 2;
      *p++ = size_lines;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* ALU_CONST_CACHE_LS_<bank> = base address (va >> 8) + NOP relocation */
   {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028F40_ALU_CONST_CACHE_LS_0 + bank * 4 - 0x28000) >> 2;
      *p++ = va_kcache_lines;
      *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }
}

static void
terakan_emit_compute_resources(struct terakan_gfx_command_writer *command_writer,
                               struct terakan_pipeline_compute const *pipeline)
{
   struct terakan_hw_state_sqc *state = &command_writer->hw_state_sqc;

   /* Enumerate bound compute resources in the FS bank.  Separate into:
    *   res_idxs[] — all resources (for SET_RESOURCE / VFETCH reading)
    *   uav_idxs[] — writable SSBOs only (for CB_COLOR / MEM_RAT writing)
    * UBOs must NOT get CB_COLOR entries: with both UBO and SSBO bound,
    * a UBO at res_idxs[0] would claim RAT0, causing the shader's MEM_RAT
    * writes (which target RAT0) to land on the read-only UBO instead of
    * the writable SSBO. */
   struct terakan_device const * const device =
      terakan_gfx_command_writer_device(command_writer);
   struct terakan_state_draw_cb_color_uav const * const cb_uavs =
      command_writer->state_draw.cb_color_uav.fs_uavs;

   BITSET_WORD const *uavs_used = pipeline->shader.uavs_for_mutable_resources_needed;
   uint32_t res_idxs[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
   uint32_t uav_idxs[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
   /* Parallel to uav_idxs[]: for each UAV m, the index into
    * `cb_uavs[]` (matches `uav_mutable_resource_index` used by the
    * graphics UAV emission path).  We need this because
    * `uav_idxs[m]` is a global resource index into
    * `state->resource_bos.fs[]`, which does not index cb_uavs. */
   uint32_t uav_mr_idxs[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
   /* Parallel to uav_idxs[]: true for storage-image UAVs (non-BUFFER
    * RESOURCE_TYPE in color.info), false for buffer UAVs
    * (SSBO / storage-texel-buffer).  Image UAVs require the Phase-3
    * dual SET_RESOURCE emission at CS+160+m (IMMED) and CS+168+m
    * (REAL); see LI-2026-04-17-04. */
   bool is_image_uav[TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE];
   uint32_t res_count = 0;
   uint32_t uav_count = 0;
   uint32_t image_uav_count = 0;
   for (uint32_t i = 0; i < TERAKAN_RESOURCE_HW_COUNT_PIXEL_COMPUTE; i++) {
      if (BITSET_TEST(state->resources_not_null.fs, i) && state->resource_bos.fs[i]) {
         res_idxs[res_count++] = i;
         if (i >= TERAKAN_SAMPLER_HW_COUNT_PER_STAGE + TERAKAN_RESOURCE_RANGE_MUTABLE_BASE) {
            uint32_t uav_idx = i - TERAKAN_SAMPLER_HW_COUNT_PER_STAGE -
                               TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
            /* Keep the CB_COLOR RAT ordering identical to lowering-time
             * terakan_nir_get_binding_uav() numbering. */
            if (uav_idx < TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL &&
                BITSET_TEST(uavs_used, uav_idx)) {
               bool const is_image =
                  G_028C70_RESOURCE_TYPE(cb_uavs[uav_idx].color.info) != V_028C70_BUFFER;
               uav_mr_idxs[uav_count] = uav_idx;
               is_image_uav[uav_count] = is_image;
               if (is_image) {
                  ++image_uav_count;
               }
               uav_idxs[uav_count++] = i;
            }
         }
      }
   }
   if (image_uav_count > TERAKAN_MAX_COMPUTE_STORAGE_IMAGES) {
      /* LI-2026-04-17-04 trip-wire: the IMMED/REAL slot windows only
       * hold TERAKAN_MAX_COMPUTE_STORAGE_IMAGES images side-by-side
       * before image m's REAL slot would alias into image (m - delta)'s
       * IMMED slot and hang the CB exporter.  Refuse the dispatch
       * instead of emitting a known-corrupt IB. */
      fprintf(stderr,
              "terakan: compute dispatch binds %u storage images, max %u; aborting.\n",
              image_uav_count, TERAKAN_MAX_COMPUTE_STORAGE_IMAGES);
      return;
   }
   if (image_uav_count > 0 &&
       debug_get_bool_option("TERAKAN_W1_MAX_BARRIER_FIRST_IMAGE_STORE", false) &&
       p_atomic_cmpxchg(&terakan_debug_w1_first_image_store_emitted, 0, 1) == 0) {
      uint32_t const cp_coher_cntl = terakan_w1_max_barrier_cp_coher_cntl();
      terakan_compute_multiplex_emit_surface_sync(command_writer, cp_coher_cntl);
      fprintf(stderr,
              "terakan/barrier: W1 first-image-store max barrier image_uav_count=%u cp_coher_cntl=0x%08x\n",
              image_uav_count, cp_coher_cntl);
   }
   if (res_count == 0) {
      return;
   }

   /* Some compute paths lower SSBO stores to store_global before mutable-UAV
    * usage tracking is populated, leaving uavs_used empty at dispatch time.
    * SFN currently routes compute stores to RAT0, so fallback to the highest
    * bound mutable descriptor (common CTS convention: output buffer last). */
   if (uav_count == 0) {
      uint32_t fallback_uav = UINT32_MAX;
      for (uint32_t n = 0; n < res_count; ++n) {
         uint32_t const i = res_idxs[n];
         if (i >= TERAKAN_SAMPLER_HW_COUNT_PER_STAGE + TERAKAN_RESOURCE_RANGE_MUTABLE_BASE) {
            uint32_t uav_idx = i - TERAKAN_SAMPLER_HW_COUNT_PER_STAGE -
                               TERAKAN_RESOURCE_RANGE_MUTABLE_BASE;
            if (uav_idx < TERAKAN_RESOURCE_RANGE_MUTABLE_MAX_COUNT_NON_PIXEL) {
               fallback_uav = i;
            }
         }
      }
      if (fallback_uav != UINT32_MAX) {
         uav_idxs[0] = fallback_uav;
         uav_count = 1;
      }
   }

   if (debug_get_bool_option("TERAKAN_DEBUG_COMPUTE_DESC", false)) {
      fprintf(stderr, "terakan/compute_resources: uav_count=%u\n", uav_count);
      for (uint32_t n = 0; n < res_count; ++n) {
         uint32_t const idx = res_idxs[n];
         uint32_t const *desc = state->resource_descriptors.fs[idx];
         bool is_uav = false;
         for (uint32_t m = 0; m < uav_count; ++m) {
            if (uav_idxs[m] == idx) {
               is_uav = true;
               break;
            }
         }
         fprintf(stderr,
                 "terakan/compute_resources: [%u/%u] res_idx=%u %s desc=[0x%08x 0x%08x 0x%08x 0x%08x]\n",
                 n, res_count, idx, is_uav ? "UAV" : "RO", desc[0], desc[1], desc[2], desc[3]);
      }
   }

   /* 1. SET_RESOURCE (VTX_FETCH read) for every bound SSBO at
    *    EG_FETCH_CONSTANTS_OFFSET_CS + ssbo_idx.  The shader's VFETCH BUFFER_ID
    *    matches ssbo_idx (terakan_nir_lower_bindings_instr_load_ssbo computes
    *    id_base = set->first_shader_resources + TERAKAN_SAMPLER_HW_COUNT_PER_STAGE,
    *    and that is the same index Terakan uses for resource_descriptors.fs[]).
    *    On compute-on-LS the HW adds EG_FETCH_CONSTANTS_OFFSET_CS (816) to the
    *    shader-emitted BUFFER_ID to form the physical SQ fetch constant slot. */
   for (uint32_t n = 0; n < res_count; ++n) {
      uint32_t const idx = res_idxs[n];
      /* Image UAVs do not participate in the VFETCH path; their
       * reads go through the SQ_TEX_RESOURCE descriptor emitted at
       * CS+168+m by the image-UAV branch below.  Skip the buffer
       * VFETCH here to avoid publishing a stale buffer-format
       * descriptor at CS+idx that the CB exporter could latch. */
      {
         bool skip_image = false;
         for (uint32_t m = 0; m < uav_count; ++m) {
            if (uav_idxs[m] == idx && is_image_uav[m]) {
               skip_image = true;
               break;
            }
         }
         if (skip_image) {
            continue;
         }
      }
      struct terakan_bo const * const bo = state->resource_bos.fs[idx];
      uint32_t const * const desc = state->resource_descriptors.fs[idx];
      uint32_t const buf_size = desc[1] + 1;

      uint32_t const bo_ref = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer,
         bo, true, true, TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);

      uint32_t const res_slot = EG_FETCH_CONSTANTS_OFFSET_CS + idx;
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 12);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_RESOURCE, 8, 0) | COMPUTE_MODE_BIT;
      *p++ = res_slot * 8;
      *p++ = 0;                            /* WORD0: base address (relocated) */
      *p++ = buf_size - 1;                 /* WORD1: size in bytes - 1 */
      /* Compute-mode VFETCH uses STRIDE=1 so the shader's byte-offset
       * arithmetic (produced by SFN's load_ssbo lowering) indexes the buffer
       * directly.  r600g does the same in evergreen_emit_vertex_buffers
       * (pkt_flags == RADEON_CP_PACKET3_COMPUTE_MODE ? 1 : graphics_stride).
       * With STRIDE=4 the hardware would multiply the byte offset by 4
       * again, reading from i*16 instead of i*4 and returning zeros beyond
       * the buffer bounds. */
      *p++ = S_030008_STRIDE(1) |
             S_030008_DATA_FORMAT(0x0D) |  /* FMT_32 */
             S_030008_NUM_FORMAT_ALL(1);   /* NUM_FORMAT_INT (raw bits) */
      *p++ = S_03000C_DST_SEL_X(V_03000C_SQ_SEL_X) |
             S_03000C_DST_SEL_Y(V_03000C_SQ_SEL_Y) |
             S_03000C_DST_SEL_Z(V_03000C_SQ_SEL_Z) |
             S_03000C_DST_SEL_W(V_03000C_SQ_SEL_W) |
             S_03000C_UNCACHED(1);         /* WORD3: identity swizzle, uncached */
      *p++ = buf_size;                     /* WORD4: num_elements (for resinfo) */
      *p++ = 0;                            /* WORD5 */
      *p++ = 0;                            /* WORD6 */
      *p++ = S_03001C_TYPE(V_03001C_SQ_TEX_VTX_VALID_BUFFER); /* WORD7 */
      *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 2. RAT (write) path.  TeraScale-2 hijacks the graphics color-buffer
    *    exporter for compute writes: each RAT slot M is backed by
    *    CB_COLOR{M}_BASE + companion registers.  Emit per-binding RAT state
    *    so the shader can write to any bound SSBO.
    *
    *    CURRENT SFN LIMITATION: Terakan's NIR lowering
    *    (terakan_nir_lower_bindings_instr_store_ssbo in
    *    terakan_nir_lower_abi.c) converts compute store_ssbo to
    *    store_global.  SFN's RatInstr::emit_global_store at
    *    sfn_instr_mem.cpp:665 uses shader.ssbo_image_offset() -- a
    *    per-shader scalar, not a per-store RAT id -- so every compute
    *    store actually lands at the same RAT slot (typically 0 for
    *    programs with no images).  Until SFN is taught to carry a
    *    per-store RAT id through the store_global path (or Terakan's
    *    lowering is switched back to nir_store_ssbo so SFN's
    *    emit_store_ssbo picks up nir_intrinsic_id_base), only the SSBO
    *    routed to CB_COLOR0 actually receives writes.  We still publish
    *    CB_COLOR{M} for M = 0..N-1 so the multi-RAT configuration is
    *    architecturally ready, CB_TARGET_MASK enables them, and the
    *    follow-up graphics bind (terakan_before_draw re-binds the
    *    graphics pipeline when sq_config_is_compute_mode is set) resets
    *    the color-buffer exporter for the next render pass.
    *
    *    The CB_COLOR register window strides by 0x3C bytes (15 dwords)
    *    per slot -- CB_COLOR{M}_BASE is at R_028C60_CB_COLOR0_BASE +
    *    M * 0x3C.  Evergreen has 12 CB slots (TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT).
    *    Cap N so we never step past that.
    *
    *    RAT0 is bound to the LAST enumerated SSBO.  SFN currently routes
    *    all compute stores to RAT0, and the common GLSL convention is to
    *    place the writable output buffer at the highest binding index, so
    *    this heuristic lines up with the shader's intent for the single-
    *    writable case until the SFN change lands. */
   uint32_t rat_count = uav_count;
   if (rat_count > TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT) {
      rat_count = TERAKAN_COLOR_HW_RTV_AND_UAV_COUNT;
   }

   /* CB_TARGET_MASK: 4 channel-enable bits per RAT slot, one group per bound
    * SSBO that we want to be writable.  (Graphics pipeline bind on the next
    * draw will reset this via terakan_before_draw when
    * sq_config_is_compute_mode is set.) */
   {
      uint32_t target_mask = 0;
      for (uint32_t m = 0; m < rat_count; ++m) {
         target_mask |= 0xFu << (4u * m);
      }
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = (R_028238_CB_TARGET_MASK - 0x28000) >> 2;
      *p++ = target_mask;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* Per-SSBO CB_COLOR{M} state.  Bind in enumeration order so CB_COLOR{M}
    * lines up with the M-th bound SSBO (lowest FS-bank index first).  RAT0
    * (which SFN currently targets for every compute store because
    * shader.ssbo_image_offset() is a per-shader scalar) thus lands on the
    * first bound SSBO, which matches the CTS convention of placing the
    * writable/result buffer at the lowest binding index (see
    * vktBindingShaderAccessTests.cpp:2510 where the result STORAGE_BUFFER
    * is added at setNdx==0 binding==0 before the test descriptors). */
   /* Track the image-index (0..image_uav_count-1) separately from the
    * enumeration index m.  Images share the RAT slot 0..rat_count-1
    * space with buffer UAVs but consume distinct CS+160+image_m /
    * CS+168+image_m slot windows per LI-2026-04-17-04. */
   uint32_t image_m = 0;
   for (uint32_t m = 0; m < rat_count; ++m) {
      uint32_t const sidx = uav_idxs[m];
      struct terakan_bo const * const bo = state->resource_bos.fs[sidx];

      if (is_image_uav[m]) {
         /* ========== Storage-image UAV (Phase-3 path) ==========
          * Emit in r600g's canonical order (see
          * steinmarder/data/capture_image_ib_reference/):
          *   (a) CB_COLOR{M}_BASE..CLEAR_WORD1 + reloc to image BO
          *   (b) CB_IMMED{M}_BASE + reloc to uav_immediate_bo
          *   (c) SET_RESOURCE at CS+(IMMED_OFFSET+image_m) (IMMED desc)
          *   (d) SET_RESOURCE at CS+(REAL_OFFSET +image_m) (REAL desc)
          * The REAL descriptor is what makes MEM_RAT STORE_TYPED
          * format validation succeed - without it the CB exporter
          * stalls on format lookup (CLAIMS C-2026-04-17-08). */
         struct terakan_state_draw_cb_color_uav const * const cb_uav =
            &cb_uavs[uav_mr_idxs[m]];
         struct terakan_color_descriptor const * const color = &cb_uav->color;
         unsigned const bytes_per_texel =
            terascale_format_bytes_per_block[G_028C70_FORMAT(color->info)];
         unsigned const bytes_per_texel_log2 = util_logbase2(bytes_per_texel);

         if (debug_get_bool_option("TERAKAN_DEBUG_COMPUTE_DESC", false)) {
            uint32_t const slice_start = (color->view >> 0) & 0x7FF;
            uint32_t const slice_max = (color->view >> 13) & 0x7FF;
            fprintf(stderr,
                    "terakan/compute_image: m=%u image_m=%u base=0x%08x pitch=0x%08x "
                    "slice=0x%08x view=0x%08x [slice_start=%u slice_max=%u] "
                    "info=0x%08x attrib=0x%08x dim=0x%08x "
                    "real=[0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x]\n",
                    m, image_m, color->base, color->pitch, color->slice, color->view,
                    slice_start, slice_max,
                    color->info, color->attrib, color->dim,
                    cb_uav->real_resource[0], cb_uav->real_resource[1],
                    cb_uav->real_resource[2], cb_uav->real_resource[3],
                    cb_uav->real_resource[4], cb_uav->real_resource[5],
                    cb_uav->real_resource[6], cb_uav->real_resource[7]);
         }

         /* (a) CB_COLOR{M}_BASE..CLEAR_WORD1 with image-native fields. */
         uint32_t const image_bo_ref = terakan_bo_reference_writer_add_reference(
            &command_writer->base.bo_reference_writer,
            bo, true, true, TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);
         uint32_t *p = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 23);
         if (unlikely(p == NULL)) return;
         *p++ = PKT3(PKT3_SET_CONTEXT_REG, 13, 0) | COMPUTE_MODE_BIT;
         *p++ = ((R_028C60_CB_COLOR0_BASE + m * 0x3Cu) - 0x28000u) >> 2;
         *p++ = color->base;                      /* CB_COLOR{M}_BASE (reloc adjusts) */
         *p++ = color->pitch;                     /* CB_COLOR{M}_PITCH */
         *p++ = color->slice;                     /* CB_COLOR{M}_SLICE */
         *p++ = color->view;                      /* CB_COLOR{M}_VIEW */
         *p++ = color->info | S_028C70_RAT(1);    /* CB_COLOR{M}_INFO + RAT */
         *p++ = color->attrib;                    /* CB_COLOR{M}_ATTRIB */
         *p++ = color->dim;                       /* CB_COLOR{M}_DIM */
         *p++ = 0;                                /* CB_COLOR{M}_CMASK */
         *p++ = 0;                                /* CB_COLOR{M}_CMASK_SLICE */
         *p++ = 0;                                /* CB_COLOR{M}_FMASK */
         *p++ = 0;                                /* CB_COLOR{M}_FMASK_SLICE */
         *p++ = 0;                                /* CB_COLOR{M}_CLEAR_WORD0 */
         *p++ = 0;                                /* CB_COLOR{M}_CLEAR_WORD1 */
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT; *p++ = 4 * image_bo_ref; /* BASE */
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT; *p++ = 4 * image_bo_ref; /* ATTRIB */
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT; *p++ = 4 * image_bo_ref; /* CMASK */
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT; *p++ = 4 * image_bo_ref; /* FMASK */
         terakan_gfx_command_writer_emit_done(command_writer, p);

         /* (b) CB_IMMED{M}_BASE + reloc to uav_immediate_bo.  The
          * register stride is 1 dword between CB_IMMED{M}_BASE
          * (see graphics terakan_hw_state_draw_emit_cb_immed at
          * terakan_hw_state.c:1171). */
         uint32_t const immed_bo_ref = terakan_bo_reference_writer_add_reference(
            &command_writer->base.bo_reference_writer,
            device->uav_immediate_bo, true, true,
            TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);
         p = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
         if (unlikely(p == NULL)) return;
         *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
         *p++ = ((R_028B9C_CB_IMMED0_BASE - 0x28000u) >> 2) + m;
         *p++ = device->uav_immediate_va_shr8[bytes_per_texel_log2];
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
         *p++ = 4 * immed_bo_ref;
         terakan_gfx_command_writer_emit_done(command_writer, p);

         /* (c) SET_RESOURCE at CS+(IMMED_OFFSET+image_m):
          *     buffer-format IMMED descriptor into uav_immediate_bo. */
         uint32_t immed_resource[8];
         terakan_color_descriptor_info_to_uav_immediate_resource(
            device, color->info, immed_resource);
         uint32_t const immed_slot =
            EG_FETCH_CONSTANTS_OFFSET_CS + R600_IMAGE_IMMED_RESOURCE_OFFSET + image_m;
         p = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 12);
         if (unlikely(p == NULL)) return;
         *p++ = PKT3(PKT3_SET_RESOURCE, 8, 0) | COMPUTE_MODE_BIT;
         *p++ = immed_slot * 8;
         for (unsigned w = 0; w < 8; ++w) {
            *p++ = immed_resource[w];
         }
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
         *p++ = 4 * immed_bo_ref;
         terakan_gfx_command_writer_emit_done(command_writer, p);

         /* (d) SET_RESOURCE at CS+(REAL_OFFSET+image_m):
          *     SQ_TEX_RESOURCE describing the image itself.  Copied
          *     from image_view->resource[] at bind time into
          *     cb_uav->real_resource via terakan_descriptor_set.c.
          *
          *     The kernel CS parser requires TWO NOP relocation entries
          *     for a tex SET_RESOURCE (base VA + mip-chain VA), even
          *     when the image has a single mip and both relocations
          *     point at the same BO.  Emitting only one relocation
          *     produces "bad SET_RESOURCE (tex)" / "No packet3 for
          *     relocation" from the kernel parser and aborts submission
          *     before the GPU is touched.  r600g reference IB at
          *     `data/capture_image_ib_reference/ib_ref_r600g_image_store_1d_r32ui.txt`
          *     GIB[411..424] shows the two-reloc pattern. */
         uint32_t const real_slot =
            EG_FETCH_CONSTANTS_OFFSET_CS + R600_IMAGE_REAL_RESOURCE_OFFSET + image_m;
         p = terakan_gfx_command_writer_emit(
            command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 14);
         if (unlikely(p == NULL)) return;
         *p++ = PKT3(PKT3_SET_RESOURCE, 8, 0) | COMPUTE_MODE_BIT;
         *p++ = real_slot * 8;
         for (unsigned w = 0; w < 8; ++w) {
            *p++ = cb_uav->real_resource[w];
         }
         /* Relocation 1: BASE VA. */
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
         *p++ = 4 * image_bo_ref;
         /* Relocation 2: MIP-chain VA (same BO for single-mip images;
          * still required by the kernel validator). */
         *p++ = PKT3(PKT3_NOP, 0, 0) | COMPUTE_MODE_BIT;
         *p++ = 4 * image_bo_ref;
         terakan_gfx_command_writer_emit_done(command_writer, p);

         ++image_m;
         continue;
      }

      /* ========== Buffer UAV (SSBO / storage-texel-buffer) ==========
       * Unchanged path: program CB_COLOR{M} with a synthetic
       * buffer-format descriptor covering the whole BO. */
      uint32_t const * const desc = state->resource_descriptors.fs[sidx];
      uint32_t const buf_size = desc[1] + 1;
      uint32_t const width_elements = buf_size / 4;
      uint32_t const pitch_aligned = (width_elements + 63) & ~63u;
      uint32_t const pitch_tile_max = (pitch_aligned / 8) - 1;
      uint32_t const dim = width_elements > 0 ? width_elements - 1 : 0;
      uint32_t const cb_color_info =
         S_028C70_FORMAT(V_028C70_COLOR_32) |
         S_028C70_ARRAY_MODE(V_028C70_ARRAY_LINEAR_ALIGNED) |
         S_028C70_NUMBER_TYPE(V_028C70_NUMBER_UINT) |
         S_028C70_COMP_SWAP(0) |
         S_028C70_BLEND_BYPASS(1) |
         S_028C70_RESOURCE_TYPE(V_028C70_BUFFER) |
         S_028C70_RAT(1);

      uint32_t const bo_ref = terakan_bo_reference_writer_add_reference(
         &command_writer->base.bo_reference_writer,
         bo, true, true, TERAKAN_BO_PRIORITY_SHADER_RW_BUFFER);

      /* CB_COLOR{M}_BASE .. CB_COLOR{M}_CLEAR_WORD1 (13 regs) + 4 NOP relocs. */
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 23);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 13, 0) | COMPUTE_MODE_BIT;
      *p++ = ((R_028C60_CB_COLOR0_BASE + m * 0x3Cu) - 0x28000u) >> 2;
      *p++ = 0;                                 /* CB_COLOR{M}_BASE (relocated) */
      *p++ = pitch_tile_max;                    /* CB_COLOR{M}_PITCH */
      *p++ = 0;                                 /* CB_COLOR{M}_SLICE */
      *p++ = 0;                                 /* CB_COLOR{M}_VIEW */
      *p++ = cb_color_info;                     /* CB_COLOR{M}_INFO */
      *p++ = S_028C74_NON_DISP_TILING_ORDER(1); /* CB_COLOR{M}_ATTRIB */
      *p++ = S_028C78_WIDTH_MAX(dim);           /* CB_COLOR{M}_DIM */
      *p++ = 0;                                 /* CB_COLOR{M}_CMASK */
      *p++ = 0;                                 /* CB_COLOR{M}_CMASK_SLICE */
      *p++ = 0;                                 /* CB_COLOR{M}_FMASK */
      *p++ = 0;                                 /* CB_COLOR{M}_FMASK_SLICE */
      *p++ = 0;                                 /* CB_COLOR{M}_CLEAR_WORD0 */
      *p++ = 0;                                 /* CB_COLOR{M}_CLEAR_WORD1 */
      *p++ = PKT3(PKT3_NOP, 0, 0); *p++ = 4 * bo_ref;   /* BASE */
      *p++ = PKT3(PKT3_NOP, 0, 0); *p++ = 4 * bo_ref;   /* ATTRIB */
      *p++ = PKT3(PKT3_NOP, 0, 0); *p++ = 4 * bo_ref;   /* CMASK */
      *p++ = PKT3(PKT3_NOP, 0, 0); *p++ = 4 * bo_ref;   /* FMASK */
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

   /* Register the shader BO for the CS submission relocation table */
   uint32_t bo_ref = terakan_bo_reference_writer_add_reference(
      &command_writer->base.bo_reference_writer,
      pipeline->shader.static_state.program_bo,
      true, false, TERAKAN_BO_PRIORITY_SHADER_BINARY);
   if (debug_get_bool_option("TERAKAN_DEBUG_COMPUTE_DESC", false)) {
      fprintf(stderr,
              "terakan/compute_state: program_bo=%p bo_ref=%u\n",
              (void *)pipeline->shader.static_state.program_bo, bo_ref);
   }
   bool const skip_cs_state_program =
      debug_get_bool_option("TERAKAN_SKIP_CS_STATE_PROGRAM", false);
   bool const skip_cs_state_num_thread =
      debug_get_bool_option("TERAKAN_SKIP_CS_STATE_NUM_THREAD", false);
   bool const skip_cs_state_input_cntl =
      debug_get_bool_option("TERAKAN_SKIP_CS_STATE_INPUT_CNTL", false);
   bool const skip_cs_state_group_size =
      debug_get_bool_option("TERAKAN_SKIP_CS_STATE_GROUP_SIZE", false);
   bool const skip_cs_state_lds_alloc =
      debug_get_bool_option("TERAKAN_SKIP_CS_STATE_LDS_ALLOC", false);
   bool const skip_cs_state_start_xyz =
      debug_get_bool_option("TERAKAN_SKIP_CS_STATE_START_XYZ", false);
   bool const cs_pgm_nop_compute_mode =
      debug_get_bool_option("TERAKAN_CS_PGM_NOP_COMPUTE_MODE", true);

   /* -1. Minimal DB state: set depth/stencil surface array mode to LINEAR_GENERAL
    * to prevent the kernel CS validator from seeing uninitialized garbage.
    * DB_Z_INFO = 0x28040, DB_STENCIL_INFO = 0x28044.
    * ARRAY_MODE = 0 (LINEAR_GENERAL) satisfies the validator.
    *
    * COMPUTE_MODE_BIT is required: DB_Z_INFO is a CONTEXT register
    * (0x28040 >= 0x28000) so without COMPUTE_MODE_BIT it writes to
    * the 3D shadow, leaving the compute shadow with uninitialized DB
    * state that can trigger kernel CS validator rejection or GPU hang. */
   if (!debug_get_bool_option("TERAKAN_SKIP_COMPUTE_DB_STATE", false)) {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 4);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 2, 0) | COMPUTE_MODE_BIT;
      *p++ = (0x28040 - 0x28000) >> 2; /* DB_Z_INFO offset */
      *p++ = 0;  /* ARRAY_MODE = LINEAR_GENERAL */
      *p++ = 0;  /* DB_STENCIL_INFO: ARRAY_MODE = LINEAR_GENERAL */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 0. CS_PARTIAL_FLUSH: reset the pipeline before compute dispatch.
    * This ensures prior graphics state (stencil, depth, CB) does not
    * contaminate the kernel CS validator surface tracking. */
   if (!debug_get_bool_option("TERAKAN_SKIP_PRE_DISPATCH_CS_FLUSH", false)) {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 2);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_EVENT_WRITE, 0, 0);
      *p++ = EVENT_TYPE_CS_PARTIAL_FLUSH | (4 << 8);
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 1. SQ_PGM_START_LS + RESOURCES_LS + RESOURCES_LS_2 + NOP reloc.
    *
    * CRITICAL: The SET_CONTEXT_REG and NOP reloc MUST be in the SAME
    * emission.  The command writer may switch indirect buffers between
    * emissions, which would separate the NOP from its SET_CONTEXT_REG.
    * If the kernel CS validator can't find the NOP, SQ_PGM_START_LS
    * stays at 0x0 and the SQ fetches garbage instructions → GPU hang. */
   if (!skip_cs_state_program) {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 7);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 3, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_0288D0_SQ_PGM_START_LS);
      *p++ = pipeline->sq_pgm_start_cs;       /* SQ_PGM_START_LS (va >> 8) */
      *p++ = pipeline->sq_pgm_resources_cs[0] |
             S_0288D4_DX10_CLAMP(1);           /* SQ_PGM_RESOURCES_LS */
      *p++ = 0;                                /* SQ_PGM_RESOURCES_LS_2 */
      /* NOP relocation must be plain PKT3_NOP (no COMPUTE_MODE bit) so
       * the DRM parser recognizes and patches it as a relocation marker.
       */
      *p++ = PKT3(PKT3_NOP, 0, 0) |
             (cs_pgm_nop_compute_mode ? COMPUTE_MODE_BIT : 0);
      *p++ = 4 * bo_ref;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 2. SPI_COMPUTE_NUM_THREAD_X/Y/Z (COMPUTE_MODE) */
   if (!skip_cs_state_num_thread) {
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

   /* 2b. SPI_COMPUTE_INPUT_CNTL: deliver thread/workgroup IDs to shader.
    *
    * This register tells the SPI hardware to populate the compute shader's
    * pinned VGPRs with thread IDs (TID_IN_GROUP_ENA) and workgroup IDs
    * (TGID_ENA).  Without it, the shader's pinned registers (sel 0 = TID,
    * sel 1 = TGID) contain stale/garbage data, causing every thread to
    * compute wrong global indices → MEM_RAT writes to wrong addresses
    * → output buffer appears untouched → 0.00 GFLOPS.
    *
    * DISABLE_INDEX_PACK prevents SPI from compacting sparse thread IDs
    * into a dense range, which would break gl_LocalInvocationID semantics.
    *
    * Gallium reference: evergreen_init_atom_start_compute_cs(), line ~720.
    * Value = 0x7 (all three bits set). */
   if (!skip_cs_state_input_cntl) {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_0286E8_SPI_COMPUTE_INPUT_CNTL);
      *p++ = S_0286E8_TID_IN_GROUP_ENA(1) |
             S_0286E8_TGID_ENA(1) |
             S_0286E8_DISABLE_INDEX_PACK(1);  /* = 0x7 */
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 3. VGT_NUM_INDICES = group_size */
   if (!skip_cs_state_group_size) {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = CONFIG_REG_OFFSET(R_008970_VGT_NUM_INDICES);
      *p++ = pipeline->group_size;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 4. VGT_COMPUTE_THREAD_GROUP_SIZE */
   if (!skip_cs_state_group_size) {
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONFIG_REG, 1, 0);
      *p++ = CONFIG_REG_OFFSET(R_0089AC_VGT_COMPUTE_THREAD_GROUP_SZ);
      *p++ = pipeline->group_size;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 5. SQ_LDS_ALLOC (COMPUTE_MODE)
    *
    * Gallium: num_waves = ceil(group_size / (16 * num_pipes)).
    * Recalculate here because the pipeline was compiled without chip info. */
   if (!skip_cs_state_lds_alloc) {
      struct terakan_device const * const dev2 =
         terakan_gfx_command_writer_device(command_writer);
      struct terakan_physical_device const * const pd2 =
         terakan_device_physical_device(dev2);
      unsigned num_pipes = 1u << pd2->chip_info.max_render_backends_log2;
      unsigned wave_div = 16 * num_pipes;
      unsigned num_waves = (pipeline->group_size + wave_div - 1) / wave_div;
      uint32_t lds_dwords = pipeline->sq_lds_alloc & 0x3FFF;
      uint32_t sq_lds = lds_dwords | (num_waves << 14);
      uint32_t *p = terakan_gfx_command_writer_emit(
         command_writer, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 3);
      if (unlikely(p == NULL)) return;
      *p++ = PKT3(PKT3_SET_CONTEXT_REG, 1, 0) | COMPUTE_MODE_BIT;
      *p++ = CONTEXT_REG_OFFSET(R_0288E8_SQ_LDS_ALLOC);
      *p++ = sq_lds;
      terakan_gfx_command_writer_emit_done(command_writer, p);
   }

   /* 6. VGT_COMPUTE_START_X/Y/Z = 0 */
   if (!skip_cs_state_start_xyz) {
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

   bool const skip_compute_multiplex =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_MULTIPLEX", false);
   bool const skip_compute_multiplex_begin =
      skip_compute_multiplex ||
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_MULTIPLEX_BEGIN", false);
   bool const skip_compute_multiplex_end =
      skip_compute_multiplex ||
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_MULTIPLEX_END", false);
   bool const skip_compute_constants =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_CONSTANTS", false);
   bool const skip_compute_resources =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_RESOURCES", false);
   bool const skip_compute_state_emit =
      debug_get_bool_option("TERAKAN_SKIP_COMPUTE_STATE_EMIT", false);
   bool const skip_dispatch_packet =
      debug_get_bool_option("TERAKAN_SKIP_DISPATCH_PACKET", false);
   bool const force_first_submit_sync =
      debug_get_bool_option("TERAKAN_FORCE_FIRST_SUBMIT_SYNC", false) ||
      debug_get_bool_option("TERAKAN_DEBUG_FORCE_FIRST_SUBMIT_SYNC", false) ||
      debug_get_bool_option("TERAKAN_FIRST_SUBMIT_SYNC", false);
   bool const debug_first_submit_sync =
      debug_get_bool_option("TERAKAN_DEBUG_FIRST_SUBMIT_SYNC", false);

   /* LI-2026-04-17-06 PROMOTED 2026-04-17 evening: the Phase-3 storage
    * image emission path in terakan_emit_compute_resources is proven
    * stable on Sumo silicon.  A 2615-test image.* CTS sweep (mesa
    * commit c272f57 with the dual-reloc fix) ran without any ring
    * hang, GPU lockup, or kernel CS parser rejection from the image
    * path.  1d and 2d with_format: 78/78 green each, 0 fails.
    * without_format: 315 green, 258 fails (compare-fails, not hangs).
    * Remaining fail clusters (array single_layer, 3D, cube multi-face)
    * all reach compute dispatch, run the shader, and compare-fail at
    * readback — they do NOT hang the ring.
    *
    * Guard removed.  `TERAKAN_FORCE_COMPUTE_IMAGE_DISPATCH=1` retained
    * (does nothing now that the default path is the Phase-3 path) for
    * diagnostic continuity with earlier recipes. */

   /* ========== Context Multiplexer: BEGIN ==========
    * Drain graphics waves, invalidate read caches, and reconfigure SQ
    * for the LS (compute) stage.  MUST happen before any compute state
    * emission because SQ_GPR_RESOURCE_MGMT_3 and SQ_THREAD_RESOURCE_MGMT_2
    * affect how the subsequent SQ_PGM_RESOURCES_LS is interpreted. */
   if (!skip_compute_multiplex_begin) {
      terakan_compute_multiplex_begin(command_writer);

      if (force_first_submit_sync && !cmd->has_compute_work) {
         uint32_t const cp_coher_cntl = terakan_w6_pre_dispatch_invalidate_cp_coher_cntl();
         terakan_compute_multiplex_emit_event(command_writer, EVENT_TYPE_CS_PARTIAL_FLUSH);
         terakan_compute_multiplex_emit_surface_sync(command_writer, cp_coher_cntl);
         if (debug_first_submit_sync) {
            fprintf(stderr,
                    "terakan/dispatch: first_submit_sync forced cp_coher_cntl=0x%08x\n",
                    cp_coher_cntl);
         }
      } else if (debug_first_submit_sync && force_first_submit_sync && cmd->has_compute_work) {
         fprintf(stderr,
                 "terakan/dispatch: first_submit_sync skipped (non-first compute in command buffer)\n");
      }
   }

   /* Emit compute pipeline state if dirty (first bind or pipeline change).
    * After the multiplexer begin boundary the SQ is ready to accept LS
    * program writes.  We unconditionally re-emit state on the first
    * dispatch after every bind because the multiplexer's SQ reconfig
    * invalidates any cached "compute state is still applied" assumption. */
   if (command_writer->compute_pipeline_dirty) {
      if (!skip_compute_state_emit) {
         terakan_emit_compute_state(command_writer, pipeline);
      }
      if (!skip_compute_resources) {
         terakan_emit_compute_resources(command_writer, pipeline);
         /* Wire KCACHE (KC0) with SSBO address for compute addressing.
          *
          * FIX-G (C-2026-04-19-03): this call stomps LS ALU_CONST_CACHE_LS_0
          * with a zero-scratch BO (the SSBO-offset=0 convention).  When FIX-G
          * wires real UBO descriptors through LS banks at CmdBindDescriptorSets
          * time, bank 0 can hold a legitimate application UBO.  Skip the
          * SSBO-offset overwrite when FIX-G is enabled; application UBOs that
          * shader loads via KC0 reach the shader correctly. */
         static int fix_g_cached = -1;
         if (fix_g_cached < 0) {
            fix_g_cached = debug_get_bool_option("TERAKAN_FIX_G_UBO_WIRING", false) ? 1 : 0;
         }
         if (!fix_g_cached && command_writer->hw_state_sqc.resource_bos.fs[2]) {
            terakan_emit_compute_kcache(command_writer,
                                        command_writer->hw_state_sqc.resource_bos.fs[2]);
         }
      }
      command_writer->compute_pipeline_dirty = false;
   }

   /* Propagate compute shader push constant usage to the command writer state
    * so that terakan_push_constants_apply knows which driver slots and app
    * bytes to upload. */
   command_writer->push_constants_state.usage_compute =
      pipeline->shader.push_constants_usage;

   if (!skip_compute_constants) {
      /* Write dispatch group counts into the driver push constant prefix.
       * The NIR lowering pass (terakan_nir_lower_bindings_instr_load_num_workgroups)
       * emits KCACHE reads from these offsets for gl_NumWorkGroups. */
      command_writer->push_constants_state.driver_constants.num_workgroups[0] = groupCountX;
      command_writer->push_constants_state.driver_constants.num_workgroups[1] = groupCountY;
      command_writer->push_constants_state.driver_constants.num_workgroups[2] = groupCountZ;
      command_writer->push_constants_state.driver_constants_modified |=
         BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_X) |
         BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_Y) |
         BITFIELD_BIT(TERAKAN_PUSH_CONSTANTS_DRIVER_INDEX_NUM_WORKGROUPS_Z);

      /* Upload push constants (driver prefix + app constants) to KCACHE bank 15.
       * This is the compute equivalent of the graphics path in terakan_draw.c.
       * push_constants_apply populates the buffer but skips the deferred FS
       * binding for compute — we emit LS KCACHE bank 15 manually below. */
      terakan_push_constants_apply(command_writer, true);

      /* Emit LS KCACHE bank 15 (push constants) with COMPUTE_MODE_BIT.
       * Only emit if push_constants_apply() actually populated the allocation.
       * After _reset(), all allocation fields are canonical zero (bo=NULL,
       * va=0, size=0).  Emitting garbage here was the root cause of GPU
       * hangs on the 3rd+ vkpeak dispatch. */
      if (command_writer->push_constants_state.allocation.bo != NULL &&
          command_writer->push_constants_state.allocation.size_kcache_lines != 0) {
         terakan_emit_compute_kcache_bank(
            command_writer,
            TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
            command_writer->push_constants_state.allocation.bo,
            command_writer->push_constants_state.allocation.va_kcache_lines,
            command_writer->push_constants_state.allocation.size_kcache_lines);
      }

      /* Bind robustness metadata (per-UAV byte sizes) to KCACHE bank 14 if
       * the compute pipeline needs it for write guards.
       * robustness_metadata_apply populates the buffer but skips the deferred
       * FS binding for compute — we emit LS KCACHE bank 14 manually.
       *
       * On repeat dispatches of the same pipeline, robustness_metadata_apply
       * returns early (bound_to_stages already set) and the LS register value
       * from the first dispatch persists in the PM4 compute shadow. */
      if (command_writer->bound_compute_pipeline != NULL &&
          (command_writer->bound_compute_pipeline->shader.kcache_needed &
           ((uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA))) {
         /* FIX-N attempt (reverted 2026-04-19): added CACHE_FLUSH_AND_
          * INV_EVENT (0x16) here on hypothesis that Evergreen caches the
          * KCACHE line per shader-program.  Empirically the flush did
          * NOT resolve the 78-test sweep failure, falsifying that
          * hypothesis.  Left as inert comment; real fix TBD after
          * register-doc + r600g kcache management review.  Keeping
          * TERAKAN_FIX_N_KCACHE_FLUSH env reserved for a correctly-
          * motivated variant in a future session. */
         terakan_robustness_metadata_apply(command_writer, true);
         terakan_emit_compute_kcache_bank(
            command_writer,
            TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
            command_writer->robustness_metadata.bo,
            command_writer->robustness_metadata.va_kcache_lines,
            1);  /* 1 KCACHE line = 256 bytes */
         if (debug_get_bool_option("TERAKAN_DEBUG_FIX_K_LS", false)) {
            fprintf(stderr, "TERAKAN_FIX_K_LS_EMIT: bank=14 bo=%p va_lines=0x%x\n",
                    (void const *)command_writer->robustness_metadata.bo,
                    command_writer->robustness_metadata.va_kcache_lines);
         }
      }
   }

   if (!skip_dispatch_packet) {
      terakan_emit_w4_warmup_dispatch_once(command_writer);

      /* Watermark checkpoint 1: PRE-dispatch baseline. */
      {
         struct terakan_device const * const dev =
            terakan_gfx_command_writer_device(command_writer);
         if (dev->debug_watermarks_enabled) {
            terakan_emit_flush_watermark(command_writer, 0, 0xAAAA1111u);
         }
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
      cmd->has_compute_work = true;
   }

   /* ========== Context Multiplexer: END ==========
    * Drain compute waves, flush RAT writes, and mark 3D state pending
    * so the next draw re-emits shader programs and CB/DB surfaces the
    * compute dispatch stomped. */
   if (!skip_compute_multiplex_end) {
      terakan_compute_multiplex_end(command_writer);
   }

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

   struct terakan_pipeline_compute const * const pipeline =
      command_writer->bound_compute_pipeline;
   if (unlikely(pipeline == NULL)) {
      assert(!"vkCmdDispatchIndirect called without a bound compute pipeline");
      return;
   }

   /* ========== Context Multiplexer: BEGIN ==========
    * Same invariant as CmdDispatch: drain graphics waves, invalidate
    * read caches, reconfigure SQ for LS.  This REPLACES the previous
    * ad-hoc SURFACE_SYNC that only invalidated TC/VC/SH — the old code
    * missed PS_PARTIAL_FLUSH and SQ reconfig, both of which are required
    * to avoid the post-draw -> dispatch hazard. */
   terakan_compute_multiplex_begin(command_writer);

   {
      bool const force_first_submit_sync =
         debug_get_bool_option("TERAKAN_FORCE_FIRST_SUBMIT_SYNC", false) ||
         debug_get_bool_option("TERAKAN_DEBUG_FORCE_FIRST_SUBMIT_SYNC", false) ||
         debug_get_bool_option("TERAKAN_FIRST_SUBMIT_SYNC", false);
      bool const debug_first_submit_sync =
         debug_get_bool_option("TERAKAN_DEBUG_FIRST_SUBMIT_SYNC", false);
      if (force_first_submit_sync && !cmd->has_compute_work) {
         uint32_t const cp_coher_cntl = terakan_w6_pre_dispatch_invalidate_cp_coher_cntl();
         terakan_compute_multiplex_emit_event(command_writer, EVENT_TYPE_CS_PARTIAL_FLUSH);
         terakan_compute_multiplex_emit_surface_sync(command_writer, cp_coher_cntl);
         if (debug_first_submit_sync) {
            fprintf(stderr,
                    "terakan/dispatch: first_submit_sync forced (indirect) cp_coher_cntl=0x%08x\n",
                    cp_coher_cntl);
         }
      } else if (debug_first_submit_sync && force_first_submit_sync && cmd->has_compute_work) {
         fprintf(stderr,
                 "terakan/dispatch: first_submit_sync skipped (indirect non-first compute in command buffer)\n");
      }
   }

   /* Emit compute pipeline state (same as direct dispatch path).
    * Indirect dispatches go through the same state machine — the only
    * difference is that the CP reads the grid dimensions from a BO
    * instead of having them inlined in the packet. */
   if (command_writer->compute_pipeline_dirty) {
      terakan_emit_compute_state(command_writer, pipeline);
      terakan_emit_compute_resources(command_writer, pipeline);
      if (command_writer->hw_state_sqc.resource_bos.fs[2])
         terakan_emit_compute_kcache(
            command_writer, command_writer->hw_state_sqc.resource_bos.fs[2]);
      command_writer->compute_pipeline_dirty = false;
   }

   /* Upload push constants and robustness metadata to KCACHE banks 15/14.
    * Indirect dispatch has no CPU-visible group counts, so we cannot
    * populate num_workgroups driver constants here — the shader must
    * fetch them from the indirect BO if it needs gl_NumWorkGroups.
    * push_constants_apply and robustness_metadata_apply populate their
    * buffers but skip the deferred FS binding for compute — we emit
    * LS KCACHE bindings manually with COMPUTE_MODE_BIT. */
   command_writer->push_constants_state.usage_compute =
      pipeline->shader.push_constants_usage;
   terakan_push_constants_apply(command_writer, true);

   /* Emit LS KCACHE bank 15 (push constants) with COMPUTE_MODE_BIT.
    * Only emit if push_constants_apply() actually populated the allocation. */
   if (command_writer->push_constants_state.allocation.bo != NULL &&
       command_writer->push_constants_state.allocation.size_kcache_lines != 0) {
      terakan_emit_compute_kcache_bank(
         command_writer,
         TERAKAN_KCACHE_BUFFER_PUSH_CONSTANTS,
         command_writer->push_constants_state.allocation.bo,
         command_writer->push_constants_state.allocation.va_kcache_lines,
         command_writer->push_constants_state.allocation.size_kcache_lines);
   }

   if (command_writer->bound_compute_pipeline != NULL &&
       (command_writer->bound_compute_pipeline->shader.kcache_needed &
        ((uint16_t)1 << TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA))) {
      terakan_robustness_metadata_apply(command_writer, true);
      terakan_emit_compute_kcache_bank(
         command_writer,
         TERAKAN_KCACHE_BUFFER_ROBUSTNESS_METADATA,
         command_writer->robustness_metadata.bo,
         command_writer->robustness_metadata.va_kcache_lines,
         1);  /* 1 KCACHE line = 256 bytes */
   }

   terakan_emit_w4_warmup_dispatch_once(command_writer);

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
   cmd->has_compute_work = true;

   /* ========== Context Multiplexer: END ==========
    * Drain compute waves, flush RAT writes, mark 3D state pending. */
   terakan_compute_multiplex_end(command_writer);
}
