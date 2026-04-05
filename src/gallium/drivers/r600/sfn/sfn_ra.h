/* -*- mesa-c++  -*-
 * Copyright 2022 Collabora LTD
 * Author: Gert Wollny <gert.wollny@collabora.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef INTERFERENCE_H
#define INTERFERENCE_H

#include "sfn_valuefactory.h"

namespace r600 {

class Shader;

bool
register_allocation(LiveRangeMap& lrm);

/* Spill-aware wrapper: retries RA after spilling longest live ranges.
 * Returns true if allocation succeeds (possibly after spilling).
 * Sets shader scratch_space_needed if any spills occurred. */
bool
register_allocation_with_spill(LiveRangeMap& lrm,
                                Shader& shader,
                                int max_spill_iterations = 10);

} // namespace r600

#endif // INTERFERENCE_H
