/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_HB_R400_US_H
#define R300_HB_R400_US_H

struct r300_screen;

/*
 * RS48x R400 fragment-shader (US) route control.
 *
 * The RS48x integrated parts are classified R300-class (caps.is_r400 false),
 * so the driver never programs R400_US_CODE_BANK / R400_US_CODE_EXT /
 * R400_US_ALU_EXT_ADDR_* and the fragment program is compiled to the R300
 * envelope (64 ALU, 32 TEX, 32 constants).  The RS482 MMIO census reads the
 * whole R400 US register footprint as present-and-responding on this silicon
 * (US_CODE_BANK retains written values), so whether the US block actually
 * executes R400 code banks is an open, testable question -- the same shape as
 * the R300_HB_TCL vertex route.
 *
 * Three states.  Off (the default): both caps stay false and every
 * fragment-shader path keys on caps.is_r400 exactly as before; the route adds
 * zero behavior.  R300_HB_R400_US=1 (emission probe): the command-stream emit
 * issues the R400_US_CODE_BANK/EXT writes an R400-class part always issues --
 * zero values for programs without code banks -- while programs still compile
 * to the R300 envelope, so a rendering change isolates the registers' effect.
 * R300_HB_R400_US=2 (full): also lift the fragment compiler to the R400
 * envelope (64 temps, 512 ALU/TEX slots, r390_mode code banks).
 * caps.is_r400 itself is left false in both: texture-format admission
 * (dxtc_swizzle, ATI2N) and the vertex-shader compiler keep their proven
 * R300-class configuration, so the route exposes only the US surface under
 * test.  Execution on RS48x silicon is unproven.
 */
void r300_hb_r400_us_init(struct r300_screen *screen);

#endif /* R300_HB_R400_US_H */
