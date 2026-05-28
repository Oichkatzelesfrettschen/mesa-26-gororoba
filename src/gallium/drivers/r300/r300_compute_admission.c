/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 *
 * Classify-only admission for compute kernels against the RS482/RS485
 * compute-as-raster substrate.  The substrate (read it out of the graphics
 * functional units: texture-LD load, FP24 ALU compute, RB3D export store, the
 * blend ADD/MIN/MAX/SUB + stencil + ZPASS reduction forms, ROP bitwise, and
 * per-pixel predicates) has no LDS, no workgroup barrier, no general atomic on
 * an arbitrary address, no arbitrary read-write storage, and no FP64.  A kernel
 * that uses any of those cannot be expressed in the substrate, so this analysis
 * rejects it deterministically -- it never lowers or executes anything.
 */

#include "r300_compute_admission.h"

#include "compiler/nir/nir.h"

static void
admit(struct r300_compute_admission *out)
{
   out->admissible = true;
   out->reason = R300_COMPUTE_ADMIT;
   out->detail = "admissible";
}

static void
reject(struct r300_compute_admission *out, enum r300_compute_reject reason,
       const char *detail)
{
   out->admissible = false;
   out->reason = reason;
   out->detail = detail;
}

/* True for a scatter the substrate cannot do.  A plain store_ssbo is NOT here:
 * the ComputeGrid->RasterGrid functor maps the kernel's buffer-output write to
 * a coordinate-indexed RB3D export, so it is the expected admissible output.
 * Storage-image stores and raw global-pointer stores are arbitrary scatter
 * beyond a single RT coordinate and have no lowering. */
static bool
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

/* Walk the kernel and detect the identity-map structural pattern:
 * exactly one store_ssbo whose value source is the SSA def of exactly one
 * load_ssbo.  The store's binding is the canonical 0 the classifier already
 * enforces; the load's binding is read off its first source so the dispatch
 * lowering knows which descriptor maps to the input sampler view.
 *
 * The index-equivalence between the load and the store (both indexed by
 * gl_GlobalInvocationID.xy) is not asserted here; the readback oracle catches
 * a non-identity index relationship.  Detection only gates the LOWERING
 * branch -- a mis-detected kernel falls back to the no-op pipeline path. */
void
r300_nir_detect_identity_map(const nir_shader *s,
                             struct r300_compute_identity_pattern *out)
{
   out->is_identity_map     = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;

   const nir_intrinsic_instr *store = NULL;
   const nir_intrinsic_instr *load  = NULL;
   unsigned store_count = 0;
   unsigned load_count  = 0;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               store = intr;
               store_count++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               load = intr;
               load_count++;
            }
         }
      }
   }

   /* Identity-map shape: one source ssbo load, one dest ssbo store, the
    * store's stored value is the load's SSA def. */
   if (store_count != 1 || load_count != 1)
      return;
   if (store->src[0].ssa != &load->def)
      return;

   /* The store binding is constant 0 (classify-time invariant).  The load's
    * binding is the descriptor index the lowering needs. */
   if (!nir_src_is_const(load->src[0]))
      return;
   out->input_ssbo_binding  = nir_src_as_uint(load->src[0]);
   out->output_ssbo_binding = nir_src_is_const(store->src[1]) ?
                              nir_src_as_uint(store->src[1]) : 0;
   out->is_identity_map = true;
}

void
r300_nir_classify_compute(const nir_shader *s,
                          struct r300_compute_admission *out)
{
   admit(out);

   /* Workgroup shared memory: no LDS on R3xx. */
   if (s->info.shared_size > 0) {
      reject(out, R300_COMPUTE_REJECT_SHARED_MEMORY,
             "workgroup shared memory (info.shared_size > 0)");
      return;
   }

   nir_foreach_variable_with_modes (var, s, nir_var_mem_shared) {
      (void)var;
      reject(out, R300_COMPUTE_REJECT_SHARED_MEMORY,
             "nir_var_mem_shared variable");
      return;
   }

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               const char *name = nir_intrinsic_infos[intr->intrinsic].name;

               if (intr->intrinsic == nir_intrinsic_barrier) {
                  reject(out, R300_COMPUTE_REJECT_BARRIER,
                         "control/memory barrier");
                  return;
               }
               /* Every atomic intrinsic carries "atomic" in its name; the
                * substrate has only the blend-add/min/max/sub, stencil, and
                * ZPASS reduction forms, none of which are an arbitrary-address
                * NIR atomic. */
               if (strstr(name, "atomic") != NULL) {
                  reject(out, R300_COMPUTE_REJECT_GENERAL_ATOMIC, name);
                  return;
               }
               if (strstr(name, "shared") != NULL) {
                  reject(out, R300_COMPUTE_REJECT_SHARED_MEMORY, name);
                  return;
               }
               if (intr->intrinsic == nir_intrinsic_store_ssbo) {
                  if (!nir_src_is_const(intr->src[1]) || nir_src_as_uint(intr->src[1]) != 0 ||
                      !nir_src_is_const(intr->src[2]) || nir_src_as_uint(intr->src[2]) != 0) {
                     reject(out, R300_COMPUTE_REJECT_RW_STORAGE, "non-canonical store_ssbo");
                     return;
                  }
               }
               if (is_rw_storage_store(intr->intrinsic)) {
                  reject(out, R300_COMPUTE_REJECT_RW_STORAGE, name);
                  return;
               }
               /* A store_deref into global storage is arbitrary scatter; a
                * store_deref into ssbo is the coordinate-indexed output the
                * functor maps to RB3D export, so it is admissible. */
               if (intr->intrinsic == nir_intrinsic_store_deref) {
                  nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
                  if (deref && nir_deref_mode_is(deref, nir_var_mem_global)) {
                     reject(out, R300_COMPUTE_REJECT_RW_STORAGE,
                            "store_deref to global");
                     return;
                  }
               }
            } else if (instr->type == nir_instr_type_alu) {
               nir_alu_instr *alu = nir_instr_as_alu(instr);
               /* FP24 ALU: a 64-bit float result or source is beyond the envelope. */
               bool uses_fp64 = false;
               if (alu->def.bit_size == 64 &&
                   nir_alu_type_get_base_type(
                      nir_op_infos[alu->op].output_type) == nir_type_float) {
                  uses_fp64 = true;
               } else {
                  for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
                     if (alu->src[i].src.ssa->bit_size == 64 &&
                         nir_alu_type_get_base_type(
                            nir_op_infos[alu->op].input_types[i]) == nir_type_float) {
                        uses_fp64 = true;
                        break;
                     }
                  }
               }
               if (uses_fp64) {
                  reject(out, R300_COMPUTE_REJECT_FP64,
                         nir_op_infos[alu->op].name);
                  return;
               }
            }
         }
      }
   }
}

const char *
r300_compute_reject_name(enum r300_compute_reject reason)
{
   switch (reason) {
   case R300_COMPUTE_ADMIT:                 return "admit";
   case R300_COMPUTE_REJECT_SHARED_MEMORY:  return "shared-memory";
   case R300_COMPUTE_REJECT_BARRIER:        return "barrier";
   case R300_COMPUTE_REJECT_GENERAL_ATOMIC: return "general-atomic";
   case R300_COMPUTE_REJECT_RW_STORAGE:     return "rw-storage";
   case R300_COMPUTE_REJECT_FP64:           return "fp64";
   }
   return "unknown";
}
