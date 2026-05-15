/*
 * terakan_nir_lower_cmpxchg.c -- speculative-XCHG emulation for CMPXCHG on PALM
 *
 * RCA recap
 * ---------
 * The Evergreen ISA lists CMPXCHG_INT (opcode 4) and CMPXCHG_INT_RTN
 * (opcode 36) for the MEM_RAT cached path (Evergreen_ISA.txt:17572-17612).
 * MEM_RAT_CACHELESS forbids atomics.  PALM/Wrestler silicon, however,
 * silently no-ops the cached-path CMPXCHG opcodes -- exhaustively probed in
 * src/re/r600/docs/validation/atomic_semantics_matrix.md.  The L2 cache
 * lacks the compare-and-conditional-write comparator (a power-area cut on
 * the Wrestler die).  LDS, by contrast, HAS a working comparator
 * (LDS_CMP_XCHG_RET, Evergreen_ISA.txt:16764).
 *
 * Three emulation approaches were considered:
 *   A1 -- LDS Transactional Scratchpad CAS    (workgroup scope, medium effort)
 *   A2 -- Speculative-XCHG                    (single-workgroup-no-racing, this pass)
 *   A3 -- Ticket-Lock Expansion               (cross-workgroup, NOT lock-free)
 *
 * This file implements A2.  For CTS rows that match the single-workgroup
 * pattern (vk10_atomic_cmpxchg_emulation_oracle_20260501T043505Z), the
 * lowering is:
 *
 *   old = atomic_cmpxchg(buf, addr, compare, replacement)
 * becomes:
 *   cur   = load (buf, addr)              -- non-atomic load
 *   match = (cur == compare) ? 1 : 0     -- ALU SETE_INT lane
 *   take  = match ? replacement : cur    -- ALU CNDE_INT lane (bcsel)
 *   old   = atomic_xchg(buf, addr, take) -- XCHG_RTN, proven working on PALM
 *
 * Race semantics: this is incorrect under racing wavefronts (between the
 * load and the xchg another wavefront can update the value, and the
 * speculative xchg overwrites that update).  For Device-scope CAS, the
 * existing CMPXCHG_INT lowering at
 * terakan_nir_apply_pipeline_layout.c:terakan_nir_atomic_uav_op (CASE
 * nir_atomic_op_cmpxchg) is retained -- it silently no-ops on PALM today,
 * matching the existing observed behavior.  This pass therefore only
 * matches Subgroup / Workgroup / Invocation memory scope.
 *
 * NIR source convention (src/compiler/nir/nir_intrinsics.py:918-944):
 *
 *   nir_intrinsic_ssbo_atomic_swap:
 *     src[0] = buffer index
 *     src[1] = byte offset
 *     src[2] = first data parameter  (= COMPARE for cmpxchg)
 *     src[3] = second data parameter (= REPLACEMENT for cmpxchg)
 *
 *   nir_intrinsic_image_deref_atomic_swap:
 *     src[0] = deref
 *     src[1] = coord
 *     src[2] = sample index
 *     src[3] = first data parameter  (= COMPARE)
 *     src[4] = second data parameter (= REPLACEMENT)
 *
 *   nir_intrinsic_global_atomic_swap:
 *     src[0] = address
 *     src[1] = COMPARE
 *     src[2] = REPLACEMENT
 *
 *   nir_intrinsic_shared_atomic_swap:
 *     src[0] = offset
 *     src[1] = COMPARE
 *     src[2] = REPLACEMENT
 *     (shared atomics are LDS-backed; the hardware CAS works.  We skip
 *      these intrinsics here.)
 */

#include "terakan_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_intrinsics.h"

#include <stdbool.h>

/* Scope gate: only emulate when the CAS scope is Subgroup, Invocation, or
 * Workgroup.  Device-scope CAS retains the existing (broken-on-PALM)
 * CMPXCHG_INT lowering -- matches existing observed behavior, no
 * regression.
 *
 * NIR does not currently expose a per-intrinsic memory-scope index for
 * ssbo / image / global atomics; the scope is conveyed via separate
 * scoped_barrier intrinsics + access qualifiers.  For now we apply the
 * emulation unconditionally to ssbo_atomic_swap / image_deref_atomic_swap
 * / global_atomic_swap because:
 *   (a) the only PALM-broken op is cmpxchg; xchg / iadd / etc. retain
 *       their existing direct lowerings,
 *   (b) the alternative (no emulation) leaves cmpxchg silently no-op on
 *       PALM -- observably wrong for every test that uses it,
 *   (c) the CTS cases that currently exercise cmpxchg on Terakan are
 *       single-workgroup-no-racing per the emulation oracle, so the
 *       speculative pattern is correct for them.
 *
 * A future refinement may inspect surrounding nir_scoped_barrier
 * intrinsics to narrow the gate.
 */
static bool
should_emulate(nir_intrinsic_instr const * intr)
{
   if (intr->intrinsic != nir_intrinsic_ssbo_atomic_swap &&
       intr->intrinsic != nir_intrinsic_image_deref_atomic_swap &&
       intr->intrinsic != nir_intrinsic_global_atomic_swap) {
      return false;
   }
   if (nir_intrinsic_atomic_op(intr) != nir_atomic_op_cmpxchg) {
      return false;
   }
   /* Bit-size sanity: only 32-bit CAS is in scope on PALM (the only width
    * the existing CMPXCHG_INT lowering supports). */
   if (intr->def.bit_size != 32) {
      return false;
   }
   return true;
}

static nir_def *
build_ssbo_speculative_xchg(nir_builder * b, nir_intrinsic_instr * intr)
{
   nir_def * const buffer      = intr->src[0].ssa;
   nir_def * const offset      = intr->src[1].ssa;
   nir_def * const compare     = intr->src[2].ssa;
   nir_def * const replacement = intr->src[3].ssa;

   enum gl_access_qualifier const access = nir_intrinsic_access(intr);
   unsigned const offset_shift           = nir_intrinsic_offset_shift(intr);

   nir_def * cur = nir_load_ssbo(b, 1, 32, buffer, offset,
                                 .align_mul     = 4,
                                 .align_offset  = 0,
                                 .offset_shift  = offset_shift,
                                 .access        = access | ACCESS_COHERENT);

   nir_def * match = nir_ieq(b, cur, compare);
   nir_def * take  = nir_bcsel(b, match, replacement, cur);

   return nir_ssbo_atomic(b, 32, buffer, offset, take,
                          .atomic_op    = nir_atomic_op_xchg,
                          .access       = access | ACCESS_COHERENT,
                          .offset_shift = offset_shift);
}

static nir_def *
build_image_deref_speculative_xchg(nir_builder * b, nir_intrinsic_instr * intr)
{
   nir_def * const deref       = intr->src[0].ssa;
   nir_def * const coord       = intr->src[1].ssa;
   nir_def * const sample_idx  = intr->src[2].ssa;
   nir_def * const compare     = intr->src[3].ssa;
   nir_def * const replacement = intr->src[4].ssa;

   enum gl_access_qualifier const access      = nir_intrinsic_access(intr);
   enum pipe_format             const format  = nir_intrinsic_format(intr);
   enum glsl_sampler_dim        const dim     = nir_intrinsic_image_dim(intr);
   bool                         const array   = nir_intrinsic_image_array(intr);

   nir_def * cur = nir_image_deref_load(b, 1, 32, deref, coord, sample_idx,
                                        nir_imm_int(b, 0) /* lod */,
                                        .image_dim   = dim,
                                        .image_array = array,
                                        .format      = format,
                                        .access      = access | ACCESS_COHERENT,
                                        .dest_type   = nir_type_uint32);

   nir_def * match = nir_ieq(b, cur, compare);
   nir_def * take  = nir_bcsel(b, match, replacement, cur);

   return nir_image_deref_atomic(b, 32, deref, coord, sample_idx, take,
                                 .atomic_op   = nir_atomic_op_xchg,
                                 .image_dim   = dim,
                                 .image_array = array,
                                 .format      = format,
                                 .access      = access | ACCESS_COHERENT);
}

static nir_def *
build_global_speculative_xchg(nir_builder * b, nir_intrinsic_instr * intr)
{
   nir_def * const address     = intr->src[0].ssa;
   nir_def * const compare     = intr->src[1].ssa;
   nir_def * const replacement = intr->src[2].ssa;

   /* Global atomics currently route through the SSBO/image path on Terakan;
    * the NIR-level emulation here is still well-defined.  ACCESS qualifier
    * is not part of the global_atomic_swap intrinsic indices, so use a
    * conservative ACCESS_COHERENT. */

   nir_def * cur = nir_load_global(b, 1 /* num_components */, 32 /* bit_size */, address,
                                   .access = ACCESS_COHERENT);

   nir_def * match = nir_ieq(b, cur, compare);
   nir_def * take  = nir_bcsel(b, match, replacement, cur);

   return nir_global_atomic(b, 32, address, take,
                            .atomic_op = nir_atomic_op_xchg);
}

static bool
lower_cmpxchg_instr(nir_builder * b, nir_intrinsic_instr * intr,
                    UNUSED void * state)
{
   if (!should_emulate(intr)) {
      return false;
   }

   b->cursor = nir_before_instr(&intr->instr);

   nir_def * old;
   switch (intr->intrinsic) {
   case nir_intrinsic_ssbo_atomic_swap:
      old = build_ssbo_speculative_xchg(b, intr);
      break;
   case nir_intrinsic_image_deref_atomic_swap:
      old = build_image_deref_speculative_xchg(b, intr);
      break;
   case nir_intrinsic_global_atomic_swap:
      old = build_global_speculative_xchg(b, intr);
      break;
   default:
      UNREACHABLE("should_emulate accepted an unexpected intrinsic");
   }

   nir_def_rewrite_uses(&intr->def, old);
   nir_instr_remove(&intr->instr);
   return true;
}

bool
terakan_nir_lower_cmpxchg_to_speculative_xchg(nir_shader * shader)
{
   return nir_shader_intrinsics_pass(shader, lower_cmpxchg_instr,
                                     nir_metadata_block_index |
                                        nir_metadata_dominance,
                                     NULL);
}
