/* SPDX-License-Identifier: MIT */
/*
 * terakan_dmabuf_carrier
 *
 * Carrier-domain policy for Palm (Wrestler GPU, CHIP_PALM,
 * Evergreen / TeraScale-2 VLIW5) dma-buf synchronization.  The policy
 * is intentionally closed: every dma-buf import classifies into one
 * of the domains below, and every domain has an explicit support
 * verdict.
 *
 * Global CAS, compressed depth or HTILE, tiled or modifier-backed
 * images, SX export, and cacheless RAT do not enter the supported
 * carrier path without probe-backed hardware evidence and a policy-table
 * update.  Buffer carriers and CB color render-target carriers use the
 * acquire/release SURFACE_SYNC path.
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
 * BUFFER and CB_COLOR are the domains with acquire/release SURFACE_SYNC
 * coverage.  The remaining domains require rejection or explicit
 * conversion before import/export.
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
 * Support verdict per domain.  SURFACE_SYNC_ALLOWED is the only verdict
 * that produces a usable carrier; all others reject or fall back.
 *
 * - PENDING_PROBE:        documented by AMD, not yet empirically
 *                         backed on Palm.
 * - FORBIDDEN:            explicitly disallowed; documentation
 *                         exists but the hardware/Palm combination
 *                         does not support correct external
 *                         semantics (e.g. global CAS).
 * - DECOMPRESS_REQUIRED:  the carrier path is reachable only via an
 *                         explicit decompression / uncompressed
 *                         fallback before export.
 */
enum terakan_carrier_support {
   TERAKAN_CARRIER_SURFACE_SYNC_ALLOWED = 0,
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
 * acquired / gpu_dirty track the carrier lifetime:
 * UNBOUND -> IMPORTED -> ACQUIRED -> GPU_READING/WRITING ->
 * GPU_DIRTY -> RELEASED.
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

#endif /* TERAKAN_DMABUF_CARRIER_H */
