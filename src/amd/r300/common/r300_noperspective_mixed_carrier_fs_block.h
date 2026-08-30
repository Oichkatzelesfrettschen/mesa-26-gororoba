/*
 * SPDX-License-Identifier: MIT
 *
 * Mixed Smooth/NoPerspective carrier US block for the mixed reciprocal carrier cell,
 * baked by r300_tcl_bypass_fs_tool --emit=noperspective-mixed-carrier; the paired
 * --check=noperspective-mixed-carrier meson test proves this file regenerates from
 * source.
 */

#ifndef R300_NOPERSPECTIVE_MIXED_CARRIER_FS_BLOCK_H
#define R300_NOPERSPECTIVE_MIXED_CARRIER_FS_BLOCK_H

#include <stdint.h>

#define R300_NOPERSPECTIVE_MIXED_CARRIER_FS_FG_DEPTH_SRC 0x00000000u
#define R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_OUT_W 0x00000000u
#define R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_ALU_INSTRUCTIONS 4u
#define R300_NOPERSPECTIVE_MIXED_CARRIER_FS_US_TEMPORARIES 3u

static const uint32_t r300_noperspective_mixed_carrier_fs_block[] = {
   0x00001180, 0x00000000, 0x00001181, 0x00000002,
   0x00001182, 0x000000c0, 0x00031184, 0x00000000,
   0x00000000, 0x00000000, 0x004000c0, 0x00031230,
   0x02804000, 0x00050600, 0x02804081, 0x02804000,
   0x000311b0, 0x01800080, 0x01840001, 0x02000001,
   0x1c000000, 0x00031270, 0x05000003, 0x00000000,
   0x01800081, 0x01800489, 0x000311f0, 0x00800000,
   0x00000000, 0x00800000, 0x01000000, 0x000012f6,
   0x00000000, 0x000011ad, 0x00000000,
};

#endif /* R300_NOPERSPECTIVE_MIXED_CARRIER_FS_BLOCK_H */
