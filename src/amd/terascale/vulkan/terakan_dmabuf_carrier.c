/* SPDX-License-Identifier: MIT */
/*
 * terakan_dmabuf_carrier
 *
 * Carrier-model implementation for Palm (Wrestler GPU, CHIP_PALM,
 * Evergreen / TeraScale-2 VLIW5).  Policy table + lookup helpers +
 * a debug dump + the import/attach/destroy entry points.
 *
 * The closed policy table is the single authoritative statement of
 * which carrier domains Palm supports for external sync.  Adding or
 * promoting a domain requires updating both `enum
 * terakan_carrier_domain` and this table; the reason strings are
 * intentionally specific enough to grep in user-visible error
 * reports.
 */

#include "terakan_dmabuf_carrier.h"

#include "terakan_bo.h"
#include "terakan_device.h"

#include "vk_alloc.h"

#include "util/log.h"
#include "util/macros.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct terakan_carrier_policy palm_carrier_policy[] = {
   [TERAKAN_CARRIER_DOMAIN_BUFFER] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_BUFFER,
      .support = TERAKAN_CARRIER_ALLOWED,
      .reason  = "buffer carrier has no image compression/modifier state; "
                 "CP_COHER_CNTL TC/VC invalidate over [va, size] is sufficient",
   },

   [TERAKAN_CARRIER_DOMAIN_CB_COLOR] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_CB_COLOR,
      .support = TERAKAN_CARRIER_ALLOWED,
      .reason  = "CB color path uses the documented PFP/ME SURFACE_SYNC + "
                 "WAIT_REG_MEM + EOP timestamp release proof",
   },

   [TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE,
      .support = TERAKAN_CARRIER_PENDING_PROBE,
      .reason  = "linear-image carrier not yet empirically backed on Palm; "
                 "refuse direct linear dma-buf imports until probe-backed",
   },

   [TERAKAN_CARRIER_DOMAIN_COMPRESSED_DEPTH] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_COMPRESSED_DEPTH,
      .support = TERAKAN_CARRIER_DECOMPRESS_REQUIRED,
      .reason  = "HTILE metadata not directly probed externally; depth carriers "
                 "must transition through an uncompressed buffer on export",
   },

   [TERAKAN_CARRIER_DOMAIN_TILED_IMAGE] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_TILED_IMAGE,
      .support = TERAKAN_CARRIER_PENDING_PROBE,
      .reason  = "no tiled/modifier-equivalence proof on Palm; refuse direct "
                 "tiled dma-buf imports until probe-backed",
   },

   [TERAKAN_CARRIER_DOMAIN_SX_EXPORT] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_SX_EXPORT,
      .support = TERAKAN_CARRIER_PENDING_PROBE,
      .reason  = "SX memory export not yet probed for external carrier "
                 "release; SX_SURFACE_SYNC bits unverified on Palm externally",
   },

   [TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT,
      .support = TERAKAN_CARRIER_FORBIDDEN,
      .reason  = "Palm native global CMPXCHG silently no-ops the "
                 "conditional write at the L2 cache (silicon power-area "
                 "cut on the Wrestler die); AMD Evergreen-Family ISA "
                 "MEM_RAT_CACHELESS forbids atomics; speculative-XCHG "
                 "rewrite is the only working compare-and-swap surface "
                 "on CHIP_PALM and is gated by "
                 "TERAKAN_PALM_CMPXCHG_POLICY -- env-unset default is "
                 "cts_speculative on CHIP_PALM (deployed) and "
                 "native_noop_legacy on every other Terakan chip "
                 "family (CHIP_CYPRESS, CHIP_JUNIPER, CHIP_REDWOOD, "
                 "CHIP_CEDAR, CHIP_SUMO, CHIP_SUMO2, CHIP_CAYMAN, "
                 "CHIP_ARUBA) whose comparators are wired up correctly",
   },
};

const struct terakan_carrier_policy *
terakan_dmabuf_carrier_policy(enum terakan_carrier_domain const domain)
{
   if ((unsigned)domain >= (unsigned)TERAKAN_CARRIER_DOMAIN_COUNT)
      return NULL;
   return &palm_carrier_policy[domain];
}

const char *
terakan_carrier_domain_name(enum terakan_carrier_domain const domain)
{
   switch (domain) {
   case TERAKAN_CARRIER_DOMAIN_BUFFER:            return "buffer";
   case TERAKAN_CARRIER_DOMAIN_CB_COLOR:          return "cb_color";
   case TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE:      return "linear_image";
   case TERAKAN_CARRIER_DOMAIN_COMPRESSED_DEPTH:  return "compressed_depth";
   case TERAKAN_CARRIER_DOMAIN_TILED_IMAGE:       return "tiled_image";
   case TERAKAN_CARRIER_DOMAIN_SX_EXPORT:         return "sx_export";
   case TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT:     return "cacheless_rat";
   case TERAKAN_CARRIER_DOMAIN_COUNT:             break;
   }
   return "unknown";
}

const char *
terakan_carrier_support_name(enum terakan_carrier_support const support)
{
   switch (support) {
   case TERAKAN_CARRIER_SUPPORT_INVALID:       return "invalid";
   case TERAKAN_CARRIER_ALLOWED:               return "allowed_phase0";
   case TERAKAN_CARRIER_PENDING_PROBE:         return "pending_probe";
   case TERAKAN_CARRIER_FORBIDDEN:             return "forbidden";
   case TERAKAN_CARRIER_DECOMPRESS_REQUIRED:   return "decompress_required";
   }
   return "unknown";
}

void
terakan_dmabuf_carrier_log_policy(UNUSED struct terakan_device * const device)
{
   STATIC_ASSERT(
      sizeof(palm_carrier_policy) / sizeof(palm_carrier_policy[0]) ==
         (size_t)TERAKAN_CARRIER_DOMAIN_COUNT);

   for (unsigned i = 0; i < (unsigned)TERAKAN_CARRIER_DOMAIN_COUNT; i++) {
      const struct terakan_carrier_policy * const p =
         &palm_carrier_policy[i];
      mesa_logi("terakan: carrier policy: domain=%-18s support=%-22s reason=%s",
                terakan_carrier_domain_name(p->domain),
                terakan_carrier_support_name(p->support),
                p->reason);
   }
}

/*
 * Env-gate parsing.  Closed: only the exact string "1" matches.  The
 * 1/true/yes/on parsing variants are intentionally not accepted so
 * shell-script misspellings fail loud instead of silently flipping
 * the gate.
 *
 * The IMPORT side defaults ON because foreign-producer dma-buf import
 * is dormant on workloads that never exercise it (vkmark-class
 * synthetic benchmarks); per-frame attach overhead is zero in that
 * regime.  TERAKAN_DISABLE_DMABUF_CARRIER_IMPORT=1 is the emergency
 * kill switch.
 *
 * The EXPORT side defaults OFF because per-frame attach overhead is
 * non-zero on trivial-shader workloads where attach amortizes poorly
 * (Palm vkmark clear scene measured 678 FPS gate-off vs 490 FPS with
 * export attach forced on).  TERAKAN_ENABLE_DMABUF_CARRIER_EXPORT=1
 * opts in for the cases where an external consumer actually imports
 * the exported fd.
 *
 * TERAKAN_ENABLE_DMABUF_CARRIER=1 is the legacy master switch and
 * forces both sides on; this preserves the existing probe/test
 * invocation contract that gates the whole subsystem on a single
 * variable.
 */
static bool
env_eq_one(const char *name)
{
   const char * const env = getenv(name);
   return env != NULL && strcmp(env, "1") == 0;
}

bool
terakan_dmabuf_carrier_import_enabled(void)
{
   if (env_eq_one("TERAKAN_ENABLE_DMABUF_CARRIER"))
      return true;
   if (env_eq_one("TERAKAN_DISABLE_DMABUF_CARRIER_IMPORT"))
      return false;
   return true;
}

bool
terakan_dmabuf_carrier_export_enabled(void)
{
   if (env_eq_one("TERAKAN_ENABLE_DMABUF_CARRIER"))
      return true;
   return env_eq_one("TERAKAN_ENABLE_DMABUF_CARRIER_EXPORT");
}

bool
terakan_dmabuf_carrier_enabled(void)
{
   return terakan_dmabuf_carrier_import_enabled() ||
          terakan_dmabuf_carrier_export_enabled();
}

/*
 * Shared classify-and-allocate helper for both attach and import.
 *
 * Returns VK_SUCCESS with *out_carrier populated (everything except
 * the BO pointer and owns_bo) when the descriptor classifies as
 * ALLOWED.  Returns VK_ERROR_FEATURE_NOT_PRESENT on any other
 * verdict, after logging the policy reason.  Returns
 * VK_ERROR_INVALID_EXTERNAL_HANDLE on a malformed descriptor and
 * VK_ERROR_OUT_OF_HOST_MEMORY on allocation failure.
 */
static VkResult
carrier_classify_and_alloc(struct terakan_device * const                    device,
                           const struct terakan_dmabuf_carrier_desc * const desc,
                           struct terakan_dmabuf_carrier ** const           out_carrier)
{
   *out_carrier = NULL;

   /* fd validity is checked by terakan_dmabuf_carrier_import() before
    * it dup()s; the attach path legitimately has no fd at descriptor
    * time (fd populated lazily by vkGetMemoryFdKHR on the export side
    * OR carried by the caller's VkDeviceMemory on the import side). */
   if (desc == NULL)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   const struct terakan_carrier_policy * const policy =
      terakan_dmabuf_carrier_policy(desc->domain);
   if (policy == NULL)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   if (policy->support != TERAKAN_CARRIER_ALLOWED) {
      /* Closed-policy rejection.  The reason string maps the
       * carrier domain to a documented hardware constraint;
       * downstream tooling greps these strings to map error
       * reports back to the policy row. */
      mesa_loge("terakan: dmabuf carrier import rejected: "
                "domain=%s support=%s reason=%s",
                terakan_carrier_domain_name(policy->domain),
                terakan_carrier_support_name(policy->support),
                policy->reason);
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   struct terakan_dmabuf_carrier * const carrier =
      vk_zalloc(&device->vk.alloc, sizeof(*carrier), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (carrier == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   carrier->dmabuf_fd       = -1;
   carrier->acquire_sync_fd = -1;
   carrier->release_sync_fd = -1;
   carrier->domain          = policy->domain;
   carrier->support         = policy->support;
   carrier->size_bytes      = desc->size_bytes;
   carrier->stride          = desc->stride;
   carrier->format          = desc->format;
   carrier->tiling_mode     = desc->tiling_mode;
   carrier->usage_mask      = desc->usage_mask;
   carrier->linear_only     =
      (desc->domain == TERAKAN_CARRIER_DOMAIN_BUFFER);

   *out_carrier = carrier;
   return VK_SUCCESS;
}

VkResult
terakan_dmabuf_carrier_attach(struct terakan_device * const                    device,
                              const struct terakan_dmabuf_carrier_desc * const desc,
                              struct terakan_bo * const                        existing_bo,
                              struct terakan_dmabuf_carrier ** const           out)
{
   if (existing_bo == NULL) {
      *out = NULL;
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   struct terakan_dmabuf_carrier *carrier = NULL;
   VkResult result = carrier_classify_and_alloc(device, desc, &carrier);
   if (result != VK_SUCCESS) {
      *out = NULL;
      return result;
   }

   /* Attach path: bind to a BO whose lifetime is owned by the
    * caller (typically a terakan_device_memory).  The carrier
    * holds a non-owning pointer; destroy MUST NOT free this BO. */
   carrier->bo       = existing_bo;
   carrier->owns_bo  = false;
   carrier->gpu_va   = existing_bo->va;
   carrier->imported = true;

   /* Defensive: the BO must not already have a carrier published.
    * Double-attach without intervening destroy is a caller bug. */
   struct terakan_dmabuf_carrier * const previous =
      atomic_load_explicit(&existing_bo->carrier, memory_order_acquire);
   assert(previous == NULL && "BO already has a published carrier");
   if (previous != NULL) {
      vk_free(&device->vk.alloc, carrier);
      *out = NULL;
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   atomic_store_explicit(&existing_bo->carrier, carrier,
                         memory_order_release);

   *out = carrier;
   return VK_SUCCESS;
}

VkResult
terakan_dmabuf_carrier_import(struct terakan_device * const                    device,
                              const struct terakan_dmabuf_carrier_desc * const desc,
                              struct terakan_dmabuf_carrier ** const           out)
{
   /* The import path requires a real fd to dup(); reject malformed
    * descriptors up front so dup() never sees -1.  The shared
    * classify-and-alloc helper leaves this check to the entry point
    * because the attach path has no fd by construction. */
   if (desc == NULL || desc->dmabuf_fd < 0) {
      *out = NULL;
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   struct terakan_dmabuf_carrier *carrier = NULL;
   VkResult result = carrier_classify_and_alloc(device, desc, &carrier);
   if (result != VK_SUCCESS) {
      *out = NULL;
      return result;
   }

   /* Duplicate the caller-supplied fd so the carrier owns its own
    * reference, independent of the caller's lifetime. */
   const int dup_fd = dup(desc->dmabuf_fd);
   if (dup_fd < 0) {
      vk_free(&device->vk.alloc, carrier);
      *out = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   carrier->dmabuf_fd = dup_fd;

   /* Use the winsys PRIME-import contract.  Default to VRAM-
    * preferred for the BUFFER carrier (device-local makes
    * surface-sync simpler); false otherwise. */
   const bool prefer_vram =
      (desc->domain == TERAKAN_CARRIER_DOMAIN_BUFFER);

   const VkResult bo_result = device->winsys_fn->bo->import_fd(
      device, carrier->dmabuf_fd, (VkDeviceSize)desc->size_bytes,
      prefer_vram, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
      &carrier->bo);
   if (bo_result != VK_SUCCESS) {
      /* Tear down the carrier; we own neither a BO nor a published
       * BO->carrier pointer yet, so destroy is a pure free. */
      if (carrier->dmabuf_fd >= 0)
         close(carrier->dmabuf_fd);
      vk_free(&device->vk.alloc, carrier);
      *out = NULL;
      return bo_result;
   }

   carrier->owns_bo  = true;
   carrier->gpu_va   = carrier->bo->va;
   carrier->imported = true;

   /* Publish the carrier on the freshly-imported BO so future
    * queue-submit walks find the carrier from either direction. */
   atomic_store_explicit(&carrier->bo->carrier, carrier,
                         memory_order_release);

   *out = carrier;
   return VK_SUCCESS;
}

void
terakan_dmabuf_carrier_destroy(struct terakan_device * const          device,
                               struct terakan_dmabuf_carrier * const  carrier)
{
   if (carrier == NULL)
      return;

   /* Clear the BO's atomic carrier pointer first so any concurrent
    * queue-submit walk observes "no carrier" before we tear down
    * the carrier's fields. */
   if (carrier->bo != NULL) {
      atomic_store_explicit(&carrier->bo->carrier,
                            (struct terakan_dmabuf_carrier *)NULL,
                            memory_order_release);
   }

   if (carrier->release_sync_fd >= 0)
      close(carrier->release_sync_fd);
   if (carrier->acquire_sync_fd >= 0)
      close(carrier->acquire_sync_fd);
   if (carrier->dmabuf_fd >= 0)
      close(carrier->dmabuf_fd);

   /* Only the import() path owns the BO.  The attach() path binds
    * to a BO whose lifetime is owned by terakan_device_memory or
    * another caller; freeing it here would double-free. */
   if (carrier->owns_bo && carrier->bo != NULL)
      terakan_bo_free(carrier->bo, NULL);

   vk_free(&device->vk.alloc, carrier);
}
