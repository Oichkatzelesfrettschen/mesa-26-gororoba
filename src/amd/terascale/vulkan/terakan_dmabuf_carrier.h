/* SPDX-License-Identifier: MIT */
/*
 * terakan_dmabuf_carrier
 *
 * Phase 0 carrier model for the Terakan PALM external-sync redesign.
 * This header introduces ONLY the policy enums, the policy table,
 * and the carrier struct.  No behavior is wired here; subsequent
 * commits introduce the PM4 packet helpers and the queue-submit
 * integration.
 *
 * Cache-domain truth boundary derives from the steinmarder
 * 2026-05-16 carrier-path cache-domain evidence matrix (filed under
 * src/re/r600/findings/active/ in the steinmarder repo).  The
 * matrix forbids global CAS, compressed depth/HTILE, tiled/modifier,
 * SX export, and cacheless RAT for Phase 0 PALM carriers; only the
 * dma-buf buffer carrier and the CB color render-target carrier are
 * empirically backed for this phase.
 *
 * The policy is intentionally CLOSED: every dma-buf import classifies
 * into one of the seven domains below, and every domain has an
 * explicit support verdict.  Adding a new domain or promoting a
 * pending-probe domain to allowed REQUIRES updating both the enum
 * and the policy table here, plus a finding-doc proof on the
 * steinmarder side.
 */

#ifndef TERAKAN_DMABUF_CARRIER_H
#define TERAKAN_DMABUF_CARRIER_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

struct terakan_bo;
struct terakan_device;

/*
 * Carrier-domain taxonomy.
 *
 * BUFFER and CB_COLOR are the only domains Phase 0 builds emit
 * surface-sync packets for.  The remaining five MUST hit one of the
 * REJECT paths during import.
 */
enum terakan_carrier_domain {
   TERAKAN_CARRIER_DOMAIN_BUFFER = 0,
   TERAKAN_CARRIER_DOMAIN_CB_COLOR,
   TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE,
   TERAKAN_CARRIER_DOMAIN_COMPRESSED_DEPTH,
   TERAKAN_CARRIER_DOMAIN_TILED_IMAGE,
   TERAKAN_CARRIER_DOMAIN_SX_EXPORT,
   TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT,

   TERAKAN_CARRIER_DOMAIN_COUNT,
};

/*
 * Support verdict per domain.  ALLOWED_PHASE0 is the only verdict
 * that produces a usable carrier; all others reject or fall back.
 *
 * - PENDING_PROBE:        documented by AMD, not yet empirically
 *                         backed on PALM.  Future work promotes via
 *                         finding-doc + matrix update.
 * - FORBIDDEN:            explicitly disallowed; documentation
 *                         exists but the hardware/PALM combination
 *                         does not support correct external
 *                         semantics (e.g. global CAS).
 * - DECOMPRESS_REQUIRED:  the carrier path is reachable only via an
 *                         explicit decompression / uncompressed
 *                         fallback before export.
 */
enum terakan_carrier_support {
   TERAKAN_CARRIER_ALLOWED_PHASE0 = 0,
   TERAKAN_CARRIER_PENDING_PROBE,
   TERAKAN_CARRIER_FORBIDDEN,
   TERAKAN_CARRIER_DECOMPRESS_REQUIRED,
};

struct terakan_carrier_policy {
   enum terakan_carrier_domain  domain;
   enum terakan_carrier_support support;
   /* Human-readable reason; emitted in debug logs and in any
    * VK_ERROR_FEATURE_NOT_PRESENT return path. */
   const char *                 reason;
};

/*
 * The carrier itself.
 *
 * gpu_va + size_bytes define the [va, va+size) range that
 * SURFACE_SYNC packets address via COHER_BASE / COHER_SIZE (each
 * 256-byte units in the [39:8] field per Evergreen 3D Registers
 * Reference, surface-sync section).  The carrier holds the dma-buf
 * fd kept open across the carrier lifetime to anchor the kernel
 * reservation object the external producer/consumer attaches fences
 * to.
 *
 * acquired / gpu_dirty track the state machine documented in the
 * Carrier_Path_Rough_Draft (UNBOUND -> IMPORTED -> ACQUIRED ->
 * GPU_READING/WRITING -> GPU_DIRTY -> RELEASED).  Future commits
 * lift these into an explicit state enum.
 */
struct terakan_dmabuf_carrier {
   int                              dmabuf_fd;

   struct terakan_bo *              bo;
   uint64_t                         gpu_va;
   uint64_t                         size_bytes;

   enum terakan_carrier_domain      domain;
   enum terakan_carrier_support     support;

   uint32_t                         stride;
   uint32_t                         format;
   uint32_t                         tiling_mode;
   uint32_t                         usage_mask;

   bool                             imported;
   bool                             acquired;
   bool                             gpu_dirty;
   bool                             linear_only;

   /* Fence sidecars: the dma-buf reservation owns the actual
    * kernel sync state.  These FDs are kept open across the
    * carrier lifetime and produced/consumed by acquire/release. */
   int                              acquire_sync_fd;
   int                              release_sync_fd;
};

/*
 * Policy table lookup.  Returns a pointer to the immutable
 * per-domain policy entry, or NULL if the domain is out of range
 * (defensive; the enum is closed so callers should never trigger
 * the NULL path in normal flow).
 */
const struct terakan_carrier_policy *
terakan_dmabuf_carrier_policy(enum terakan_carrier_domain domain);

/*
 * Human-readable name for log + error-message strings.
 */
const char *
terakan_carrier_domain_name(enum terakan_carrier_domain domain);

const char *
terakan_carrier_support_name(enum terakan_carrier_support support);

/*
 * Dump the policy table to the terakan log channel.  Called once at
 * physical-device init when TERAKAN_DEBUG includes "carrier".  No
 * carrier state side-effect.
 */
void
terakan_dmabuf_carrier_log_policy(struct terakan_device *device);

/*
 * Returns true when TERAKAN_ENABLE_DMABUF_CARRIER=1 is in the
 * environment.  Centralizes the env-gate check so every caller uses
 * the same parsing rule (exact "1", not 1/true/yes).
 */
bool
terakan_dmabuf_carrier_enabled(void);

/*
 * Caller-provided descriptor for an import attempt.
 *
 * The caller classifies the dma-buf into a carrier domain before
 * import; the policy table decides whether that domain is supported.
 */
struct terakan_dmabuf_carrier_desc {
   int                              dmabuf_fd;
   enum terakan_carrier_domain      domain;
   uint64_t                         size_bytes;
   uint32_t                         stride;
   uint32_t                         format;
   uint32_t                         tiling_mode;
   uint32_t                         usage_mask;
};

/*
 * Import a dma-buf as a Terakan carrier, subject to the closed
 * policy table.  On ALLOWED_PHASE0 the function imports the fd
 * through the winsys bo->import_fd path, populates *out, and
 * returns VK_SUCCESS.  On any other support verdict the function
 * logs the policy reason and returns VK_ERROR_FEATURE_NOT_PRESENT.
 * Returns other VK_ERROR_* codes for I/O failures originating in
 * the underlying winsys import.
 */
VkResult
terakan_dmabuf_carrier_import(struct terakan_device *                    device,
                              const struct terakan_dmabuf_carrier_desc * desc,
                              struct terakan_dmabuf_carrier **           out);

/*
 * Release any resources the carrier holds: BO unref, dmabuf fd
 * close, sync FDs close.  Safe to call on a partially-constructed
 * carrier (NULL-tolerant; tolerates fields left at zero/-1).
 */
void
terakan_dmabuf_carrier_destroy(struct terakan_device *         device,
                               struct terakan_dmabuf_carrier * carrier);

#endif /* TERAKAN_DMABUF_CARRIER_H */
