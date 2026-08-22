/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CARRIER_FORMAT_PIPE_H
#define R300_CARRIER_FORMAT_PIPE_H

#include "amd/r300/common/r300_carrier_policy.h"
#include "util/format/u_formats.h"

/* Gallium adapter only.  The common carrier policy carries no pipe_format
 * dependency; this mapping is the complete translation of its format
 * vocabulary into Gallium's public format identity. */
static inline enum pipe_format
r300_carrier_format_to_pipe(enum r300_carrier_format format)
{
   switch (format) {
   case R300_CARRIER_FORMAT_R8G8B8A8_UNORM:
      return PIPE_FORMAT_R8G8B8A8_UNORM;
   case R300_CARRIER_FORMAT_R32_FLOAT:
      return PIPE_FORMAT_R32_FLOAT;
   case R300_CARRIER_FORMAT_R32G32_FLOAT:
      return PIPE_FORMAT_R32G32_FLOAT;
   case R300_CARRIER_FORMAT_R32G32B32A32_FLOAT:
      return PIPE_FORMAT_R32G32B32A32_FLOAT;
   default:
      return PIPE_FORMAT_NONE;
   }
}

#endif /* R300_CARRIER_FORMAT_PIPE_H */
