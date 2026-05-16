/* SPDX-License-Identifier: MIT */
/*
 * terakan_palm_sync
 *
 * PM4 packet helpers for the dma-buf carrier acquire/release path on
 * Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5).
 *
 * Helpers generalize the in-driver compute-multiplex SURFACE_SYNC
 * and EOP-timestamp patterns to accept a [gpu_va, gpu_va+size_bytes)
 * range plus an engine selector. PKT3 opcodes and CP_COHER_CNTL bit
 * layout are defined in the AMD Evergreen-Family ISA reference and
 * the AMD 3D Engine Programming Guide for Evergreen.
 */

#include "terakan_palm_sync.h"

#include "terakan_barrier.h"
#include "terakan_command_buffer.h"

#include "amd/terascale/common/terascale_evergreend.h"

#include "util/log.h"
#include "util/macros.h"

#include <assert.h>

/* PKT3 macro per the AMD Evergreen-Family ISA reference: type 3,
 * opcode, count = body_dwords - 1, predicate. Kept local to this TU
 * to avoid coupling on the dispatch-side definition. */
#define TERAKAN_PALM_PKT3(op, count, predicate)                 \
   (((3u) << 30) |                                              \
    (((count) & 0x3FFFu) << 16) |                               \
    (((op)    & 0xFFu)   << 8)  |                               \
    ((predicate) & 0x1u))

/* PKT3 opcodes per the AMD Evergreen-Family ISA reference. */
#define TERAKAN_PALM_PKT3_SURFACE_SYNC      0x43u
#define TERAKAN_PALM_PKT3_EVENT_WRITE_EOP   0x47u
#define TERAKAN_PALM_PKT3_WAIT_REG_MEM      0x3Cu

/* EVENT_WRITE_EOP body field constants per Evergreen PM4 packet
 * spec. CACHE_FLUSH_AND_INV_TS_EVENT writes the timestamp after CB
 * and DB caches drain. */
#define TERAKAN_PALM_EOP_EVENT_TYPE_CACHE_FLUSH_AND_INV_TS 20u
#define TERAKAN_PALM_EOP_EVENT_INDEX_TS                    5u
#define TERAKAN_PALM_EOP_DATA_SEL_SEND_32BIT_LOW           1u
#define TERAKAN_PALM_EOP_INT_SEL_NONE                      0u

/* WAIT_REG_MEM body field constants. */
#define TERAKAN_PALM_WRM_FUNC_GEQ                          (5u << 0)
#define TERAKAN_PALM_WRM_MEM_SPACE_MEMORY                  (1u << 4)
#define TERAKAN_PALM_WRM_ENGINE_PFP                        (1u << 8)

void
terakan_palm_emit_surface_sync_range(
   struct terakan_gfx_command_writer * const cw,
   enum terakan_palm_sync_engine const       engine,
   uint32_t const                            coher_cntl,
   uint64_t const                            gpu_va,
   uint64_t const                            size_bytes)
{
   /* SURFACE_SYNC's COHER_BASE / COHER_SIZE are 256-byte units of
    * the [39:8] portion of the address. Round size up; require
    * caller-provided VA to be 256-byte aligned. */
   assert((gpu_va & 0xFFu) == 0u);
   assert(size_bytes != 0u);

   uint64_t const aligned_size = (size_bytes + 0xFFu) & ~(uint64_t)0xFFu;
   uint32_t const coher_size   = (uint32_t)(aligned_size >> 8);
   uint32_t const coher_base   = (uint32_t)(gpu_va       >> 8);

   /* DW2 = ENGINE | COHER_CNTL. ENGINE_ME = bit 31. ENGINE_PFP = 0.
    * The kept-local definition matches terakan_barrier.h. */
   uint32_t const engine_bit =
      (engine == TERAKAN_PALM_SYNC_ENGINE_ME)
         ? TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME
         : 0u;

   /* COHER_CNTL is 29 bits wide ([28:0]); the top 3 bits include
    * ENGINE at bit 31. */
   assert((coher_cntl & 0xE0000000u) == 0u);

   uint32_t *p = terakan_gfx_command_writer_emit(
      cw, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 5);
   if (unlikely(p == NULL))
      return;

   *p++ = TERAKAN_PALM_PKT3(TERAKAN_PALM_PKT3_SURFACE_SYNC, 3, 0);
   *p++ = coher_cntl | engine_bit;
   *p++ = coher_size;
   *p++ = coher_base;
   *p++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;

   terakan_gfx_command_writer_emit_done(cw, p);
}

void
terakan_palm_emit_eop_release_timestamp(
   struct terakan_gfx_command_writer * const cw,
   uint64_t const                            fence_gpu_va,
   uint32_t const                            seq_value)
{
   assert((fence_gpu_va & 0x3u) == 0u);

   /* PKT3_EVENT_WRITE_EOP body (5 dwords after header, count = 4):
    *   DW1: EVENT_TYPE(20=CACHE_FLUSH_AND_INV_TS) | EVENT_INDEX(5=TS)
    *   DW2: ADDRESS_LO (4-byte aligned, byte 0 holds DATA_SEL+INT_SEL)
    *   DW3: ADDRESS_HI (bits 39:32) | DATA_SEL << 29 | INT_SEL << 24
    *   DW4: DATA_LO (32 bits of payload to store at ADDRESS)
    *   DW5: DATA_HI (high 32 bits when DATA_SEL == 64-bit; unused here)
    */
   uint32_t *p = terakan_gfx_command_writer_emit(
      cw, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 6);
   if (unlikely(p == NULL))
      return;

   *p++ = TERAKAN_PALM_PKT3(TERAKAN_PALM_PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   *p++ = (TERAKAN_PALM_EOP_EVENT_TYPE_CACHE_FLUSH_AND_INV_TS << 0) |
          (TERAKAN_PALM_EOP_EVENT_INDEX_TS                   << 8);
   *p++ = (uint32_t)(fence_gpu_va & 0xFFFFFFFFu);
   *p++ = ((uint32_t)((fence_gpu_va >> 32) & 0xFFu)) |
          (TERAKAN_PALM_EOP_DATA_SEL_SEND_32BIT_LOW << 29) |
          (TERAKAN_PALM_EOP_INT_SEL_NONE            << 24);
   *p++ = seq_value;
   *p++ = 0;

   terakan_gfx_command_writer_emit_done(cw, p);
}

void
terakan_palm_emit_wait_fence_geq(
   struct terakan_gfx_command_writer * const cw,
   uint64_t const                            fence_gpu_va,
   uint32_t const                            seq_value)
{
   assert((fence_gpu_va & 0x3u) == 0u);

   /* PKT3_WAIT_REG_MEM body (6 dwords after header, count = 5):
    *   DW1: function (>=) | mem-space | engine (PFP)
    *   DW2: ADDRESS_LO
    *   DW3: ADDRESS_HI
    *   DW4: REFERENCE
    *   DW5: MASK (0xFFFFFFFF -> compare all bits)
    *   DW6: POLL_INTERVAL
    */
   uint32_t *p = terakan_gfx_command_writer_emit(
      cw, TERAKAN_GFX_COMMAND_WRITER_EMIT_CONTENTS_OTHER, 7);
   if (unlikely(p == NULL))
      return;

   *p++ = TERAKAN_PALM_PKT3(TERAKAN_PALM_PKT3_WAIT_REG_MEM, 5, 0);
   *p++ = TERAKAN_PALM_WRM_FUNC_GEQ |
          TERAKAN_PALM_WRM_MEM_SPACE_MEMORY |
          TERAKAN_PALM_WRM_ENGINE_PFP;
   *p++ = (uint32_t)(fence_gpu_va & 0xFFFFFFFCu);
   *p++ = (uint32_t)((fence_gpu_va >> 32) & 0xFFu);
   *p++ = seq_value;
   *p++ = 0xFFFFFFFFu;
   *p++ = TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL;

   terakan_gfx_command_writer_emit_done(cw, p);
}

uint32_t
terakan_palm_build_acquire_coher_mask(enum terakan_carrier_domain const domain)
{
   /* Acquire invalidates source-cache domains so Palm's first read of
    * the carrier does not see stale TC/VC/SH state.
    *
    * BUFFER invalidates TC + VC + SH, covering texture, vertex, shader
    * fetch, and constant-cache fetch paths. CB_COLOR carriers are normally
    * write-only on Palm acquire; gpu_dirty bookkeeping handles prior
    * Palm writes with a release-style flush before re-acquire. */
   switch (domain) {
   case TERAKAN_CARRIER_DOMAIN_BUFFER:
      return S_0085F0_TC_ACTION_ENA(1) |
             S_0085F0_VC_ACTION_ENA(1) |
             S_0085F0_SH_ACTION_ENA(1);

   case TERAKAN_CARRIER_DOMAIN_CB_COLOR:
      return 0u;

   case TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE:
   case TERAKAN_CARRIER_DOMAIN_COMPRESSED_DEPTH:
   case TERAKAN_CARRIER_DOMAIN_TILED_IMAGE:
   case TERAKAN_CARRIER_DOMAIN_SX_EXPORT:
   case TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT:
   case TERAKAN_CARRIER_DOMAIN_COUNT:
      /* Unsupported carrier domains are rejected before any emit-helper
       * call. Return 0 defensively. */
      return 0u;
   }

   return 0u;
}

uint32_t
terakan_palm_build_release_coher_mask(enum terakan_carrier_domain const domain)
{
   /* Release flushes destination-cache domains so external consumers see
    * the post-Palm state. CB_COLOR uses CB_ACTION_ENA and all
    * CB*_DEST_BASE_ENA bits so the kernel checks every bound CB base
    * address; SH catches metadata writes. BUFFER uses TC_ACTION_ENA
    * and SH_ACTION_ENA for shader write paths. */
   switch (domain) {
   case TERAKAN_CARRIER_DOMAIN_BUFFER:
      return S_0085F0_TC_ACTION_ENA(1) |
             S_0085F0_SH_ACTION_ENA(1);

   case TERAKAN_CARRIER_DOMAIN_CB_COLOR:
      return S_0085F0_CB_ACTION_ENA(1) |
             S_0085F0_SH_ACTION_ENA(1) |
             S_0085F0_CB0_DEST_BASE_ENA(1) | S_0085F0_CB1_DEST_BASE_ENA(1) |
             S_0085F0_CB2_DEST_BASE_ENA(1) | S_0085F0_CB3_DEST_BASE_ENA(1) |
             S_0085F0_CB4_DEST_BASE_ENA(1) | S_0085F0_CB5_DEST_BASE_ENA(1) |
             S_0085F0_CB6_DEST_BASE_ENA(1) | S_0085F0_CB7_DEST_BASE_ENA(1);

   case TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE:
   case TERAKAN_CARRIER_DOMAIN_COMPRESSED_DEPTH:
   case TERAKAN_CARRIER_DOMAIN_TILED_IMAGE:
   case TERAKAN_CARRIER_DOMAIN_SX_EXPORT:
   case TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT:
   case TERAKAN_CARRIER_DOMAIN_COUNT:
      return 0u;
   }

   return 0u;
}
