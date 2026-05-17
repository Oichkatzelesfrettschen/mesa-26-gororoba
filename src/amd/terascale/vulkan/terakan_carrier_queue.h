/* SPDX-License-Identifier: MIT */
/*
 * terakan_carrier_queue
 *
 * Submit-time carrier discovery + PM4 acquire/release emission for
 * the Terakan dmabuf-carrier path on Palm (Wrestler GPU, CHIP_PALM,
 * Evergreen / TeraScale-2 VLIW5).
 *
 * Carrier discovery walks the indirect-buffer BO reference list,
 * loads each terakan_bo::carrier atomic, classifies the carrier as
 * acquire-only / release-only / acquire+release based on per-BO
 * read_domains / write_domain in the radeon CS reloc, dedupes by
 * carrier pointer identity, and prepares two parallel arrays the
 * emit helpers consume.
 *
 * PM4 emission writes PFP-engine SURFACE_SYNC packets for the
 * acquire prologue, ME-engine SURFACE_SYNC packets for the release
 * epilogue, and an EVENT_WRITE_EOP for a single post-release
 * fence-word write per submission.  See the AMD Evergreen-Family ISA
 * reference (PM4 packet table) and AMD 3D Engine Programming Guide
 * for Evergreen (CP_COHER_CNTL, SURFACE_SYNC) for the underlying
 * hardware contract.
 *
 * The whole subsystem is gated by terakan_dmabuf_carrier_enabled()
 * (TERAKAN_ENABLE_DMABUF_CARRIER=1).  When the gate is off the
 * caller MUST NOT invoke any of these helpers; the queue-submit path
 * is byte-for-byte unchanged.
 */

#ifndef TERAKAN_CARRIER_QUEUE_H
#define TERAKAN_CARRIER_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

struct terakan_bo;
struct terakan_dmabuf_carrier;

/* Inline-storage cap for the acquire + release lists.  Submissions
 * with more distinct carriers than this fall back to a heap-allocated
 * extension; the inline cap covers the dominant Palm workloads where
 * a frame touches a handful of imported dma-bufs. */
#define TERAKAN_CARRIER_INLINE_CAP 8u

/* Worst-case PM4 dwords emitted per acquire / release carrier on the
 * radeon-DRM TERAKAN_QUEUE_RELOCATION_TYPE_DRM_NOP path:
 *   - PKT3_SURFACE_SYNC: 5 dwords (header + 4 body)
 *   - PKT3_NOP reloc pair: 2 dwords (header + bo_reference dword
 *                                    offset payload)
 * The radeon Evergreen CS validator
 * (drivers/gpu/drm/radeon/evergreen_cs.c::evergreen_cs_check_reg
 * and evergreen_cs_packet_next_reloc) requires every packet that
 * references a BO via GPU virtual address to be paired with a
 * PKT3_NOP whose payload encodes the byte offset (in drm_radeon_cs_
 * reloc-sized units expressed as 4*bo_reference_index) of that BO
 * within the reloc array.  The post-release EOP fence write remains 5
 * dwords (PKT3_EVENT_WRITE_EOP) and is only emitted when fence_gpu_va
 * is non-zero; the present queue-wiring path keeps fence_gpu_va = 0,
 * so EOP_DWORDS contributes nothing to the IB capacity assertion in
 * practice.
 *
 * Callers size their combined-IB allocation by:
 *   acquire_count * TERAKAN_CARRIER_DWORDS_PER_ACQUIRE
 * + release_count * TERAKAN_CARRIER_DWORDS_PER_RELEASE
 * + TERAKAN_CARRIER_DWORDS_PER_EOP    (only when a fence target
 *                                      is wired) */
#define TERAKAN_CARRIER_DWORDS_PER_SURFACE_SYNC 5u
#define TERAKAN_CARRIER_DWORDS_PER_RELOC_NOP    2u
#define TERAKAN_CARRIER_DWORDS_PER_ACQUIRE \
   (TERAKAN_CARRIER_DWORDS_PER_SURFACE_SYNC + TERAKAN_CARRIER_DWORDS_PER_RELOC_NOP)
#define TERAKAN_CARRIER_DWORDS_PER_RELEASE \
   (TERAKAN_CARRIER_DWORDS_PER_SURFACE_SYNC + TERAKAN_CARRIER_DWORDS_PER_RELOC_NOP)
#define TERAKAN_CARRIER_DWORDS_PER_EOP     5u

/* Per-entry record on the acquire / release lists.  The reloc-pairing
 * DRM_NOP packet needs the BO's index into the bo_references array
 * passed to the radeon CS ioctl; the collector captures that index in
 * lockstep with the carrier pointer and the emit helpers pass it
 * straight through.  bo_offset_bytes is reserved for future
 * subrange carriers; the present whole-BO Phase 0 path stores 0 and
 * the emit helper treats COHER_BASE as BO-relative (kernel applies
 * the BO's GPU VA at submit time via the reloc pairing). */
struct terakan_carrier_submit_entry {
   struct terakan_dmabuf_carrier * carrier;
   uint32_t                        bo_reference_index;
   uint32_t                        bo_offset_bytes;
};

struct terakan_carrier_submit_lists {
   struct terakan_carrier_submit_entry inline_acquires[TERAKAN_CARRIER_INLINE_CAP];
   struct terakan_carrier_submit_entry inline_releases[TERAKAN_CARRIER_INLINE_CAP];

   /* Heap extension; NULL when inline storage is in use. */
   struct terakan_carrier_submit_entry * heap_acquires;
   struct terakan_carrier_submit_entry * heap_releases;

   unsigned acquire_count;
   unsigned acquire_cap;
   unsigned release_count;
   unsigned release_cap;
};

void
terakan_carrier_submit_lists_init(struct terakan_carrier_submit_lists * lists);

void
terakan_carrier_submit_lists_destroy(struct terakan_carrier_submit_lists * lists);

/* Get the n-th entry from the acquire or release list. */
struct terakan_carrier_submit_entry
terakan_carrier_submit_lists_acquire(
   const struct terakan_carrier_submit_lists * lists, unsigned index);

struct terakan_carrier_submit_entry
terakan_carrier_submit_lists_release(
   const struct terakan_carrier_submit_lists * lists, unsigned index);

/* Append-with-dedupe by pointer identity.  No-op if the carrier
 * pointer is already present in the list.  Returns false on heap
 * allocation failure (rare; non-fatal -- caller skips that emission
 * for the submit and logs a warning).  NULL carriers are silently
 * ignored.  bo_reference_index records where the BO sits in the radeon
 * CS bo_references array so the DRM_NOP reloc-pairing packet emitted
 * alongside each SURFACE_SYNC can name it. */
bool
terakan_carrier_submit_lists_append_acquire(
   struct terakan_carrier_submit_lists * lists,
   struct terakan_dmabuf_carrier *       carrier,
   uint32_t                              bo_reference_index,
   uint32_t                              bo_offset_bytes);

bool
terakan_carrier_submit_lists_append_release(
   struct terakan_carrier_submit_lists * lists,
   struct terakan_dmabuf_carrier *       carrier,
   uint32_t                              bo_reference_index,
   uint32_t                              bo_offset_bytes);

/* Walk the (winsys-opaque) reloc array paired with the writer's
 * reference_bos[].  For each carrier-published BO, classify by
 * read_domains / write_domain in the matching radeon reloc and
 * append into the lists.  Caller passes the radeon reloc array;
 * non-radeon winsys paths MUST NOT invoke this (they have no carrier
 * import path either).
 *
 * Returns the total number of acquire+release entries appended
 * (counting an acquire+release BO as 2). */
unsigned
terakan_carrier_collect_from_radeon_relocs(
   struct terakan_bo const * const *     reference_bos,
   uint32_t                              reference_count,
   const void *                          radeon_relocs,
   uint32_t                              radeon_reloc_count,
   struct terakan_carrier_submit_lists * lists);

/* Emit acquire prologue into ib_dwords[*cursor ..].  Advances *cursor
 * by the number of dwords written.  ib_capacity_dwords is the cap
 * size of the dword buffer; the helper writes at most
 * acquire_count * TERAKAN_CARRIER_DWORDS_PER_ACQUIRE dwords and
 * asserts capacity.  Indefinitely CPU-polls each carrier's
 * acquire_sync_fd before emitting its PFP-engine SURFACE_SYNC packet;
 * an FD that fails to signal (poll returns POLLERR/POLLHUP/POLLNVAL
 * or poll itself errors) is not safe to read past, so the helper
 * returns VK_ERROR_DEVICE_LOST and stops emission for that submit.
 * Returns VK_SUCCESS on normal completion or when no carriers carry
 * an attached producer fence (FD < 0). */
VkResult
terakan_carrier_emit_acquires_dwords(
   uint32_t *                                  ib_dwords,
   uint32_t                                    ib_capacity_dwords,
   uint32_t *                                  cursor,
   const struct terakan_carrier_submit_lists * lists);

/* Emit release epilogue + (optional) EOP fence into ib_dwords[*cursor
 * ..].  fence_gpu_va == 0 skips the EOP. */
void
terakan_carrier_emit_releases_dwords(
   uint32_t *                                  ib_dwords,
   uint32_t                                    ib_capacity_dwords,
   uint32_t *                                  cursor,
   const struct terakan_carrier_submit_lists * lists,
   uint64_t                                    fence_gpu_va,
   uint32_t                                    fence_seq);

#endif /* TERAKAN_CARRIER_QUEUE_H */
