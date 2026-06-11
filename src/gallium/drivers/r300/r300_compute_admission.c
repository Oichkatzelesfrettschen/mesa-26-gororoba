/*
 * SPDX-License-Identifier: MIT
 *
 * Classify-only admission for compute kernels against the RS482/RS485
 * compute-as-raster substrate.  That substrate is bounded by texture-LD load,
 * FP24 ALU compute, RB3D export store, blend ADD/MIN/MAX/SUB, stencil and
 * ZPASS reductions, ROP bitwise ops, and per-pixel predicates.  It has no LDS,
 * no workgroup barrier, no general atomic on an arbitrary address, no arbitrary
 * read-write storage, and no FP64.  A kernel that uses any of those cannot be
 * expressed in the substrate, so this analysis rejects it deterministically; it
 * never lowers or executes anything.
 */

#include <string.h>

#include "r300_compute_admission.h"

#include "compiler/nir/nir.h"
#include "util/macros.h"

/* Recursion bound for the def-graph walkers below.  A balanced integer
 * add-reduction tree of depth D has up to 2^D load leaves; depth 8 admits up
 * to 256 leaves, far past the three-tap box kernel and the predicate chains the
 * detectors actually match, so the bound never truncates a real match.  It
 * exists only to keep the walk total on adversarial or malformed NIR. */
#define R300_COMPUTE_DETECT_MAX_DEPTH 8u

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

/* SSBO atomics are two NIR intrinsics: ssbo_atomic (the load-op-store forms --
 * add/min/max/and/or/xor/exchange) and ssbo_atomic_swap (compare-and-swap).
 * Every detector here either rejects all atomics or recognizes exactly one
 * ssbo_atomic of a named op, so both must count toward the atomic tally;
 * omitting the swap form would let a kernel carrying a compare-and-swap slip
 * past a "no atomic" or "exactly one atomic" gate. */
static bool
is_ssbo_atomic(nir_intrinsic_op op)
{
   return op == nir_intrinsic_ssbo_atomic ||
          op == nir_intrinsic_ssbo_atomic_swap;
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

   /* nir_type_invalid carries no base type; treat an absent type as non-float
    * rather than feeding nir_type_invalid into nir_alu_type_get_base_type. */
   if (type == nir_type_invalid)
      return false;
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

   /* The store's write mask must cover every component.  The downstream
    * carriers copy whole elements sized from util_format_get_blocksize (the
    * default R8G8B8A8 path and the opt-in R32G32B32A32 FP32x4 path selected in
    * r300vk_identity_map_replay_format), so a partial-mask store -- one writing
    * only some lanes of its vec -- cannot be transported faithfully: the
    * unwritten lanes would receive carrier bytes.  Admit only a fully-written
    * store; a masked store falls through to the no-op compute lifecycle. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
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

   /* Require a full store write mask, as the identity-map detector does: the
    * binary-map carrier copies whole elements, so a partial-lane store would be
    * transported with carrier bytes in the unwritten lanes. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
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
            if (is_ssbo_atomic(intr->intrinsic)) {
               /* Capture only the load-op form as the candidate reducer; a
                * compare-and-swap still counts, forcing the tally past one so
                * the exactly-one-atomic gate below rejects it. */
               if (intr->intrinsic == nir_intrinsic_ssbo_atomic)
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
   /* A lone ssbo_atomic_swap satisfies the count but is not the recognized
    * load-op reducer, so atomic stays NULL.  Reject before dereferencing it. */
   if (!atomic)
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
   if (!def || depth >= R300_COMPUTE_DETECT_MAX_DEPTH)
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
            if (is_ssbo_atomic(intr->intrinsic)) {
               /* Only the load-op form is the candidate counter; a
                * compare-and-swap counts toward the tally but leaves atomic
                * NULL, so the exactly-one gate plus the NULL guard reject it. */
               if (intr->intrinsic == nir_intrinsic_ssbo_atomic) {
                  atomic = intr;
                  atomic_block = block;
               }
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
   if (!atomic)
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
               if (is_ssbo_atomic(intr->intrinsic)) {
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
            if (is_ssbo_atomic(intr->intrinsic)) {
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
   if (!def || depth >= R300_COMPUTE_DETECT_MAX_DEPTH)
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
   if (!def || depth >= R300_COMPUTE_DETECT_MAX_DEPTH)
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
            } else if (is_ssbo_atomic(intr->intrinsic)) {
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

   /* Every SSBO load must be a tap leaf of the reduction tree.  A stray
    * load_ssbo outside the tree (load_count > taps) is an input the box-3
    * multi-TEX lowering does not sample, so the kernel is not faithfully the
    * recognized neighborhood convolution; reject rather than mis-replay. */
   if (load_count != (unsigned)taps)
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

/* Quantized dot-product (DP4) detector.  Mirrors the binary-map two-load pattern
 * with the ALU op set narrowed to the float dot opcodes: store_ssbo's value is
 * the def of an nir_op_fdot{2,3,4} whose two sources are the defs of two distinct
 * load_ssbo intrinsics -- out[gid] = dot(in_a[gid], in_b[gid]).
 *
 * On RS482 this lowers to the US fragment ALU DP4 instruction.  The dot runs in
 * FP24, byte-exact for <= 7-bit-magnitude (quantized) operands (4*127^2 = 64516
 * < 2^17, hardware-confirmed) and the ordinary FP24-precise float dot otherwise.
 *
 * Discriminator from every prior admitted shape: a dot opcode (NOT in
 * binary_map_op_admitted's simple-binary set, so binary-map rejects it) with
 * exactly two load_ssbo inputs and no atomic / loop / if.  identity-map's store
 * value is a load directly; binary-map's a simple binary op; multitap's an
 * add-reduction of >= 3 load leaves; blend-acc / ZPASS carry an atomic; multipass
 * a loop; predicated-store an if. */
static bool
dp4_op_admitted(uint16_t op, uint8_t *components)
{
   switch (op) {
   case nir_op_fdot2: *components = 2; return true;
   case nir_op_fdot3: *components = 3; return true;
   case nir_op_fdot4: *components = 4; return true;
   default:
      return false;
   }
}

void
r300_nir_detect_dp4_pattern(const nir_shader *s,
                            struct r300_compute_dp4_pattern *out)
{
   out->is_dp4               = false;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;
   out->dot_op               = 0;
   out->components           = 0;

   const nir_intrinsic_instr *store  = NULL;
   const nir_intrinsic_instr *load_a = NULL;
   const nir_intrinsic_instr *load_b = NULL;
   unsigned store_count = 0, load_count = 0, atomic_count = 0;
   bool has_loop = false, in_if = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               has_loop = true;
            if (p->type == nir_cf_node_if)
               in_if = true;
         }
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
            } else if (is_ssbo_atomic(intr->intrinsic)) {
               atomic_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2 || atomic_count != 0 ||
       has_loop || in_if)
      return;
   if (!store->src[0].ssa)
      return;

   /* The dot result is carried back as an RGBA8 integer-encode: R300 has no FP32
    * render target (hardware-confirmed -- an FP32 color FBO is incomplete), so
    * an IEEE-754 float dot cannot be written exactly.  The admissible shape is
    * therefore the UINT-output dot, out_uint[gid] = uint(dot(a,b)), whose store
    * value is f2u32(fdot(a,b)) -- the integer reads back exactly from the encode.
    * A plain float-output dot is NOT admissible.  Require the f2u32 cast and
    * unwrap it to the underlying dot op. */
   const nir_alu_instr *cast = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!cast || cast->op != nir_op_f2u32)
      return;
   const nir_alu_instr *alu = nir_def_as_alu_or_null(cast->src[0].src.ssa);
   if (!alu)
      return;
   uint8_t comps = 0;
   if (!dp4_op_admitted(alu->op, &comps))
      return;

   /* The dot's two inputs must be the two collected load_ssbo defs, order-
    * independent -- the same two-input test the binary-map detector uses. */
   if (nir_op_infos[alu->op].num_inputs != 2)
      return;
   const nir_def *s0 = alu->src[0].src.ssa;
   const nir_def *s1 = alu->src[1].src.ssa;
   if (!((s0 == &load_a->def && s1 == &load_b->def) ||
         (s0 == &load_b->def && s1 == &load_a->def)))
      return;

   out->dot_op     = (uint16_t)alu->op;
   out->components = comps;
   /* Capture constant binding indices when present; the orchestrator's
    * descriptor-set layout fallback picks the first-three STORAGE_BUFFER
    * bindings when these stay at the defaults (same policy as binary-map). */
   if (nir_src_is_const(load_a->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load_a->src[0]);
   if (nir_src_is_const(load_b->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load_b->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_dp4 = true;
}

/* Resolve one component of a Hamilton permutation vec4 to a signed channel of
 * the base quaternion q2.  The component is q2.channel or -q2.channel, possibly
 * reached through an nir_op_mov (what nir_channel emits) and/or an nir_op_fneg;
 * compose the swizzles through each so the result is the lane of q2 the
 * component ultimately reads and whether it is negated.  Returns false if the
 * component is anything other than a +/- channel of base. */
static bool
qmul_signed_b_channel(const nir_def *base, const nir_alu_src *as,
                      unsigned *chan_out, bool *neg_out)
{
   bool neg = false;
   unsigned swz = as->swizzle[0];
   nir_def *d = as->src.ssa;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(d);
   if (alu && alu->op == nir_op_fneg) {
      neg = true;
      swz = alu->src[0].swizzle[swz];
      d = alu->src[0].src.ssa;
      alu = nir_def_as_alu_or_null(d);
   }
   if (alu && alu->op == nir_op_mov) {
      swz = alu->src[0].swizzle[swz];
      d = alu->src[0].src.ssa;
   }
   if (d != base)
      return false;

   *chan_out = swz;
   *neg_out = neg;
   return true;
}

void
r300_nir_detect_qmul_pattern(const nir_shader *s,
                             struct r300_compute_qmul_pattern *out)
{
   out->is_qmul              = false;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *store  = NULL;
   const nir_intrinsic_instr *load_a = NULL;
   const nir_intrinsic_instr *load_b = NULL;
   unsigned store_count = 0, load_count = 0, atomic_count = 0;
   bool has_loop = false, in_if = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               has_loop = true;
            if (p->type == nir_cf_node_if)
               in_if = true;
         }
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
            } else if (is_ssbo_atomic(intr->intrinsic)) {
               atomic_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2 || atomic_count != 0 ||
       has_loop || in_if)
      return;
   if (!store->src[0].ssa)
      return;
   /* The product writes the whole quaternion; a partial-lane store would carry
    * render-target bytes in the unwritten lanes. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   /* Store value assembles the four product lanes. */
   const nir_alu_instr *vec = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!vec || vec->op != nir_op_vec4)
      return;

   /* The four Hamilton rows in (w,x,y,z) layout: for output lane i, the
    * (q2 channel, sign) of each of the four permuted dot operands.  Verified
    * against the catalog self-check (1,2,3,4)*(5,6,7,8) = (-60,12,30,24):
    *   w = q1 . ( x2,-y2,-z2,-w2)
    *   x = q1 . ( y2, x2, w2,-z2)
    *   y = q1 . ( z2,-w2, x2, y2)
    *   z = q1 . ( w2, z2,-y2, x2). */
   static const struct { uint8_t chan; bool neg; } hamilton[4][4] = {
      { {0, false}, {1, true},  {2, true},  {3, true}  },
      { {1, false}, {0, false}, {3, false}, {2, true}  },
      { {2, false}, {3, true},  {0, false}, {1, false} },
      { {3, false}, {2, false}, {1, true},  {0, false} },
   };

   const nir_def *q1 = &load_a->def;
   const nir_def *q2 = &load_b->def;

   for (unsigned lane = 0; lane < 4; lane++) {
      const nir_alu_instr *dot = nir_def_as_alu_or_null(vec->src[lane].src.ssa);
      if (!dot || dot->op != nir_op_fdot4)
         return;

      /* One dot input is q1 used directly; the other is the permuted q2.  fdot
       * is commutative, so accept either operand order. */
      const nir_alu_src *q1_src = NULL, *perm_src = NULL;
      for (unsigned k = 0; k < 2; k++) {
         if (dot->src[k].src.ssa == q1)
            q1_src = &dot->src[k];
         else
            perm_src = &dot->src[k];
      }
      if (!q1_src || !perm_src)
         return;

      /* q1 must be dotted identity-swizzled (the Hamilton rows permute q2, not
       * q1), and the permuted q2 must enter the dot directly: a non-identity
       * swizzle on either operand would change the product. */
      for (unsigned c = 0; c < 4; c++)
         if (q1_src->swizzle[c] != c || perm_src->swizzle[c] != c)
            return;

      const nir_alu_instr *perm = nir_def_as_alu_or_null(perm_src->src.ssa);
      if (!perm || perm->op != nir_op_vec4)
         return;
      for (unsigned j = 0; j < 4; j++) {
         unsigned chan;
         bool neg;
         if (!qmul_signed_b_channel(q2, &perm->src[j], &chan, &neg))
            return;
         if (chan != hamilton[lane][j].chan || neg != hamilton[lane][j].neg)
            return;
      }
   }

   if (nir_src_is_const(load_a->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load_a->src[0]);
   if (nir_src_is_const(load_b->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load_b->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_qmul = true;
}

/* One lane of a permutation row: which channel of the base quaternion the lane
 * reads, and whether it is negated.  Named (not anonymous) so the Hamilton and
 * conj-composed rotation tables share a type with the OMUL matcher helpers that
 * take a row as a parameter; two separate anonymous struct declarations are
 * incompatible pointer types in C. */
struct r300_perm_entry { uint8_t chan; bool neg; };

/* The Hamilton permutation rows in (w,x,y,z) layout, shared by the QROTATE and
 * OMUL matchers below; identical to the table inlined in the QMUL detector. */
static const struct r300_perm_entry r300_hamilton_rows[4][4] = {
   { {0, false}, {1, true},  {2, true},  {3, true}  },
   { {1, false}, {0, false}, {3, false}, {2, true}  },
   { {2, false}, {3, true},  {0, false}, {1, false} },
   { {3, false}, {2, false}, {1, true},  {0, false} },
};

/* Resolve an alu_src to a signed channel (base.chan or -base.chan) reached
 * through an nir_op_mov / nir_op_fneg, discovering the base def.  Always sets
 * the outputs; the caller checks that the base is the one it expects. */
static void
resolve_signed_channel(const nir_alu_src *as, const nir_def **base,
                       unsigned *chan, bool *neg)
{
   bool n = false;
   unsigned swz = as->swizzle[0];
   nir_def *d = as->src.ssa;

   /* Unwrap a chain of nir_op_fneg / nir_op_mov, toggling the sign through each
    * fneg.  The rotation sandwich folds conj(q) into the outer permutation as a
    * double fneg (the conjugate's negate composed with the Hamilton row's), so a
    * single-level unwrap would stop at the inner fneg instead of reaching q. */
   for (;;) {
      const nir_alu_instr *alu = nir_def_as_alu_or_null(d);
      if (alu && alu->op == nir_op_fneg) {
         n = !n;
         swz = alu->src[0].swizzle[swz];
         d = alu->src[0].src.ssa;
         continue;
      }
      if (alu && alu->op == nir_op_mov) {
         swz = alu->src[0].swizzle[swz];
         d = alu->src[0].src.ssa;
         continue;
      }
      break;
   }
   *base = d;
   *chan = swz;
   *neg = n;
}

/* The outer permutation rows of the rotation sandwich: perm_i(conj(q)) expressed
 * over q's channels, the Hamilton row composed with the conjugate's sign (negate
 * on x,y,z).  The inner rows are the plain Hamilton rows (r300_hamilton_rows)
 * applied to embed(v), where channel 0 of a row means the constant 0. */
static const struct r300_perm_entry qrotate_outer_rows[4][4] = {
   { {0, false}, {1, false}, {2, false}, {3, false} },
   { {1, true},  {0, false}, {3, true},  {2, false} },
   { {2, true},  {3, false}, {0, false}, {1, true}  },
   { {3, true},  {2, true},  {1, false}, {0, false} },
};

void
r300_nir_detect_qrotate_pattern(const nir_shader *s,
                                struct r300_compute_qrotate_pattern *out)
{
   out->is_qrotate          = false;
   out->input_q_ssbo_binding = 0;
   out->input_v_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *store  = NULL;
   const nir_intrinsic_instr *load_a = NULL;
   const nir_intrinsic_instr *load_b = NULL;
   unsigned store_count = 0, load_count = 0, atomic_count = 0;
   bool has_loop = false, in_if = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               has_loop = true;
            if (p->type == nir_cf_node_if)
               in_if = true;
         }
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
            } else if (is_ssbo_atomic(intr->intrinsic)) {
               atomic_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2 || atomic_count != 0 ||
       has_loop || in_if)
      return;
   if (!store->src[0].ssa)
      return;
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   /* The compiler folds embed(v) (the scalar-part 0 into the inner permutations)
    * and conj(q) (a double fneg into the outer permutations), so the sandwich
    * reaches here as two nested products over the raw q and v loads, not over
    * explicit embed/conj vec4s.  Match the folded form directly. */
   const nir_alu_instr *outer = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!outer || outer->op != nir_op_vec4)
      return;

   /* Outer: out[lane] = dot(t, P_lane) where P_lane references q's channels with
    * the conj-composed signs (qrotate_outer_rows) and t -- the inner product --
    * is the common other operand. */
   const nir_def *t = NULL, *qd = NULL;
   for (unsigned lane = 0; lane < 4; lane++) {
      const nir_alu_instr *dot = nir_def_as_alu_or_null(outer->src[lane].src.ssa);
      if (!dot || dot->op != nir_op_fdot4)
         return;
      const nir_alu_src *perm_src = NULL, *t_src = NULL;
      const nir_def *base = NULL;
      for (unsigned k = 0; k < 2 && !perm_src; k++) {
         const nir_alu_instr *cand = nir_def_as_alu_or_null(dot->src[k].src.ssa);
         if (!cand || cand->op != nir_op_vec4)
            continue;
         const nir_def *b0 = NULL;
         bool shared = true;
         for (unsigned j = 0; j < 4; j++) {
            const nir_def *b;
            unsigned c;
            bool n;
            resolve_signed_channel(&cand->src[j], &b, &c, &n);
            if (j == 0)
               b0 = b;
            else if (b != b0)
               shared = false;
         }
         if (shared && (b0 == &load_a->def || b0 == &load_b->def)) {
            perm_src = &dot->src[k];
            t_src = &dot->src[1 - k];
            base = b0;
         }
      }
      if (!perm_src)
         return;
      for (unsigned c = 0; c < 4; c++)
         if (perm_src->swizzle[c] != c || t_src->swizzle[c] != c)
            return;
      const nir_alu_instr *perm = nir_def_as_alu_or_null(perm_src->src.ssa);
      for (unsigned j = 0; j < 4; j++) {
         const nir_def *b;
         unsigned c;
         bool n;
         resolve_signed_channel(&perm->src[j], &b, &c, &n);
         if (b != base || c != qrotate_outer_rows[lane][j].chan ||
             n != qrotate_outer_rows[lane][j].neg)
            return;
      }
      if (lane == 0) {
         t = t_src->src.ssa;
         qd = base;
      } else if (t_src->src.ssa != t || base != qd) {
         return;
      }
   }

   /* Inner: t[lane] = dot(q, P_lane), the Hamilton row applied to embed(v) --
    * channel 0 of the row is the constant 0 (the scalar part), the others are
    * v.(chan-1) with the row's sign. */
   const nir_alu_instr *inner = nir_def_as_alu_or_null(t);
   if (!inner || inner->op != nir_op_vec4)
      return;
   const nir_def *vd = (qd == &load_a->def) ? &load_b->def : &load_a->def;
   for (unsigned lane = 0; lane < 4; lane++) {
      const nir_alu_instr *dot = nir_def_as_alu_or_null(inner->src[lane].src.ssa);
      if (!dot || dot->op != nir_op_fdot4)
         return;
      const nir_alu_src *perm_src = NULL, *q_src = NULL;
      for (unsigned k = 0; k < 2; k++) {
         if (dot->src[k].src.ssa == qd)
            q_src = &dot->src[k];
         else
            perm_src = &dot->src[k];
      }
      if (!q_src || !perm_src)
         return;
      for (unsigned c = 0; c < 4; c++)
         if (q_src->swizzle[c] != c || perm_src->swizzle[c] != c)
            return;
      const nir_alu_instr *perm = nir_def_as_alu_or_null(perm_src->src.ssa);
      if (!perm || perm->op != nir_op_vec4)
         return;
      for (unsigned j = 0; j < 4; j++) {
         uint8_t hc = r300_hamilton_rows[lane][j].chan;
         bool hn = r300_hamilton_rows[lane][j].neg;
         if (hc == 0) {
            nir_def *cd = perm->src[j].src.ssa;
            if (!nir_def_is_const(cd) ||
                nir_def_as_load_const(cd)->value[perm->src[j].swizzle[0]].f32 != 0.0f)
               return;
         } else {
            const nir_def *b;
            unsigned c;
            bool n;
            resolve_signed_channel(&perm->src[j], &b, &c, &n);
            if (b != vd || c != (unsigned)(hc - 1) || n != hn)
               return;
         }
      }
   }

   const nir_intrinsic_instr *q_load = (qd == &load_a->def) ? load_a : load_b;
   const nir_intrinsic_instr *v_load = (qd == &load_a->def) ? load_b : load_a;
   if (nir_src_is_const(q_load->src[0]))
      out->input_q_ssbo_binding = nir_src_as_uint(q_load->src[0]);
   if (nir_src_is_const(v_load->src[0]))
      out->input_v_ssbo_binding = nir_src_as_uint(v_load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_qrotate = true;
}

/* Collect the lone store_ssbo and load_ssbo of a one-input / one-output kernel,
 * rejecting any loop, nested if, atomic, second load, or second store.  The
 * single-lane quaternion ops (QCONJ, QNORM) share this preamble: one input
 * quaternion, one output, no control flow.  Returns true with *store and *load
 * set only when the shape is exactly one full-width store of one load. */
static bool
collect_unary_ssbo_shape(const nir_shader *s,
                         const nir_intrinsic_instr **store_out,
                         const nir_intrinsic_instr **load_out)
{
   const nir_intrinsic_instr *store = NULL, *load = NULL;
   unsigned store_count = 0, load_count = 0, atomic_count = 0;
   bool has_loop = false, in_if = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               has_loop = true;
            if (p->type == nir_cf_node_if)
               in_if = true;
         }
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
            } else if (is_ssbo_atomic(intr->intrinsic)) {
               atomic_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 1 || atomic_count != 0 ||
       has_loop || in_if)
      return false;
   if (!store->src[0].ssa)
      return false;
   /* A partial-lane store would leave render-target bytes in the unwritten
    * lanes; the quaternion result is the whole vec4. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return false;

   *store_out = store;
   *load_out = load;
   return true;
}

void
r300_nir_detect_qconj_pattern(const nir_shader *s,
                              struct r300_compute_qconj_pattern *out)
{
   out->is_qconj            = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;

   const nir_intrinsic_instr *store, *load;
   if (!collect_unary_ssbo_shape(s, &store, &load))
      return;

   /* Store value = vec4(a.x, -a.y, -a.z, -a.w): the quaternion conjugate keeps
    * the scalar lane and negates the three vector lanes.  Each vec4 lane must be
    * a signed channel of the single load -- lane 0 is +channel 0, lanes 1..3 are
    * -channel lane.  resolve_signed_channel unwraps the fneg/mov the compiler
    * leaves on a negated lane (the same helper the rotation sandwich uses). */
   const nir_alu_instr *vec = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!vec || vec->op != nir_op_vec4)
      return;
   for (unsigned lane = 0; lane < 4; lane++) {
      const nir_def *base;
      unsigned chan;
      bool neg;
      resolve_signed_channel(&vec->src[lane], &base, &chan, &neg);
      if (base != &load->def || chan != lane || neg != (lane != 0))
         return;
   }

   if (nir_src_is_const(load->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_qconj = true;
}

void
r300_nir_detect_qnorm_pattern(const nir_shader *s,
                              struct r300_compute_qnorm_pattern *out)
{
   out->is_qnorm            = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;

   const nir_intrinsic_instr *store, *load;
   if (!collect_unary_ssbo_shape(s, &store, &load))
      return;

   /* Store value = vec4(dot(a, a)): the squared norm broadcast to four lanes so
    * the substrate's vec4 FP16 readback carries it (the kernel reads lane 0).
    * After CSE the four vec4 lanes reference one scalar fdot4 def whose two
    * operands are the same load, identity-swizzled.  Match that splat. */
   const nir_alu_instr *vec = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!vec || vec->op != nir_op_vec4)
      return;
   const nir_def *scalar = vec->src[0].src.ssa;
   for (unsigned lane = 0; lane < 4; lane++)
      if (vec->src[lane].src.ssa != scalar || vec->src[lane].swizzle[0] != 0)
         return;
   const nir_alu_instr *dot = nir_def_as_alu_or_null(scalar);
   if (!dot || dot->op != nir_op_fdot4)
      return;
   if (dot->src[0].src.ssa != &load->def || dot->src[1].src.ssa != &load->def)
      return;
   for (unsigned c = 0; c < 4; c++)
      if (dot->src[0].swizzle[c] != c || dot->src[1].swizzle[c] != c)
         return;

   if (nir_src_is_const(load->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_qnorm = true;
}

/* True if `def` is a vec4 whose four lanes are the signed channels of `base`
 * given by row[] (chan, neg), each reached through resolve_signed_channel (so
 * the fneg/mov the compiler folds onto a negated lane is unwrapped).  The
 * permutation tables are the Hamilton rows (a plain product) or the conj-
 * composed rotation rows (a product against a conjugated operand). */
static bool
omul_match_perm(const nir_def *def, const nir_def *base,
                const struct r300_perm_entry row[4])
{
   const nir_alu_instr *v = nir_def_as_alu_or_null(def);
   if (!v || v->op != nir_op_vec4)
      return false;
   for (unsigned j = 0; j < 4; j++) {
      const nir_def *b;
      unsigned c;
      bool n;
      resolve_signed_channel(&v->src[j], &b, &c, &n);
      if (b != base || c != row[j].chan || n != row[j].neg)
         return false;
   }
   return true;
}

/* Match a Hamilton-lane dot of an identity-swizzled `id_base` load against a
 * permutation vec4 over `perm_base` given by row[].  Both operands enter the
 * fdot4 identity-swizzled (the permutation lives inside the vec4); fdot is
 * commutative, so accept either operand order. */
static bool
omul_match_id_perm_dot(const nir_def *dot_def,
                       const nir_def *id_base, const nir_def *perm_base,
                       const struct r300_perm_entry row[4])
{
   const nir_alu_instr *dot = nir_def_as_alu_or_null(dot_def);
   if (!dot || dot->op != nir_op_fdot4)
      return false;
   for (unsigned k = 0; k < 2; k++) {
      const nir_alu_src *id_src = &dot->src[k];
      const nir_alu_src *perm_src = &dot->src[1 - k];
      if (id_src->src.ssa != id_base)
         continue;
      bool id_ident = true, perm_ident = true;
      for (unsigned c = 0; c < 4; c++) {
         if (id_src->swizzle[c] != c)
            id_ident = false;
         if (perm_src->swizzle[c] != c)
            perm_ident = false;
      }
      if (id_ident && perm_ident &&
          omul_match_perm(perm_src->src.ssa, perm_base, row))
         return true;
   }
   return false;
}

/* Match a Hamilton-lane dot whose BOTH operands are permutation vec4s -- the
 * conj(d)*b lane of the octonion product, where conj(d) folds to a vec4 (the
 * Hamilton row 0 over d, the conjugate pattern) instead of staying a plain load.
 * Accept either operand order; require both identity-swizzled into the dot. */
static bool
omul_match_two_perm_dot(const nir_def *dot_def,
                        const nir_def *base1,
                        const struct r300_perm_entry row1[4],
                        const nir_def *base2,
                        const struct r300_perm_entry row2[4])
{
   const nir_alu_instr *dot = nir_def_as_alu_or_null(dot_def);
   if (!dot || dot->op != nir_op_fdot4)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (dot->src[0].swizzle[c] != c || dot->src[1].swizzle[c] != c)
         return false;
   const nir_def *o0 = dot->src[0].src.ssa, *o1 = dot->src[1].src.ssa;
   return (omul_match_perm(o0, base1, row1) && omul_match_perm(o1, base2, row2)) ||
          (omul_match_perm(o1, base1, row1) && omul_match_perm(o0, base2, row2));
}

/* Match one half of the octonion product: a vec4 of four Hamilton-lane dots
 * combined by `combine` (fsub for the lower half a*c - conj(d)*b, fadd for the
 * upper d*a + b*conj(c)).  The first product P is an identity-load Hamilton dot
 * (p_id against the Hamilton rows of p_perm).  The second product Q is either
 * the two-perm conj(d)*b form (q_is_two_perm: conj(d) = Hamilton row 0 over
 * q_conj_base, against the Hamilton rows of q_perm) or another identity-load
 * Hamilton dot over the q_row table (q_id against q_row of q_perm). */
static bool
omul_match_half(const nir_def *store_val, nir_op combine,
                const nir_def *p_id, const nir_def *p_perm,
                const nir_def *q_id, const nir_def *q_perm, bool q_is_two_perm,
                const nir_def *q_conj_base,
                const struct r300_perm_entry q_row[4])
{
   const nir_alu_instr *top = nir_def_as_alu_or_null(store_val);
   if (!top || top->op != combine)
      return false;
   const nir_alu_instr *pvec = nir_def_as_alu_or_null(top->src[0].src.ssa);
   const nir_alu_instr *qvec = nir_def_as_alu_or_null(top->src[1].src.ssa);
   if (!pvec || pvec->op != nir_op_vec4 || !qvec || qvec->op != nir_op_vec4)
      return false;
   for (unsigned lane = 0; lane < 4; lane++) {
      if (!omul_match_id_perm_dot(pvec->src[lane].src.ssa, p_id, p_perm,
                                  r300_hamilton_rows[lane]))
         return false;
      if (q_is_two_perm) {
         if (!omul_match_two_perm_dot(qvec->src[lane].src.ssa,
                                      q_conj_base, r300_hamilton_rows[0],
                                      q_perm, r300_hamilton_rows[lane]))
            return false;
      } else {
         const struct r300_perm_entry *row =
            q_row == NULL ? r300_hamilton_rows[lane] : qrotate_outer_rows[lane];
         if (!omul_match_id_perm_dot(qvec->src[lane].src.ssa, q_id, q_perm, row))
            return false;
      }
   }
   return true;
}

void
r300_nir_detect_omul_pattern(const nir_shader *s,
                             struct r300_compute_omul_pattern *out)
{
   out->is_omul                = false;
   out->input_a_ssbo_binding   = 0;
   out->input_b_ssbo_binding   = 0;
   out->input_c_ssbo_binding   = 0;
   out->input_d_ssbo_binding   = 0;
   out->output_lo_ssbo_binding = 0;
   out->output_hi_ssbo_binding = 0;

   const nir_intrinsic_instr *store_lo = NULL, *store_hi = NULL;
   const nir_intrinsic_instr *load[4] = { NULL, NULL, NULL, NULL };
   unsigned store_count = 0, load_count = 0, atomic_count = 0;
   bool has_loop = false, in_if = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         for (const nir_cf_node *p = block->cf_node.parent; p; p = p->parent) {
            if (p->type == nir_cf_node_loop)
               has_loop = true;
            if (p->type == nir_cf_node_if)
               in_if = true;
         }
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               if (store_count == 0)
                  store_lo = intr;
               else if (store_count == 1)
                  store_hi = intr;
               store_count++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               if (load_count < 4)
                  load[load_count] = intr;
               load_count++;
            } else if (is_ssbo_atomic(intr->intrinsic)) {
               atomic_count++;
            }
         }
      }
   }

   if (store_count != 2 || load_count != 4 || atomic_count != 0 ||
       has_loop || in_if)
      return;
   if (!store_lo->src[0].ssa || !store_hi->src[0].ssa)
      return;
   for (unsigned i = 0; i < 2; i++) {
      const nir_intrinsic_instr *st = i == 0 ? store_lo : store_hi;
      if (nir_intrinsic_has_write_mask(st) &&
          nir_intrinsic_write_mask(st) != BITFIELD_MASK(st->num_components))
         return;
   }

   /* Canonical input order: the kernel reads a,b,c,d in declaration order, so
    * the four collected loads are a,b,c,d.  o_lo = a*c - conj(d)*b is a Hamilton
    * difference (a identity-dotted against the Hamilton rows of c, minus conj(d)
    * -- a vec4, Hamilton row 0 over d -- dotted against the Hamilton rows of b).
    * o_hi = d*a + b*conj(c) is a Hamilton sum (d against the rows of a, plus b
    * against the conj-composed rotation rows of c). */
   const nir_def *a = &load[0]->def, *b = &load[1]->def;
   const nir_def *c = &load[2]->def, *d = &load[3]->def;

   if (!omul_match_half(store_lo->src[0].ssa, nir_op_fsub,
                        a, c, NULL, b, true, d, NULL))
      return;
   if (!omul_match_half(store_hi->src[0].ssa, nir_op_fadd,
                        d, a, b, c, false, NULL, qrotate_outer_rows[0]))
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(load[2]->src[0]))
      out->input_c_ssbo_binding = nir_src_as_uint(load[2]->src[0]);
   if (nir_src_is_const(load[3]->src[0]))
      out->input_d_ssbo_binding = nir_src_as_uint(load[3]->src[0]);
   if (nir_src_is_const(store_lo->src[1]))
      out->output_lo_ssbo_binding = nir_src_as_uint(store_lo->src[1]);
   if (nir_src_is_const(store_hi->src[1]))
      out->output_hi_ssbo_binding = nir_src_as_uint(store_hi->src[1]);
   out->is_omul = true;
}

/* Collect the load_ssbo and store_ssbo intrinsics of a kernel in program order,
 * with the no-loop / no-if / no-atomic checks the octonion elementwise ops share.
 * loads[]/stores[] are filled up to their caps; the counts report the totals so
 * the caller can require an exact shape. */
static void
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

static bool
store_is_full_width(const nir_intrinsic_instr *st)
{
   if (!st->src[0].ssa)
      return false;
   return !nir_intrinsic_has_write_mask(st) ||
          nir_intrinsic_write_mask(st) == BITFIELD_MASK(st->num_components);
}

/* True if `alu` (fadd or fsub) reduces the two identity-swizzled loads x and y.
 * fsub is non-commutative so x must be the minuend; fadd accepts either order. */
static bool
oaddsub_operands_match(const nir_alu_instr *alu, const nir_def *x,
                       const nir_def *y)
{
   for (unsigned c = 0; c < 4; c++)
      if (alu->src[0].swizzle[c] != c || alu->src[1].swizzle[c] != c)
         return false;
   const nir_def *s0 = alu->src[0].src.ssa, *s1 = alu->src[1].src.ssa;
   if (alu->op == nir_op_fsub)
      return s0 == x && s1 == y;
   return (s0 == x && s1 == y) || (s0 == y && s1 == x);
}

void
r300_nir_detect_oaddsub_pattern(const nir_shader *s,
                                struct r300_compute_oaddsub_pattern *out)
{
   out->is_oaddsub             = false;
   out->is_sub                 = false;
   out->input_a_ssbo_binding   = 0;
   out->input_b_ssbo_binding   = 0;
   out->input_c_ssbo_binding   = 0;
   out->input_d_ssbo_binding   = 0;
   out->output_lo_ssbo_binding = 0;
   out->output_hi_ssbo_binding = 0;

   const nir_intrinsic_instr *load[4], *store[2];
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 4, &nload, store, 2, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 4 || nstore != 2 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]) || !store_is_full_width(store[1]))
      return;

   /* o_lo = a (+|-) c, o_hi = b (+|-) d -- the octonion componentwise add/sub,
    * with the two halves combined by the SAME op.  Loads are read in declaration
    * order a,b,c,d (load[0..3]); the lower half reduces a with c, the upper b
    * with d. */
   const nir_alu_instr *lo = nir_def_as_alu_or_null(store[0]->src[0].ssa);
   const nir_alu_instr *hi = nir_def_as_alu_or_null(store[1]->src[0].ssa);
   if (!lo || !hi)
      return;
   if (lo->op != nir_op_fadd && lo->op != nir_op_fsub)
      return;
   if (hi->op != lo->op)
      return;
   if (!oaddsub_operands_match(lo, &load[0]->def, &load[2]->def) ||
       !oaddsub_operands_match(hi, &load[1]->def, &load[3]->def))
      return;

   out->is_sub = (lo->op == nir_op_fsub);
   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(load[2]->src[0]))
      out->input_c_ssbo_binding = nir_src_as_uint(load[2]->src[0]);
   if (nir_src_is_const(load[3]->src[0]))
      out->input_d_ssbo_binding = nir_src_as_uint(load[3]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_lo_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   if (nir_src_is_const(store[1]->src[1]))
      out->output_hi_ssbo_binding = nir_src_as_uint(store[1]->src[1]);
   out->is_oaddsub = true;
}

void
r300_nir_detect_oconj_pattern(const nir_shader *s,
                              struct r300_compute_oconj_pattern *out)
{
   out->is_oconj               = false;
   out->input_a_ssbo_binding   = 0;
   out->input_b_ssbo_binding   = 0;
   out->output_lo_ssbo_binding = 0;
   out->output_hi_ssbo_binding = 0;

   const nir_intrinsic_instr *load[2], *store[2];
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 2, &nload, store, 2, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 2 || nstore != 2 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]) || !store_is_full_width(store[1]))
      return;

   /* The octonion conjugate conj((a,b)) = (conj(a), -b): the lower half keeps
    * a's scalar lane and negates its three vector lanes (a vec4 of signed
    * channels), the upper half negates ALL of b (a whole-vector fneg). */
   const nir_alu_instr *lo = nir_def_as_alu_or_null(store[0]->src[0].ssa);
   if (!lo || lo->op != nir_op_vec4)
      return;
   for (unsigned lane = 0; lane < 4; lane++) {
      const nir_def *base;
      unsigned chan;
      bool neg;
      resolve_signed_channel(&lo->src[lane], &base, &chan, &neg);
      if (base != &load[0]->def || chan != lane || neg != (lane != 0))
         return;
   }
   const nir_alu_instr *hi = nir_def_as_alu_or_null(store[1]->src[0].ssa);
   if (!hi || hi->op != nir_op_fneg)
      return;
   if (hi->src[0].src.ssa != &load[1]->def)
      return;
   for (unsigned c = 0; c < 4; c++)
      if (hi->src[0].swizzle[c] != c)
         return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_lo_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   if (nir_src_is_const(store[1]->src[1]))
      out->output_hi_ssbo_binding = nir_src_as_uint(store[1]->src[1]);
   out->is_oconj = true;
}

void
r300_nir_detect_onorm_pattern(const nir_shader *s,
                              struct r300_compute_onorm_pattern *out)
{
   out->is_onorm               = false;
   out->input_a_ssbo_binding   = 0;
   out->input_b_ssbo_binding   = 0;
   out->output_ssbo_binding    = 0;

   const nir_intrinsic_instr *load[2], *store[1];
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 2, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 2 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]))
      return;

   /* The octonion squared norm |(a,b)|^2 = dot(a,a) + dot(b,b), broadcast to four
    * lanes (the kernel reads lane 0).  The store is a vec4 splat of one scalar
    * fadd of the two self-dots. */
   const nir_alu_instr *vec = nir_def_as_alu_or_null(store[0]->src[0].ssa);
   if (!vec || vec->op != nir_op_vec4)
      return;
   const nir_def *scalar = vec->src[0].src.ssa;
   for (unsigned lane = 0; lane < 4; lane++)
      if (vec->src[lane].src.ssa != scalar || vec->src[lane].swizzle[0] != 0)
         return;
   const nir_alu_instr *sum = nir_def_as_alu_or_null(scalar);
   if (!sum || sum->op != nir_op_fadd)
      return;
   const nir_alu_instr *d0 = nir_def_as_alu_or_null(sum->src[0].src.ssa);
   const nir_alu_instr *d1 = nir_def_as_alu_or_null(sum->src[1].src.ssa);
   if (!d0 || d0->op != nir_op_fdot4 || !d1 || d1->op != nir_op_fdot4)
      return;
   /* Each dot is a self-dot of one load; the two halves cover both loads in
    * either order (fadd is commutative). */
   const nir_def *b0 = d0->src[0].src.ssa, *b1 = d1->src[0].src.ssa;
   if (d0->src[1].src.ssa != b0 || d1->src[1].src.ssa != b1)
      return;
   bool ok = (b0 == &load[0]->def && b1 == &load[1]->def) ||
             (b0 == &load[1]->def && b1 == &load[0]->def);
   if (!ok)
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_onorm = true;
}

/* True if `def` is fdot4(base, base) (a self-dot), both operands identity-
 * swizzled -- the per-quaternion term of an octonion squared norm. */
static bool
odiv_is_self_dot(const nir_def *def, const nir_def *base)
{
   const nir_alu_instr *a = nir_def_as_alu_or_null(def);
   if (!a || a->op != nir_op_fdot4)
      return false;
   if (a->src[0].src.ssa != base || a->src[1].src.ssa != base)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (a->src[0].swizzle[c] != c || a->src[1].swizzle[c] != c)
         return false;
   return true;
}

/* True if `src` reads `scalar` broadcast across all four lanes (scalar.xxxx) --
 * the reciprocal r applied to a quaternion in inv(y) = conj(y)*r. */
static bool
odiv_is_scalar_splat(const nir_alu_src *src, const nir_def *scalar)
{
   if (src->src.ssa != scalar)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (src->swizzle[c] != 0)
         return false;
   return true;
}

void
r300_nir_detect_odiv_pattern(const nir_shader *s,
                             struct r300_compute_odiv_pattern *out)
{
   out->is_odiv                = false;
   out->input_xlo_ssbo_binding = 0;
   out->input_xhi_ssbo_binding = 0;
   out->input_ylo_ssbo_binding = 0;
   out->input_yhi_ssbo_binding = 0;
   out->output_lo_ssbo_binding = 0;
   out->output_hi_ssbo_binding = 0;

   const nir_intrinsic_instr *load[4], *store[2];
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 4, &nload, store, 2, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 4 || nstore != 2 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]) || !store_is_full_width(store[1]))
      return;

   /* Loads in declaration order: x = (xlo,xhi), y = (ylo,yhi). */
   const nir_def *xlo = &load[0]->def, *xhi = &load[1]->def;
   const nir_def *ylo = &load[2]->def, *yhi = &load[3]->def;

   /* Find the reciprocal r = 1 / (dot(ylo,ylo) + dot(yhi,yhi)) and the two
    * inverse halves c = conj(ylo)*r, d = (-yhi)*r by walking the ALU.  The
    * reciprocal of the octonion squared norm is the signature of division: only
    * a divide produces it, and matching it (plus the OMUL of x against the
    * scaled conjugate) makes the admission sound. */
   const nir_def *r = NULL;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op != nir_op_fdiv)
               continue;
            nir_def *num = alu->src[0].src.ssa;
            if (!nir_def_is_const(num) ||
                nir_def_as_load_const(num)->value[alu->src[0].swizzle[0]].f32 != 1.0f)
               continue;
            const nir_alu_instr *denom = nir_def_as_alu_or_null(alu->src[1].src.ssa);
            if (!denom || denom->op != nir_op_fadd)
               continue;
            const nir_def *t0 = denom->src[0].src.ssa, *t1 = denom->src[1].src.ssa;
            if ((odiv_is_self_dot(t0, ylo) && odiv_is_self_dot(t1, yhi)) ||
                (odiv_is_self_dot(t0, yhi) && odiv_is_self_dot(t1, ylo)))
               r = &alu->def;
         }
      }
   }
   if (!r)
      return;

   /* c = conj(ylo) * r.xxxx and d = fneg(yhi) * r.xxxx -- the inverse halves. */
   const nir_def *c = NULL, *d = NULL;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *mul = nir_instr_as_alu(instr);
            if (mul->op != nir_op_fmul)
               continue;
            const nir_alu_src *other;
            if (odiv_is_scalar_splat(&mul->src[1], r))
               other = &mul->src[0];
            else if (odiv_is_scalar_splat(&mul->src[0], r))
               other = &mul->src[1];
            else
               continue;
            for (unsigned k = 0; k < 4; k++)
               if (other->swizzle[k] != k)
                  goto next_mul;
            if (omul_match_perm(other->src.ssa, ylo, r300_hamilton_rows[0]))
               c = &mul->def;          /* conj(ylo)*r */
            else {
               const nir_alu_instr *ng = nir_def_as_alu_or_null(other->src.ssa);
               if (ng && ng->op == nir_op_fneg && ng->src[0].src.ssa == yhi) {
                  bool id = true;
                  for (unsigned k = 0; k < 4; k++)
                     if (ng->src[0].swizzle[k] != k)
                        id = false;
                  if (id)
                     d = &mul->def;    /* (-yhi)*r */
               }
            }
            next_mul:;
         }
      }
   }
   if (!c || !d)
      return;

   /* The eight-wide product x * inv(y), where inv(y) = (c, d): exactly the OMUL
    * fold with x's halves as a,b and the inverse halves as c,d.  o_lo = a*c -
    * conj(d)*b, o_hi = d*a + b*conj(c). */
   if (!omul_match_half(store[0]->src[0].ssa, nir_op_fsub,
                        xlo, c, NULL, xhi, true, d, NULL))
      return;
   if (!omul_match_half(store[1]->src[0].ssa, nir_op_fadd,
                        d, xlo, xhi, c, false, NULL, qrotate_outer_rows[0]))
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_xlo_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_xhi_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(load[2]->src[0]))
      out->input_ylo_ssbo_binding = nir_src_as_uint(load[2]->src[0]);
   if (nir_src_is_const(load[3]->src[0]))
      out->input_yhi_ssbo_binding = nir_src_as_uint(load[3]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_lo_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   if (nir_src_is_const(store[1]->src[1]))
      out->output_hi_ssbo_binding = nir_src_as_uint(store[1]->src[1]);
   out->is_odiv = true;
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
