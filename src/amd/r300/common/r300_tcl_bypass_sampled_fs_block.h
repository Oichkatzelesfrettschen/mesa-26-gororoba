/*
 * SPDX-License-Identifier: MIT
 *
 * Sampled-texture US block for the TCL-bypass sampling cell,
 * baked by r300_tcl_bypass_fs_tool --emit=sampled; the paired
 * --check=sampled meson test proves this file regenerates from
 * source.
 */

#ifndef R300_TCL_BYPASS_SAMPLED_FS_BLOCK_H
#define R300_TCL_BYPASS_SAMPLED_FS_BLOCK_H

#include <stdint.h>

#define R300_TCL_BYPASS_SAMPLED_FS_FG_DEPTH_SRC 0x00000000u
#define R300_TCL_BYPASS_SAMPLED_FS_US_OUT_W 0x00000000u

static const uint32_t r300_tcl_bypass_sampled_fs_block[] = {
   0x00001180, 0x00000008, 0x00001181, 0x00000000,
   0x00001182, 0x00000000, 0x00031184, 0x00000000,
   0x00000000, 0x00000000, 0x00400000, 0x00001230,
   0x02804000, 0x000011b0, 0x1c000000, 0x00001270,
   0x01800489, 0x000011f0, 0x01000000, 0x00001188,
   0x00008000, 0x000012f6, 0x00000000, 0x000011ad,
   0x00000000,
};

#endif /* R300_TCL_BYPASS_SAMPLED_FS_BLOCK_H */
