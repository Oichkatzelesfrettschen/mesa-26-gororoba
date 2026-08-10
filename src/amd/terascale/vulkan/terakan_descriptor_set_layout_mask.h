/* SPDX-License-Identifier: MIT */

#ifndef TERAKAN_DESCRIPTOR_SET_LAYOUT_MASK_H
#define TERAKAN_DESCRIPTOR_SET_LAYOUT_MASK_H

#include <assert.h>
#include <limits.h>
#include <stdint.h>

/* Per-stage sampler occupancy uses uint32_t bitfields.  A full-width
 * binding therefore maps directly to UINT32_MAX; evaluating 1u << 32
 * has undefined behavior in C.  Pipeline-layout creation applies the
 * tighter TERAKAN_SAMPLER_HW_COUNT_PER_STAGE hardware limit.
 */
static inline uint32_t
terakan_descriptor_set_layout_sampler_mask(uint32_t descriptor_count)
{
   uint32_t const mask_bit_count = sizeof(uint32_t) * CHAR_BIT;
   if (descriptor_count >= mask_bit_count) {
      assert(descriptor_count == mask_bit_count);
      return UINT32_MAX;
   }

   return ((uint32_t)1 << descriptor_count) - 1u;
}

#endif
