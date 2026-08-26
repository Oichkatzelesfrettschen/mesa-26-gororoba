/*
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_CARRIER_SUBMIT_SCRATCH_H
#define TERAKAN_CARRIER_SUBMIT_SCRATCH_H

#include "vk_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_carrier_submit_scratch {
   uint32_t * dwords;
   size_t capacity_dwords;
};

static inline void
terakan_carrier_submit_scratch_init(struct terakan_carrier_submit_scratch * const scratch)
{
   scratch->dwords = NULL;
   scratch->capacity_dwords = 0;
}

static inline bool
terakan_carrier_submit_scratch_ensure(struct terakan_carrier_submit_scratch * const scratch,
                                      VkAllocationCallbacks const * const allocator,
                                      size_t const required_dwords)
{
   if (required_dwords <= scratch->capacity_dwords)
      return true;

   if (required_dwords > SIZE_MAX / sizeof(*scratch->dwords))
      return false;

   uint32_t * const new_dwords =
      (uint32_t *)vk_alloc(allocator, required_dwords * sizeof(*scratch->dwords), alignof(uint32_t),
                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (new_dwords == NULL)
      return false;

   vk_free(allocator, scratch->dwords);
   scratch->dwords = new_dwords;
   scratch->capacity_dwords = required_dwords;
   return true;
}

static inline void
terakan_carrier_submit_scratch_finish(struct terakan_carrier_submit_scratch * const scratch,
                                      VkAllocationCallbacks const * const allocator)
{
   vk_free(allocator, scratch->dwords);
   scratch->dwords = NULL;
   scratch->capacity_dwords = 0;
}

#ifdef __cplusplus
}
#endif

#endif
