/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_R2VB_REINGEST_STATE_H
#define R300_R2VB_REINGEST_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "r300_reg.h"

/* The position-output-format and vertex-assembly gates share the
 * position-only output-format packet. */
static inline bool
r300_r2vb_position_only_output_enabled(bool output_format_gate,
                                       bool assembly_gate)
{
   return output_format_gate || assembly_gate;
}

static inline unsigned
r300_r2vb_position_only_output_dwords(bool output_format_gate,
                                      bool assembly_gate)
{
   return r300_r2vb_position_only_output_enabled(output_format_gate,
                                                 assembly_gate)
             ? 3
             : 0;
}

static inline unsigned
r300_r2vb_position_only_assembly_dwords(bool assembly_gate)
{
   return assembly_gate ? 3 : 0;
}

/* VTE W0 gate emits one VTE override and one same-IB restore packet. */
static inline unsigned
r300_r2vb_vte_w0_dwords(bool w0_gate)
{
   return w0_gate ? 2 : 0;
}

static inline unsigned
r300_r2vb_vte_restore_dwords(bool w0_gate)
{
   return w0_gate ? 2 : 0;
}

static inline uint32_t
r300_r2vb_position_only_output_fmt0(void)
{
   return R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT;
}

static inline uint32_t
r300_r2vb_position_only_output_fmt1(void)
{
   return 0;
}

static inline uint32_t
r300_r2vb_position_only_vtx_state_cntl(void)
{
   return 0;
}

static inline uint32_t
r300_r2vb_position_only_vtx_assm(void)
{
   return R300_INPUT_CNTL_POS;
}

static inline bool
r300_r2vb_position_only_output_matches(uint32_t fmt0, uint32_t fmt1)
{
   return fmt0 == r300_r2vb_position_only_output_fmt0() &&
          fmt1 == r300_r2vb_position_only_output_fmt1();
}

#endif
