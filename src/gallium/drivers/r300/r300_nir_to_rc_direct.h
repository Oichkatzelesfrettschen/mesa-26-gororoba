/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Direct NIR->RC translation for r300, bypassing the TGSI intermediate.
 */

#ifndef R300_NIR_TO_RC_DIRECT_H
#define R300_NIR_TO_RC_DIRECT_H

#include <stdbool.h>

struct radeon_compiler;
struct nir_shader;
struct pipe_screen;
struct r300_fragment_program_external_state;
struct tgsi_shader_info;

/* Fill tgsi_shader_info from NIR variable metadata.  Used in place of
 * tgsi_scan_shader() on the PIPE_SHADER_IR_NIR path.  Only the fields
 * consumed by r300_shader_read_fs_inputs() and find_output_registers()
 * are populated: num_inputs/outputs, input/output_semantic_name/index,
 * and properties[FS_COLOR0_WRITES_ALL_CBUFS].
 */
void r300_nir_fill_shader_info(const struct nir_shader *s,
                               struct tgsi_shader_info *info);

/* Translate a NIR shader directly into the radeon_compiler RC instruction
 * list, bypassing the ureg/TGSI serialization round-trip.
 *
 * The caller must run all NIR lowering passes (r300_nir_lower_for_direct_rc
 * or the equivalent sequence from nir_to_rc()) before calling this function.
 * The compiler must be initialized with rc_init() before calling.
 *
 * TODO: verify output on RS482/RS485 hardware before removing the TGSI path.
 */
void r300_nir_to_rc_direct(struct radeon_compiler *compiler,
                            const struct nir_shader *s,
                            struct pipe_screen *screen,
                            struct r300_fragment_program_external_state state);

#endif /* R300_NIR_TO_RC_DIRECT_H */
