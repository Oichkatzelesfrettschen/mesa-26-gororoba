/*
 * SPDX-License-Identifier: MIT
 *
 * Reciprocal-carrier US block for the NoPerspective carrier cell,
 * baked by r300_tcl_bypass_fs_tool --emit=noperspective-reciprocal; the paired
 * --check=noperspective-reciprocal meson test proves this file regenerates from
 * source.
 */

#ifndef R300_NOPERSPECTIVE_RECIPROCAL_FS_BLOCK_H
#define R300_NOPERSPECTIVE_RECIPROCAL_FS_BLOCK_H

#include <stdint.h>

#define R300_NOPERSPECTIVE_RECIPROCAL_FS_FG_DEPTH_SRC 0x00000000u
#define R300_NOPERSPECTIVE_RECIPROCAL_FS_US_OUT_W 0x00000000u

static const uint32_t r300_noperspective_reciprocal_fs_block[] = {
   0x00001180, 0x00000000, 0x00001181, 0x00000001,
   0x00001182, 0x00000080, 0x00031184, 0x00000000,
   0x00000000, 0x00000000, 0x00400080, 0x00021230,
   0x00004081, 0x00050680, 0x02804000, 0x000211b0,
   0x00000001, 0x03800000, 0x1c000000, 0x00021270,
   0x05000000, 0x00040509, 0x01800489, 0x000211f0,
   0x00840000, 0x00800040, 0x01000000, 0x000012f6,
   0x00000000, 0x000011ad, 0x00000000,
};

#endif /* R300_NOPERSPECTIVE_RECIPROCAL_FS_BLOCK_H */
