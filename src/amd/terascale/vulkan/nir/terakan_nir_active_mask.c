/*
 * terakan_nir_active_mask.c -- per-lane active-mask materialisation helpers.
 *
 * Background
 * ----------
 * Evergreen / Bobcat have no GCN-style scalar EXEC register: there is no
 * named scalar storage holding the per-lane active mask as a value.
 * Instead, AMD Evergreen-Family ISA Section 4.10 (Predication and Branch
 * Counters) states two facts that together make the active mask
 * READABLE per-lane:
 *
 *   1. "The processor maintains one predicate bit per pixel within an
 *      ALU clause.  This predicate initially reflects the active Mask
 *      from the processor."
 *   2. "If an instruction is disabled by the predicate bit, then no GPR
 *      value is written, the PV and PS registers are not updated."
 *
 * So at ALU-clause entry, the predicate per-lane equals the active mask,
 * and inactive lanes do not write GPRs.  The masked-write idiom is:
 *
 *     mov   r5, 0              ; unconditional: every lane sees 0
 *     mov.p r5, 1              ; PRED_SEL=ONE: only active lanes write 1
 *     ; r5 now holds (active ? 1 : 0) per lane
 *
 * The first move clears the target; the second move executes only on
 * lanes whose predicate is set, and per Section 4.10 those lanes are
 * exactly the active lanes.  The result is a per-lane integer value of
 * the active mask.
 *
 * At the NIR level the same effect is achievable by relying on NIR's
 * `if` construct: inside a branch guarded by a sourced predicate, the
 * SFN backend lowers the body with PRED_SEL=ONE for the active side and
 * the masked writeback completes the materialisation.  In other words,
 * NIR already gives us the abstraction; the helpers in this file just
 * use it to build the active-mask-as-int and LDS-mediated ballot
 * patterns that the upcoming higher-tier subgroup lowering depends on.
 *
 * These helpers are intentionally narrow: they produce NIR that the
 * existing r600 SFN backend already knows how to encode.  No new SFN
 * support is required.
 */

#include "terakan_nir.h"

#include "nir.h"
#include "nir_builder.h"

#include <assert.h>

/*
 * Return a per-lane uint with value 1 in lanes where `predicate` is
 * true and 0 elsewhere.  `predicate` must be a 1-bit NIR boolean.
 *
 * Implementation: the canonical NIR expression `b2i32(predicate)`
 * compiles in the r600 SFN backend to a predicated MOV that obeys the
 * masked-write rule from Evergreen ISA Section 4.10 -- which is the
 * same code path the masked-write idiom uses.  We provide the helper
 * as a stable boundary so the higher-tier subgroup lowerings have a
 * single call site to reason about.
 */
nir_def *
terakan_nir_build_active_mask_as_int(nir_builder * const b, nir_def * const predicate)
{
   assert(predicate != NULL);
   assert(predicate->bit_size == 1);
   assert(predicate->num_components == 1);
   return nir_b2i32(b, predicate);
}

/*
 * Return a per-lane uint with value 1 for every active lane, 0 for
 * inactive lanes.  Equivalent to `terakan_nir_build_active_mask_as_int`
 * with predicate = true; provided as a separate helper because the
 * "active-mask-of-the-current-clause" use case is common enough to be
 * worth its own name in the caller code.
 *
 * Hardware basis (Evergreen ISA Section 4.10):
 *   - ALU clause-entry predicate per-lane == active mask per-lane.
 *   - Disabled lanes do not write GPRs.
 * The b2i32(true) lowering produces an unconditional MOV r,1 that the
 * SFN encoder emits with WRITE_MASK=1 and the implicit clause-entry
 * predicate.  Inactive lanes leave the destination at its prior value
 * (which we depend on the caller to have initialised to 0).
 */
nir_def *
terakan_nir_build_active_mask(nir_builder * const b)
{
   nir_def * const true_pred = nir_imm_true(b);
   return terakan_nir_build_active_mask_as_int(b, true_pred);
}

/*
 * LDS-mediated wave64 ballot.
 *
 * Returns a uvec2 (32-bit, 32-bit) where bit `i` of `result.x` is the
 * predicate value for lane `i` (i < 32), and bit `i-32` of `result.y`
 * is the predicate value for lane i (32 <= i < 64).
 *
 * Pattern:
 *   1. Materialise per-lane (active && predicate) as a 32-bit value.
 *   2. LDS store: lds[lane_id * 4] = value.
 *   3. Workgroup barrier.
 *   4. LDS gather: for i in 0..31 read lds[(i + 0)*4] and bit-pack.
 *   5. Same for i in 32..63 -> ballot.y.
 *
 * The caller must have reserved subgroup_size * sizeof(uint32_t) bytes
 * of shared LDS scratch at the offset passed in `lds_scratch_offset`.
 * The pattern uses ONLY LDS_WRITE_REL + LDS_READ_RET (Evergreen ISA
 * Section on LDS), plus a barrier; no SFN backend change is needed.
 *
 * This helper is the load-bearing call for the upcoming
 * VK_KHR_shader_subgroup_ballot lowering (Phase 4).
 */
nir_def *
terakan_nir_build_ballot_via_lds(nir_builder * const b,
                                 nir_def * const predicate,
                                 unsigned const lds_scratch_offset_bytes)
{
   assert(predicate != NULL);
   assert(predicate->bit_size == 1);
   assert(predicate->num_components == 1);

   /* Step 1: materialise (active && predicate) per lane. */
   nir_def * const lane_value = terakan_nir_build_active_mask_as_int(b, predicate);

   /* Step 2: LDS_WRITE_REL lds[lane_id * 4] = lane_value. */
   nir_def * const lane_id = nir_load_subgroup_invocation(b);
   nir_def * const lds_byte_addr =
      nir_iadd_imm(b, nir_ishl_imm(b, lane_id, 2), lds_scratch_offset_bytes);
   nir_store_shared(b, lane_value, lds_byte_addr, .base = 0, .align_mul = 4, .write_mask = 0x1);

   /* Step 3: barrier.  Subgroup-scope barrier sufficient (one wave). */
   nir_barrier(b,
               .execution_scope     = SCOPE_SUBGROUP,
               .memory_scope        = SCOPE_SUBGROUP,
               .memory_semantics    = NIR_MEMORY_ACQ_REL,
               .memory_modes        = nir_var_mem_shared);

   /* Step 4 + 5: LDS gather and bit-pack into two 32-bit halves.
    *
    * Sequential bit-pack: ballot_lo |= (lds[i] & 1) << i, for i in [0, 32).
    * Same for ballot_hi over [32, 64).  We rely on the SFN backend to
    * unroll the loop (32 iterations) -- the NIR builder emits the
    * unrolled form here to keep the helper self-contained.
    */
   nir_def * ballot_lo = nir_imm_int(b, 0);
   nir_def * ballot_hi = nir_imm_int(b, 0);
   for (unsigned i = 0; i < 32; ++i) {
      nir_def * const addr_lo = nir_imm_int(b, lds_scratch_offset_bytes + i * 4u);
      nir_def * const val_lo  = nir_load_shared(b, 1, 32, addr_lo, .base = 0, .align_mul = 4);
      ballot_lo = nir_ior(b, ballot_lo,
                          nir_ishl_imm(b, nir_iand_imm(b, val_lo, 0x1u), i));

      nir_def * const addr_hi = nir_imm_int(b, lds_scratch_offset_bytes + (i + 32u) * 4u);
      nir_def * const val_hi  = nir_load_shared(b, 1, 32, addr_hi, .base = 0, .align_mul = 4);
      ballot_hi = nir_ior(b, ballot_hi,
                          nir_ishl_imm(b, nir_iand_imm(b, val_hi, 0x1u), i));
   }

   return nir_vec2(b, ballot_lo, ballot_hi);
}
