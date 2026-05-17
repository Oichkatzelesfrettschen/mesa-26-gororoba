/* SPDX-License-Identifier: MIT */
/*
 * terakan_dmabuf_carrier
 *
 * Carrier model for the Terakan external-sync path on Palm
 * (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5).
 *
 * Defines the closed policy table that gates which carrier domains
 * are externally sync-correct on Palm, the carrier struct itself,
 * and the import + attach + destroy entry points.
 *
 * Policy is intentionally CLOSED: every dma-buf import classifies
 * into one of the seven domains below, and every domain has an
 * explicit support verdict.  Adding a new domain or promoting a
 * pending-probe domain to allowed requires updating both the enum
 * and the policy table.  See the AMD R6xx/R7xx 3D Engine Programming
 * Guide (SURFACE_SYNC, CB_COLOR0_INFO, CP_COHER_CNTL) and the
 * AMD Evergreen-Family ISA reference for the underlying hardware
 * constraints; cache-domain coverage notes live in Terakan's
 * companion docs, not in source comments.
 */

#ifndef TERAKAN_DMABUF_CARRIER_H
#define TERAKAN_DMABUF_CARRIER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan_core.h>

struct terakan_bo;
struct terakan_device;

/*
 * Carrier-domain taxonomy.
 *
 * BUFFER and CB_COLOR are the only domains the Palm build emits
 * surface-sync packets for.  The remaining five MUST hit one of the
 * reject paths during import.
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
 * Support verdict per domain.
 *
 * INVALID is the explicit zero-initialised sentinel.  A
 * `terakan_carrier_policy` row read from `{0}`-initialised memory
 * MUST classify as INVALID so accidental zero-init does not silently
 * masquerade as ALLOWED.
 *
 * - ALLOWED:               carrier path is empirically backed on
 *                          Palm and produces a usable carrier.
 * - PENDING_PROBE:         documented by AMD, not yet backed on
 *                          Palm; reject at import.
 * - FORBIDDEN:             explicitly disallowed (e.g. global CAS
 *                          semantics are not linearizable on Palm).
 * - DECOMPRESS_REQUIRED:   reachable only via an explicit
 *                          uncompressed-fallback before export.
 */
enum terakan_carrier_support {
   TERAKAN_CARRIER_SUPPORT_INVALID = 0,
   TERAKAN_CARRIER_ALLOWED,
   TERAKAN_CARRIER_PENDING_PROBE,
   TERAKAN_CARRIER_FORBIDDEN,
   TERAKAN_CARRIER_DECOMPRESS_REQUIRED,
};

struct terakan_carrier_policy {
   enum terakan_carrier_domain  domain;
   enum terakan_carrier_support support;
   /* Human-readable reason emitted in debug logs and any
    * VK_ERROR_FEATURE_NOT_PRESENT return path. */
   const char *                 reason;
};

/*
 * The carrier itself.
 *
 * gpu_va + size_bytes define the [va, va+size) range that
 * SURFACE_SYNC addresses via COHER_BASE / COHER_SIZE (each in
 * 256-byte units in the [39:8] field per the AMD Evergreen-Family
 * 3D Registers Reference, surface-sync section).
 *
 * owns_bo distinguishes the two construction paths:
 *   - terakan_dmabuf_carrier_import() allocates a fresh BO via the
 *     winsys PRIME-import contract; owns_bo = true, destroy frees
 *     the BO.
 *   - terakan_dmabuf_carrier_attach() binds to an existing BO
 *     already owned by a terakan_device_memory; owns_bo = false,
 *     destroy MUST NOT free the BO.
 *
 * acquired / gpu_dirty track the (UNBOUND -> IMPORTED -> ACQUIRED
 * -> GPU_READING/WRITING -> GPU_DIRTY -> RELEASED) state machine.
 */
struct terakan_dmabuf_carrier {
   int                              dmabuf_fd;

   struct terakan_bo *              bo;
   bool                             owns_bo;
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
 * per-domain policy entry, or NULL if the domain is out of range.
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
 * Env-gate helpers for the dma-buf carrier subsystem.
 *
 * The import side (foreign-producer dma-buf brought in via
 * VkImportMemoryFdInfoKHR with handleType DMA_BUF_BIT_EXT) defaults
 * ON.  Empirical per-frame attach cost on the import side is zero on
 * Palm because vkmark-class workloads never exercise it; the path
 * stays dormant unless an actual foreign producer hands us an fd.
 * Kill switch TERAKAN_DISABLE_DMABUF_CARRIER_IMPORT=1 is provided for
 * emergency disablement.
 *
 * The export side (locally allocated VkDeviceMemory whose
 * export_handle_types contains DMA_BUF_BIT_EXT) defaults OFF because
 * per-frame attach cost on trivial-shader workloads is measurable
 * (Palm vkmark clear scene: 678 FPS gate-off, 490 FPS export-attach
 * on).  Opt in via TERAKAN_ENABLE_DMABUF_CARRIER_EXPORT=1 when an
 * external consumer actually imports the exported fd.
 *
 * Backwards-compatible master switch TERAKAN_ENABLE_DMABUF_CARRIER=1
 * forces BOTH sides on regardless of the individual gates, preserving
 * the probe/test invocation contract that predates the split.
 *
 * Parsing rule: exact string "1".  Misspellings fail loud rather than
 * silently activating or deactivating the path.
 */
bool
terakan_dmabuf_carrier_import_enabled(void);

bool
terakan_dmabuf_carrier_export_enabled(void);

/*
 * Returns true when either the import or the export gate is enabled.
 * Call sites that walk per-BO published-carrier state without knowing
 * which side produced the carrier (queue collector, device-init log
 * dump) gate on this aggregate so they remain active whenever any
 * carrier can have been published on the device.
 */
bool
terakan_dmabuf_carrier_enabled(void);

/*
 * Caller-provided descriptor for an import or attach attempt.
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
 * Attach a carrier to an existing BO owned by the caller (typically
 * a terakan_device_memory's BO).  Does NOT call the winsys
 * import_fd path and does NOT dup the caller's dma-buf fd; the
 * caller already holds those references via its own owning struct.
 *
 * On ALLOWED the carrier is allocated, populated with `existing_bo`
 * as its non-owning BO pointer (carrier->owns_bo = false), and the
 * BO's atomic carrier pointer is published via release-store.  On
 * any other support verdict the function returns
 * VK_ERROR_FEATURE_NOT_PRESENT and *out remains NULL.
 *
 * The BO's atomic carrier field MUST be NULL at entry; attaching
 * twice without an intervening destroy is a caller bug and asserts
 * in debug builds.
 */
VkResult
terakan_dmabuf_carrier_attach(struct terakan_device *                    device,
                              const struct terakan_dmabuf_carrier_desc * desc,
                              struct terakan_bo *                        existing_bo,
                              struct terakan_dmabuf_carrier **           out);

/*
 * Import a dma-buf as a Terakan carrier without a pre-existing BO.
 * Allocates a fresh BO via the winsys PRIME-import contract,
 * publishes the carrier on it, and sets owns_bo = true so destroy
 * frees the BO.
 *
 * Prefer terakan_dmabuf_carrier_attach() when the dma-buf is
 * already imported into a terakan_device_memory; double-importing
 * the same dma-buf into two distinct terakan_bo wrappers is a
 * carrier-association soundness bug.
 */
VkResult
terakan_dmabuf_carrier_import(struct terakan_device *                    device,
                              const struct terakan_dmabuf_carrier_desc * desc,
                              struct terakan_dmabuf_carrier **           out);

/*
 * Release any resources the carrier holds: clear the BO's atomic
 * carrier pointer (release-store of NULL), close carrier-owned sync
 * FDs, free the BO only when owns_bo is true, then free the carrier
 * itself.  Safe to call on a partially-constructed carrier
 * (NULL-tolerant; tolerates fields left at zero/-1).
 */
void
terakan_dmabuf_carrier_destroy(struct terakan_device *         device,
                               struct terakan_dmabuf_carrier * carrier);

#endif /* TERAKAN_DMABUF_CARRIER_H */
