// SPDX-License-Identifier: MIT
#ifndef R300_PUBLIC_H
#define R300_PUBLIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct radeon_winsys;
struct pipe_screen_config;

struct pipe_screen* r300_screen_create(struct radeon_winsys *rws,
                                       const struct pipe_screen_config *config);

/*
 * ICD direct-emit extraction API.
 *
 * These functions allow a Vulkan ICD (r300vk) to retrieve pre-compiled
 * hardware machine code from opaque Gallium CSO handles without depending
 * on r300g private headers.  The private-struct knowledge stays inside r300g;
 * only value-type descriptors cross the public boundary.
 *
 * All returned pointers alias r300g's internal storage and are valid until
 * the CSO is destroyed via delete_*_state.  Callers must not free or modify
 * the data behind returned pointers.
 *
 * r300_vs_get_hw_code: fills *out from a CSO handle returned by
 *   create_vs_state().  Returns false if vs_cso is NULL, if compilation
 *   failed (dummy shader), or if the VS runs through SW-TCL (no hw code).
 *
 * r300_fs_get_hw_code: fills *out from a CSO handle returned by
 *   create_fs_state().  cb_code is the full pre-baked PM4 sequence for the
 *   US_* / FG_* register block; emit it verbatim at draw time.
 *   Returns false if fs_cso is NULL, compilation failed, or cb_code is NULL.
 */

#define R300_VS_MAX_FC_OPS_PUBLIC 16

struct r300_vs_hw_code {
   const uint32_t *dwords;           /* body.d -- hw instruction words */
   unsigned        length;           /* dword count */
   uint32_t        inputs_read;      /* InputsRead bitmask */
   uint32_t        outputs_written;  /* OutputsWritten bitmask */
   int             last_pos_write;
   int             last_input_read;
   int             num_temporaries;
   int             pos_end;
   unsigned        num_fc_ops;
   uint32_t        fc_ops;
   const uint32_t *fc_op_addrs_r300; /* [R300_VS_MAX_FC_OPS_PUBLIC] */
   const int32_t  *fc_loop_index;    /* [R300_VS_MAX_FC_OPS_PUBLIC] */
};

struct r300_fs_hw_code {
   const uint32_t *cb_code;      /* pre-baked PM4 register writes */
   unsigned        cb_code_size; /* dword count */
   uint32_t        fg_depth_src; /* R300_FG_DEPTH_SRC (0x4bd8) */
   uint32_t        us_out_w;     /* R300_US_W_FMT (0x46b4) */
};

bool r300_vs_get_hw_code(void *vs_cso, struct r300_vs_hw_code *out);
bool r300_fs_get_hw_code(void *fs_cso, struct r300_fs_hw_code *out);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
