/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include "r300_hb_tcl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r300_screen.h"
#include "r300_reg.h"

/* The default config must reproduce the historical TCL_BYPASS VAP_CNTL word
 * exactly, or a default screen would change the emitted register.  This is the
 * value the live RS482 readback shows. */
_Static_assert((R300_PVS_NUM_SLOTS(R300_HB_TCL_DEFAULT_NUM_SLOTS) |
                R300_PVS_NUM_CNTLRS(R300_HB_TCL_DEFAULT_NUM_CNTLRS) |
                R300_PVS_NUM_FPUS(R300_HB_TCL_DEFAULT_VERT_FPU) |
                R300_PVS_VF_MAX_VTX_NUM(R300_HB_TCL_DEFAULT_VF_MAX_VTX_NUM)) == 0x0014025au,
               "HB_TCL default VAP_CNTL drifted from the historical bypass word 0x0014025a");

/*
 * RC410 and RS480 are the RS48x integrated parts that expose the PVS control
 * surface without a usable hardware vertex FPU.  The hybrid-TCL probe is gated
 * to them so it cannot perturb a part that runs an ordinary route.
 */
static bool
r300_hb_tcl_is_hb_family(const struct r300_screen *screen)
{
   return screen->caps.family == CHIP_RC410 ||
          screen->caps.family == CHIP_RS480;
}

/*
 * Parse R300_HB_VERT_FPU into the PVS_NUM_FPUS field range.  Returns the
 * default on an absent, malformed, or out-of-range value so a typo keeps the
 * historical allocation instead of corrupting the register.  The accepted
 * range is the full 4-bit field [0, 15]: the previous [1, 31] guard let values
 * 16..31 shift set bits past bit 11 into the reserved span.
 */
static unsigned
r300_hb_tcl_parse_vert_fpu(void)
{
   const char *env = getenv("R300_HB_VERT_FPU");
   if (!env)
      return R300_HB_TCL_DEFAULT_VERT_FPU;

   char *end = NULL;
   long probe = strtol(env, &end, 0);

   if (end && *end == '\0' && probe >= 0 && probe <= (long)R300_HB_TCL_VERT_FPU_MAX)
      return (unsigned)probe;

   fprintf(stderr,
           "r300: ignoring R300_HB_VERT_FPU=%s; use an integer in [0, %u] "
           "(PVS_NUM_FPUS is a four-bit field)\n",
           env, R300_HB_TCL_VERT_FPU_MAX);
   return R300_HB_TCL_DEFAULT_VERT_FPU;
}

void
r300_hb_tcl_init(struct r300_screen *screen)
{
   struct r300_hb_tcl_config *cfg = &screen->hb_tcl;

   cfg->enabled = false;
   cfg->vert_fpu = R300_HB_TCL_DEFAULT_VERT_FPU;
   cfg->num_slots = R300_HB_TCL_DEFAULT_NUM_SLOTS;
   cfg->num_cntlrs = R300_HB_TCL_DEFAULT_NUM_CNTLRS;
   cfg->vf_max_vtx_num = R300_HB_TCL_DEFAULT_VF_MAX_VTX_NUM;

   const char *hb_tcl = getenv("R300_HB_TCL");
   if (screen->caps.has_tcl ||
       !r300_hb_tcl_is_hb_family(screen) ||
       !hb_tcl || strcmp(hb_tcl, "1") != 0)
      return;

   cfg->enabled = true;
   cfg->vert_fpu = r300_hb_tcl_parse_vert_fpu();

   /* Route force.  has_tcl selects the hardware-TCL path everywhere it is
    * tested: r300_create_context skips draw_create (no Gallium SW-TCL), the
    * vertex NIR options switch from gallivm to the r300 VS compiler,
    * r300_emit_vs_state uploads the PVS program and clears TCL_BYPASS, and the
    * draw dispatch uses the hardware-TCL primitive path.  num_vert_fpus drives
    * the PVS_NUM_FPUS that r300_emit_vs_state programs, so it moves with the
    * route or the emit would request zero FPUs.
    *
    * has_hardware_tcl is deliberately left as the chipset set it (false for
    * RS48x): that flag means the silicon is proven to execute PVS, which is the
    * open question this route exists to test, not something this assertion
    * grants.  The resulting has_tcl && !has_hardware_tcl state is the honest
    * "attempting hardware TCL on unproven silicon" configuration; the only
    * has_hardware_tcl readers gate on !has_tcl, so they stay inert here. */
   screen->caps.has_tcl = true;
   screen->caps.num_vert_fpus = cfg->vert_fpu;

   fprintf(stderr,
           "r300: RS48x HB_TCL route force on; has_tcl=true num_vert_fpus=%u "
           "(has_hardware_tcl stays %u; PVS execution is unproven and under "
           "test)\n",
           cfg->vert_fpu, screen->caps.has_hardware_tcl);
}

uint32_t
r300_hb_tcl_vap_cntl(const struct r300_hb_tcl_config *cfg)
{
   return R300_PVS_NUM_SLOTS(cfg->num_slots) |
          R300_PVS_NUM_CNTLRS(cfg->num_cntlrs) |
          R300_PVS_NUM_FPUS(cfg->vert_fpu) |
          R300_PVS_VF_MAX_VTX_NUM(cfg->vf_max_vtx_num);
}
