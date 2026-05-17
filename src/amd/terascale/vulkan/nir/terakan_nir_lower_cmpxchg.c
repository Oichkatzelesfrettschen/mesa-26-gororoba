/*
 * Palm (Wrestler GPU, CHIP_PALM, Evergreen / TeraScale-2 VLIW5) does not
 * execute cached MEM_RAT CMPXCHG as a real compare-and-conditional-write
 * operation.  Per AMD Evergreen-Family ISA, MEM_RAT_CACHELESS excludes
 * atomics while cached MEM_RAT lists CMPXCHG_INT and CMPXCHG_INT_RTN.
 *
 * This pass can rewrite selected compare-and-swap intrinsics to a
 * load-plus-atomic-xchg sequence:
 *
 *   old = atomic_cmpxchg(buf, addr, compare, replacement)
 *   cur = load(buf, addr)
 *   take = (cur == compare) ? replacement : cur
 *   old = atomic_xchg(buf, addr, take)
 *
 * That sequence is valid only for non-racing probe shapes.  It is guarded by
 * TERAKAN_EXPERIMENTAL_SPECULATIVE_CMPXCHG so normal shaders keep atomic
 * compare-and-swap semantics instead of replacing them with a non-atomic
 * read-modify-write window.
 */

#include "terakan_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_intrinsics.h"

#include "util/u_debug.h"

#include <stdbool.h>

static bool
speculative_cmpxchg_enabled(void)
{
   return debug_get_bool_option("TERAKAN_EXPERIMENTAL_SPECULATIVE_CMPXCHG", false);
}

static bool
should_emulate(nir_intrinsic_instr const * intr)
{
   if (!speculative_cmpxchg_enabled()) {
      return false;
   }

   if (intr->intrinsic != nir_intrinsic_ssbo_atomic_swap &&
       intr->intrinsic != nir_intrinsic_image_deref_atomic_swap &&
       intr->intrinsic != nir_intrinsic_global_atomic_swap) {
      return false;
   }
   if (nir_intrinsic_atomic_op(intr) != nir_atomic_op_cmpxchg) {
      return false;
   }
   /* The MEM_RAT CMPXCHG path only carries 32-bit operands on
    * Evergreen / TeraScale-2 VLIW5. */
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
