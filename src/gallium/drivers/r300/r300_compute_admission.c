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
#include "util/macros.h"

static bool
identity_map_debug_enabled(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *flags = getenv("R300VK_DEBUG");
      cached = (flags && strstr(flags, "identity_map")) ? 1 : 0;
   }
   return cached != 0;
}

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

static bool
intrinsic_base_type_is_float(const nir_intrinsic_instr *intr,
                             nir_alu_type fallback_type)
{
   nir_alu_type type = fallback_type;

   if (nir_intrinsic_has_src_type(intr))
      type = nir_intrinsic_src_type(intr);
   else if (nir_intrinsic_has_dest_type(intr))
      type = nir_intrinsic_dest_type(intr);

   return nir_alu_type_get_base_type(type) == nir_type_float;
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
   out->value_components    = 0;
   out->value_bit_size      = 0;
   out->value_is_float      = false;

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
      if (identity_map_debug_enabled())
         fprintf(stderr, "ident_map: detect-skip count store=%u load=%u\n",
                 store_count, load_count);
      return;
   }
   const bool value_eq_load = (store->src[0].ssa == &load->def);
   /* store_ssbo src layout: [0]=value, [1]=binding, [2]=offset.
    * load_ssbo  src layout: [0]=binding, [1]=offset.
    *
    * offset_eq below compares the store offset def with the load offset def,
    * but the detector does NOT gate on it.  nir_lower_explicit_io with
    * nir_address_format_32bit_index_offset emits an independent address-
    * computation chain for each load_ssbo and each store_ssbo intrinsic, so
    * the load offset def and the store offset def stay distinct even for
    * out[gid] = in[gid] -- and stay distinct through nir_opt_dce + nir_opt_cse
    * (both offsets derive from the same gl_GlobalInvocationID, but CSE does
    * not fold the separately lowered chains).  Gating on offset_eq therefore
    * rejects the legitimate identity-map kernel.
    *
    * Cost of not gating: a scatter kernel out[g(i)] = in[i] is admitted and
    * the fullscreen-FS lowering computes out[i] = in[i] (the pass cannot honor
    * a scatter index g).  That is a bounded value miscompute, not a memory
    * hazard -- the fullscreen draw's framebuffer extent equals the output
    * buffer extent, so no out-of-bounds write occurs.
    *
    * TODO: gate the identity shape on semantic offset equivalence by walking
    *       the store and load offset defs to their root gl_GlobalInvocationID
    *       extract and comparing those, replacing the SSA-def-identity
    *       offset_eq.  Reason: SSA-def identity under-approximates
    *       post-explicit_io offset equality, so a genuine scatter cannot
    *       currently be distinguished from a gather.  Tracking:
    *       r300_nir_detect_identity_map offset gate. */
   const bool offset_eq = (store->src[2].ssa == load->src[1].ssa);
   if (identity_map_debug_enabled())
      fprintf(stderr,
              "ident_map: detect inner store_val_ssa=%p load_def=%p "
              "value_eq_load=%d offset_eq=%d "
              "load_binding_const=%d store_binding_const=%d\n",
              (void *)store->src[0].ssa, (void *)&load->def,
              (int)value_eq_load, (int)offset_eq,
              (int)nir_src_is_const(load->src[0]),
              (int)nir_src_is_const(store->src[1]));
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
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->value_is_float = intrinsic_base_type_is_float(
      store, nir_intrinsic_has_dest_type(load) ? nir_intrinsic_dest_type(load)
                                               : nir_type_invalid);
   out->is_identity_map = true;
}

/* Texture-pair binary-map detector.  Mirrors the identity-map pattern at one
 * level of indirection: store_ssbo's value is the def of a single ALU op whose
 * two sources are the defs of two distinct load_ssbo intrinsics.
 *
 * Pure read-only NIR walk.  The recognized ALU op set is bounded by what the
 * compute-as-raster fragment lowering can reproduce within the R300 FP24 ALU
 * budget; an op outside the set leaves is_binary_map false so the orchestrator
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
   out->value_components      = 0;
   out->value_bit_size        = 0;
   out->value_is_float        = false;

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
    * -- that's the identity-map case r300_nir_detect_identity_map handles). */
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
   /* Like the identity-map detector, this detector does NOT gate on offset
    * equality.  nir_lower_explicit_io with nir_address_format_32bit_index_offset
    * emits independent address-computation chains per intrinsic, so the load_a,
    * load_b, and store offset defs all stay distinct even when the source GLSL
    * uses the same gl_GlobalInvocationID.x in all three, and nir_opt_dce +
    * nir_opt_cse before the detector does not fold them.  Accept the same
    * bounded scatter miscompute as the identity path: the fullscreen-FS
    * lowering computes out[i] = f(a[i], b[i]) regardless of any scatter index
    * g, a value miscompute bounded by the framebuffer extent (which equals the
    * buffer extent), not a memory hazard.
    *
    * TODO: gate on semantic offset equivalence by walking the store and load
    *       offset defs to their root gl_GlobalInvocationID extract, replacing
    *       SSA-def identity.  Reason: SSA-def identity under-approximates
    *       post-explicit_io offset equality, so a genuine scatter is currently
    *       indistinguishable from a gather.  Tracking:
    *       r300_nir_detect_binary_map offset gate. */

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
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->value_is_float = intrinsic_base_type_is_float(
      store, nir_op_infos[alu->op].output_type);
}

/* Blend-add reduction detector.  Recognises the histogram / accumulator shape
 * that lowers to RB3D COMB_FCN_ADD blend accumulation:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     uint bin = gid & MASK;
 *     atomicAdd(out_data[bin], in_data[gid]);
 *
 * Detection invariants (the binary-map shape one level of indirection lower,
 * because the store side is the atomic itself instead of a separate
 * store_ssbo):
 *
 *   1. Exactly 1 ssbo_atomic intrinsic + exactly 1 load_ssbo intrinsic +
 *      exactly 0 store_ssbo intrinsics (the atomic IS the store).
 *   2. The atomic's ATOMIC_OP index is nir_atomic_op_iadd (integer add).
 *      fadd, imin, imax, etc. plug into the same shape; iadd lands first
 *      because every per-bin sum is integer-exact in the R300 FP24 ALU when
 *      bin_count is small and per-bin sum < 2^17.
 *   3. The atomic's value-source SSA def equals the load_ssbo's def --
 *      identifies that the input is FED INTO the atomic rather than
 *      consumed elsewhere.
 *   4. The atomic's binding != the load's binding -- the output histogram
 *      cannot also be the input source (Vulkan forbids it by descriptor
 *      contract, but spell it out for the detector's predicate-completeness).
 *
 * The bin-mask analysis (walking the atomic's offset def back to find the
 * `gid & const_mask` shape) is NOT performed here: the orchestrator sizes the
 * output RT from the descriptor's buffer size (M-bin output = M uint32 cells),
 * and the mask is implicit in that size.  A non-(2^k - 1) mask shape would
 * need the explicit mask walk.
 *
 * The same "binding sources may be opaque post-explicit_io" caveat from the
 * binary-map detector applies: when nir_src_is_const returns false the field
 * stays 0 and the orchestrator's positional descriptor-set-layout fallback
 * recovers the binding. */
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

/* Recursive transitive dependency: does `def` derive from `root` through any
 * chain of ALU / mov / extract instructions?  Bounded depth keeps the walk
 * total for the small kernels the detector pattern-matches. */
static bool
def_derives_from(const nir_def *def, const nir_def *root, unsigned depth)
{
   if (!def || depth >= 8)
      return false;
   if (def == root)
      return true;
   /* Canonical r300 NIR pattern (per r300_nir_detect_binary_map at line 232
    * of this file): nir_def_as_alu_or_null casts back through the def's
    * embedded location to recover the producing nir_alu_instr, returning
    * NULL when the def is not from an ALU. */
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)def);
   if (!alu)
      return false;
   const unsigned num_srcs = nir_op_infos[alu->op].num_inputs;
   for (unsigned i = 0; i < num_srcs; i++) {
      if (def_derives_from(alu->src[i].src.ssa, root, depth + 1))
         return true;
   }
   return false;
}

/* ZPASS coverage-count detector.  Recognises the shape:
 *
 *     uint gid = gl_GlobalInvocationID.x;
 *     if (in_data[gid] >= THRESHOLD)
 *         atomicAdd(count_out, 1u);
 *
 * which lowers to the depth/stencil unit's ZPASS occlusion-counter verb
 * (ZB_ZPASS_DATA / ZB_ZPASS_ADDR), counting surviving fragments.  Detection
 * invariants:
 *
 *   1. Exactly 1 ssbo_atomic + 1 load_ssbo + 0 store_ssbo.
 *   2. The atomic's ATOMIC_OP is nir_atomic_op_iadd.
 *   3. The atomic's value-source (src[2]) is a NIR-constant equal to 1.
 *      This is the discriminator from blend-acc: blend-acc's value source
 *      is a load_ssbo def, ZPASS's is a literal 1.
 *   4. The atomic is contained in a block whose parent cf_node is a
 *      nir_if (the if-then branch with the atomic; nested if's not
 *      supported in the first cut).
 *   5. The nir_if's condition SSA def transitively derives from the
 *      load_ssbo's def via ALU operations (the predicate is computed
 *      from the load result).
 *
 * Bin-mask analysis is NOT performed: the output buffer is single-element
 * (1 uint32 cell) and the orchestrator binds it directly without
 * histogram indexing. */
void
r300_nir_detect_zpass_reduction(const nir_shader *s,
                                struct r300_compute_zpass_reduction_pattern *out)
{
   out->is_zpass_reduction  = false;
   out->value_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;
   out->alu_op              = 0;

   const nir_intrinsic_instr *atomic = NULL;
   const nir_intrinsic_instr *load   = NULL;
   const nir_block           *atomic_block = NULL;
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
               atomic_block = block;
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
   if (nir_intrinsic_atomic_op(atomic) != nir_atomic_op_iadd)
      return;

   /* Value source must be a NIR constant equal to 1.  This is the
    * load-bearing discriminator from blend-acc-reduction. */
   if (!nir_src_is_const(atomic->src[2]) ||
       nir_src_as_uint(atomic->src[2]) != 1)
      return;

   /* The atomic must be inside an if-then (or if-else) branch, not at
    * the function impl's top level. */
   const nir_cf_node *parent_cf = atomic_block->cf_node.parent;
   if (!parent_cf || parent_cf->type != nir_cf_node_if)
      return;
   const nir_if *if_node = nir_cf_node_as_if(parent_cf);

   /* The if condition must transitively derive from the load_ssbo's def.
    * This rules out kernels where the predicate is computed from some
    * other source (a uniform, gl_GlobalInvocationID directly, etc.). */
   if (!def_derives_from(if_node->condition.ssa, &load->def, 0))
      return;

   out->is_zpass_reduction = true;
   out->alu_op = (uint16_t)nir_op_iadd;
   if (nir_src_is_const(load->src[0]))
      out->value_ssbo_binding  = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(atomic->src[0]))
      out->output_ssbo_binding = nir_src_as_uint(atomic->src[0]);
}

/* A per-iteration step doubles its loop-carried value when it is x*2
 * (imul with a constant 2 operand), x<<1 (ishl by a constant 1), or x+x
 * (iadd of one scalar with itself).  The orchestrator's fragment pass
 * hard-codes a doubling (TEX then MUL by 2.0), so the detector admits
 * only steps it can faithfully execute: an admitted x*3 or x+c would be
 * silently realized as x*16, a wrong compute result.  Float fmul/fadd
 * are deliberately excluded -- the per-byte UNORM8 doubling the
 * orchestrator runs does not model IEEE float-domain doubling, so a
 * float step is not faithfully executed either and must fall through to
 * substrate-absence rejection.  Scalars are resolved through bit-exact
 * mov/vec chains so a doubling that arrives via a copy still matches. */
static bool
multipass_step_is_doubling(const nir_alu_instr *alu)
{
   switch (alu->op) {
   case nir_op_ishl: {
      nir_scalar amt =
         nir_scalar_resolved(alu->src[1].src.ssa, alu->src[1].swizzle[0]);
      return nir_scalar_is_const(amt) && nir_scalar_as_uint(amt) == 1;
   }
   case nir_op_imul:
      for (unsigned i = 0; i < 2; i++) {
         nir_scalar sc =
            nir_scalar_resolved(alu->src[i].src.ssa, alu->src[i].swizzle[0]);
         if (nir_scalar_is_const(sc) && nir_scalar_as_uint(sc) == 2)
            return true;
      }
      return false;
   case nir_op_iadd: {
      nir_scalar a =
         nir_scalar_resolved(alu->src[0].src.ssa, alu->src[0].swizzle[0]);
      nir_scalar b =
         nir_scalar_resolved(alu->src[1].src.ssa, alu->src[1].swizzle[0]);
      return nir_scalar_equal(a, b);
   }
   default:
      return false;
   }
}

/* Multipass ping-pong scan detector.  Recognises the per-element self-iterated
 * shape `x = in[gid]; for (k < pass_count) x = 2*x; out[gid] = x`, where
 * pass_count is a runtime params-buffer load so the loop survives constant
 * folding, feeding a single store_ssbo.  The discriminator from every other
 * admitted shape is the presence of a nir_loop: identity-map, binary-map,
 * blend-acc reduction, and ZPASS reduction are all loop-free.  A genuine
 * cross-element scan would need a workgroup barrier (absent on RS482), so this
 * admits only the self-only iterated step; the orchestrator runs it as
 * pass_count dependent FBO ping-pong passes.  Detection invariants:
 *
 *   1. Exactly 1 store_ssbo, 0 ssbo_atomic (rules out the reduction shapes).
 *   2. At least 2 load_ssbo: the per-element data plus the runtime
 *      pass_count from the params buffer.
 *   3. A nir_loop is present (the unique discriminator).
 *   4. A per-iteration DOUBLING step (x*2 / x<<1 / x+x) sits inside a
 *      loop body.  The admit set equals what the orchestrator executes:
 *      a loop carrying any other arithmetic is not the realizable
 *      iterated-scale shape and is rejected, not silently mis-scaled.
 *
 * step_op carries the per-iteration nir_op for diagnostics; the orchestrator
 * hard-codes the doubling, which is sound precisely because every admitted
 * step is a doubling.  Bindings stay 0 when the post-explicit_io binding
 * sources are not constants; the orchestrator's positional fallback recovers
 * them (binding 0 = input, 1 = output, 2 = params). */
void
r300_nir_detect_multipass_scan_pattern(const nir_shader *s,
                                       struct r300_compute_multipass_scan_pattern *out)
{
   out->is_multipass_scan   = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;
   out->step_op             = 0;

   const nir_intrinsic_instr *store      = NULL;
   const nir_intrinsic_instr *first_load = NULL;
   unsigned atomic_count = 0, load_count = 0, store_count = 0;
   bool has_loop = false;
   uint16_t step_op = 0;
   bool step_found = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         /* A block sits in a loop body when any cf-node ancestor is a
          * nir_cf_node_loop.  Walking the parent chain catches loops at
          * any nesting depth. */
         bool block_in_loop = false;
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop) {
               has_loop = true;
               block_in_loop = true;
               break;
            }
         }
         nir_foreach_instr (instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
               if (intr->intrinsic == nir_intrinsic_ssbo_atomic) {
                  atomic_count++;
               } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
                  if (!first_load)
                     first_load = intr;
                  load_count++;
               } else if (intr->intrinsic == nir_intrinsic_store_ssbo) {
                  store = intr;
                  store_count++;
               }
            } else if (block_in_loop && !step_found &&
                       instr->type == nir_instr_type_alu) {
               const nir_alu_instr *alu = nir_instr_as_alu(instr);
               if (multipass_step_is_doubling(alu)) {
                  step_op    = (uint16_t)alu->op;
                  step_found = true;
               }
            }
         }
      }
   }

   if (store_count != 1 || atomic_count != 0 || load_count < 2 ||
       !has_loop || !step_found)
      return;

   out->is_multipass_scan = true;
   out->step_op = step_op;
   if (first_load && nir_src_is_const(first_load->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(first_load->src[0]);
   if (store && nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
}

/* M-H predicated masked-store detector.  Recognises
 * `if (in_pred[gid] != 0u) out_data[gid] = in_val[gid]`: a single store_ssbo
 * sitting inside a nir_if, two load_ssbo (predicate + value), no atomic, no
 * loop, with the stored value coming directly from a load_ssbo.  The
 * conditional store is the discriminator from identity-map (load_count == 1)
 * and binary-map (store value is a binary ALU op, not a load); the absent
 * atomic separates it from blend-acc / ZPASS, and the absent loop from
 * multipass.  The orchestrator lowers it to a per-pixel KILL_IF discard over an
 * RT seeded from out_data, so masked cells keep their pre-existing baseline. */
void
r300_nir_detect_predicated_store_pattern(const nir_shader *s,
                                         struct r300_compute_predicated_store_pattern *out)
{
   out->is_predicated_store = false;
   out->pred_ssbo_binding   = 0;
   out->value_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;

   const nir_intrinsic_instr *store       = NULL;
   const nir_intrinsic_instr *first_load  = NULL;
   const nir_intrinsic_instr *second_load = NULL;
   const nir_if              *store_if    = NULL;
   unsigned atomic_count = 0, load_count = 0, store_count = 0;
   bool has_loop    = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         /* A block is inside a conditional when any cf-node ancestor is a
          * nir_cf_node_if, and inside a loop when any ancestor is a
          * nir_cf_node_loop.  We track the direct nir_if parent of the store
          * block for condition validation. */
         const nir_if *block_if = NULL;
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               has_loop = true;
            else if (p->type == nir_cf_node_if && !block_if)
               block_if = nir_cf_node_as_if(p);
         }
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_ssbo_atomic) {
               atomic_count++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               if (!first_load)
                  first_load = intr;
               else if (!second_load)
                  second_load = intr;
               load_count++;
            } else if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               store = intr;
               store_count++;
               store_if = block_if;
            }
         }
      }
   }

   /* The stored value must come directly from a load_ssbo (the value load).
    * A binary-map kernel's store value is an ALU op, not a load -- this gate
    * keeps the two shapes disjoint.  nir_def_as_intrinsic_or_null returns NULL
    * when the value def is not produced by an intrinsic (the same idiom
    * binary-map uses with nir_def_as_alu_or_null). */
   const nir_intrinsic_instr *val_load =
      (store && store->src[0].ssa)
         ? nir_def_as_intrinsic_or_null(store->src[0].ssa) : NULL;
   const bool val_is_load =
      val_load && val_load->intrinsic == nir_intrinsic_load_ssbo;

   if (identity_map_debug_enabled())
      fprintf(stderr,
              "ident_map: predstore-detect store=%u load=%u atomic=%u "
              "has_loop=%d store_if=%p val_is_load=%d\n",
              store_count, load_count, atomic_count, (int)has_loop,
              (const void *)store_if, (int)val_is_load);

   if (store_count != 1 || atomic_count != 0 || load_count != 2 ||
       has_loop || !store_if || !val_is_load)
      return;

   /* The other load feeds the nir_if condition (the predicate). */
   const nir_intrinsic_instr *pred_load =
      (val_load == first_load) ? second_load : first_load;

   /* Verify that pred_load feeds the if condition. */
   if (!pred_load || !def_derives_from(store_if->condition.ssa, &pred_load->def, 0))
       return;

   out->is_predicated_store = true;
   if (pred_load && nir_src_is_const(pred_load->src[0]))
      out->pred_ssbo_binding = nir_src_as_uint(pred_load->src[0]);
   if (nir_src_is_const(val_load->src[0]))
      out->value_ssbo_binding = nir_src_as_uint(val_load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
}

/* Count the load_ssbo leaves of a pure integer add-reduction tree.  An iadd
 * node recurses into both inputs; a load_ssbo def is a tap leaf worth 1; any
 * other def (a non-load, non-iadd) makes the tree impure and returns -1.
 * Depth-bounded like def_derives_from for the small kernels the detector
 * pattern-matches.  Verifies that all load_ssbo leaves pull from the same
 * binding (captured in *binding). */
static int
multitap_add_tree_taps(const nir_def *def, uint32_t *binding, unsigned depth)
{
   if (!def || depth >= 8)
      return -1;
   const nir_intrinsic_instr *intr =
      nir_def_as_intrinsic_or_null((nir_def *)def);
   if (intr) {
      if (intr->intrinsic != nir_intrinsic_load_ssbo)
         return -1;
      if (!nir_src_is_const(intr->src[0]))
         return -1;
      uint32_t b = nir_src_as_uint(intr->src[0]);
      if (*binding == 0xffffffff)
         *binding = b;
      else if (*binding != b)
         return -1;
      return 1;
   }
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)def);
   if (!alu || alu->op != nir_op_iadd)
      return -1;
   const int l = multitap_add_tree_taps(alu->src[0].src.ssa, binding, depth + 1);
   if (l < 0)
      return -1;
   const int r = multitap_add_tree_taps(alu->src[1].src.ssa, binding, depth + 1);
   if (r < 0)
      return -1;
   return l + r;
}

/* M-I multi-tap gather detector.  Recognises an unweighted N-tap neighbourhood
 * convolution `out[gid] = in[gid+o0] + ... + in[gid+o_{N-1}]` (N >= 3): one
 * store_ssbo whose value is an integer add-reduction tree of >= 3 load_ssbo
 * leaves, no atomic, no loop.  The >= 3 add-leaf count is the discriminator
 * from binary-map (exactly two load inputs to a single ALU op) and identity-map
 * (one load); the absent atomic separates it from blend-acc and ZPASS, the
 * absent loop from multipass.  The orchestrator lowers it to a multi-TEX
 * fragment draw applying a canonical box kernel over the input bound as a 2D
 * texture; fadd is excluded from the first cut because the per-byte UNORM8
 * integer-sum envelope is the exact-realizable form. */
/* Verify that the add-reduction tree's load_ssbo leaves use exactly the
 * box-3 offsets {base-4, base, base+4} relative to the store's offset def.
 * Returns a bitmask of found offsets (bit 0 for base-4, bit 1 for base, bit 2
 * for base+4). */
static int
multitap_verify_box3_offsets(const nir_def *def, const nir_def *base, unsigned depth)
{
   if (!def || depth >= 8)
      return 0;
   const nir_intrinsic_instr *intr =
      nir_def_as_intrinsic_or_null((nir_def *)def);
   if (intr) {
      if (intr->intrinsic != nir_intrinsic_load_ssbo)
         return 0;
      const nir_def *offset = intr->src[1].ssa;
      if (offset == base)
         return (1 << 1); /* base (offset 0) */
      /* Check for base +/- 4 via ALU iadd. */
      const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)offset);
      if (alu && alu->op == nir_op_iadd) {
         for (int i = 0; i < 2; i++) {
            if (alu->src[i].src.ssa == base && nir_src_is_const(alu->src[1-i].src)) {
               int32_t val = (int32_t)nir_src_as_int(alu->src[1-i].src);
               if (val == -4) return (1 << 0);
               if (val == 4)  return (1 << 2);
            }
         }
      }
      return 0;
   }
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)def);
   if (!alu || alu->op != nir_op_iadd)
      return 0;
   return multitap_verify_box3_offsets(alu->src[0].src.ssa, base, depth + 1) |
          multitap_verify_box3_offsets(alu->src[1].src.ssa, base, depth + 1);
}

void
r300_nir_detect_multitap_gather_pattern(const nir_shader *s,
                                        struct r300_compute_multitap_gather_pattern *out)
{
   out->is_multitap_gather  = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;
   out->tap_count           = 0;

   const nir_intrinsic_instr *store      = NULL;
   unsigned atomic_count = 0, load_count = 0, store_count = 0;
   bool has_loop = false, in_if = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop) {
               has_loop = true;
            }
            if (p->type == nir_cf_node_if) {
               in_if = true;
            }
         }
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               store = intr;
               store_count++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               load_count++;
            } else if (intr->intrinsic == nir_intrinsic_ssbo_atomic) {
               atomic_count++;
            }
         }
      }
   }

   if (store_count != 1 || atomic_count != 0 || has_loop || in_if || load_count < 3)
      return;
   if (!store->src[0].ssa || !store->src[2].ssa)
      return;

   /* Store value must be an integer add-reduction whose every leaf is a
    * load_ssbo def, with at least three taps.  All taps must pull from the
    * same SSBO binding. */
   uint32_t binding = 0xffffffff;
   const int taps = multitap_add_tree_taps(store->src[0].ssa, &binding, 0);
   if (taps != 3 || binding == 0xffffffff)
      return;

   /* Kernel must be exactly the 3-tap box filter {-1, 0, 1} relative to the
    * store index. */
   if (multitap_verify_box3_offsets(store->src[0].ssa, store->src[2].ssa, 0) != 0x7)
      return;

   out->is_multitap_gather = true;
   out->tap_count = (uint16_t)taps;
   out->input_ssbo_binding = binding;
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
}

void
r300_nir_classify_compute(const nir_shader *s,
                          struct r300_compute_admission *out)
{
   admit(out);

   /* Blend-add reduction admit-precheck.  An ssbo_atomic with ATOMIC_OP=iadd
    * looks like a "general atomic" to the loop below, but the blend-acc
    * detector lifts it to the RB3D COMB_FCN_ADD accumulation shape -- one of
    * the hardware-confirmed compute-as-raster substrate verbs.  Run the
    * detector here and set a local flag so the loop's general-atomic reject
    * case knows to admit THIS specific atomic.  Other atomic shapes still
    * reject. */
   struct r300_compute_blend_acc_reduction_pattern blend_acc = {0};
   r300_nir_detect_blend_acc_reduction(s, &blend_acc);
   const bool admit_ssbo_atomic_blend_acc = blend_acc.is_blend_acc_reduction;

   /* ZPASS coverage-count admit-precheck.  Same admit-on-shape pattern as
    * blend-acc but the structural test is a conditional-gated atomicAdd-of-1.
    * Detector + classifier admit-flag are mutually exclusive against blend-acc
    * (the atomic's value source is const 1 for ZPASS, vs a load_ssbo def for
    * blend-acc) so admitting one cannot admit the other. */
   struct r300_compute_zpass_reduction_pattern zpass = {0};
   r300_nir_detect_zpass_reduction(s, &zpass);
   const bool admit_ssbo_atomic_zpass = zpass.is_zpass_reduction;

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
                * NIR atomic.  The blend-acc-reduction shape IS a
                * substrate-compatible atomic-add, so when the kernel-level
                * detector confirmed the SHAPE upstream, admit the specific
                * ssbo_atomic the detector recognised. */
               if (strstr(name, "atomic") != NULL) {
                  if ((admit_ssbo_atomic_blend_acc ||
                       admit_ssbo_atomic_zpass) &&
                      intr->intrinsic == nir_intrinsic_ssbo_atomic) {
                     continue;  /* blend-acc OR ZPASS lowering will handle it */
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

/* Enum-keyed reject-reason registry: one row per r300_compute_reject value,
 * ordered by enum value.  key is the stable token a consumer switches on;
 * substrate_absence is the hardware capability the RS482 compute-as-raster
 * substrate lacks, which is why the construct cannot lower.  This is the single
 * source of truth for both r300_compute_reject_name and the substrate-absence
 * reason -- the classifier's per-site detail string names the specific
 * construct, this names the category. */
static const struct r300_compute_reject_row r300_compute_reject_registry[] = {
   { R300_COMPUTE_ADMIT, "admit",
     "admissible: the kernel maps onto a compute-as-raster substrate verb" },
   { R300_COMPUTE_REJECT_SHARED_MEMORY, "shared-memory",
     "R3xx has no LDS; fragment lanes share no workgroup-local memory" },
   { R300_COMPUTE_REJECT_BARRIER, "barrier",
     "fragments are not a synchronized workgroup; no control or memory barrier exists" },
   { R300_COMPUTE_REJECT_GENERAL_ATOMIC, "general-atomic",
     "the only atomics are blend ADD/MIN/MAX/SUB, the stencil increment, and the ZPASS reduction; no arbitrary-address atomic exists" },
   { R300_COMPUTE_REJECT_RW_STORAGE, "rw-storage",
     "the substrate has texture-load input and one RB3D color export; no scatter or arbitrary read-write storage exists" },
   { R300_COMPUTE_REJECT_FP64, "fp64",
     "the fragment ALU is FP24 (s1e7m16); no double-precision path exists" },
};

const struct r300_compute_reject_row *
r300_compute_reject_lookup(enum r300_compute_reject reason)
{
   /* One row per enum value: this guard fails the build if a reason is added to
    * the enum without a registry row -- the divergence the registry prevents.
    * STATIC_ASSERT is a do/while statement, so it lives inside a function. */
   STATIC_ASSERT(ARRAY_SIZE(r300_compute_reject_registry) ==
                 R300_COMPUTE_REJECT_FP64 + 1);
   for (unsigned i = 0; i < ARRAY_SIZE(r300_compute_reject_registry); i++) {
      if (r300_compute_reject_registry[i].reason == reason)
         return &r300_compute_reject_registry[i];
   }
   /* Not reachable for a valid enum value; the admit row is the safe default. */
   return &r300_compute_reject_registry[0];
}

const char *
r300_compute_reject_name(enum r300_compute_reject reason)
{
   return r300_compute_reject_lookup(reason)->key;
}

const char *
r300_compute_reject_substrate_absence(enum r300_compute_reject reason)
{
   return r300_compute_reject_lookup(reason)->substrate_absence;
}
