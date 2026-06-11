/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_hb_r400_us.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r300_screen.h"

/* The route is gated to the RS48x integrated parts: they expose the R400 US
 * register footprint without being classified R400-class, which is the
 * combination this probe exists to test.  Any other part keeps its ordinary
 * route untouched. */
static bool
r300_hb_r400_us_is_hb_family(const struct r300_screen *screen)
{
   return screen->caps.family == CHIP_RC410 ||
          screen->caps.family == CHIP_RS480;
}

void
r300_hb_r400_us_init(struct r300_screen *screen)
{
   screen->caps.hb_r400_us = false;
   screen->caps.hb_r400_us_envelope = false;

   const char *hb_us = getenv("R300_HB_R400_US");
   if (screen->caps.is_r400 ||
       !r300_hb_r400_us_is_hb_family(screen) ||
       !hb_us)
      return;

   /* Two rungs so the register-surface probe and the compiler envelope are
    * falsifiable independently.  "1": emission-only -- programs still compile
    * to the R300 envelope, the emit adds the R400_US_CODE_BANK/EXT writes an
    * R400-class part always issues (zero for non-code-bank programs), so a
    * rendering change isolates the registers' effect on this silicon.  "2":
    * also lift the fragment compiler to the R400 envelope (64 temps, 512
    * ALU/TEX, r390_mode code banks), which changes register allocation even
    * for small programs and engages bank switching for large ones. */
   if (strcmp(hb_us, "1") == 0) {
      screen->caps.hb_r400_us = true;
   } else if (strcmp(hb_us, "2") == 0) {
      screen->caps.hb_r400_us = true;
      screen->caps.hb_r400_us_envelope = true;
   } else {
      fprintf(stderr,
              "r300: ignoring R300_HB_R400_US=%s; use 1 (R400 US register "
              "emission probe) or 2 (emission + R400 fragment compiler "
              "envelope)\n", hb_us);
      return;
   }

   /* caps.is_r400 is deliberately left false: it also drives dxtc_swizzle,
    * ATI2N format admission, and the vertex-shader compiler class, none of
    * which this route puts under test.  Only the fragment-shader compile and
    * emit read the hb_r400_us flags, so the probe surface is exactly the US
    * block. */
   fprintf(stderr,
           "r300: RS48x HB_R400_US route force on (emission%s); the US block "
           "is treated as R400-class; execution on this silicon is unproven "
           "and under test\n",
           screen->caps.hb_r400_us_envelope ? " + compiler envelope" : "");
}
