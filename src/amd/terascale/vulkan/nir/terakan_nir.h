/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef TERAKAN_NIR_H
#define TERAKAN_NIR_H

#include "terakan_pipeline_layout.h"

#include "util/bitset.h"
#include "nir.h"
#include "nir_builder.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emits a buffer resource load intrinsic for a buffer resource with a stride of 1 producing a NIR
 * vector.
 * Supports 1, 2 and 4 components for 8 and 16 bits, and 1, 2, 3 and 4 components for 32 bits.
 * Vectorization is expected to be done externally before lowering to this intrinsic if needed,
 * taking alignment and robustness requirements into account.
 */
nir_def * terakan_nir_load_raw_resource_buffer(nir_builder * b, unsigned num_components,
                                               unsigned bit_size, enum gl_access_qualifier access,
                                               unsigned resource_index_base,
                                               nir_def * resource_index, unsigned byte_address_base,
                                               nir_def * byte_address);

/* Compact fragment shader data output locations, so all RTVs precede all UAVs as required and to be
 * able to allocate arrays of UAVs without fragmentation, and so there are no holes in
 * CB_SHADER_MASK, which, according to RadeonSI (but apparently applicable to earlier generations),
 * may cause hangs.
 * The mask of which pre-compaction fragment data outputs are used is written to
 * uncompacted_locations_out.
 */
bool terakan_nir_compact_fragment_data_locations(nir_shader * shader,
                                                 uint8_t * uncompacted_locations_out);

/* Translate Vulkan set and binding indices and push constant loads into hardware constant indices,
 * and lower various instructions involving bindings into TeraScale-specific intrinsics and logic.
 *
 * Works with instructions generated from nir_lower_explicit_io.
 *
 * It's recommended to run nir_opt_load_store_vectorize before the pass, as this transforms certain
 * loads and stores into hardware-specific intrinsics.
 *
 * The function adds new bits to `resources_needed_accum` and `samplers_needed_accum`, but keeps
 * already set ones.
 *
 * `uavs_for_mutable_resources_needed_out_opt`, however, is overwritten completely (more precisely,
 * bitset words for the maximum count of mutable resources at the stage are touched), as UAV indices
 * are prefix sums, and thus new bits can't be set after this lowering, and it also may be omitted
 * by the caller if UAVs are not needed (such as if the stage doesn't support UAVs).
 */
/*
 * robust_buffer_access: if true, inject software ALU umin bounds clamps
 * for storage-buffer / texel-buffer access.  This is NON-NEGOTIABLE on
 * Terascale silicon — the hardware does not provide reliable OOB
 * guarantees for UAV writes or byte-granular texture fetches.  The caller
 * must compute the effective flag from the device feature plus any
 * per-pipeline VK_EXT_pipeline_robustness state.  See
 * terakan_nir_buffer_uav_coord() for the clamp implementation.
 */
bool terakan_nir_lower_bindings(nir_shader * shader, struct terakan_pipeline_layout const * layout,
                                BITSET_WORD * resources_needed_accum,
                                uint32_t * samplers_needed_accum, unsigned uav_base,
                                BITSET_WORD * uavs_for_mutable_resources_needed_out_opt,
                                uint32_t * driver_push_constants_used_accum,
                                uint16_t * kcache_needed_accum_out,
                                bool robust_buffer_access);

bool terakan_nir_lower_sin_cos(nir_shader * shader);

/* Speculative-XCHG emulation for atomic compare-and-swap on Evergreen/PALM.
 *
 * The hardware CMPXCHG_INT (RAT_INST opcode 4) and CMPXCHG_INT_RTN
 * (opcode 36) are listed in the Evergreen ISA but silently no-op on PALM
 * silicon (the L2 cache lacks the compare-and-conditional-write
 * comparator).  This pass rewrites supported CAS intrinsics
 * (ssbo_atomic_swap, image_deref_atomic_swap, global_atomic_swap, all with
 * atomic_op == cmpxchg and bit_size == 32) to a three-instruction
 * sequence:
 *     cur = load (...)
 *     take = (cur == compare) ? replacement : cur
 *     old = atomic_xchg(..., take)
 * which is correct under single-workgroup-no-racing CTS test patterns
 * (vk10_atomic_cmpxchg_emulation_oracle_20260501T043505Z).  Device-scope
 * CAS retains the existing CMPXCHG_INT lowering (silently no-ops on
 * PALM today, matching observed behavior).
 *
 * Must run BEFORE terakan_nir_lower_bindings so the lowered xchg flows
 * through the standard atomic UAV path. */
bool terakan_nir_lower_cmpxchg_to_speculative_xchg(nir_shader * shader);

/* LDS-emulated cross-lane subgroup primitives for chips without native
 * cross-lane ALU (Palm / Wrestler -- CHIP_PALM, Evergreen / TeraScale-2
 * VLIW5).  Lowers nir_intrinsic_ballot, nir_intrinsic_read_first_invocation,
 * and the broader VOTE/REDUCE family (decomposed by nir_lower_subgroups
 * into the foundational primitives) to LDS shared-memory atomic-OR /
 * write-broadcast sequences with workgroup barriers.
 *
 * Must run BEFORE nir_lower_subgroups so the ballot/read_first_invocation
 * primitives this pass needs as decomposition targets remain visible to
 * NIR's decomposer.  Allocates LDS bytes by bumping
 * shader->info.shared_size; existing shader-declared shared memory is
 * preserved at the same offsets it already occupies (the pass appends).
 *
 * Compute and kernel stages only -- subgroup ops are advertised in
 * terakan_physical_device.c with subgroupSupportedStages =
 * VK_SHADER_STAGE_COMPUTE_BIT. */
bool terakan_nir_lower_subgroup_lds(nir_shader * shader);

/* Active-mask materialisation helpers (terakan_nir_active_mask.c).
 *
 * Per AMD Evergreen-Family ISA Section 4.10, the per-lane predicate
 * at ALU-clause entry equals the active mask, and disabled lanes do
 * not write GPRs.  These helpers emit NIR that the SFN backend
 * lowers to that masked-write idiom, materialising the active mask
 * as a per-lane integer value.  Used by the LDS-mediated subgroup
 * primitives (terakan_nir_lower_subgroup_lds) and any other call
 * site that needs to gather across lanes without a scalar EXEC. */
nir_def * terakan_nir_build_active_mask_as_int(nir_builder * b, nir_def * predicate);
nir_def * terakan_nir_build_active_mask(nir_builder * b);
nir_def * terakan_nir_build_ballot_via_lds(nir_builder * b, nir_def * predicate,
                                           unsigned lds_scratch_offset_bytes);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_NIR_H */
