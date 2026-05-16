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

struct terakan_bo;
struct terakan_dmabuf_carrier;

/* Inline-storage cap for the acquire + release lists.  Submissions
 * with more distinct carriers than this fall back to a heap-allocated
 * extension; the inline cap covers the dominant Palm workloads where
 * a frame touches a handful of imported dma-bufs. */
#define TERAKAN_CARRIER_INLINE_CAP 8u

/* Worst-case PM4 dwords emitted per acquire carrier (SURFACE_SYNC =
 * 5 dwords header+body) and per release carrier (5 dwords).  The
 * post-release EOP fence write is 5 dwords (PKT3_EVENT_WRITE_EOP).
 * Callers size their combined-IB allocation by:
 *   acquire_count * TERAKAN_CARRIER_DWORDS_PER_ACQUIRE
 * + release_count * TERAKAN_CARRIER_DWORDS_PER_RELEASE
 * + TERAKAN_CARRIER_DWORDS_PER_EOP    (only when a fence target
 *                                      is wired) */
#define TERAKAN_CARRIER_DWORDS_PER_ACQUIRE 5u
#define TERAKAN_CARRIER_DWORDS_PER_RELEASE 5u
#define TERAKAN_CARRIER_DWORDS_PER_EOP     5u

struct terakan_carrier_submit_lists {
   struct terakan_dmabuf_carrier * inline_acquires[TERAKAN_CARRIER_INLINE_CAP];
   struct terakan_dmabuf_carrier * inline_releases[TERAKAN_CARRIER_INLINE_CAP];

   /* Heap extension; NULL when inline storage is in use. */
   struct terakan_dmabuf_carrier ** heap_acquires;
   struct terakan_dmabuf_carrier ** heap_releases;

   unsigned acquire_count;
   unsigned acquire_cap;
   unsigned release_count;
   unsigned release_cap;
};

void
terakan_carrier_submit_lists_init(struct terakan_carrier_submit_lists * lists);

void
terakan_carrier_submit_lists_destroy(struct terakan_carrier_submit_lists * lists);

/* Get the n-th carrier from the acquire or release list. */
struct terakan_dmabuf_carrier *
terakan_carrier_submit_lists_acquire(
   const struct terakan_carrier_submit_lists * lists, unsigned index);

struct terakan_dmabuf_carrier *
terakan_carrier_submit_lists_release(
   const struct terakan_carrier_submit_lists * lists, unsigned index);

/* Append-with-dedupe by pointer identity.  No-op if the carrier
 * pointer is already present in the list.  Returns false on heap
 * allocation failure (rare; non-fatal -- caller skips that emission
 * for the submit and logs a warning).  NULL carriers are silently
 * ignored. */
bool
terakan_carrier_submit_lists_append_acquire(
   struct terakan_carrier_submit_lists * lists,
   struct terakan_dmabuf_carrier *       carrier);

bool
terakan_carrier_submit_lists_append_release(
   struct terakan_carrier_submit_lists * lists,
   struct terakan_dmabuf_carrier *       carrier);

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
 * asserts capacity.  CPU-polls each carrier's acquire_sync_fd before
 * emitting its PFP-engine SURFACE_SYNC packet. */
void
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
