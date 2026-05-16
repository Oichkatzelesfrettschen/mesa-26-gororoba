/* SPDX-License-Identifier: MIT */
/*
 * terakan_dmabuf_carrier
 *
 * Phase 0 carrier-model implementation.  Policy table + lookup
 * helpers + a debug dump.  No carrier construction is wired here;
 * import / acquire / release / destroy land in subsequent commits.
 *
 * The closed policy table below is the single authoritative
 * statement of which carrier domains PALM supports for external
 * sync.  Adding or promoting a domain requires:
 *
 *   1. updating enum terakan_carrier_domain (header), and
 *   2. updating this table, and
 *   3. landing a finding-doc on the steinmarder side under
 *      src/re/r600/findings/active/ that empirically backs the
 *      promotion via a probe in tools/workspace/x130e-kit/staged/
 *      radeon-perf-probe/.
 *
 * The reason strings are intentionally specific enough to be
 * grep-friendly in user-visible error reports.
 */

#include "terakan_dmabuf_carrier.h"

#include "terakan_bo.h"
#include "terakan_device.h"

#include "vk_alloc.h"

#include "util/log.h"
#include "util/macros.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const struct terakan_carrier_policy palm_carrier_policy[] = {
   [TERAKAN_CARRIER_DOMAIN_BUFFER] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_BUFFER,
      .support = TERAKAN_CARRIER_ALLOWED_PHASE0,
      .reason  = "buffer carrier has no image compression/modifier state; "
                 "CP_COHER_CNTL TC/VC invalidate over [va, size] is sufficient",
   },

   [TERAKAN_CARRIER_DOMAIN_CB_COLOR] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_CB_COLOR,
      .support = TERAKAN_CARRIER_ALLOWED_PHASE0,
      .reason  = "CB color path uses the documented PFP/ME SURFACE_SYNC + "
                 "WAIT_REG_MEM + EOP timestamp release proof",
   },

   [TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_LINEAR_IMAGE,
      .support = TERAKAN_CARRIER_PENDING_PROBE,
      .reason  = "pending P4 linear-image carrier probe; row not yet promoted "
                 "in the cache-domain evidence matrix",
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
      .reason  = "no tiled/modifier-equivalence proof on PALM; refuse direct "
                 "tiled dma-buf imports until probe-backed",
   },

   [TERAKAN_CARRIER_DOMAIN_SX_EXPORT] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_SX_EXPORT,
      .support = TERAKAN_CARRIER_PENDING_PROBE,
      .reason  = "SX memory export not yet probed for external carrier "
                 "release; SX_SURFACE_SYNC bits unverified on PALM externally",
   },

   [TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_CACHELESS_RAT,
      .support = TERAKAN_CARRIER_FORBIDDEN,
      .reason  = "global compare-exchange semantics are not linearizable on "
                 "PALM; RAT cacheless path cannot be exposed as carrier-safe",
   },
};

/* `STATIC_ASSERT` from util/macros.h is the project-canonical form;
 * it expands to a do/while wrapper compatible with file-scope use
 * when placed inside any function that the compiler will instantiate
 * with carrier-policy access.  Instead of doing that, use a one-off
 * compile-time sanity helper triggered from the log-dump function
 * below. */

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
   case TERAKAN_CARRIER_ALLOWED_PHASE0:        return "allowed_phase0";
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

bool
terakan_dmabuf_carrier_enabled(void)
{
   /* Closed env-gate: only the exact string "1" enables the carrier
    * path.  Avoid the 1/true/yes/on parsing variants so misspellings
    * in shell scripts fail loud instead of silently turning the
    * feature on. */
   const char * const env = getenv("TERAKAN_ENABLE_DMABUF_CARRIER");
   return env != NULL && strcmp(env, "1") == 0;
}

VkResult
terakan_dmabuf_carrier_import(struct terakan_device * const                    device,
                              const struct terakan_dmabuf_carrier_desc * const desc,
                              struct terakan_dmabuf_carrier ** const            out)
{
   *out = NULL;

   if (desc == NULL || desc->dmabuf_fd < 0)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   const struct terakan_carrier_policy * const policy =
      terakan_dmabuf_carrier_policy(desc->domain);
   if (policy == NULL)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;

   if (policy->support != TERAKAN_CARRIER_ALLOWED_PHASE0) {
      /* Closed-policy rejection.  The reason string is keyed to the
       * carrier-path cache-domain evidence matrix; downstream tools
       * grep these strings to map error reports back to matrix rows. */
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

   /* Duplicate the caller-supplied fd so the carrier owns its own
    * reference, independent of the caller's lifetime. */
   const int dup_fd = dup(desc->dmabuf_fd);
   if (dup_fd < 0) {
      vk_free(&device->vk.alloc, carrier);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   carrier->dmabuf_fd = dup_fd;

   /* Use the winsys PRIME-import contract.  We do not have a
    * carrier-level VRAM preference; default to VRAM-preferred when
    * the BUFFER carrier is the import target (device-local makes
    * surface-sync simpler), false otherwise. */
   const bool prefer_vram =
      (desc->domain == TERAKAN_CARRIER_DOMAIN_BUFFER);

   const VkResult bo_result = device->winsys_fn->bo->import_fd(
      device, carrier->dmabuf_fd, (VkDeviceSize)desc->size_bytes,
      prefer_vram, NULL, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT,
      &carrier->bo);
   if (bo_result != VK_SUCCESS) {
      terakan_dmabuf_carrier_destroy(device, carrier);
      return bo_result;
   }

   carrier->gpu_va  = carrier->bo->va;
   carrier->imported = true;

   *out = carrier;
   return VK_SUCCESS;
}

void
terakan_dmabuf_carrier_destroy(struct terakan_device * const          device,
                               struct terakan_dmabuf_carrier * const  carrier)
{
   if (carrier == NULL)
      return;

   if (carrier->bo != NULL)
      terakan_bo_free(carrier->bo, NULL);

   if (carrier->release_sync_fd >= 0)
      close(carrier->release_sync_fd);
   if (carrier->acquire_sync_fd >= 0)
      close(carrier->acquire_sync_fd);
   if (carrier->dmabuf_fd >= 0)
      close(carrier->dmabuf_fd);

   vk_free(&device->vk.alloc, carrier);
}
