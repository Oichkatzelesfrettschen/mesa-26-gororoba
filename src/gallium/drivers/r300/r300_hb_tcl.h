/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_HB_TCL_H
#define R300_HB_TCL_H

#include <stdbool.h>
#include <stdint.h>

struct r300_screen;

/*
 * RS48x hybrid-TCL VAP resource configuration and route control.
 *
 * Two states.  When the gate is off (the default), caps.has_tcl stays false:
 * the part never calls r300_emit_vs_state(), and the VAP front-end keeps a
 * single static R300_VAP_CNTL written once at context setup.  This descriptor
 * owns that word's fields so they live in one validated place instead of an
 * inline expression, and so the PVS_NUM_FPUS probe is clamped before it can
 * reach the register.  The default config reproduces the historical bypass
 * word 0x0014025a exactly.
 *
 * When R300_HB_TCL=1 takes effect on an RS48x HB part without
 * R300_R2VB_TIMING, r300_hb_tcl_init forces caps.has_tcl = true and
 * caps.num_vert_fpus = vert_fpu: the driver then takes
 * the hardware-TCL route (no Gallium draw, r300 VS compiler, PVS upload,
 * TCL_BYPASS cleared) so the PVS upload and draw path become reachable.  A
 * live draw-correlation oracle on measured RS485M reached that path and timed
 * out at the first hardware-TCL draw before a fence signal or framebuffer
 * verdict.  That is a ring-wedge observation, not proof that PVS never
 * executes, so this route is an experimental harness, not a usable rendering
 * path.  caps.has_hardware_tcl is left false, which honestly reads
 * "attempting hardware TCL while PVS execution remains unproven."
 *
 * vert_fpu is the PVS_NUM_FPUS field (R300_VAP_CNTL bits 11:8) and the value
 * r300_emit_vs_state programs on the route.  The field is four bits wide, so
 * the only legal values are 0..15; a probe outside that range would shift set
 * bits past bit 11 into the reserved span below VF_MAX_VTX_NUM (bit 18) and
 * corrupt the register.  The default is 2.
 */
struct r300_hb_tcl_config {
   bool enabled;             /* R300_HB_TCL=1 forced the hardware-TCL route */
   unsigned vert_fpu;        /* PVS_NUM_FPUS / num_vert_fpus, clamped to [0, 15] */
   unsigned num_slots;       /* PVS_NUM_SLOTS (bypass word only) */
   unsigned num_cntlrs;      /* PVS_NUM_CNTLRS (bypass word only) */
   unsigned vf_max_vtx_num;  /* PVS_VF_MAX_VTX_NUM (bypass word only) */
};

/* PVS_NUM_FPUS is R300_VAP_CNTL bits 11:8: four bits, values 0..15. */
#define R300_HB_TCL_VERT_FPU_MAX 15u

/* The historical TCL_BYPASS VAP resource allocation (RS48x, no hardware TCL). */
#define R300_HB_TCL_DEFAULT_VERT_FPU       2u
#define R300_HB_TCL_DEFAULT_NUM_SLOTS      10u
#define R300_HB_TCL_DEFAULT_NUM_CNTLRS     5u
#define R300_HB_TCL_DEFAULT_VF_MAX_VTX_NUM 5u

/*
 * Populate screen->hb_tcl with the default bypass allocation, then, when
 * R300_HB_TCL=1 on an RC410/RS485M part without hardware TCL and
 * R300_R2VB_TIMING is unset, mark it enabled and fold in a validated
 * R300_HB_VERT_FPU probe.  R300_R2VB_TIMING reserves the no-TCL shape for the
 * R2VB packet self-test.  Reads getenv once at screen create.  Safe to call on
 * any screen: a part with hardware TCL keeps the default config and the
 * bypass word is never emitted.
 */
void r300_hb_tcl_init(struct r300_screen *screen);

/*
 * Assemble the R300_VAP_CNTL value the static TCL_BYPASS setup writes from a
 * validated config.  Returns exactly the historical word
 * (NUM_SLOTS(10)|NUM_CNTLRS(5)|NUM_FPUS(2)|VF_MAX_VTX_NUM(5) = 0x0014025a) for
 * the default config, so the bypass path is byte-identical unless a probe
 * changed vert_fpu.  That word is the value the live RS485M readback shows; the
 * VF_MAX_VTX_NUM=5 nibble is what fingerprints it as this bypass write rather
 * than the hardware-TCL emit path, which writes VF_MAX_VTX_NUM=12.
 */
uint32_t r300_hb_tcl_vap_cntl(const struct r300_hb_tcl_config *cfg);

#endif /* R300_HB_TCL_H */
