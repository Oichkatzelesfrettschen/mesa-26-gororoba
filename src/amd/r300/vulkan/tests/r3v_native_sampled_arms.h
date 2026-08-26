/*
 * SPDX-License-Identifier: MIT
 *
 * The attended sampling arms: one texture shape per parameter the
 * sampling route has to address.  The arming runner emits each arm's
 * cell for its digest and the attended runner drives the same shape
 * through the public surface, so both read this one table and an arming
 * report names the stream the submission carries.
 */

#ifndef R3V_NATIVE_SAMPLED_ARMS_H
#define R3V_NATIVE_SAMPLED_ARMS_H

#include "amd/r300/common/r300_tcl_bypass_triangle.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct r3v_sampled_arm {
   const char *name;
   /* Memory lane order of the texels; FORMAT1's selects route it. */
   enum r300_triangle_lane_order lanes;
   /* VK_IMAGE_TYPE_1D, the height-one member of the same layout. */
   bool one_dimensional;
   uint32_t width;
   uint32_t height;
   uint32_t array_layers;
   /* The layer the view selects; its stride joins TX_OFFSET_0. */
   uint32_t view_layer;
   /* The upper half of every layer holds the upper texel and the lower
    * half the lower one, so two oracle pixels separate an addressed
    * fetch from a constant one.
    */
   bool split_rows;
   /* Every unselected layer holds the lower texel, so a dropped layer
    * stride reads a value the oracle names in advance.
    */
   bool decoy_layers;
};

static const struct r3v_sampled_arm r3v_sampled_arms[] = {
   { "rgba", R300_TRIANGLE_LANES_R8G8B8A8, false, 16, 16, 1, 0, false, false },
   { "bgra", R300_TRIANGLE_LANES_B8G8R8A8, false, 16, 16, 1, 0, false, false },
   { "rows", R300_TRIANGLE_LANES_R8G8B8A8, false, 16, 16, 1, 0, true, false },
   { "wide", R300_TRIANGLE_LANES_R8G8B8A8, false, 256, 256, 1, 0, true,
     false },
   { "layer", R300_TRIANGLE_LANES_R8G8B8A8, false, 16, 16, 3, 2, false, true },
   { "row1", R300_TRIANGLE_LANES_R8G8B8A8, true, 16, 1, 1, 0, false, false },
};

#define R3V_SAMPLED_ARM_COUNT \
   (sizeof(r3v_sampled_arms) / sizeof(r3v_sampled_arms[0]))

/* The sampling family's row pitch rounds the row to 64 bytes
 * (r3v_native_transfer_row_pitch_bytes).  The attended runner compares
 * this against the layout the driver reports before it arms, so the two
 * expressions of the rule stay one contract.
 */
static inline uint32_t
r3v_sampled_arm_row_pitch_texels(const struct r3v_sampled_arm *arm)
{
   return ((arm->width * 4u + 63u) & ~63u) / 4u;
}

static inline uint32_t
r3v_sampled_arm_layer_pitch_bytes(const struct r3v_sampled_arm *arm)
{
   return r3v_sampled_arm_row_pitch_texels(arm) * 4u * arm->height;
}

static inline uint32_t
r3v_sampled_arm_texture_offset(const struct r3v_sampled_arm *arm)
{
   return arm->view_layer * r3v_sampled_arm_layer_pitch_bytes(arm);
}

static inline const struct r3v_sampled_arm *
r3v_sampled_arm_find(const char *name)
{
   for (size_t i = 0; i < R3V_SAMPLED_ARM_COUNT; i++) {
      if (strcmp(name, r3v_sampled_arms[i].name) == 0)
         return &r3v_sampled_arms[i];
   }
   return NULL;
}

#endif /* R3V_NATIVE_SAMPLED_ARMS_H */
