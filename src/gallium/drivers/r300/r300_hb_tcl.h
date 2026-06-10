/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_HB_TCL_H
#define R300_HB_TCL_H

#include <stdbool.h>
#include <stdint.h>

struct r300_screen;

/*
 * RS48x hybrid-TCL VAP resource configuration for the no-hardware-TCL path.
 *
 * A part with caps.has_tcl == false never calls r300_emit_vs_state(), so the
 * VAP front-end keeps a single static R300_VAP_CNTL written once at context
 * setup.  That word allocates the vertex-engine resources: slot, controller,
 * and FPU counts plus the max vertex number.  This descriptor owns those
 * values so they live in one validated place instead of an inline expression,
 * and so the experimental PVS_NUM_FPUS probe (R300_HB_VERT_FPU) is clamped to
 * the 4-bit field width before it can reach the register.
 *
 * vert_fpu is the PVS_NUM_FPUS field (R300_VAP_CNTL bits 11:8).  The field is
 * four bits wide, so the only legal values are 0..15; a probe outside that
 * range would shift set bits past bit 11 into the reserved span below
 * VF_MAX_VTX_NUM (bit 18) and corrupt the register.  The default is 2, which
 * reproduces the historical bypass value exactly.
 */
struct r300_hb_tcl_config {
   bool enabled;             /* R300_HB_TCL=1 took effect on an RS48x HB part */
   unsigned vert_fpu;        /* PVS_NUM_FPUS, clamped to [0, 15] */
   unsigned num_slots;       /* PVS_NUM_SLOTS */
   unsigned num_cntlrs;      /* PVS_NUM_CNTLRS */
   unsigned vf_max_vtx_num;  /* PVS_VF_MAX_VTX_NUM */
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
 * R300_HB_TCL=1 on an RC410/RS480 part without hardware TCL, mark it enabled
 * and fold in a validated R300_HB_VERT_FPU probe.  Reads getenv once at screen
 * create.  Safe to call on any screen: a part with hardware TCL keeps the
 * default config and the bypass word is never emitted.
 */
void r300_hb_tcl_init(struct r300_screen *screen);

/*
 * Assemble the R300_VAP_CNTL value the static TCL_BYPASS setup writes from a
 * validated config.  Returns exactly the historical word
 * (NUM_SLOTS(10)|NUM_CNTLRS(5)|NUM_FPUS(2)|VF_MAX_VTX_NUM(5) = 0x0014025a) for
 * the default config, so the bypass path is byte-identical unless a probe
 * changed vert_fpu.  That word is the value the live RS482 readback shows; the
 * VF_MAX_VTX_NUM=5 nibble is what fingerprints it as this bypass write rather
 * than the hardware-TCL emit path, which writes VF_MAX_VTX_NUM=12.
 */
uint32_t r300_hb_tcl_vap_cntl(const struct r300_hb_tcl_config *cfg);

#endif /* R300_HB_TCL_H */
