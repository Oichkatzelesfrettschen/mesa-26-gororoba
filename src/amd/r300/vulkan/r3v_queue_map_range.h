/*
 * SPDX-License-Identifier: MIT
 *
 * Gallium buffer-map range representability for R3V replay.
 */

#ifndef R3V_QUEUE_MAP_RANGE_H
#define R3V_QUEUE_MAP_RANGE_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* pipe_buffer_map_range forwards its unsigned range to u_box_1d(), whose
 * pipe_box x and width fields are int32_t.  Keep both fields positive and
 * representable before the r300 buffer transfer map adds the offset to its
 * CPU mapping. */
static inline bool
r3v_map_range_representable(uint64_t offset, uint64_t size,
                            uint64_t buffer_size)
{
   if (offset > INT32_MAX || size > INT32_MAX ||
       offset > UINT_MAX || size > UINT_MAX || size > buffer_size)
      return false;

   return offset <= buffer_size - size && offset <= UINT_MAX - size;
}

#endif /* R3V_QUEUE_MAP_RANGE_H */
