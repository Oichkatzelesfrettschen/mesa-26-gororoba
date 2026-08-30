/*
 * SPDX-License-Identifier: MIT
 *
 * Q-lane carrier US block for the NoPerspective q-lane cell,
 * baked by r300_tcl_bypass_fs_tool --emit=noperspective-q-lane; the paired
 * --check=noperspective-q-lane meson test proves this file regenerates from
 * source.
 */

#ifndef R300_NOPERSPECTIVE_Q_LANE_FS_BLOCK_H
#define R300_NOPERSPECTIVE_Q_LANE_FS_BLOCK_H

#include <stdint.h>

#define R300_NOPERSPECTIVE_Q_LANE_FS_FG_DEPTH_SRC 0x00000000u
#define R300_NOPERSPECTIVE_Q_LANE_FS_US_OUT_W 0x00000000u

static const uint32_t r300_noperspective_q_lane_fs_block[] = {
   0x00001180, 0x00000000, 0x00001181, 0x00000001,
   0x00001182, 0x000000c0, 0x00031184, 0x00000000,
   0x00000000, 0x00000000, 0x004000c0, 0x00031230,
   0x00004081, 0x00050600, 0x02804000, 0x02804000,
   0x000311b0, 0x00000000, 0x03800000, 0x03800000,
   0x1c000000, 0x00031270, 0x05000009, 0x00000000,
   0x00000000, 0x01800891, 0x000311f0, 0x00840000,
   0x00000001, 0x00000000, 0x01000000, 0x000012f6,
   0x00000000, 0x000011ad, 0x00000000,
};

#endif /* R300_NOPERSPECTIVE_Q_LANE_FS_BLOCK_H */
