/* SPDX-License-Identifier: MIT */
/*
 * terakan_dmabuf_carrier
 *
 * Closed carrier-domain policy for Palm (Wrestler GPU, CHIP_PALM,
 * Evergreen / TeraScale-2 VLIW5) dma-buf synchronization.  Adding or
 * promoting a domain requires updating enum terakan_carrier_domain,
 * updating this table, and retaining probe-backed hardware evidence.
 *
 * The reason strings are intentionally specific enough to be
 * grep-friendly in user-visible error reports.
 */

#include "terakan_dmabuf_carrier.h"

#include "terakan_device.h"

#include "util/log.h"
#include "util/macros.h"

static const struct terakan_carrier_policy palm_carrier_policy[] = {
   [TERAKAN_CARRIER_DOMAIN_BUFFER] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_BUFFER,
      .support = TERAKAN_CARRIER_SURFACE_SYNC_ALLOWED,
      .reason  = "buffer carrier has no image compression/modifier state; "
                 "CP_COHER_CNTL TC/VC invalidate over [va, size] is sufficient",
   },

   [TERAKAN_CARRIER_DOMAIN_CB_COLOR] = {
      .domain  = TERAKAN_CARRIER_DOMAIN_CB_COLOR,
      .support = TERAKAN_CARRIER_SURFACE_SYNC_ALLOWED,
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
      .reason  = "global compare-exchange semantics are not linearizable on "
                 "Palm; RAT cacheless path cannot be exposed as carrier-safe",
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
   case TERAKAN_CARRIER_SURFACE_SYNC_ALLOWED:  return "surface_sync_allowed";
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
