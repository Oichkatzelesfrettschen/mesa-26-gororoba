/* SPDX-License-Identifier: MIT */
/*
 * terakan_palm_sync
 *
 * PR B of the dmabuf-carrier Phase 0 redesign.  Introduces the PM4
 * packet helpers the carrier acquire/release paths will emit once
 * PR C wires the queue-submit integration.  No queue path calls
 * these yet; they are declared + defined and exercised only via the
 * future PR C exposure surface.
 *
 * Three packet families:
 *
 *   1. SURFACE_SYNC ranged over [gpu_va, gpu_va + size_bytes), with
 *      explicit PFP / ME engine selector.  Replaces the
 *      address-space-wide pattern in terakan_compute_multiplex_emit_
 *      surface_sync with a ranged variant suitable for carrier acquire
 *      (PFP source-cache invalidate) and release (ME destination
 *      flush).
 *
 *   2. EOP timestamp writeback.  PKT3_EVENT_WRITE_EOP that writes a
 *      32-bit sequence value to a fence-BO target VA.  Used at the
 *      end of the release sequence so an external consumer can wait
 *      on the sync_file derived from the dma-buf reservation that
 *      the kernel will signal once the carrier BO retires.
 *
 *   3. WAIT_REG_MEM on the EOP-written fence word.  PFP wait on
 *      [fence_va] >= seq_value so subsequent packets see retirement.
 *
 * Why no WAIT_REG_MEM on CP_COHER_STATUS: that status register is a
 * Gfx7+ construct and is not defined in the Evergreen terascale
 * register header (terascale_evergreend.h has 0x85F0/0x85F4/0x85F8
 * for CP_COHER_CNTL/SIZE/BASE but no 0x85FC status counterpart).
 * On TeraScale-2 the SURFACE_SYNC packet's own POLL_INTERVAL is the
 * documented wait mechanism; the EOP-written fence-word path is the
 * external retirement-visibility proof.
 */

#ifndef TERAKAN_PALM_SYNC_H
#define TERAKAN_PALM_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "terakan_dmabuf_carrier.h"

struct terakan_gfx_command_writer;

/*
 * PM4 SURFACE_SYNC engine selector.  The PKT3_SURFACE_SYNC packet's
 * DW2 bit 31 selects the engine that performs the sync.  PFP gates
 * front-end work (index DMA, VGT requests); ME gates middle-engine
 * (rasterization, depth, color) work.  Acquire paths use PFP so
 * index/vertex fetches do not race PALM with stale carrier data;
 * release paths use ME so destination color/depth writes are
 * flushed before the EOP timestamp retires.
 */
enum terakan_palm_sync_engine {
   TERAKAN_PALM_SYNC_ENGINE_PFP = 0,
   TERAKAN_PALM_SYNC_ENGINE_ME  = 1,
};

/*
 * Emit a PKT3_SURFACE_SYNC over [gpu_va, gpu_va + size_bytes).
 *
 * COHER_BASE and COHER_SIZE are 256-byte-aligned [39:8] fields per
 * Evergreen 3D Registers Reference, surface-sync section.  Inputs
 * are 1-byte-granular addresses + sizes; the helper rounds size up
 * to 256 bytes and asserts gpu_va is 256-byte aligned.
 *
 * coher_cntl is a 29-bit OR of `S_0085F0_*_ACTION_ENA` bits from
 * the kernel-generated terascale_evergreend.h header.  Callers
 * construct the appropriate per-access-class mask via
 * `terakan_palm_build_acquire_coher_mask` /
 * `terakan_palm_build_release_coher_mask` below.
 */
void
terakan_palm_emit_surface_sync_range(
   struct terakan_gfx_command_writer *cw,
   enum terakan_palm_sync_engine      engine,
   uint32_t                           coher_cntl,
   uint64_t                           gpu_va,
   uint64_t                           size_bytes);

/*
 * Emit a PKT3_EVENT_WRITE_EOP that writes `seq_value` to
 * [fence_gpu_va, fence_gpu_va + 4) when the GPU has retired all
 * prior work in this IB.  fence_gpu_va must be 4-byte aligned.
 *
 * Used at the tail of a carrier release sequence.  The external
 * consumer's sync_file derived from the carrier dma-buf reservation
 * is signaled by the kernel once the kernel observes this fence
 * word transition; on TeraScale-2 the fence write is the
 * load-bearing retirement-visibility proof.
 */
void
terakan_palm_emit_eop_release_timestamp(
   struct terakan_gfx_command_writer *cw,
   uint64_t                           fence_gpu_va,
   uint32_t                           seq_value);

/*
 * Emit a PKT3_WAIT_REG_MEM that stalls the PFP engine until the
 * 32-bit memory at [fence_gpu_va] is >= seq_value.  Pairs with
 * `terakan_palm_emit_eop_release_timestamp` when the same IB needs
 * to wait on the same EOP fence.
 */
void
terakan_palm_emit_wait_fence_geq(
   struct terakan_gfx_command_writer *cw,
   uint64_t                           fence_gpu_va,
   uint32_t                           seq_value);

/*
 * Translate carrier domain + access class into the COHER_CNTL bit
 * mask used by SURFACE_SYNC.  Returns an OR of `S_0085F0_*_ACTION_ENA`
 * values.
 *
 * The build helpers are intentionally narrow: they cover ONLY the
 * Phase 0 domains (BUFFER, CB_COLOR).  Domains marked
 * PENDING_PROBE / FORBIDDEN / DECOMPRESS_REQUIRED in the policy
 * table return 0 (no bits set); callers should never reach these
 * helpers for those domains because the import path rejects them
 * earlier.
 */
uint32_t
terakan_palm_build_acquire_coher_mask(enum terakan_carrier_domain domain);

uint32_t
terakan_palm_build_release_coher_mask(enum terakan_carrier_domain domain);

#endif /* TERAKAN_PALM_SYNC_H */
