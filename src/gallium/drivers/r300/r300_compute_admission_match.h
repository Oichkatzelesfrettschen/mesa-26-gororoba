/*
 * SPDX-License-Identifier: MIT
 *
 * Structural intrinsic-shape matchers shared across the compute-as-raster
 * pattern detectors in r300_compute_admission.c.  Each detector
 * (r300_nir_detect_*) recognizes one kernel shape by walking the NIR for its
 * load_ssbo / store_ssbo / atomic skeleton; these helpers are the skeleton
 * walk every detector reuses, factored out so the detector bodies read as the
 * shape they match rather than the bookkeeping that finds it.
 *
 * Kept header-only (static inline) so the detectors can be split across
 * translation units later without exporting these symbols from libr300 or
 * tripping -Werror=unused-function in a unit that matches only one shape.  They
 * call NIR core (and each other) but nothing from the detector bodies, so the
 * dependency runs one way: detectors -> matchers, never back.
 */

#ifndef R300_COMPUTE_ADMISSION_MATCH_H
#define R300_COMPUTE_ADMISSION_MATCH_H

#include "compiler/nir/nir.h"
#include "util/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True for a scatter the substrate cannot do.  A plain store_ssbo is NOT here:
 * the ComputeGrid->RasterGrid functor maps the kernel's buffer-output write to
 * a coordinate-indexed RB3D export, so it is the expected admissible output.
 * Storage-image stores and raw global-pointer stores are arbitrary scatter
 * beyond a single RT coordinate and have no lowering. */
static inline bool
is_rw_storage_store(nir_intrinsic_op op)
{
   switch (op) {
   case nir_intrinsic_store_global:
   case nir_intrinsic_store_global_2x32:
   case nir_intrinsic_image_store:
   case nir_intrinsic_image_deref_store:
   case nir_intrinsic_bindless_image_store:
      return true;
   default:
      return false;
   }
}

/* SSBO atomics are two NIR intrinsics: ssbo_atomic (the load-op-store forms --
 * add/min/max/and/or/xor/exchange) and ssbo_atomic_swap (compare-and-swap).
 * Every detector here either rejects all atomics or recognizes exactly one
 * ssbo_atomic of a named op, so both must count toward the atomic tally;
 * omitting the swap form would let a kernel carrying a compare-and-swap slip
 * past a "no atomic" or "exactly one atomic" gate. */
static inline bool
is_ssbo_atomic(nir_intrinsic_op op)
{
   return op == nir_intrinsic_ssbo_atomic ||
          op == nir_intrinsic_ssbo_atomic_swap;
}

static inline bool
intrinsic_base_type_is_float(const nir_intrinsic_instr *intr,
                             nir_alu_type fallback_type)
{
   nir_alu_type type = fallback_type;

   if (nir_intrinsic_has_src_type(intr))
      type = nir_intrinsic_src_type(intr);
   else if (nir_intrinsic_has_dest_type(intr))
      type = nir_intrinsic_dest_type(intr);

   /* nir_type_invalid carries no base type; treat an absent type as non-float
    * rather than feeding nir_type_invalid into nir_alu_type_get_base_type. */
   if (type == nir_type_invalid)
      return false;
   return nir_alu_type_get_base_type(type) == nir_type_float;
}

/* Collect the load_ssbo and store_ssbo intrinsics of a kernel in program order,
 * with the no-loop / no-if / no-atomic checks the octonion elementwise ops share.
 * loads[]/stores[] are filled up to their caps; the counts report the totals so
 * the caller can require an exact shape. */
static inline void
collect_loads_stores(const nir_shader *s,
                     const nir_intrinsic_instr **loads, unsigned max_loads,
                     unsigned *nload,
                     const nir_intrinsic_instr **stores, unsigned max_stores,
                     unsigned *nstore, unsigned *natomic,
                     bool *has_loop, bool *in_if)
{
   *nload = *nstore = *natomic = 0;
   *has_loop = *in_if = false;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               *has_loop = true;
            if (p->type == nir_cf_node_if)
               *in_if = true;
         }
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               if (*nstore < max_stores)
                  stores[*nstore] = intr;
               (*nstore)++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               if (*nload < max_loads)
                  loads[*nload] = intr;
               (*nload)++;
            } else if (is_ssbo_atomic(intr->intrinsic)) {
               (*natomic)++;
            }
         }
      }
   }
}

static inline bool
store_is_full_width(const nir_intrinsic_instr *st)
{
   if (!st->src[0].ssa)
      return false;
   return !nir_intrinsic_has_write_mask(st) ||
          nir_intrinsic_write_mask(st) == BITFIELD_MASK(st->num_components);
}

#ifdef __cplusplus
}
#endif

#endif /* R300_COMPUTE_ADMISSION_MATCH_H */
