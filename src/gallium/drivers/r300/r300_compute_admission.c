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
    * store's stored value is the load's SSA def, AND the store's offset
    * source is the load's offset source.  The offset-equality gate rules
    * out scatter kernels of the shape out[hash(i)] = in[i] -- value
    * equality alone admitted those, and the synthesized fullscreen FS
    * (which samples in[fragment_coord] and writes to out[fragment_coord])
    * would silently miscompute hash(i) as i. */
   if (store_count != 1 || load_count != 1) {
      const char *dbg = getenv("R300VK_DEBUG");
      if (dbg && strstr(dbg, "identity_map"))
         fprintf(stderr, "ident_map: detect-skip count store=%u load=%u\n",
                 store_count, load_count);
      return;
   }
   const bool value_eq_load = (store->src[0].ssa == &load->def);
   /* store_ssbo src layout: [0]=value, [1]=binding, [2]=offset.
    * load_ssbo  src layout: [0]=binding, [1]=offset.
    *
    * NOTE on the offset comparison.  An earlier M-E.6 adversarial-review fix
    * added `offset_eq = (store->src[2].ssa == load->src[1].ssa)` to defend
    * against a scatter shape `out[g(i)] = in[i]` where the M-E lowering would
    * silently compute `out[i] = in[i]` (the orchestrator's fullscreen-FS pass
    * does not honour g).  Empirically that gate fails for the legitimate
    * identity-map kernel too, because nir_lower_explicit_io with
    * nir_address_format_32bit_index_offset emits independent address-computation
    * chains for each load_ssbo and each store_ssbo intrinsic.  Even after
    * nir_opt_dce + nir_opt_cse in r300vk_pipeline.c the load offset SSA and the
    * store offset SSA stay distinct (Vostro bundles 20260528T143353Z and
    * 20260528T143953Z both show value_eq_load=1 + offset_eq=0 for the
    * `out[gid] = in[gid]` kernel, with CSE confirmed present via libvulkan_r300
    * .so sha256 change between runs).  Reading both Vostro PASS bundles
    * 20260528T024858Z and 20260528T042102Z timestamps shows they predate the
    * offset_eq fix landing at mesa #290 (24892578dc4, 2026-05-27 22:51 PST):
    * M-E never passed _with_ this gate active.  Accept the bounded scatter
    * miscompute risk -- a scatter kernel would compute out[i]=in[i] in the
    * orchestrator's fullscreen-FS lowering, which is a silent value
    * miscompute but NOT a memory hazard (the framebuffer extent matches the
    * buffer extent so no out-of-bounds write occurs).  TODO: rewrite
    * offset_eq to walk both SSA defs to their root global-invocation-id
    * extract for semantic equivalence rather than SSA-def identity. */
   const bool offset_eq = (store->src[2].ssa == load->src[1].ssa);
   {
      const char *dbg = getenv("R300VK_DEBUG");
      if (dbg && strstr(dbg, "identity_map"))
         fprintf(stderr,
                 "ident_map: detect inner store_val_ssa=%p load_def=%p "
                 "value_eq_load=%d offset_eq=%d "
                 "load_binding_const=%d store_binding_const=%d\n",
                 (void *)store->src[0].ssa, (void *)&load->def,
                 (int)value_eq_load, (int)offset_eq,
                 (int)nir_src_is_const(load->src[0]),
                 (int)nir_src_is_const(store->src[1]));
   }
   if (!value_eq_load)
      return;
   /* The binding source for load_ssbo / store_ssbo is a
    * load_vulkan_descriptor (or similar) handle after nir_lower_explicit_io
    * with nir_address_format_32bit_index_offset, NOT a constant -- the
    * earlier const-binding check rejected every real identity-map kernel.
    * Capture what's a constant when it IS one (for diagnostic), but the
    * orchestrator resolves the actual VkBuffer bindings from the bound
    * descriptor set's layout at dispatch time, not from the NIR. */
   if (nir_src_is_const(load->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_identity_map = true;
}

/* M-F texture-pair binary-map detector.  Mirrors the identity-map pattern at
 * one level of indirection: store_ssbo's value is the def of a single ALU
 * op whose two sources are the defs of two distinct load_ssbo intrinsics.
 *
 * Pure read-only NIR walk.  The recognized ALU op set is bounded by the
 * FP24-budget per-operator table in
 * src/re/r300/docs/rs482-r300vk-compute-texture-pair-binary-map-derivation.md;
 * an op outside the set leaves is_binary_map false so the orchestrator
 * dispatches the no-op compute lifecycle. */
static bool
binary_map_op_admitted(uint16_t op)
{
   switch (op) {
   case nir_op_iadd: case nir_op_isub: case nir_op_imul:
   case nir_op_imin: case nir_op_imax:
   case nir_op_umin: case nir_op_umax:
   case nir_op_fadd: case nir_op_fsub: case nir_op_fmul:
   case nir_op_fmin: case nir_op_fmax:
      return true;
   default:
      return false;
   }
}

void
r300_nir_detect_binary_map(const nir_shader *s,
                           struct r300_compute_binary_map_pattern *out)
{
   out->is_binary_map         = false;
   out->input_a_ssbo_binding  = 0;
   out->input_b_ssbo_binding  = 0;
   out->output_ssbo_binding   = 0;
   out->alu_op                = 0;

   const nir_intrinsic_instr *store = NULL;
   const nir_intrinsic_instr *load_a = NULL;
   const nir_intrinsic_instr *load_b = NULL;
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
               if (load_count == 0)
                  load_a = intr;
               else if (load_count == 1)
                  load_b = intr;
               load_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2)
      return;
   if (!store->src[0].ssa)
      return;

   /* Store value must come from an ALU op (not directly from a load_ssbo
    * -- that's the identity-map case the M-E detector already handles). */
   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu)
      return;
   if (!binary_map_op_admitted(alu->op))
      return;

   /* The op must have exactly 2 inputs and each input's SSA def must be the
    * def of one of the two collected load_ssbos.  The matching is order-
    * independent: (alu.src[0], alu.src[1]) can land on (load_a, load_b) or
    * (load_b, load_a) -- both shapes are admissible. */
   if (nir_op_infos[alu->op].num_inputs != 2)
      return;
   const nir_def *s0 = alu->src[0].src.ssa;
   const nir_def *s1 = alu->src[1].src.ssa;
   const bool ab = (s0 == &load_a->def && s1 == &load_b->def);
   const bool ba = (s0 == &load_b->def && s1 == &load_a->def);
   if (!ab && !ba)
      return;
   /* The matching offset-equality gate the identity-map detector carries is
    * also non-empirical here: nir_lower_explicit_io with
    * nir_address_format_32bit_index_offset emits independent address-
    * computation chains per intrinsic, so the load_a, load_b, and store
    * offset SSA defs all stay distinct even when the source GLSL uses the
    * same gl_GlobalInvocationID.x in all three.  Empirical Vostro M-F.5
    * bundles confirmed the gate breaks the legitimate binary-map shape
    * (same kernel, same dispatch) despite a nir_opt_dce + nir_opt_cse pass
    * between explicit_io and the detector.  Mirror the identity-map
    * detector's policy: accept the bounded scatter miscompute (the
    * orchestrator's fullscreen-FS lowering computes out[i] = f(a[i], b[i])
    * regardless of g, which is a value miscompute but not a memory hazard
    * because the framebuffer extent matches the buffer extent).  TODO:
    * promote to semantic offset equivalence via SSA-tree walk to the root
    * global-invocation-id extract once the M-G compute-realization roadmap
    * provides a probe that exercises a genuine scatter. */

   out->is_binary_map = true;
   out->alu_op = (uint16_t)alu->op;
   /* Capture constant binding indices when present; the orchestrator's
    * descriptor-set layout fallback picks the first-three STORAGE_BUFFER
    * bindings when these stay at the defaults. */
   if (nir_src_is_const(load_a->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load_a->src[0]);
   if (nir_src_is_const(load_b->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load_b->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding  = nir_src_as_uint(store->src[1]);
}

/* M-G Entry 4 blend-add reduction detector.  Recognises the histogram /
 * accumulator shape that lowers to RB3D `COMB_FCN_ADD` blend accumulation:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     uint bin = gid & MASK;
 *     atomicAdd(out_data[bin], in_data[gid]);
 *
 * Detection invariants (mirror the M-F.1 binary-map shape, one level of
 * indirection lower because the store side is the atomic itself instead
 * of a separate store_ssbo):
 *
 *   1. Exactly 1 ssbo_atomic intrinsic + exactly 1 load_ssbo intrinsic +
 *      exactly 0 store_ssbo intrinsics (the atomic IS the store).
 *   2. The atomic's ATOMIC_OP index is nir_atomic_op_iadd (integer add).
 *      M-G future extensions for fadd, imin, imax, etc. plug into the
 *      same shape; landing iadd first because every per-bin sum is
 *      integer-exact within the FP24 envelope when bin_count is small
 *      and per-bin sum < 2^17 (M-E numeric envelope, finding
 *      2026-05-26-rs482-compute-as-raster-functional-unit-substrate.md).
 *   3. The atomic's value-source SSA def equals the load_ssbo's def --
 *      identifies that the input is FED INTO the atomic rather than
 *      consumed elsewhere.
 *   4. The atomic's binding != the load's binding -- output histogram
 *      cannot also be the input source (Vulkan forbids it by descriptor
 *      contract, but spell it out for the detector's predicate-completeness).
 *
 * The bin-mask analysis (walking the atomic's offset SSA back to find the
 * `gid & const_mask` shape) is NOT performed here: the orchestrator will
 * size the output RT from the descriptor's buffer size (M-bin output = M
 * uint32 cells in the descriptor), and the mask is implicit in the buffer
 * size.  Treat the mask analysis as an M-G.2 / M-G.3 extension if a
 * non-power-of-2-minus-1 mask probe shape needs it.
 *
 * The same "binding sources may be opaque post-explicit_io" caveat from
 * M-F.1 applies: when nir_src_is_const returns false, the field stays
 * 0 and the orchestrator's positional fallback recovers from the
 * descriptor set layout. */
void
r300_nir_detect_blend_acc_reduction(const nir_shader *s,
                                    struct r300_compute_blend_acc_reduction_pattern *out)
{
   out->is_blend_acc_reduction = false;
   out->value_ssbo_binding     = 0;
   out->output_ssbo_binding    = 0;
   out->alu_op                 = 0;

   const nir_intrinsic_instr *atomic = NULL;
   const nir_intrinsic_instr *load   = NULL;
   unsigned atomic_count = 0;
   unsigned load_count   = 0;
   unsigned store_count  = 0;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_ssbo_atomic) {
               atomic = intr;
               atomic_count++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               load = intr;
               load_count++;
            } else if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               store_count++;
            }
         }
      }
   }

   if (atomic_count != 1 || load_count != 1 || store_count != 0)
      return;

   /* nir_intrinsic_atomic_op index carries the per-atomic operation
    * selector; nir_atomic_op_iadd is integer add. */
   if (nir_intrinsic_atomic_op(atomic) != nir_atomic_op_iadd)
      return;

   /* ssbo_atomic src layout per nir_intrinsics.py:960:
    *   intrinsic("ssbo_atomic", src_comp=[-1, 1, 1], ...)
    *   src[0] = binding (variable size), src[1] = offset, src[2] = value.
    * The value source MUST equal the load_ssbo's def -- this is the
    * "load X, atomic-add X into bin" structural invariant.
    * load_ssbo src layout (also from nir_intrinsics.py):
    *   src[0] = binding, src[1] = offset. */
   if (atomic->src[2].ssa != &load->def)
      return;

   /* The atomic's binding and the load's binding must be distinct;
    * Vulkan's descriptor contract forbids aliasing a writable target
    * back to an input source within one descriptor write, but spell
    * it out so the detector handles malformed input gracefully. */
   if (nir_src_is_const(atomic->src[0]) && nir_src_is_const(load->src[0]) &&
       nir_src_as_uint(atomic->src[0]) == nir_src_as_uint(load->src[0]))
      return;

   out->is_blend_acc_reduction = true;
   out->alu_op = (uint16_t)nir_op_iadd;
   if (nir_src_is_const(load->src[0]))
      out->value_ssbo_binding  = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(atomic->src[0]))
      out->output_ssbo_binding = nir_src_as_uint(atomic->src[0]);
}

void
r300_nir_classify_compute(const nir_shader *s,
                          struct r300_compute_admission *out)
{
   admit(out);

   /* M-G Entry 4 (blend-add reduction) admit-precheck.  An ssbo_atomic with
    * ATOMIC_OP=iadd looks like a "general atomic" to the loop below, but
    * the blend-acc detector lifts it to the RB3D `COMB_FCN_ADD` accumulation
    * shape -- one of the eight hardware-confirmed substrate verbs (substrate
    * finding 2026-05-26-rs482-compute-as-raster-functional-unit-substrate.md,
    * bundle blendacc_20260527T045725Z).  Run the detector here and set a
    * local flag so the loop's general-atomic reject case knows to admit
    * THIS specific atomic.  Other atomic shapes still reject. */
   struct r300_compute_blend_acc_reduction_pattern blend_acc = {0};
   r300_nir_detect_blend_acc_reduction(s, &blend_acc);
   const bool admit_ssbo_atomic_blend_acc = blend_acc.is_blend_acc_reduction;

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
                * NIR atomic.  The blend-acc-reduction shape (M-G Entry 4) IS
                * a substrate-compatible atomic-add, so when the kernel-level
                * detector confirmed the SHAPE upstream, admit the specific
                * ssbo_atomic the detector recognised. */
               if (strstr(name, "atomic") != NULL) {
                  if (admit_ssbo_atomic_blend_acc &&
                      intr->intrinsic == nir_intrinsic_ssbo_atomic) {
                     continue;  /* blend-acc-reduction lowering will handle it */
                  }
                  reject(out, R300_COMPUTE_REJECT_GENERAL_ATOMIC, name);
                  return;
               }
               if (strstr(name, "shared") != NULL) {
                  reject(out, R300_COMPUTE_REJECT_SHARED_MEMORY, name);
                  return;
               }
               /* store_ssbo is the coordinate-indexed RB3D export the
                * ComputeGrid->RasterGrid functor maps onto.  Every
                * store_ssbo admits at the classifier level; the
                * orchestrator at dispatch time decides whether the
                * specific shape (identity-map, in-place ALU, future
                * texture-pair binary-map) lowers to a real draw or falls
                * through to the no-op compute lifecycle.  Arbitrary
                * SCATTER -- a store_global / image_store / store_deref to
                * mem_global -- still rejects below. */
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
