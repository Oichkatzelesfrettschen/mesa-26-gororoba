/* SPDX-License-Identifier: MIT */
/*
 * terakan_carrier_queue
 *
 * Submit-time carrier discovery + PM4 acquire/release emission on
 * Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5).
 *
 * The collector walks the writer's reference_bos[] array in
 * lockstep with the radeon CS reloc array (matched by sequential
 * index -- the bo_reference_writer guarantees reference_bos[i]
 * corresponds to the i-th drm_radeon_cs_reloc).  For each BO whose
 * atomic carrier pointer is non-NULL, the matching reloc's
 * read_domains and write_domain fields classify the carrier:
 *   read_domains != 0 -> append into acquires list
 *   write_domain != 0 -> append into releases list
 * Append-with-dedupe by carrier pointer identity keeps duplicate
 * SURFACE_SYNC emissions out of the merged IB.
 *
 * The emit helpers write directly into a flat dword buffer (the
 * combined IB that queue-submit builds via alloca) rather than into
 * a terakan_gfx_command_writer.  At submit time the writer is
 * already torn down; the IB is finalized and its contents are
 * patched into a larger alloca'd buffer.  PM4 encoding matches the
 * AMD Evergreen-Family ISA reference (PM4 packet table) and the AMD
 * 3D Engine Programming Guide for Evergreen (CP_COHER_CNTL,
 * SURFACE_SYNC section).
 */

#include "terakan_carrier_queue.h"

#include "terakan_bo.h"
#include "terakan_dmabuf_carrier.h"
#include "terakan_palm_sync.h"

#include "amd/terascale/common/terascale_evergreend.h"

#include "util/log.h"
#include "util/macros.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <radeon_drm.h>

/* Local PKT3 + opcode macros mirror terakan_palm_sync.c.  The
 * encoding is defined in the AMD Evergreen-Family ISA reference,
 * PM4 packet header layout. */
#define TERAKAN_CARRIER_PKT3(op, count, predicate)              \
   (((3u) << 30) |                                              \
    (((count) & 0x3FFFu) << 16) |                               \
    (((op)    & 0xFFu)   << 8)  |                               \
    ((predicate) & 0x1u))

#define TERAKAN_CARRIER_PKT3_SURFACE_SYNC      0x43u
#define TERAKAN_CARRIER_PKT3_EVENT_WRITE_EOP   0x47u
#define TERAKAN_CARRIER_PKT3_NOP               0x10u

/* SURFACE_SYNC engine selector bit (DW1 bit 31).  Mirrors
 * TERAKAN_BARRIER_SURFACE_SYNC_ENGINE_ME in terakan_barrier.h but
 * defined locally so this TU does not have to pull the barrier
 * header in. */
#define TERAKAN_CARRIER_SURFACE_SYNC_ENGINE_ME (1u << 31)

/* POLL_INTERVAL value used by the rest of terakan for SURFACE_SYNC
 * (matches TERAKAN_BARRIER_SURFACE_SYNC_POLL_INTERVAL). */
#define TERAKAN_CARRIER_SURFACE_SYNC_POLL_INTERVAL 10u

/* EVENT_WRITE_EOP encoding constants per the AMD Evergreen-Family
 * ISA reference, EVENT_WRITE_EOP packet definition. */
#define TERAKAN_CARRIER_EOP_EVENT_TYPE_CACHE_FLUSH_AND_INV_TS 20u
#define TERAKAN_CARRIER_EOP_EVENT_INDEX_TS                    5u
#define TERAKAN_CARRIER_EOP_DATA_SEL_SEND_32BIT_LOW           1u
#define TERAKAN_CARRIER_EOP_INT_SEL_NONE                      0u

/* Bounded CPU wait for an imported acquire sync_fd, in milliseconds.
 * The dma-buf reservation that produced this FD is expected to be
 * already-signaled by the time the importer reaches queue submit;
 * the poll is a defensive read-back, not a real blocking point. */
#define TERAKAN_CARRIER_ACQUIRE_POLL_TIMEOUT_MS 250

void
terakan_carrier_submit_lists_init(struct terakan_carrier_submit_lists * const lists)
{
   memset(lists, 0, sizeof(*lists));
   lists->acquire_cap = TERAKAN_CARRIER_INLINE_CAP;
   lists->release_cap = TERAKAN_CARRIER_INLINE_CAP;
}

void
terakan_carrier_submit_lists_destroy(struct terakan_carrier_submit_lists * const lists)
{
   free(lists->heap_acquires);
   free(lists->heap_releases);
   lists->heap_acquires = NULL;
   lists->heap_releases = NULL;
   lists->acquire_count = 0;
   lists->release_count = 0;
}

struct terakan_carrier_submit_entry
terakan_carrier_submit_lists_acquire(
   const struct terakan_carrier_submit_lists * const lists, unsigned const index)
{
   assert(index < lists->acquire_count);
   if (lists->heap_acquires != NULL)
      return lists->heap_acquires[index];
   return lists->inline_acquires[index];
}

struct terakan_carrier_submit_entry
terakan_carrier_submit_lists_release(
   const struct terakan_carrier_submit_lists * const lists, unsigned const index)
{
   assert(index < lists->release_count);
   if (lists->heap_releases != NULL)
      return lists->heap_releases[index];
   return lists->inline_releases[index];
}

static bool
spill_to_heap(struct terakan_carrier_submit_entry * const           inline_storage,
              struct terakan_carrier_submit_entry ** const          heap_storage,
              unsigned const                                        current_count,
              unsigned * const                                      cap_inout,
              unsigned const                                        new_cap)
{
   struct terakan_carrier_submit_entry * const new_buf =
      malloc(sizeof(struct terakan_carrier_submit_entry) * new_cap);
   if (new_buf == NULL)
      return false;

   if (*heap_storage != NULL) {
      memcpy(new_buf, *heap_storage,
             sizeof(struct terakan_carrier_submit_entry) * current_count);
      free(*heap_storage);
   } else {
      memcpy(new_buf, inline_storage,
             sizeof(struct terakan_carrier_submit_entry) * current_count);
   }

   *heap_storage = new_buf;
   *cap_inout    = new_cap;
   return true;
}

static struct terakan_dmabuf_carrier *
list_entry_carrier_at(struct terakan_carrier_submit_entry const * const inline_storage,
                      struct terakan_carrier_submit_entry const * const heap_storage,
                      unsigned const                                    index)
{
   return ((heap_storage != NULL) ? heap_storage[index] : inline_storage[index]).carrier;
}

bool
terakan_carrier_submit_lists_append_acquire(
   struct terakan_carrier_submit_lists * const lists,
   struct terakan_dmabuf_carrier * const       carrier,
   uint32_t const                              bo_reference_index,
   uint32_t const                              bo_offset_bytes)
{
   if (carrier == NULL)
      return true;

   /* Pointer-identity dedupe. */
   for (unsigned i = 0; i < lists->acquire_count; ++i) {
      if (list_entry_carrier_at(lists->inline_acquires, lists->heap_acquires, i) == carrier)
         return true;
   }

   if (lists->acquire_count >= lists->acquire_cap) {
      unsigned const new_cap = lists->acquire_cap * 2u;
      if (!spill_to_heap(lists->inline_acquires, &lists->heap_acquires,
                         lists->acquire_count, &lists->acquire_cap, new_cap))
         return false;
   }

   struct terakan_carrier_submit_entry const entry = {
      .carrier            = carrier,
      .bo_reference_index = bo_reference_index,
      .bo_offset_bytes    = bo_offset_bytes,
   };
   if (lists->heap_acquires != NULL) {
      lists->heap_acquires[lists->acquire_count] = entry;
   } else {
      lists->inline_acquires[lists->acquire_count] = entry;
   }
   lists->acquire_count++;
   return true;
}

bool
terakan_carrier_submit_lists_append_release(
   struct terakan_carrier_submit_lists * const lists,
   struct terakan_dmabuf_carrier * const       carrier,
   uint32_t const                              bo_reference_index,
   uint32_t const                              bo_offset_bytes)
{
   if (carrier == NULL)
      return true;

   for (unsigned i = 0; i < lists->release_count; ++i) {
      if (list_entry_carrier_at(lists->inline_releases, lists->heap_releases, i) == carrier)
         return true;
   }

   if (lists->release_count >= lists->release_cap) {
      unsigned const new_cap = lists->release_cap * 2u;
      if (!spill_to_heap(lists->inline_releases, &lists->heap_releases,
                         lists->release_count, &lists->release_cap, new_cap))
         return false;
   }

   struct terakan_carrier_submit_entry const entry = {
      .carrier            = carrier,
      .bo_reference_index = bo_reference_index,
      .bo_offset_bytes    = bo_offset_bytes,
   };
   if (lists->heap_releases != NULL) {
      lists->heap_releases[lists->release_count] = entry;
   } else {
      lists->inline_releases[lists->release_count] = entry;
   }
   lists->release_count++;
   return true;
}

unsigned
terakan_carrier_collect_from_radeon_relocs(
   struct terakan_bo const * const * const     reference_bos,
   uint32_t const                              reference_count,
   const void * const                          radeon_relocs,
   uint32_t const                              radeon_reloc_count,
   struct terakan_carrier_submit_lists * const lists)
{
   if (reference_bos == NULL || radeon_relocs == NULL)
      return 0u;

   /* The bo_reference_writer guarantees reference_bos[i] corresponds
    * to the i-th entry in the opaque references array, which on the
    * drm_radeon winsys is a drm_radeon_cs_reloc[].  Both arrays are
    * the same length (bo_reference_count). */
   uint32_t const count = MIN2(reference_count, radeon_reloc_count);
   struct drm_radeon_cs_reloc const * const relocs =
      (struct drm_radeon_cs_reloc const *)radeon_relocs;

   unsigned appended = 0u;
   for (uint32_t i = 0; i < count; ++i) {
      struct terakan_bo const * const bo_const = reference_bos[i];
      if (bo_const == NULL)
         continue;

      /* The atomic carrier field is mutable state on a BO that's
       * otherwise treated as immutable here.  Cast away const for
       * the atomic load only; the load itself does not mutate. */
      struct terakan_bo * const bo = (struct terakan_bo *)bo_const;
      struct terakan_dmabuf_carrier * const c =
         atomic_load_explicit(&bo->carrier, memory_order_acquire);
      if (c == NULL)
         continue;

      bool const is_reading = (relocs[i].read_domains != 0u);
      bool const is_writing = (relocs[i].write_domain != 0u);

      /* Whole-BO carriers in Phase 0; the BO-relative offset is 0.
       * Subrange carriers (future work) carry a non-zero offset. */
      uint32_t const bo_reference_index = i;
      uint32_t const bo_offset_bytes    = 0u;

      if (is_reading) {
         if (terakan_carrier_submit_lists_append_acquire(
                lists, c, bo_reference_index, bo_offset_bytes))
            ++appended;
         else
            mesa_logw("terakan: carrier acquire list spill alloc failed; "
                      "skipping carrier %p for this submit", (void *)c);
      }
      if (is_writing) {
         if (terakan_carrier_submit_lists_append_release(
                lists, c, bo_reference_index, bo_offset_bytes))
            ++appended;
         else
            mesa_logw("terakan: carrier release list spill alloc failed; "
                      "skipping carrier %p for this submit", (void *)c);
      }
   }
   return appended;
}

/* CPU-wait on an imported acquire sync_fd with a bounded deadline.
 * Returns true if the FD reported readability (signaled) or the FD
 * is invalid (no fence attached); returns false on poll error or
 * timeout, in which case the caller logs and continues -- the
 * SURFACE_SYNC packet is still emitted, the worst-case outcome is
 * the carrier path proceeds without external producer ordering for
 * this submit. */
static bool
wait_acquire_fence_cpu(int const acquire_sync_fd)
{
   if (acquire_sync_fd < 0)
      return true;

   struct pollfd pfd = {
      .fd      = acquire_sync_fd,
      .events  = POLLIN,
      .revents = 0,
   };
   int rc;
   do {
      rc = poll(&pfd, 1, TERAKAN_CARRIER_ACQUIRE_POLL_TIMEOUT_MS);
   } while (rc == -1 && errno == EINTR);

   if (rc < 0) {
      mesa_logw("terakan: carrier acquire poll() errno=%d on fd=%d",
                errno, acquire_sync_fd);
      return false;
   }
   if (rc == 0) {
      mesa_logw("terakan: carrier acquire fd=%d wait timed out after %dms",
                acquire_sync_fd, TERAKAN_CARRIER_ACQUIRE_POLL_TIMEOUT_MS);
      return false;
   }
   return true;
}

static void
emit_surface_sync_range(uint32_t * const ib,
                        uint32_t * const cursor,
                        uint32_t const   capacity,
                        uint32_t const   coher_cntl,
                        bool const       engine_me,
                        uint64_t const   bo_offset_bytes,
                        uint64_t const   size_bytes,
                        uint32_t const   bo_reference_index)
{
   /* COHER_BASE / COHER_SIZE are 256-byte units of the [39:8]
    * portion of the address per the AMD Evergreen 3D Registers
    * Reference, surface-sync section.
    *
    * On the radeon DRM uAPI the address is BO-relative: COHER_BASE
    * is the offset within the BO (in 256-byte units), and the
    * kernel CS validator
    * (drivers/gpu/drm/radeon/evergreen_cs.c::evergreen_cs_packet_
    * next_reloc) rewrites the absolute GPU VA at submit time using
    * the BO referenced by the immediately-following PKT3_NOP
    * reloc-pairing packet.  Whole-BO carriers therefore emit
    * COHER_BASE = 0; future subrange carriers will emit
    * bo_offset_bytes >> 8. */
   assert((bo_offset_bytes & 0xFFu) == 0u);
   assert(size_bytes != 0u);
   assert(*cursor + TERAKAN_CARRIER_DWORDS_PER_SURFACE_SYNC +
                       TERAKAN_CARRIER_DWORDS_PER_RELOC_NOP <= capacity);
   assert((coher_cntl & 0xE0000000u) == 0u);

   uint64_t const aligned_size = (size_bytes + 0xFFu) & ~(uint64_t)0xFFu;
   uint32_t const coher_size   = (uint32_t)(aligned_size >> 8);
   uint32_t const coher_base   = (uint32_t)(bo_offset_bytes >> 8);
   uint32_t const engine_bit   = engine_me ? TERAKAN_CARRIER_SURFACE_SYNC_ENGINE_ME : 0u;

   uint32_t n = *cursor;
   ib[n++] = TERAKAN_CARRIER_PKT3(TERAKAN_CARRIER_PKT3_SURFACE_SYNC, 3, 0);
   ib[n++] = coher_cntl | engine_bit;
   ib[n++] = coher_size;
   ib[n++] = coher_base;
   ib[n++] = TERAKAN_CARRIER_SURFACE_SYNC_POLL_INTERVAL;

   /* DRM_NOP reloc-pairing packet.  Wire form matches the in-tree
    * pattern in terakan_queue_submit_wsi_wait_indirect_buffer
    * (PKT3_WAIT_REG_MEM + trailing PKT3_NOP whose body dword is
    * 4 * bo_reference_index, i.e. the dword offset within the
    * radeon_cs reloc array of the matching drm_radeon_cs_reloc
    * entry).  The address-bearing SURFACE_SYNC packet emits FIRST;
    * the DRM_NOP pairing packet emits SECOND, mirroring the
    * indirect_buffer_append_ptr convention of
    * terakan_gfx_command_writer_add_relocation_for_40_bits. */
   ib[n++] = TERAKAN_CARRIER_PKT3(TERAKAN_CARRIER_PKT3_NOP, 0, 0);
   ib[n++] = 4u * bo_reference_index;
   *cursor = n;
}

static void
emit_eop_release_timestamp(uint32_t * const ib,
                           uint32_t * const cursor,
                           uint32_t const   capacity,
                           uint64_t const   fence_gpu_va,
                           uint32_t const   seq_value)
{
   assert((fence_gpu_va & 0x3u) == 0u);
   assert(*cursor + TERAKAN_CARRIER_DWORDS_PER_EOP <= capacity);

   uint32_t n = *cursor;
   ib[n++] = TERAKAN_CARRIER_PKT3(TERAKAN_CARRIER_PKT3_EVENT_WRITE_EOP, 5 - 1, 0);
   ib[n++] = (TERAKAN_CARRIER_EOP_EVENT_TYPE_CACHE_FLUSH_AND_INV_TS << 0) |
             (TERAKAN_CARRIER_EOP_EVENT_INDEX_TS                   << 8);
   ib[n++] = (uint32_t)(fence_gpu_va & 0xFFFFFFFFu);
   ib[n++] = ((uint32_t)((fence_gpu_va >> 32) & 0xFFu)) |
             (TERAKAN_CARRIER_EOP_DATA_SEL_SEND_32BIT_LOW << 29) |
             (TERAKAN_CARRIER_EOP_INT_SEL_NONE            << 24);
   ib[n++] = seq_value;
   *cursor = n;
}

void
terakan_carrier_emit_acquires_dwords(
   uint32_t * const                                    ib_dwords,
   uint32_t const                                      ib_capacity_dwords,
   uint32_t * const                                    cursor,
   const struct terakan_carrier_submit_lists * const   lists)
{
   for (unsigned i = 0; i < lists->acquire_count; ++i) {
      struct terakan_carrier_submit_entry const entry =
         terakan_carrier_submit_lists_acquire(lists, i);
      struct terakan_dmabuf_carrier * const c = entry.carrier;
      if (c == NULL)
         continue;

      (void)wait_acquire_fence_cpu(c->acquire_sync_fd);

      uint32_t const mask = terakan_palm_build_acquire_coher_mask(c->domain);
      if (mask == 0u)
         continue;

      emit_surface_sync_range(ib_dwords, cursor, ib_capacity_dwords,
                              mask, /* engine_me = */ false,
                              entry.bo_offset_bytes, c->size_bytes,
                              entry.bo_reference_index);
   }
}

void
terakan_carrier_emit_releases_dwords(
   uint32_t * const                                    ib_dwords,
   uint32_t const                                      ib_capacity_dwords,
   uint32_t * const                                    cursor,
   const struct terakan_carrier_submit_lists * const   lists,
   uint64_t const                                      fence_gpu_va,
   uint32_t const                                      fence_seq)
{
   for (unsigned i = 0; i < lists->release_count; ++i) {
      struct terakan_carrier_submit_entry const entry =
         terakan_carrier_submit_lists_release(lists, i);
      struct terakan_dmabuf_carrier * const c = entry.carrier;
      if (c == NULL)
         continue;

      uint32_t const mask = terakan_palm_build_release_coher_mask(c->domain);
      if (mask == 0u)
         continue;

      emit_surface_sync_range(ib_dwords, cursor, ib_capacity_dwords,
                              mask, /* engine_me = */ true,
                              entry.bo_offset_bytes, c->size_bytes,
                              entry.bo_reference_index);
   }

   if (lists->release_count > 0u && fence_gpu_va != 0u) {
      emit_eop_release_timestamp(ib_dwords, cursor, ib_capacity_dwords,
                                 fence_gpu_va, fence_seq);
   }
}
