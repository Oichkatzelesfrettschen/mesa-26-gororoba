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

#include <math.h>
#include <string.h>

#include "r300_compute_admission.h"
#include "r300_compute_admission_match.h"

#include "compiler/nir/nir.h"
#include "util/format/u_formats.h"
#include "util/macros.h"

/* Recursion bound for the def-graph walkers below.  A balanced integer
 * add-reduction tree of depth D has up to 2^D load leaves; depth 8 admits up
 * to 256 leaves, far past the three-tap box kernel and the predicate chains the
 * detectors actually match, so the bound never truncates a real match.  It
 * exists only to keep the walk total on adversarial or malformed NIR. */
#define R300_COMPUTE_DETECT_MAX_DEPTH 8u
#define R300_COMPUTE_STORE_ADDR_MAX_DEPTH 8u
#define R300_COMPUTE_OFFSET_EQ_MAX_DEPTH 8u

static bool
identity_map_debug_enabled(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *flags = getenv("R3V_DEBUG");
      if (!flags)
         flags = getenv("R300VK_DEBUG");
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

static bool store_ssbo_addr_def_is_supported(const nir_def *def,
                                             unsigned depth);

static bool
store_ssbo_addr_src_is_supported(nir_src src, unsigned depth)
{
   if (!src.ssa)
      return false;
   if (nir_src_is_const(src))
      return true;
   return store_ssbo_addr_def_is_supported(src.ssa, depth + 1);
}

static bool
store_ssbo_addr_alu_input_is_const(const nir_alu_instr *alu, unsigned input)
{
   return input < nir_op_infos[alu->op].num_inputs &&
          nir_src_is_const(alu->src[input].src);
}

static bool
store_ssbo_addr_def_is_supported(const nir_def *def, unsigned depth)
{
   if (!def || depth > R300_COMPUTE_STORE_ADDR_MAX_DEPTH)
      return false;
   if (nir_def_is_const(def))
      return true;

   const nir_instr *instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_intrinsic) {
      const nir_intrinsic_instr *intr =
         nir_instr_as_intrinsic((nir_instr *)instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_load_global_invocation_id:
      case nir_intrinsic_load_global_invocation_index:
      case nir_intrinsic_load_base_global_invocation_id:
      case nir_intrinsic_load_vulkan_descriptor:
         return true;
      default:
         return false;
      }
   }

   if (instr->type != nir_instr_type_alu)
      return false;

   const nir_alu_instr *alu = nir_instr_as_alu((nir_instr *)instr);
   const unsigned num_inputs = nir_op_infos[alu->op].num_inputs;

   if (nir_op_is_vec_or_mov(alu->op)) {
      for (unsigned i = 0; i < num_inputs; i++) {
         if (!store_ssbo_addr_src_is_supported(alu->src[i].src, depth + 1))
            return false;
      }
      return true;
   }

   switch (alu->op) {
   case nir_op_iadd:
   case nir_op_isub:
   case nir_op_u2u32:
   case nir_op_u2u64:
   case nir_op_i2i32:
   case nir_op_i2i64:
      for (unsigned i = 0; i < num_inputs; i++) {
         if (!store_ssbo_addr_src_is_supported(alu->src[i].src, depth + 1))
            return false;
      }
      return true;
   case nir_op_imul:
      return (store_ssbo_addr_alu_input_is_const(alu, 0) &&
              store_ssbo_addr_src_is_supported(alu->src[1].src, depth + 1)) ||
             (store_ssbo_addr_alu_input_is_const(alu, 1) &&
              store_ssbo_addr_src_is_supported(alu->src[0].src, depth + 1));
   case nir_op_ishl:
      return store_ssbo_addr_src_is_supported(alu->src[0].src, depth + 1) &&
             store_ssbo_addr_alu_input_is_const(alu, 1);
   default:
      return false;
   }
}

static bool
store_ssbo_address_is_supported(const nir_intrinsic_instr *intr,
                                const char **detail)
{
   if (!store_ssbo_addr_src_is_supported(intr->src[1], 0)) {
      *detail = "store_ssbo buffer index";
      return false;
   }
   if (!store_ssbo_addr_src_is_supported(intr->src[2], 0)) {
      *detail = "store_ssbo byte offset";
      return false;
   }
   return true;
}

/* Structural (not SSA-identity) equality of two scalar offset expressions.
 * Gates the identity-map and binary-map detectors on genuine pointwise index
 * equivalence between a store's offset and a load's offset.
 *
 * nir_lower_explicit_io with nir_address_format_32bit_index_offset emits an
 * independent address-computation chain per load_ssbo/store_ssbo intrinsic,
 * so even out[gid] = in[gid] leaves the load offset def and the store offset
 * def as distinct nir_def objects that nir_opt_cse does not fold across (see
 * the callers below for the full rationale).  A raw `==` on the two defs --
 * the old offset_eq -- is therefore false for every real kernel and cannot be
 * gated on.
 *
 * This instead walks both chains in lockstep, resolving through mov/vecN
 * wrappers first (nir_scalar_chase_movs), then requiring each node's shape to
 * match on both sides: the same ALU op with pairwise-equal operands, the same
 * leaf invocation-id-family intrinsic at the same vector component, or equal
 * compile-time constants.  Two chains that are opcode-for-opcode identical
 * down to a shared root are declared equal despite being different SSA defs,
 * because gl_GlobalInvocationID (and its sibling system values below) reads
 * the same value at every use within one invocation, however many times the
 * lowering separately materialized the load.
 *
 * Any divergence -- a stride mismatch, an extra permutation node, a
 * transposed .x/.y component pick, or a node this walker does not
 * recognize -- makes some level fail to match and the walk returns false.
 * That is the conservative, sound direction: "not proven equal" is always
 * treated as "reject," never as "equal," so a scatter/gather kernel this
 * walker cannot fully reason about falls through to the safe no-op compute
 * lifecycle instead of being silently mis-lowered. */
static bool
offset_scalar_semantically_equal(nir_scalar a, nir_scalar b, unsigned depth)
{
   if (depth > R300_COMPUTE_OFFSET_EQ_MAX_DEPTH)
      return false;

   a = nir_scalar_chase_movs(a);
   b = nir_scalar_chase_movs(b);

   if (nir_scalar_is_const(a) || nir_scalar_is_const(b)) {
      return nir_scalar_is_const(a) && nir_scalar_is_const(b) &&
             nir_scalar_as_uint(a) == nir_scalar_as_uint(b);
   }

   const nir_instr *ia = nir_def_instr(a.def);
   const nir_instr *ib = nir_def_instr(b.def);
   if (ia->type != ib->type)
      return false;

   if (ia->type == nir_instr_type_intrinsic) {
      if (nir_scalar_intrinsic_op(a) != nir_scalar_intrinsic_op(b))
         return false;
      switch (nir_scalar_intrinsic_op(a)) {
      case nir_intrinsic_load_global_invocation_id:
      case nir_intrinsic_load_global_invocation_index:
      case nir_intrinsic_load_base_global_invocation_id:
      case nir_intrinsic_load_workgroup_id:
      case nir_intrinsic_load_local_invocation_id:
      case nir_intrinsic_load_local_invocation_index:
      case nir_intrinsic_load_num_workgroups:
         /* Root reached.  Includes pre-lower global-id forms and the
          * workgroup/local-id roots nir_lower_compute_system_values emits
          * when has_cs_global_id is false.  Component must still match. */
         return a.comp == b.comp;
      default:
         /* Any other intrinsic reads state this walker cannot prove
          * pointwise-invariant across the two chains (a buffer load, a
          * differently-indexed push constant, etc.); reject rather than
          * risk a false match. */
         return false;
      }
   }

   if (ia->type != nir_instr_type_alu)
      return false;

   const nir_alu_instr *alu_a = nir_scalar_as_alu(a);
   const nir_alu_instr *alu_b = nir_scalar_as_alu(b);
   if (alu_a->op != alu_b->op)
      return false;

   /* Positional (not commutativity-aware) operand comparison: sound for
    * every op including non-commutative ones (isub, ishl), and adequate in
    * practice since both chains come from the same lowering pass applying
    * the same formula to load and store alike. */
   const unsigned num_inputs = nir_op_infos[alu_a->op].num_inputs;
   for (unsigned i = 0; i < num_inputs; i++) {
      /* nir_scalar_chase_alu_src requires sized ALU inputs of size 1;
       * vector/horizontal ops (fdot, fdph, ...) are not scalar-safe. */
      if (nir_op_infos[alu_a->op].input_sizes[i] != 0 &&
          nir_op_infos[alu_a->op].input_sizes[i] != 1)
         return false;
      if (nir_ssa_alu_instr_src_components(alu_a, i) != 1 ||
          nir_ssa_alu_instr_src_components(alu_b, i) != 1)
         return false;
      nir_scalar src_a = nir_scalar_chase_alu_src(a, i);
      nir_scalar src_b = nir_scalar_chase_alu_src(b, i);
      if (!offset_scalar_semantically_equal(src_a, src_b, depth + 1))
         return false;
   }
   return true;
}

/* Walk the kernel and detect the identity-map structural pattern:
 * exactly one store_ssbo whose value source is the SSA def of exactly one
 * load_ssbo.  The load and store binding sources are recorded when they are
 * compile-time constants; descriptor-lowered Vulkan kernels carry opaque
 * handles that the dispatch resolves through the descriptor-set layout.
 *
 * The index-equivalence between the load and the store (both indexed by
 * gl_GlobalInvocationID.xy) is asserted via offset_scalar_semantically_equal,
 * a structural walk that is sound against separately-lowered but
 * pointwise-equal address chains (see that function for why raw SSA
 * identity cannot be used here).  Detection only gates the LOWERING branch --
 * a mis-detected kernel falls back to the no-op pipeline path. */
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
    * offset_eq gates the identity shape on genuine pointwise index
    * equivalence rather than raw SSA-def identity (see
    * offset_scalar_semantically_equal for why the latter always reads false
    * post-nir_lower_explicit_io).  Without this gate a scatter kernel
    * out[g(i)] = in[i] would be admitted and the fullscreen-FS lowering
    * would compute out[i] = in[i] (the pass cannot honor a scatter index g)
    * -- a silent value miscompute. */
   const bool offset_eq =
      offset_scalar_semantically_equal(nir_get_scalar(store->src[2].ssa, 0),
                                       nir_get_scalar(load->src[1].ssa, 0), 0) &&
      nir_intrinsic_offset_shift(store) == nir_intrinsic_offset_shift(load);
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
   if (!offset_eq) {
      if (identity_map_debug_enabled())
         fprintf(stderr, "ident_map: detect-skip offset mismatch\n");
      return;
   }

   /* The store's write mask must cover every component.  The downstream
    * carriers copy whole elements sized from util_format_get_blocksize (the
    * default R8G8B8A8 path and the opt-in R32G32B32A32 FP32x4 path selected in
    * r3v_identity_map_replay_format), so a partial-mask store -- one writing
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

   /* Both operands must be per-element values of the same width.  The binary-map
    * orchestrator wraps in_a and in_b as per-element samplers spanning the whole
    * raster extent, so a 1-component operand multiplied against a 4-component one
    * -- out[gid] = a[gid] * s, the broadcast scalar -- is not a binary map: its
    * narrow buffer holds one value, and sampling it per-element reads past the
    * end.  That shape is QFMUL's (the scalar rides the fragment constant file);
    * the width asymmetry hands the kernel to r300_nir_detect_qfmul_pattern. */
   if (load_a->def.num_components != load_b->def.num_components)
      return;

   /* Require a full store write mask, as the identity-map detector does: the
    * binary-map carrier copies whole elements, so a partial-lane store would be
    * transported with carrier bytes in the unwritten lanes. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;
   /* Like the identity-map detector, this detector gates on genuine
    * pointwise index equivalence rather than raw SSA-def identity: the
    * store's offset must match BOTH load offsets structurally (see
    * offset_scalar_semantically_equal).  Without this gate a scatter kernel
    * out[g(i)] = f(a[i], b[i]) would be admitted and the fullscreen-FS
    * lowering would compute out[i] = f(a[i], b[i]) regardless of any
    * scatter index g -- a silent value miscompute. */
   if (!offset_scalar_semantically_equal(nir_get_scalar(store->src[2].ssa, 0),
                                         nir_get_scalar(load_a->src[1].ssa, 0),
                                         0) ||
       !offset_scalar_semantically_equal(nir_get_scalar(store->src[2].ssa, 0),
                                         nir_get_scalar(load_b->src[1].ssa, 0),
                                         0))
      return;

   out->is_binary_map = true;
   out->alu_op = (uint16_t)alu->op;
   /* Normalize to operand order: input_a is the op's left operand (src[0]),
    * input_b is the right operand (src[1]).  For commutative ops the order
    * does not affect correctness; for isub/fsub it must match the op.  When
    * the ba case fires the compiler emitted the loads in the reverse of the
    * operand order, so remap before capturing binding indices. */
   const nir_intrinsic_instr *op_lhs = ab ? load_a : load_b;
   const nir_intrinsic_instr *op_rhs = ab ? load_b : load_a;
   /* Capture constant binding indices when present; the orchestrator's
    * descriptor-set layout fallback picks the first three compute-visible
    * STORAGE_BUFFER bindings when these stay at the defaults. */
   if (nir_src_is_const(op_lhs->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(op_lhs->src[0]);
   if (nir_src_is_const(op_rhs->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(op_rhs->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding  = nir_src_as_uint(store->src[1]);
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->value_is_float = intrinsic_base_type_is_float(
      store, nir_op_infos[alu->op].output_type);
}

static bool
unary_alu_src_is_identity_load(const nir_alu_instr *alu, unsigned i,
                               const nir_def *load_def, unsigned components)
{
   if (alu->src[i].src.ssa != load_def)
      return false;
   for (unsigned c = 0; c < components; c++) {
      if (alu->src[i].swizzle[c] != c)
         return false;
   }
   return true;
}

/* A uniform constant operand of the affine ALU: either a literal float whose
 * value is known at pipeline-create time, or a push-constant scalar whose
 * value arrives at command-record time and is therefore recorded by byte
 * offset into the push window. */
struct unary_const_operand {
   bool     from_push;
   float    literal;       /* valid when !from_push */
   uint32_t push_offset;   /* push-window byte offset, when from_push */
};

/* Walk component comp of def through mov and vecN wrappers to the scalar
 * (def, component) pair it ultimately reads.  A broadcast of a scalar reaches
 * an ALU operand either as a swizzled direct read or wrapped in
 * vecN(s.x, s.x, ...) / mov chains depending on which builder or frontend
 * produced it; resolving both forms to the origin lets the uniformity test
 * compare origins instead of surface shapes. */
static const nir_def *
unary_resolve_scalar_origin(const nir_def *def, unsigned comp,
                            unsigned *out_comp)
{
   for (;;) {
      const nir_instr *parent = nir_def_instr(def);
      if (parent->type != nir_instr_type_alu)
         break;
      const nir_alu_instr *alu = nir_instr_as_alu((nir_instr *)parent);
      if (alu->op == nir_op_mov) {
         comp = alu->src[0].swizzle[comp];
         def = alu->src[0].src.ssa;
      } else if (nir_op_is_vec(alu->op)) {
         const nir_def *elem = alu->src[comp].src.ssa;
         comp = alu->src[comp].swizzle[0];
         def = elem;
      } else {
         break;
      }
   }
   *out_comp = comp;
   return def;
}

/* Match a uniform constant feeding ALU operand i.  The unary-map fragment
 * program broadcasts one c0/c1 scalar across the output lanes, so the operand
 * is uniform exactly when (literal case) every live lane reads the same value
 * or (push case) every live lane selects the same component of one
 * load_push_constant whose offset folds to a compile-time constant.  The push
 * offset must land a whole float inside the 128-byte window (the Vulkan
 * minimum push size and the window the dispatch replay binds at FS CONST[0],
 * eight vec4 slots); an offset past that has no constant-file address. */
static bool
unary_alu_src_const_operand(const nir_alu_instr *alu, unsigned i,
                            unsigned components,
                            struct unary_const_operand *op)
{
   if (nir_src_is_const(alu->src[i].src)) {
      const float first = nir_src_comp_as_float(alu->src[i].src,
                                                alu->src[i].swizzle[0]);
      for (unsigned c = 1; c < components; c++) {
         if (nir_src_comp_as_float(alu->src[i].src,
                                   alu->src[i].swizzle[c]) != first)
            return false;
      }
      op->from_push = false;
      op->literal = first;
      op->push_offset = 0;
      return true;
   }

   /* Resolve each live lane through mov and vecN splats to its scalar origin
    * (a builder- or SPIR-V-expanded broadcast reaches the ALU as
    * vec4(c.x, c.x, c.x, c.x) rather than a swizzled direct read); the
    * operand is uniform exactly when every lane lands on the same component
    * of the same def. */
   unsigned comp = 0;
   const nir_def *base =
      unary_resolve_scalar_origin(alu->src[i].src.ssa,
                                  alu->src[i].swizzle[0], &comp);
   for (unsigned c = 1; c < components; c++) {
      unsigned lane_comp = 0;
      const nir_def *lane =
         unary_resolve_scalar_origin(alu->src[i].src.ssa,
                                     alu->src[i].swizzle[c], &lane_comp);
      if (lane != base || lane_comp != comp)
         return false;
   }

   const nir_instr *parent = nir_def_instr(base);
   if (parent->type != nir_instr_type_intrinsic)
      return false;
   const nir_intrinsic_instr *push =
      nir_instr_as_intrinsic((nir_instr *)parent);
   if (push->intrinsic != nir_intrinsic_load_push_constant)
      return false;
   if (push->def.bit_size != 32 || !nir_src_is_const(push->src[0]))
      return false;
   const uint64_t byte_offset = (uint64_t)nir_intrinsic_base(push) +
                                nir_src_as_uint(push->src[0]) +
                                (uint64_t)comp * 4;
   if (byte_offset + 4 > 128)
      return false;
   op->from_push = true;
   op->literal = 0.0f;
   op->push_offset = (uint32_t)byte_offset;
   return true;
}

/* Single-input affine unary-map detector.  Mirrors the identity-map walk (one
 * store_ssbo + one load_ssbo) but the store value is an affine ALU of the load
 * with constant scale c0 and bias c1, not the load def itself.  Three forms:
 *   ffma(load, c0, c1)               -> c0, c1 captured
 *   fadd(fmul(load, c0), c1)         -> c0, c1 captured
 *   fmul(load, c0)                   -> c1 = 0
 *   fadd(load, c1)                   -> c0 = 1
 * Each form's mul/add operands are commutative, so both operand orders match.
 * Pure read-only NIR walk. */
void
r300_nir_detect_unary_map(const nir_shader *s,
                          struct r300_compute_unary_map_pattern *out)
{
   out->is_unary_map        = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;
   out->input_ssbo_binding_valid  = false;
   out->output_ssbo_binding_valid = false;
   out->mul_const           = 1.0f;
   out->add_const           = 0.0f;
   out->mul_const_from_push = false;
   out->add_const_from_push = false;
   out->mul_const_push_offset = 0;
   out->add_const_push_offset = 0;
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

   if (store_count != 1 || load_count != 1)
      return;
   if (!store->src[0].ssa)
      return;

   /* The store value must be an ALU op (a plain load def is the identity-map
    * case r300_nir_detect_identity_map handles). */
   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu)
      return;

   const nir_def *load_def = &load->def;
   const unsigned components = store->num_components;
   struct unary_const_operand c0 = { .from_push = false, .literal = 1.0f };
   struct unary_const_operand c1 = { .from_push = false, .literal = 0.0f };
   bool matched = false;

   if (alu->op == nir_op_ffma) {
      /* ffma(a, b, c) = a*b + c: one of a,b is the load, the other is c0; c=c1. */
      if (unary_alu_src_is_identity_load(alu, 0, load_def, components) &&
          unary_alu_src_const_operand(alu, 1, components, &c0) &&
          unary_alu_src_const_operand(alu, 2, components, &c1)) {
         matched = true;
      } else if (unary_alu_src_is_identity_load(alu, 1, load_def, components) &&
                 unary_alu_src_const_operand(alu, 0, components, &c0) &&
                 unary_alu_src_const_operand(alu, 2, components, &c1)) {
         matched = true;
      }
   } else if (alu->op == nir_op_fmul) {
      /* fmul(load, c0): pure scale, c1 = 0. */
      if (unary_alu_src_is_identity_load(alu, 0, load_def, components) &&
          unary_alu_src_const_operand(alu, 1, components, &c0)) {
         matched = true;
      } else if (unary_alu_src_is_identity_load(alu, 1, load_def, components) &&
                 unary_alu_src_const_operand(alu, 0, components, &c0)) {
         matched = true;
      }
   } else if (alu->op == nir_op_fadd) {
      /* fadd(X, c1): X is either the load (c0 = 1) or fmul(load, c0). */
      int x_src = -1;
      if (unary_alu_src_const_operand(alu, 1, components, &c1)) {
         x_src = 0;
      } else if (unary_alu_src_const_operand(alu, 0, components, &c1)) {
         x_src = 1;
      }
      if (x_src >= 0 &&
          unary_alu_src_is_identity_load(alu, (unsigned)x_src, load_def,
                                         components)) {
         matched = true;
      } else if (x_src >= 0) {
         const nir_alu_instr *mul = nir_def_as_alu_or_null(alu->src[x_src].src.ssa);
         if (mul && mul->op == nir_op_fmul) {
            if (unary_alu_src_is_identity_load(mul, 0, load_def, components) &&
                unary_alu_src_const_operand(mul, 1, components, &c0)) {
               matched = true;
            } else if (unary_alu_src_is_identity_load(mul, 1, load_def, components) &&
                       unary_alu_src_const_operand(mul, 0, components, &c0)) {
               matched = true;
            }
         }
      }
   }

   if (!matched)
      return;

   /* Full write mask, as the identity/binary detectors require: the carrier
    * copies whole elements, so a partial-lane store would carry stale bytes. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   /* Two carrier widths exist: vec4 (R32G32B32A32_FLOAT sampler -> FP16x4 RT)
    * and scalar (R32_FLOAT sampler -> X lane of the FP16x4 RT).  The affine
    * fragment program is channel-uniform, so any width whose carrier transports
    * whole elements is admissible; 2/3-component stores stay rejected because
    * no carrier policy covers them. */
   if ((store->num_components != 4 && store->num_components != 1) ||
       store->src[0].ssa->bit_size != 32 ||
       !intrinsic_base_type_is_float(store, nir_op_infos[alu->op].output_type))
      return;

   /* Capture constant bindings when present; the orchestrator's positional
    * descriptor-set fallback resolves them otherwise (input 0, output 1; an
    * in-place kernel binds the same buffer to both). */
   if (nir_src_is_const(load->src[0])) {
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
      out->input_ssbo_binding_valid = true;
   }
   if (nir_src_is_const(store->src[1])) {
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
      out->output_ssbo_binding_valid = true;
   }
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->value_is_float = intrinsic_base_type_is_float(
      store, nir_op_infos[alu->op].output_type);
   out->mul_const = c0.from_push ? 1.0f : c0.literal;
   out->add_const = c1.from_push ? 0.0f : c1.literal;
   out->mul_const_from_push = c0.from_push;
   out->add_const_from_push = c1.from_push;
   out->mul_const_push_offset = c0.from_push ? (uint16_t)c0.push_offset : 0;
   out->add_const_push_offset = c1.from_push ? (uint16_t)c1.push_offset : 0;
   out->is_unary_map = true;
}

/* Image-store forms that count as color-export candidates or competing
 * stores.  image_heap_store is a peer of image_store and must count. */
static bool
image_store_op(nir_intrinsic_op op)
{
   return op == nir_intrinsic_image_deref_store ||
          op == nir_intrinsic_image_store ||
          op == nir_intrinsic_bindless_image_store ||
          op == nir_intrinsic_image_heap_store;
}

/* Image-coordinate walker for RT-export: the coordinate must depend on
 * gl_GlobalInvocationID (or index) and may only combine that id with
 * constants and simple ALU (add/sub/mul-by-const/shift/casts/vecs).
 * Pure constants, descriptor loads, and unknown sources fail closed. */
static bool image_coord_def_is_gid_derived(const nir_def *def, unsigned depth,
                                           bool *saw_gid);

static bool
image_coord_src_is_gid_derived(nir_src src, unsigned depth, bool *saw_gid)
{
   if (!src.ssa)
      return false;
   if (nir_src_is_const(src))
      return true;
   return image_coord_def_is_gid_derived(src.ssa, depth + 1, saw_gid);
}

static bool
image_coord_def_is_gid_derived(const nir_def *def, unsigned depth,
                               bool *saw_gid)
{
   if (!def || depth > R300_COMPUTE_STORE_ADDR_MAX_DEPTH)
      return false;
   if (nir_def_is_const(def))
      return true;

   const nir_instr *instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_intrinsic) {
      const nir_intrinsic_instr *intr =
         nir_instr_as_intrinsic((nir_instr *)instr);
      switch (intr->intrinsic) {
      case nir_intrinsic_load_global_invocation_id:
      case nir_intrinsic_load_global_invocation_index:
      case nir_intrinsic_load_base_global_invocation_id:
         *saw_gid = true;
         return true;
      default:
         /* Descriptor loads and other system values are not coordinates. */
         return false;
      }
   }

   if (instr->type != nir_instr_type_alu)
      return false;

   const nir_alu_instr *alu = nir_instr_as_alu((nir_instr *)instr);
   const unsigned num_inputs = nir_op_infos[alu->op].num_inputs;

   if (nir_op_is_vec_or_mov(alu->op)) {
      for (unsigned i = 0; i < num_inputs; i++) {
         if (!image_coord_src_is_gid_derived(alu->src[i].src, depth + 1,
                                             saw_gid))
            return false;
      }
      return true;
   }

   switch (alu->op) {
   case nir_op_iadd:
   case nir_op_isub:
   case nir_op_u2u32:
   case nir_op_u2u64:
   case nir_op_i2i32:
   case nir_op_i2i64:
      for (unsigned i = 0; i < num_inputs; i++) {
         if (!image_coord_src_is_gid_derived(alu->src[i].src, depth + 1,
                                             saw_gid))
            return false;
      }
      return true;
   case nir_op_imul:
      return (store_ssbo_addr_alu_input_is_const(alu, 0) &&
              image_coord_src_is_gid_derived(alu->src[1].src, depth + 1,
                                             saw_gid)) ||
             (store_ssbo_addr_alu_input_is_const(alu, 1) &&
              image_coord_src_is_gid_derived(alu->src[0].src, depth + 1,
                                             saw_gid));
   case nir_op_ishl:
      return image_coord_src_is_gid_derived(alu->src[0].src, depth + 1,
                                            saw_gid) &&
             store_ssbo_addr_alu_input_is_const(alu, 1);
   default:
      return false;
   }
}

static bool
image_coord_is_gid_derived(nir_src coord)
{
   if (!coord.ssa)
      return false;

   /* Common image-store shape is a vec2/vec4 construction.  Require gid
    * participation in the first two components (X and Y); a coordinate that
    * only folds gid into Z/W is not a 2D RT-export address. */
   const nir_instr *instr = nir_def_instr(coord.ssa);
   if (instr->type == nir_instr_type_alu) {
      const nir_alu_instr *alu = nir_instr_as_alu((nir_instr *)instr);
      if (nir_op_is_vec_or_mov(alu->op) &&
          coord.ssa->num_components >= 2 &&
          nir_op_infos[alu->op].num_inputs >= 2) {
         for (unsigned c = 0; c < 2; c++) {
            bool saw_gid = false;
            if (!image_coord_src_is_gid_derived(alu->src[c].src, 0, &saw_gid) ||
                !saw_gid)
               return false;
         }
         return true;
      }
   }

   bool saw_gid = false;
   if (!image_coord_src_is_gid_derived(coord, 0, &saw_gid))
      return false;
   return saw_gid;
}

/* Format for image stores: prefer the intrinsic qualifier, then the
 * variable's image format when the intrinsic carries PIPE_FORMAT_NONE. */
static enum pipe_format
image_store_effective_format(const nir_intrinsic_instr *store,
                             const nir_variable *var)
{
   enum pipe_format fmt = nir_intrinsic_format(store);
   if (fmt != PIPE_FORMAT_NONE)
      return fmt;
   if (var && var->data.image.format != PIPE_FORMAT_NONE)
      return (enum pipe_format)var->data.image.format;
   return PIPE_FORMAT_NONE;
}

/* True when the deref chain indexes an array or struct element of the
 * image variable (dynamic descriptor-array or multi-image binding). */
static bool
image_deref_has_array_or_struct_index(const nir_deref_instr *deref)
{
   for (const nir_deref_instr *d = deref; d;
        d = nir_deref_instr_parent(d)) {
      if (d->deref_type == nir_deref_type_array ||
          d->deref_type == nir_deref_type_array_wildcard ||
          d->deref_type == nir_deref_type_ptr_as_array ||
          d->deref_type == nir_deref_type_struct)
         return true;
   }
   return false;
}

/* Admissible storage-image RT-export detector (see the header).  Read-only. */
void
r300_nir_detect_image_store_rt_export(
   const nir_shader *s, struct r300_image_store_rt_export_pattern *out)
{
   out->is_rt_exportable = false;
   out->image_binding = 0;
   out->image_binding_valid = false;

   const nir_intrinsic_instr *store = NULL;
   unsigned store_count = 0;
   bool disqualified = false;

   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            const nir_intrinsic_op op = intr->intrinsic;
            if (image_store_op(op)) {
               store = intr;
               store_count++;
               continue;
            }
            /* Image load/atomic (RMW or read-back the substrate cannot do),
             * or any competing scatter/atomic/barrier write, disqualifies. */
            const char *name = nir_intrinsic_infos[op].name;
            if ((strstr(name, "image") &&
                 (strstr(name, "load") || strstr(name, "atomic"))) ||
                strstr(name, "barrier") != NULL ||
                op == nir_intrinsic_store_ssbo ||
                op == nir_intrinsic_store_global ||
                op == nir_intrinsic_store_global_2x32 ||
                op == nir_intrinsic_ssbo_atomic ||
                op == nir_intrinsic_ssbo_atomic_swap ||
                op == nir_intrinsic_global_atomic ||
                op == nir_intrinsic_global_atomic_swap ||
                op == nir_intrinsic_global_atomic_2x32 ||
                op == nir_intrinsic_shared_atomic ||
                op == nir_intrinsic_shared_atomic_swap ||
                op == nir_intrinsic_deref_atomic ||
                op == nir_intrinsic_deref_atomic_swap) {
               disqualified = true;
               continue;
            }
            /* store_deref into global/shared is a competing side effect. */
            if (op == nir_intrinsic_store_deref) {
               nir_deref_instr *d = nir_src_as_deref(intr->src[0]);
               if (d && (d->modes & (nir_var_mem_global | nir_var_mem_shared |
                                     nir_var_mem_ssbo)))
                  disqualified = true;
            }
         }
      }
   }

   if (s->info.shared_size > 0)
      disqualified = true;
   if (disqualified || store_count != 1 || store == NULL)
      return;

   /* Metadata is read off the deref form; raw/bindless handles stay opaque. */
   if (store->intrinsic != nir_intrinsic_image_deref_store)
      return;

   nir_deref_instr *deref = nir_src_as_deref(store->src[0]);
   if (deref == NULL)
      return;
   const nir_variable *var = nir_deref_instr_get_variable(deref);
   if (var == NULL)
      return;

   /* Variable type must be a plain image (not an array-of-images). */
   if (glsl_type_is_array(var->type) || glsl_type_is_cmat(var->type))
      return;
   /* Dynamic array/struct indexing of the image binding is rejected. */
   if (image_deref_has_array_or_struct_index(deref))
      return;

   const struct glsl_type *itype = var->type;
   if (!glsl_type_is_image(itype) ||
       glsl_get_sampler_dim(itype) != GLSL_SAMPLER_DIM_2D ||
       glsl_sampler_type_is_array(itype))
      return;

   /* R8G8B8A8_UNORM only: direct colorbuffer export the identity carrier
    * already proves.  Prefer intrinsic format, else variable image format. */
   if (image_store_effective_format(store, var) != PIPE_FORMAT_R8G8B8A8_UNORM)
      return;

   /* Value is four 32-bit components with a full write mask (RGBA8 export). */
   if (store->num_components != 4 ||
       !store->src[3].ssa ||
       store->src[3].ssa->bit_size != 32)
      return;
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != 0xf)
      return;

   /* Coordinate is 2D and gid-derived. */
   if (!store->src[1].ssa || store->src[1].ssa->num_components < 2)
      return;
   if (!image_coord_is_gid_derived(store->src[1]))
      return;

   /* LOD (src[4]) must be constant zero when present. */
   if (store->src[4].ssa) {
      if (!nir_src_is_const(store->src[4]))
         return;
      if (nir_src_as_uint(store->src[4]) != 0)
         return;
   }

   out->image_binding = var->data.binding;
   out->image_binding_valid = true;
   out->is_rt_exportable = true;
}

/* The single-input transcendentals the unary-transcendental verb admits.  Each
 * is a native r300 US scalar ALU op (nir_to_rc.c lowers them to
 * RCP/RSQ/EX2/LG2/SIN/COS/FRC/ROUND); fsqrt is lowered to an RSQ/RCP form by the
 * backend.  fmul/fadd/ffma are deliberately absent -- those are the affine
 * unary_map, keeping the two detectors disjoint. */
bool
r300_nir_is_unary_transcendental_op(uint16_t op)
{
   switch ((nir_op)op) {
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt:
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_fsin:
   case nir_op_fcos:
   case nir_op_ffract:
   case nir_op_ffloor:
   case nir_op_fround_even:
      return true;
   default:
      return false;
   }
}

/* Single-input transcendental-map detector.  out[gid] = f(in[gid]): exactly one
 * store_ssbo whose value is a single-source transcendental ALU op of exactly one
 * load_ssbo def.  Mirrors the unary_map walk (one load + one store) but the ALU
 * op is a transcendental rather than the affine MAD chain, so the op set keeps
 * it disjoint from unary_map.  Pure read-only NIR walk. */
void
r300_nir_detect_unary_transcendental(
   const nir_shader *s,
   struct r300_compute_unary_transcendental_pattern *out)
{
   out->is_unary_transcendental   = false;
   out->alu_op                    = 0;
   out->input_ssbo_binding        = 0;
   out->output_ssbo_binding       = 0;
   out->input_ssbo_binding_valid  = false;
   out->output_ssbo_binding_valid = false;
   out->value_components          = 0;
   out->value_bit_size            = 0;

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

   if (store_count != 1 || load_count != 1)
      return;
   if (!store->src[0].ssa)
      return;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu)
      return;

   const unsigned components = store->num_components;
   nir_op effective_op;

   if (alu->op == nir_op_fdiv) {
      /* The reciprocal arm: GLSL 1.0/x reaches the classifier as fdiv(1.0, x),
       * not a bare frcp -- the classify clone folds constants but does not lower
       * fdiv.  A unit numerator over the identity load is the frcp the FS already
       * builds; a non-unit numerator is a scaled reciprocal, outside this verb. */
      struct unary_const_operand num = {0};
      if (!unary_alu_src_const_operand(alu, 0, components, &num) ||
          num.from_push || num.literal != 1.0f ||
          !unary_alu_src_is_identity_load(alu, 1, &load->def, components))
         return;
      effective_op = nir_op_frcp;
   } else {
      if (!r300_nir_is_unary_transcendental_op(alu->op) ||
          nir_op_infos[alu->op].num_inputs != 1 ||
          !unary_alu_src_is_identity_load(alu, 0, &load->def, components))
         return;
      effective_op = alu->op;
   }

   /* Full write mask: the carrier copies whole elements. */
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   /* Scalar float32 only: the FP16x4 RT carrier and the X-lane gather readback
    * (the same scalar carrier the unary_map verb uses) transport one float per
    * element.  Wider transcendental maps need their own carrier audit. */
   if (store->num_components != 1 ||
       store->src[0].ssa->bit_size != 32 ||
       !intrinsic_base_type_is_float(store, nir_op_infos[alu->op].output_type))
      return;

   if (nir_src_is_const(load->src[0])) {
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
      out->input_ssbo_binding_valid = true;
   }
   if (nir_src_is_const(store->src[1])) {
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
      out->output_ssbo_binding_valid = true;
   }
   out->alu_op = (uint16_t)effective_op;
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->is_unary_transcendental = true;
}

/* The two non-commutative transcendental binaries the binary-transcendental verb
 * admits.  fpow lowers to EX2(LG2(a)*b), fdiv to a*RCP(b); both are native to
 * the fragment ALU but absent from binary_map's commutative set. */
static bool
binary_transcendental_op_admitted(uint16_t op)
{
   switch ((nir_op)op) {
   case nir_op_fpow:
   case nir_op_fdiv:
      return true;
   default:
      return false;
   }
}

/* Two-input transcendental-map detector.  out[gid] = f(a[gid], b[gid]) for f in
 * {fpow, fdiv}: one store_ssbo whose value is a 2-input ALU op of two distinct
 * load_ssbo defs.  Order-preserving (src[0] -> input_a, src[1] -> input_b) since
 * neither op is commutative; a unit-numerator fdiv(1.0, x) fails here because its
 * numerator is a constant, not a load, and is handled by the unary reciprocal
 * arm.  Pure read-only NIR walk. */
void
r300_nir_detect_binary_transcendental(
   const nir_shader *s,
   struct r300_compute_binary_transcendental_pattern *out)
{
   out->is_binary_transcendental = false;
   out->alu_op                   = 0;
   out->input_a_ssbo_binding     = 0;
   out->input_b_ssbo_binding     = 0;
   out->output_ssbo_binding      = 0;
   out->value_components         = 0;
   out->value_bit_size           = 0;

   const nir_intrinsic_instr *store = NULL;
   const nir_intrinsic_instr *load0 = NULL;
   const nir_intrinsic_instr *load1 = NULL;
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
                  load0 = intr;
               else if (load_count == 1)
                  load1 = intr;
               load_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2)
      return;
   if (!store->src[0].ssa)
      return;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu || !binary_transcendental_op_admitted(alu->op))
      return;
   if (nir_op_infos[alu->op].num_inputs != 2)
      return;

   const unsigned components = store->num_components;
   const nir_def *s0 = alu->src[0].src.ssa;
   const nir_def *s1 = alu->src[1].src.ssa;

   /* Order-preserving operand binding: alu src[0] is input_a, src[1] is input_b.
    * Both must be the two distinct loads, with identity swizzles. */
   const nir_intrinsic_instr *a_load;
   const nir_intrinsic_instr *b_load;
   if (s0 == &load0->def && s1 == &load1->def) {
      a_load = load0;
      b_load = load1;
   } else if (s0 == &load1->def && s1 == &load0->def) {
      a_load = load1;
      b_load = load0;
   } else {
      return;
   }
   if (!unary_alu_src_is_identity_load(alu, 0, &a_load->def, components) ||
       !unary_alu_src_is_identity_load(alu, 1, &b_load->def, components))
      return;

   if (load0->def.num_components != load1->def.num_components)
      return;

   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   /* Float32, scalar or vec4: the dispatch carries a 1-component kernel through
    * the R32_FLOAT scalar path and a 4-component one through the R32G32B32A32
    * vec4 path.  Intermediate widths (2, 3) have no carrier and are rejected. */
   if ((store->num_components != 1 && store->num_components != 4) ||
       store->src[0].ssa->bit_size != 32 ||
       !intrinsic_base_type_is_float(store, nir_op_infos[alu->op].output_type))
      return;

   if (nir_src_is_const(a_load->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(a_load->src[0]);
   if (nir_src_is_const(b_load->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(b_load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->alu_op = (uint16_t)alu->op;
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->is_binary_transcendental = true;
}

/* The bitwise binaries the RB3D ROP logic op computes: AND, OR, XOR.  All three
 * commute, so the detector does not track operand order.  Shifts are not logic
 * ops and are deliberately absent. */
static bool
bitwise_logicop_op_admitted(uint16_t op)
{
   switch ((nir_op)op) {
   case nir_op_iand:
   case nir_op_ior:
   case nir_op_ixor:
      return true;
   default:
      return false;
   }
}

/* Two-input bitwise-map detector.  out[gid] = a[gid] OP b[gid] for OP in
 * {iand, ior, ixor}: one store_ssbo whose value is a 2-input bitwise ALU op of
 * two distinct load_ssbo defs.  Order-independent (the ops commute).  Scalar
 * uint32 only -- each element packs as one RGBA8 texel for the ROP logic op.
 * Disjoint from binary_map (iadd/imul/...) and binary_transcendental (fpow/fdiv)
 * by op set.  Pure read-only NIR walk. */
void
r300_nir_detect_bitwise_logicop(
   const nir_shader *s,
   struct r300_compute_bitwise_logicop_pattern *out)
{
   out->is_bitwise_logicop   = false;
   out->alu_op               = 0;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;
   out->value_components     = 0;
   out->value_bit_size       = 0;

   const nir_intrinsic_instr *store = NULL;
   const nir_intrinsic_instr *load0 = NULL;
   const nir_intrinsic_instr *load1 = NULL;
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
                  load0 = intr;
               else if (load_count == 1)
                  load1 = intr;
               load_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2)
      return;
   if (!store->src[0].ssa)
      return;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu || !bitwise_logicop_op_admitted(alu->op))
      return;
   if (nir_op_infos[alu->op].num_inputs != 2)
      return;

   const unsigned components = store->num_components;
   const nir_def *s0 = alu->src[0].src.ssa;
   const nir_def *s1 = alu->src[1].src.ssa;
   const nir_intrinsic_instr *a_load;
   const nir_intrinsic_instr *b_load;
   if (s0 == &load0->def && s1 == &load1->def) {
      a_load = load0;
      b_load = load1;
   } else if (s0 == &load1->def && s1 == &load0->def) {
      a_load = load1;
      b_load = load0;
   } else {
      return;
   }
   if (!unary_alu_src_is_identity_load(alu, 0, &a_load->def, components) ||
       !unary_alu_src_is_identity_load(alu, 1, &b_load->def, components))
      return;

   if (load0->def.num_components != load1->def.num_components)
      return;

   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   /* Scalar 32-bit only: one uint32 element per RGBA8 texel.  Wider widths would
    * need a multi-texel carrier the ROP path does not provide. */
   if (store->num_components != 1 || store->src[0].ssa->bit_size != 32)
      return;

   if (nir_src_is_const(a_load->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(a_load->src[0]);
   if (nir_src_is_const(b_load->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(b_load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->alu_op = (uint16_t)alu->op;
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->is_bitwise_logicop = true;
}

/* Constant-shift detector.  out[gid] = a[gid] << k (ishl), >> k unsigned (ushr),
 * or >> k signed (ishr): exactly one store_ssbo whose value is a 2-input shift
 * ALU op whose first source is the identity load and whose second source is a
 * scalar compile-time constant k in [1, 31].  ishr records is_arithmetic so the
 * carrier sign-extends; ishl/ushr record is_arithmetic false.  Scalar uint32 only
 * -- one element per RGBA8 texel for the byte-recombination carrier.  A variable
 * shift amount (the second source is a load, not a constant) and a constant
 * outside [1, 31] (k = 0 is the identity, k >= 32 is GLSL-undefined) leave
 * is_shift_logical false, so they stay UNKNOWN_SHAPE rather than produce a wrong
 * result.  Pure read-only NIR walk. */
void
r300_nir_detect_shift_logical(
   const nir_shader *s,
   struct r300_compute_shift_logical_pattern *out)
{
   out->is_shift_logical    = false;
   out->is_left             = false;
   out->is_arithmetic       = false;
   out->shift_amount        = 0;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;
   out->value_components    = 0;
   out->value_bit_size      = 0;

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

   if (store_count != 1 || load_count != 1)
      return;
   if (!store->src[0].ssa)
      return;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu)
      return;
   bool is_left = false, is_arithmetic = false;
   if (alu->op == nir_op_ishl)
      is_left = true;
   else if (alu->op == nir_op_ushr)
      is_left = false;
   else if (alu->op == nir_op_ishr)
      is_arithmetic = true;   /* right shift, sign-extending */
   else
      return;
   if (nir_op_infos[alu->op].num_inputs != 2)
      return;

   const unsigned components = store->num_components;
   if (!unary_alu_src_is_identity_load(alu, 0, &load->def, components))
      return;

   /* The shift amount must be a scalar compile-time constant in [1, 31]. */
   if (!nir_src_is_const(alu->src[1].src))
      return;
   const uint64_t k =
      nir_src_comp_as_uint(alu->src[1].src, alu->src[1].swizzle[0]);
   if (k < 1 || k > 31)
      return;

   if (store->num_components != 1 || store->src[0].ssa->bit_size != 32)
      return;
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   if (nir_src_is_const(load->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_left = is_left;
   out->is_arithmetic = is_arithmetic;
   out->shift_amount = (uint8_t)k;
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->is_shift_logical = true;
}

/* The shifts the variable-amount carrier admits.  All lower onto the per-element
 * 2^M lookup + multilimb multiply; ishr adds the per-element sign-extension fill
 * on top of the ushr result. */
static bool
shift_variable_op_admitted(uint16_t op)
{
   switch ((nir_op)op) {
   case nir_op_ishl:
   case nir_op_ushr:
   case nir_op_ishr:
      return true;
   default:
      return false;
   }
}

/* Variable-amount shift detector.  out[gid] = a[gid] << b[gid] (ishl) or
 * a[gid] >> b[gid] (ushr): one store_ssbo whose value is a 2-input shift ALU op
 * of two distinct load_ssbo defs -- src[0] the value a, src[1] the per-element
 * amount b, order-preserving since the operands are not interchangeable.  Scalar
 * uint32 only.  load_count == 2 separates this from the constant-shift detector
 * (one load + a literal amount); the op set separates it from binary_map.  Pure
 * read-only NIR walk. */
void
r300_nir_detect_shift_variable(
   const nir_shader *s,
   struct r300_compute_shift_variable_pattern *out)
{
   out->is_shift_variable   = false;
   out->is_left             = false;
   out->is_arithmetic       = false;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;
   out->value_components     = 0;
   out->value_bit_size       = 0;

   const nir_intrinsic_instr *store = NULL;
   const nir_intrinsic_instr *load0 = NULL;
   const nir_intrinsic_instr *load1 = NULL;
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
                  load0 = intr;
               else if (load_count == 1)
                  load1 = intr;
               load_count++;
            }
         }
      }
   }

   if (store_count != 1 || load_count != 2)
      return;
   if (!store->src[0].ssa)
      return;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!alu || !shift_variable_op_admitted(alu->op))
      return;
   if (nir_op_infos[alu->op].num_inputs != 2)
      return;
   const bool is_left = (alu->op == nir_op_ishl);
   const bool is_arithmetic = (alu->op == nir_op_ishr);

   const unsigned components = store->num_components;
   const nir_def *s0 = alu->src[0].src.ssa;   /* value a */
   const nir_def *s1 = alu->src[1].src.ssa;   /* amount b */
   const nir_intrinsic_instr *a_load;
   const nir_intrinsic_instr *b_load;
   if (s0 == &load0->def && s1 == &load1->def) {
      a_load = load0;
      b_load = load1;
   } else if (s0 == &load1->def && s1 == &load0->def) {
      a_load = load1;
      b_load = load0;
   } else {
      return;
   }
   if (!unary_alu_src_is_identity_load(alu, 0, &a_load->def, components) ||
       !unary_alu_src_is_identity_load(alu, 1, &b_load->def, components))
      return;

   if (load0->def.num_components != load1->def.num_components)
      return;
   if (nir_intrinsic_has_write_mask(store) &&
       nir_intrinsic_write_mask(store) != BITFIELD_MASK(store->num_components))
      return;

   if (store->num_components != 1 || store->src[0].ssa->bit_size != 32)
      return;

   if (nir_src_is_const(a_load->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(a_load->src[0]);
   if (nir_src_is_const(b_load->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(b_load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_left = is_left;
   out->is_arithmetic = is_arithmetic;
   out->value_components = store->num_components;
   out->value_bit_size = store->src[0].ssa->bit_size;
   out->is_shift_variable = true;
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

/* The maximum tap-leaf count the gather walker collects: box-3 is the only
 * realized kernel, but the tree walk caps collection rather than recursion so
 * an oversized reduction reports its true count and fails the == 3 check. */
#define R300_MULTITAP_MAX_TAPS 8

/* Collect the load_ssbo leaves of a pure integer add-reduction tree into
 * taps[].  An iadd node recurses into both inputs; a load_ssbo def is a tap
 * leaf worth 1; any other def (a non-load, non-iadd) makes the tree impure
 * and returns -1.  Depth-bounded like def_derives_from for the small kernels
 * the detector pattern-matches.  The collected prefix must pull from the same
 * SSBO: post-explicit_io the binding source is an opaque descriptor-chain def
 * shared between the taps (CSE folds the identical chains), so the test is
 * def equality, which also covers inline-constant bindings.  Leaves beyond the
 * bounded prefix are still counted; the caller rejects them by tap_total. */
static int
multitap_add_tree_taps(const nir_def *def,
                       const nir_intrinsic_instr **taps, unsigned *tap_count,
                       unsigned depth)
{
   if (!def || depth >= R300_COMPUTE_DETECT_MAX_DEPTH)
      return -1;
   const nir_intrinsic_instr *intr =
      nir_def_as_intrinsic_or_null((nir_def *)def);
   if (intr) {
      if (intr->intrinsic != nir_intrinsic_load_ssbo)
         return -1;
      /* Count every pure load leaf; store only the bounded prefix needed by
       * the exact box-3 detector.  Oversized pure reductions then fail the
       * tap_total == 3 check instead of being reported as impure trees. */
      if (*tap_count < R300_MULTITAP_MAX_TAPS) {
         if (*tap_count > 0 && taps[0]->src[0].ssa != intr->src[0].ssa)
            return -1;
         taps[(*tap_count)++] = intr;
      }
      return 1;
   }
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)def);
   if (!alu || alu->op != nir_op_iadd)
      return -1;
   const int l = multitap_add_tree_taps(alu->src[0].src.ssa, taps, tap_count,
                                        depth + 1);
   if (l < 0)
      return -1;
   const int r = multitap_add_tree_taps(alu->src[1].src.ssa, taps, tap_count,
                                        depth + 1);
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
/* Classify one tap's byte offset against the store's byte offset in the
 * DIRECT form: the tap offset is the store offset def itself (center) or
 * iadd(store_offset, +/-4) (edge).  Returns the box-3 bit (bit 0 for -4,
 * bit 1 for center, bit 2 for +4) or 0 for no match. */
static int
multitap_tap_bit_direct(const nir_def *offset, const nir_def *base)
{
   if (offset == base)
      return (1 << 1);
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

/* Match def = index scaled by the 4-byte u32 element stride -- imul/amul by
 * constant 4 or ishl by constant 2 -- and return the index (ssa def plus the
 * swizzle component the scalar ALU selected). */
static bool
multitap_match_scaled_index(const nir_def *def, const nir_def **idx_ssa,
                            unsigned *idx_comp)
{
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)def);
   if (!alu)
      return false;
   if (alu->op == nir_op_imul || alu->op == nir_op_amul) {
      for (int i = 0; i < 2; i++) {
         if (nir_src_is_const(alu->src[i].src) &&
             nir_src_comp_as_uint(alu->src[i].src, alu->src[i].swizzle[0]) == 4) {
            *idx_ssa = alu->src[1 - i].src.ssa;
            *idx_comp = alu->src[1 - i].swizzle[0];
            return true;
         }
      }
      return false;
   }
   if (alu->op == nir_op_ishl &&
       nir_src_is_const(alu->src[1].src) &&
       nir_src_comp_as_uint(alu->src[1].src, alu->src[1].swizzle[0]) == 2) {
      *idx_ssa = alu->src[0].src.ssa;
      *idx_comp = alu->src[0].swizzle[0];
      return true;
   }
   return false;
}

/* Resolve (idx_ssa, idx_comp) against the center index: the center itself
 * (delta 0), iadd(center, +/-1), or isub(center, 1).  SPIR-V OpISub reaches
 * NIR as nir_op_isub and the classify prep runs no algebraic pass that would
 * canonicalize it to iadd(center, -1), so `gid - 1u` must match as isub
 * directly (hardware-observed in the box-3 kernel's lowered NIR). */
static bool
multitap_match_index_delta(const nir_def *idx_ssa, unsigned idx_comp,
                           const nir_def *center_ssa, unsigned center_comp,
                           int *delta)
{
   if (idx_ssa == center_ssa && idx_comp == center_comp) {
      *delta = 0;
      return true;
   }
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)idx_ssa);
   if (!alu)
      return false;
   if (alu->op == nir_op_iadd) {
      for (int i = 0; i < 2; i++) {
         if (alu->src[i].src.ssa == center_ssa &&
             alu->src[i].swizzle[0] == center_comp &&
             nir_src_is_const(alu->src[1 - i].src)) {
            const int64_t val = nir_src_comp_as_int(alu->src[1 - i].src,
                                                    alu->src[1 - i].swizzle[0]);
            if (val == -1 || val == 1) {
               *delta = (int)val;
               return true;
            }
         }
      }
      return false;
   }
   if (alu->op == nir_op_isub &&
       alu->src[0].src.ssa == center_ssa &&
       alu->src[0].swizzle[0] == center_comp &&
       nir_src_is_const(alu->src[1].src)) {
      const int64_t val = nir_src_comp_as_int(alu->src[1].src,
                                              alu->src[1].swizzle[0]);
      if (val == 1 || val == -1) {
         *delta = -(int)val;
         return true;
      }
   }
   return false;
}

/* Classify one tap's byte offset in the DESCRIPTOR-BASE form the
 * post-explicit_io SPIR-V shape carries: every ssbo byte offset is
 * iadd(descriptor_base, scaled_index) where scaled_index = element_index * 4.
 * The store offset shares its scaled_index def with the center tap (CSE folds
 * the identical scale chains) while its descriptor base belongs to the OUTPUT
 * buffer, so store/tap offsets never compare equal directly; the edge taps
 * scale iadd(element_index, +/-1) by the same stride.  All taps must share
 * one descriptor base (*tap_base, captured from the first tap). */
static int
multitap_tap_bit_desc_base(const nir_def *offset,
                           const nir_def *center_idx_ssa,
                           unsigned center_idx_comp,
                           const nir_def **tap_base)
{
   const nir_alu_instr *alu = nir_def_as_alu_or_null((nir_def *)offset);
   if (!alu || alu->op != nir_op_iadd)
      return 0;
   for (int i = 0; i < 2; i++) {
      const nir_def *scaled = alu->src[i].src.ssa;
      const nir_def *base   = alu->src[1 - i].src.ssa;
      const nir_def *idx_ssa = NULL;
      unsigned idx_comp = 0;
      int delta = 0;
      if (!multitap_match_scaled_index(scaled, &idx_ssa, &idx_comp))
         continue;
      if (!multitap_match_index_delta(idx_ssa, idx_comp,
                                      center_idx_ssa, center_idx_comp, &delta))
         continue;
      if (!*tap_base)
         *tap_base = base;
      else if (*tap_base != base)
         return 0;
      return 1 << (delta + 1);
   }
   return 0;
}

/* Verify the collected tap loads form exactly the centered box-3
 * {index-1, index, index+1} relative to the store's element index, in either
 * the direct form (tap offset = store offset def +/- 4; the shape a
 * pre-scaled synthetic kernel builds) or the descriptor-base form (the shape
 * real SPIR-V reaches after nir_lower_explicit_io).  Returns the full 0x7
 * mask exactly when all three taps land on distinct box-3 positions. */
static int
multitap_verify_box3_offsets(const nir_intrinsic_instr *const *taps,
                             unsigned tap_count, const nir_def *store_offset)
{
   int mask = 0;
   for (unsigned t = 0; t < tap_count; t++)
      mask |= multitap_tap_bit_direct(taps[t]->src[1].ssa, store_offset);
   if (mask == 0x7)
      return mask;

   /* Descriptor-base form: split the store offset as iadd(out_base, scaled)
    * trying both operand orders; the assignment whose scaled half is a
    * stride-4 scale of an index is the element-index half. */
   const nir_alu_instr *store_alu =
      nir_def_as_alu_or_null((nir_def *)store_offset);
   if (!store_alu || store_alu->op != nir_op_iadd)
      return mask;
   for (int i = 0; i < 2; i++) {
      const nir_def *center_idx_ssa = NULL;
      unsigned center_idx_comp = 0;
      if (!multitap_match_scaled_index(store_alu->src[i].src.ssa,
                                       &center_idx_ssa, &center_idx_comp))
         continue;
      const nir_def *tap_base = NULL;
      mask = 0;
      for (unsigned t = 0; t < tap_count; t++)
         mask |= multitap_tap_bit_desc_base(taps[t]->src[1].ssa,
                                            center_idx_ssa, center_idx_comp,
                                            &tap_base);
      if (mask == 0x7)
         return mask;
   }
   return 0;
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
    * load_ssbo def, with at least three taps, all pulling from the same SSBO
    * (same binding source def). */
   const nir_intrinsic_instr *taps[R300_MULTITAP_MAX_TAPS] = {0};
   unsigned tap_count = 0;
   const int tap_total =
      multitap_add_tree_taps(store->src[0].ssa, taps, &tap_count, 0);
   if (tap_total != 3 || tap_count != 3)
      return;

   /* Every SSBO load must be a tap leaf of the reduction tree.  A stray
    * load_ssbo outside the tree (load_count > taps) is an input the box-3
    * multi-TEX lowering does not sample, so the kernel is not faithfully the
    * recognized neighborhood convolution; reject rather than mis-replay. */
   if (load_count != tap_count)
      return;

   /* Kernel must be exactly the 3-tap box filter {-1, 0, 1} relative to the
    * store index. */
   if (multitap_verify_box3_offsets(taps, tap_count, store->src[2].ssa) != 0x7)
      return;

   out->is_multitap_gather = true;
   out->tap_count = (uint16_t)tap_count;
   /* Bindings stay 0 when the post-explicit_io sources are opaque descriptor
    * defs; the orchestrator's positional layout fallback recovers input = 1st
    * compute-visible STORAGE_BUFFER, output = 2nd. */
   if (nir_src_is_const(taps[0]->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(taps[0]->src[0]);
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
   case nir_op_fdot2_replicated: *components = 2; return true;
   case nir_op_fdot3_replicated: *components = 3; return true;
   case nir_op_fdot4_replicated: *components = 4; return true;
   default:
      return false;
   }
}

struct dp4_offset_affine {
   uint64_t ax, ay, az, b;
   bool has_base;
};

static bool dp4_offset_resolve(const nir_def *def, unsigned channel,
                               unsigned depth,
                               struct dp4_offset_affine *out);

static bool
dp4_offset_in_range(const struct dp4_offset_affine *e)
{
   return e->ax <= UINT32_MAX && e->ay <= UINT32_MAX &&
          e->az <= UINT32_MAX && e->b <= UINT32_MAX;
}

static bool
dp4_offset_resolve_intrinsic(const nir_intrinsic_instr *intr, unsigned channel,
                             struct dp4_offset_affine *out)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_base_global_invocation_id:
   case nir_intrinsic_load_base_workgroup_id:
      return true;
   case nir_intrinsic_load_global_invocation_id:
   case nir_intrinsic_load_global_invocation_index:
      out->ax = channel == 0;
      out->ay = channel == 1;
      out->az = channel >= 2;
      return true;
   case nir_intrinsic_load_vulkan_descriptor:
      out->has_base = true;
      return true;
   default:
      return false;
   }
}

static bool
dp4_offset_resolve_add(const nir_alu_instr *alu, unsigned channel,
                       unsigned depth, struct dp4_offset_affine *out)
{
   struct dp4_offset_affine a, b;

   if (!dp4_offset_resolve(alu->src[0].src.ssa,
                           alu->src[0].swizzle[channel], depth + 1, &a) ||
       !dp4_offset_resolve(alu->src[1].src.ssa,
                           alu->src[1].swizzle[channel], depth + 1, &b) ||
       (a.has_base && b.has_base))
      return false;

   out->ax = a.ax + b.ax;
   out->ay = a.ay + b.ay;
   out->az = a.az + b.az;
   out->b = a.b + b.b;
   out->has_base = a.has_base || b.has_base;
   return dp4_offset_in_range(out);
}

static bool
dp4_offset_resolve_mul(const nir_alu_instr *alu, unsigned channel,
                       unsigned depth, struct dp4_offset_affine *out)
{
   struct dp4_offset_affine a;
   unsigned ci = 2;

   if (nir_src_is_const(alu->src[1].src))
      ci = 1;
   else if (nir_src_is_const(alu->src[0].src))
      ci = 0;
   if (ci == 2)
      return false;

   const int64_t cv = nir_src_as_int(alu->src[ci].src);
   if (cv < 0 ||
       !dp4_offset_resolve(alu->src[ci ^ 1].src.ssa,
                           alu->src[ci ^ 1].swizzle[channel], depth + 1,
                           &a) ||
       a.has_base)
      return false;

   out->ax = a.ax * (uint64_t)cv;
   out->ay = a.ay * (uint64_t)cv;
   out->az = a.az * (uint64_t)cv;
   out->b = a.b * (uint64_t)cv;
   return dp4_offset_in_range(out);
}

static bool
dp4_offset_resolve_shift(const nir_alu_instr *alu, unsigned channel,
                         unsigned depth, struct dp4_offset_affine *out)
{
   struct dp4_offset_affine a;

   if (!nir_src_is_const(alu->src[1].src))
      return false;
   const uint64_t shift = nir_src_as_uint(alu->src[1].src);
   if (shift >= 32 ||
       !dp4_offset_resolve(alu->src[0].src.ssa,
                           alu->src[0].swizzle[channel], depth + 1, &a) ||
       a.has_base)
      return false;

   out->ax = a.ax << shift;
   out->ay = a.ay << shift;
   out->az = a.az << shift;
   out->b = a.b << shift;
   return dp4_offset_in_range(out);
}

static bool
dp4_offset_resolve(const nir_def *def, unsigned channel, unsigned depth,
                   struct dp4_offset_affine *out)
{
   if (depth > 12 || channel > 3)
      return false;
   memset(out, 0, sizeof(*out));

   const nir_instr *instr = nir_def_instr(def);
   if (instr->type == nir_instr_type_load_const) {
      const nir_load_const_instr *lc = nir_def_as_load_const((nir_def *)def);
      out->b = lc->value[channel].u32;
      return true;
   }
   if (instr->type == nir_instr_type_intrinsic)
      return dp4_offset_resolve_intrinsic(nir_instr_as_intrinsic(instr),
                                          channel, out);
   if (instr->type != nir_instr_type_alu)
      return false;

   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_u2u32:
   case nir_op_i2i32:
      return dp4_offset_resolve(alu->src[0].src.ssa,
                                alu->src[0].swizzle[channel], depth + 1, out);
   case nir_op_iadd:
      return dp4_offset_resolve_add(alu, channel, depth, out);
   case nir_op_imul:
   case nir_op_amul:
      return dp4_offset_resolve_mul(alu, channel, depth, out);
   case nir_op_ishl:
      return dp4_offset_resolve_shift(alu, channel, depth, out);
   default:
      return false;
   }
}

static bool
dp4_offset_to_element_index(const nir_def *def, unsigned element_stride,
                            struct dp4_offset_affine *out)
{
   struct dp4_offset_affine byte_offset;
   if (!element_stride ||
       !dp4_offset_resolve(def, 0, 0, &byte_offset))
      return false;
   if (byte_offset.ax % element_stride || byte_offset.ay % element_stride ||
       byte_offset.az % element_stride || byte_offset.b % element_stride)
      return false;

   out->ax = byte_offset.ax / element_stride;
   out->ay = byte_offset.ay / element_stride;
   out->az = byte_offset.az / element_stride;
   out->b = byte_offset.b / element_stride;
   out->has_base = false;
   return true;
}

static bool
dp4_offset_is_contiguous_invocation_stream(const struct dp4_offset_affine *e)
{
   return e->ax == 1 && e->ay == 0 && e->az == 0 && e->b == 0;
}

static bool
dp4_offsets_match_replay_stream(const nir_intrinsic_instr *load_a,
                                const nir_intrinsic_instr *load_b,
                                const nir_intrinsic_instr *store,
                                uint8_t components)
{
   const unsigned input_stride = components == 2 ? 8 : 16;
   const unsigned output_stride = 4;
   struct dp4_offset_affine a, b, out;

   if (!dp4_offset_to_element_index(load_a->src[1].ssa, input_stride, &a) ||
       !dp4_offset_to_element_index(load_b->src[1].ssa, input_stride, &b) ||
       !dp4_offset_to_element_index(store->src[2].ssa, output_stride, &out))
      return false;
   if (!dp4_offset_is_contiguous_invocation_stream(&a) ||
       !dp4_offset_is_contiguous_invocation_stream(&b) ||
       !dp4_offset_is_contiguous_invocation_stream(&out))
      return false;

   return a.ax == b.ax && a.ay == b.ay && a.az == b.az && a.b == b.b &&
          a.ax == out.ax && a.ay == out.ay && a.az == out.az && a.b == out.b;
}

static bool
dp4_store_writes_scalar_output(const nir_intrinsic_instr *store)
{
   if (!store->src[0].ssa)
      return false;

   /* The replay writes one complete scalar uint output element per invocation.
    * Wider store masks describe lanes the DP4 replay does not transport. */
   return !nir_intrinsic_has_write_mask(store) ||
          nir_intrinsic_write_mask(store) == 0x1;
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
   if (!dp4_store_writes_scalar_output(store))
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

   /* The replay samples texel i from both input buffers and copies texel i back
    * to the output buffer.  Normalize each byte offset by its carrier stride
    * before admitting the pattern: fdot2 input records are 8 bytes, fdot3/fdot4
    * input records use the 16-byte FP32x4 carrier, and the uint output record is
    * 4 bytes.  Any shifted, strided, or non-contiguous address would make the
    * replay compute a different element stream than the compute shader. */
   if (!dp4_offsets_match_replay_stream(load_a, load_b, store, comps))
      return;

   out->dot_op     = (uint16_t)alu->op;
   out->components = comps;
   /* Capture constant binding indices when present; the orchestrator's
    * descriptor-set layout fallback picks the first three compute-visible
    * STORAGE_BUFFER bindings when these stay at the defaults (same policy as
    * binary-map). */
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

void
r300_nir_detect_qnormalize_pattern(const nir_shader *s,
                                   struct r300_compute_qnormalize_pattern *out)
{
   out->is_qnormalize       = false;
   out->input_ssbo_binding  = 0;
   out->output_ssbo_binding = 0;

   const nir_intrinsic_instr *store, *load;
   if (!collect_unary_ssbo_shape(s, &store, &load))
      return;

   /* Store value = a * rsqrt(dot(a,a)): the quaternion scaled by its reciprocal
    * length.  The fmul reads a -- the identity-swizzled load -- on one side and
    * the reciprocal-sqrt scalar broadcast (xxxx) on the other.  That scalar is
    * frsq(fdot4(a,a)): the US RSQ over the one-DP4 squared norm. */
   const nir_alu_instr *mul = nir_def_as_alu_or_null(store->src[0].ssa);
   if (!mul || mul->op != nir_op_fmul)
      return;
   const nir_def *a = &load->def;

   const nir_def *rsplat = NULL;
   for (unsigned k = 0; k < 2; k++) {
      const nir_alu_src *as = &mul->src[k];        /* candidate a operand */
      const nir_alu_src *rs = &mul->src[1 - k];    /* candidate rsqrt splat */
      bool a_id = as->src.ssa == a;
      bool r_splat = true;
      for (unsigned c = 0; c < 4; c++) {
         if (as->swizzle[c] != c)
            a_id = false;
         if (rs->swizzle[c] != 0)
            r_splat = false;
      }
      if (a_id && r_splat) {
         rsplat = rs->src.ssa;
         break;
      }
   }
   if (!rsplat)
      return;

   const nir_alu_instr *rsq = nir_def_as_alu_or_null(rsplat);
   if (!rsq || rsq->op != nir_op_frsq)
      return;
   const nir_alu_instr *dot = nir_def_as_alu_or_null(rsq->src[0].src.ssa);
   if (!dot || dot->op != nir_op_fdot4)
      return;
   if (dot->src[0].src.ssa != a || dot->src[1].src.ssa != a)
      return;
   for (unsigned c = 0; c < 4; c++)
      if (dot->src[0].swizzle[c] != c || dot->src[1].swizzle[c] != c)
         return;

   if (nir_src_is_const(load->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load->src[0]);
   if (nir_src_is_const(store->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store->src[1]);
   out->is_qnormalize = true;
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

   const nir_intrinsic_instr *load[4] = {0}, *store[2] = {0};
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

   const nir_intrinsic_instr *load[2] = {0}, *store[2] = {0};
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

   const nir_intrinsic_instr *load[2] = {0}, *store[1] = {0};
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
   out->is_left                = false;
   out->input_xlo_ssbo_binding = 0;
   out->input_xhi_ssbo_binding = 0;
   out->input_ylo_ssbo_binding = 0;
   out->input_yhi_ssbo_binding = 0;
   out->output_lo_ssbo_binding = 0;
   out->output_hi_ssbo_binding = 0;

   const nir_intrinsic_instr *load[4] = {0}, *store[2] = {0};
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

   /* The eight-wide product of x and inv(y) = (c, d), an OMUL fold.  Right
    * division x*inv(y) folds with x's halves as the first operand (a,b) and the
    * inverse as (c,d): o_lo = a*c - conj(d)*b, o_hi = d*a + b*conj(c).  Left
    * division inv(y)*x swaps the operands -- (c,d) first, x = (a,b) second:
    * o_lo = c*a - conj(b)*d, o_hi = b*c + d*conj(a).  Try right, then left;
    * the two folds are structurally distinct so a kernel matches exactly one. */
   const bool right =
      omul_match_half(store[0]->src[0].ssa, nir_op_fsub,
                      xlo, c, NULL, xhi, true, d, NULL) &&
      omul_match_half(store[1]->src[0].ssa, nir_op_fadd,
                      d, xlo, xhi, c, false, NULL, qrotate_outer_rows[0]);
   const bool left = !right &&
      omul_match_half(store[0]->src[0].ssa, nir_op_fsub,
                      c, xlo, NULL, d, true, xhi, NULL) &&
      omul_match_half(store[1]->src[0].ssa, nir_op_fadd,
                      xhi, c, d, xlo, false, NULL, qrotate_outer_rows[0]);
   if (!right && !left)
      return;
   out->is_left = left;

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

/* conj(-q) over q: conj(-q) = -conj(q) = (-q.x, q.y, q.z, q.w), the negation of
 * the Hamilton row-0 conjugate.  The OTRANS outer product t*conj(x) carries this
 * on its conj(x).hi = -xhi lane, where the kernel's fneg-then-conjugate folds to
 * a single permutation of xhi with the negated-conjugate signs. */
static const struct r300_perm_entry otrans_neg_conj_row[4] = {
   {0, true}, {1, false}, {2, false}, {3, false},
};

/* Find the identity-load Hamilton difference (combine == fsub) or sum (fadd) that
 * equals one half of the inner octonion product OMUL((xlo,xhi),(vlo,vhi)) -- the
 * intermediate t = x*v.  The lower half is xlo*vlo - conj(vhi)*xhi, the upper
 * vhi*xlo + xhi*conj(vlo); both reuse the OMUL half-matcher over the raw loads. */
static const nir_def *
otrans_find_inner_half(const nir_shader *s, nir_op combine,
                       const nir_def *xlo, const nir_def *xhi,
                       const nir_def *vlo, const nir_def *vhi)
{
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op != combine)
               continue;
            if (combine == nir_op_fsub) {
               if (omul_match_half(&alu->def, nir_op_fsub, xlo, vlo, NULL, xhi,
                                   true, vhi, NULL))
                  return &alu->def;
            } else if (omul_match_half(&alu->def, nir_op_fadd, vhi, xlo, xhi, vlo,
                                       false, NULL, qrotate_outer_rows[0])) {
               return &alu->def;
            }
         }
      }
   }
   return NULL;
}

/* The whole-vector fneg of `base` (an identity-swizzled negation of all four
 * lanes) -- conj(x).hi = -xhi, the kernel's `ch`. */
static const nir_def *
otrans_find_vec_fneg(const nir_shader *s, const nir_def *base)
{
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *alu = nir_instr_as_alu(instr);
            if (alu->op != nir_op_fneg || alu->def.num_components != 4 ||
                alu->src[0].src.ssa != base)
               continue;
            bool id = true;
            for (unsigned c = 0; c < 4; c++)
               if (alu->src[0].swizzle[c] != c)
                  id = false;
            if (id)
               return &alu->def;
         }
      }
   }
   return NULL;
}

/* Match the OTRANS outer lower half: olo = qm(tl, conj(xlo)) - qm(conj(-xhi), th).
 * conj(xlo) folds into the conj-composed rotation rows over xlo (the P product),
 * and conj(-xhi) is the negated Hamilton row 0 over xhi (the two-perm Q). */
static bool
otrans_match_outer_lo(const nir_def *store_val, const nir_def *tl,
                      const nir_def *th, const nir_def *xlo, const nir_def *xhi)
{
   const nir_alu_instr *top = nir_def_as_alu_or_null(store_val);
   if (!top || top->op != nir_op_fsub)
      return false;
   const nir_alu_instr *pvec = nir_def_as_alu_or_null(top->src[0].src.ssa);
   const nir_alu_instr *qvec = nir_def_as_alu_or_null(top->src[1].src.ssa);
   if (!pvec || pvec->op != nir_op_vec4 || !qvec || qvec->op != nir_op_vec4)
      return false;
   for (unsigned lane = 0; lane < 4; lane++) {
      if (!omul_match_id_perm_dot(pvec->src[lane].src.ssa, tl, xlo,
                                  qrotate_outer_rows[lane]))
         return false;
      if (!omul_match_two_perm_dot(qvec->src[lane].src.ssa,
                                   xhi, otrans_neg_conj_row,
                                   th, r300_hamilton_rows[lane]))
         return false;
   }
   return true;
}

/* Match the OTRANS outer upper half: ohi = qm(-xhi, tl) + qm(th, xlo).  Both are
 * identity-load Hamilton dots -- ch = -xhi against the Hamilton rows of tl, and th
 * against the Hamilton rows of xlo (conj(conj(xlo)) = xlo).  fadd is commutative,
 * so accept either summand order. */
static bool
otrans_match_outer_hi(const nir_def *store_val, const nir_def *tl,
                      const nir_def *th, const nir_def *ch, const nir_def *xlo)
{
   const nir_alu_instr *top = nir_def_as_alu_or_null(store_val);
   if (!top || top->op != nir_op_fadd)
      return false;
   const nir_alu_instr *v0 = nir_def_as_alu_or_null(top->src[0].src.ssa);
   const nir_alu_instr *v1 = nir_def_as_alu_or_null(top->src[1].src.ssa);
   if (!v0 || v0->op != nir_op_vec4 || !v1 || v1->op != nir_op_vec4)
      return false;
   for (unsigned order = 0; order < 2; order++) {
      const nir_alu_instr *pvec = order == 0 ? v0 : v1;
      const nir_alu_instr *qvec = order == 0 ? v1 : v0;
      bool ok = true;
      for (unsigned lane = 0; lane < 4 && ok; lane++) {
         if (!omul_match_id_perm_dot(pvec->src[lane].src.ssa, ch, tl,
                                     r300_hamilton_rows[lane]) ||
             !omul_match_id_perm_dot(qvec->src[lane].src.ssa, th, xlo,
                                     r300_hamilton_rows[lane]))
            ok = false;
      }
      if (ok)
         return true;
   }
   return false;
}

void
r300_nir_detect_otrans_pattern(const nir_shader *s,
                               struct r300_compute_otrans_pattern *out)
{
   out->is_otrans              = false;
   out->input_xlo_ssbo_binding = 0;
   out->input_xhi_ssbo_binding = 0;
   out->input_vlo_ssbo_binding = 0;
   out->input_vhi_ssbo_binding = 0;
   out->output_lo_ssbo_binding = 0;
   out->output_hi_ssbo_binding = 0;

   const nir_intrinsic_instr *load[4] = {0}, *store[2] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 4, &nload, store, 2, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 4 || nstore != 2 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]) || !store_is_full_width(store[1]))
      return;

   /* Loads in declaration order: x = (xlo,xhi), v = (vlo,vhi). */
   const nir_def *xlo = &load[0]->def, *xhi = &load[1]->def;
   const nir_def *vlo = &load[2]->def, *vhi = &load[3]->def;

   /* The intermediate octonion t = x*v -- its two halves as the Hamilton
    * difference (lower) and sum (upper) over the raw loads. */
   const nir_def *tl = otrans_find_inner_half(s, nir_op_fsub, xlo, xhi, vlo, vhi);
   const nir_def *th = otrans_find_inner_half(s, nir_op_fadd, xlo, xhi, vlo, vhi);
   if (!tl || !th)
      return;

   /* conj(x).hi = -xhi, the whole-vector negate the outer product multiplies. */
   const nir_def *ch = otrans_find_vec_fneg(s, xhi);
   if (!ch)
      return;

   /* The two stores are the eight-wide product OMUL((tl,th), conj(x)).  The lower
    * store is the fsub of the conj-folded P and the negated-conjugate Q; the upper
    * is the fadd of two identity-load Hamilton dots.  Matching both makes the
    * sandwich admission sound: only x*v*conj(x) reaches here. */
   if (!otrans_match_outer_lo(store[0]->src[0].ssa, tl, th, xlo, xhi))
      return;
   if (!otrans_match_outer_hi(store[1]->src[0].ssa, tl, th, ch, xlo))
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_xlo_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_xhi_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(load[2]->src[0]))
      out->input_vlo_ssbo_binding = nir_src_as_uint(load[2]->src[0]);
   if (nir_src_is_const(load[3]->src[0]))
      out->input_vhi_ssbo_binding = nir_src_as_uint(load[3]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_lo_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   if (nir_src_is_const(store[1]->src[1]))
      out->output_hi_ssbo_binding = nir_src_as_uint(store[1]->src[1]);
   out->is_otrans = true;
}

/* True if `def` is the Hamilton product a*b: a vec4 whose four lanes are the
 * identity-load Hamilton dots of a against the permuted rows of b.  The single-
 * quaternion building block of the QFM fused ops, reusing the OMUL lane matcher. */
static bool
qmul_match(const nir_def *def, const nir_def *a, const nir_def *b)
{
   const nir_alu_instr *v = nir_def_as_alu_or_null(def);
   if (!v || v->op != nir_op_vec4)
      return false;
   for (unsigned lane = 0; lane < 4; lane++)
      if (!omul_match_id_perm_dot(v->src[lane].src.ssa, a, b,
                                  r300_hamilton_rows[lane]))
         return false;
   return true;
}

/* True if `src` reads `base` identity-swizzled (all four lanes in order). */
static bool
qfm_is_identity(const nir_alu_src *src, const nir_def *base)
{
   if (src->src.ssa != base)
      return false;
   for (unsigned c = 0; c < 4; c++)
      if (src->swizzle[c] != c)
         return false;
   return true;
}

void
r300_nir_detect_qfmadd_pattern(const nir_shader *s,
                               struct r300_compute_qfmadd_pattern *out)
{
   out->is_qfmadd           = false;
   out->is_sub              = false;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->input_c_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *load[3] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 3, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 3 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]))
      return;

   /* Store = a*b +/- c: the Hamilton product of the first two loads combined with
    * the third by fadd (QFMADD) or fsub (QFMSUB).  fadd is commutative so the
    * product and addend may appear in either src order; fsub is not commutative so
    * the product must be src[0] (left operand). */
   const nir_def *a = &load[0]->def, *b = &load[1]->def, *c = &load[2]->def;
   const nir_alu_instr *top = nir_def_as_alu_or_null(store[0]->src[0].ssa);
   if (!top || (top->op != nir_op_fadd && top->op != nir_op_fsub))
      return;
   bool ok;
   if (top->op == nir_op_fadd) {
      ok = (qmul_match(top->src[0].src.ssa, a, b) &&
            qfm_is_identity(&top->src[1], c)) ||
           (qmul_match(top->src[1].src.ssa, a, b) &&
            qfm_is_identity(&top->src[0], c));
   } else {
      /* fsub(product, c): product must be src[0] */
      ok = qmul_match(top->src[0].src.ssa, a, b) &&
           qfm_is_identity(&top->src[1], c);
   }
   if (!ok)
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(load[2]->src[0]))
      out->input_c_ssbo_binding = nir_src_as_uint(load[2]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_sub    = (top->op == nir_op_fsub);
   out->is_qfmadd = true;
}

void
r300_nir_detect_qfmmul_pattern(const nir_shader *s,
                               struct r300_compute_qfmmul_pattern *out)
{
   out->is_qfmmul           = false;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->input_c_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *load[3] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 3, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 3 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]))
      return;

   /* Store = (a*b)*c: find the intermediate t = a*b (the inner Hamilton product),
    * then verify the store is t*c.  Associativity in H makes (a*b)*c = a*(b*c), so
    * the canonical left-folded form is the admissible one. */
   const nir_def *a = &load[0]->def, *b = &load[1]->def, *c = &load[2]->def;
   const nir_def *t = NULL;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *v = nir_instr_as_alu(instr);
            if (v->op == nir_op_vec4 && qmul_match(&v->def, a, b)) {
               t = &v->def;
               break;
            }
         }
         if (t)
            break;
      }
   }
   if (!t)
      return;
   if (!qmul_match(store[0]->src[0].ssa, t, c))
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(load[2]->src[0]))
      out->input_c_ssbo_binding = nir_src_as_uint(load[2]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_qfmmul = true;
}

/* Return the non-const operand of iand(X, mask) when the other operand is the
 * given compile-time mask, or NULL when the shape does not match. */
static const nir_def *
as_iand_const(const nir_def *val, uint32_t mask)
{
   const nir_alu_instr *alu = nir_def_as_alu_or_null(val);
   if (!alu || alu->op != nir_op_iand)
      return NULL;
   for (unsigned k = 0; k < 2; k++) {
      if (!nir_src_is_const(alu->src[k].src))
         continue;
      if (nir_src_comp_as_uint(alu->src[k].src, alu->src[k].swizzle[0]) == mask)
         return alu->src[1 - k].src.ssa;
   }
   return NULL;
}

/* Return the shifted value if val = ushr(X, shift) with a constant shift, or NULL. */
static const nir_def *
as_ushr_const(const nir_def *val, uint32_t shift)
{
   const nir_alu_instr *alu = nir_def_as_alu_or_null(val);
   if (!alu || alu->op != nir_op_ushr)
      return NULL;
   if (!nir_src_is_const(alu->src[1].src))
      return NULL;
   if (nir_src_comp_as_uint(alu->src[1].src, alu->src[1].swizzle[0]) != shift)
      return NULL;
   return alu->src[0].src.ssa;
}

/* Return the shifted value if val = ishl(X, shift) with a constant shift, or NULL. */
static const nir_def *
as_ishl_const(const nir_def *val, uint32_t shift)
{
   const nir_alu_instr *alu = nir_def_as_alu_or_null(val);
   if (!alu || alu->op != nir_op_ishl)
      return NULL;
   if (!nir_src_is_const(alu->src[1].src))
      return NULL;
   if (nir_src_comp_as_uint(alu->src[1].src, alu->src[1].swizzle[0]) != shift)
      return NULL;
   return alu->src[0].src.ssa;
}

void
r300_nir_detect_q16_16_add_pattern(const nir_shader *s,
                                   struct r300_compute_q16_16_add_pattern *out)
{
   memset(out, 0, sizeof(*out));

   const nir_intrinsic_instr *load[2] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 2, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 2 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store[0] || !store[0]->src[0].ssa || !load[0] || !load[1])
      return;

   const nir_def *val = store[0]->src[0].ssa;
   if (val->num_components != 1 || val->bit_size != 32)
      return;

   /* Top node: ior(ishl(hi_result, 16), iand(lo_sum, 0xFFFF)).  ior is
    * commutative so either ordering is accepted. */
   const nir_alu_instr *top = nir_def_as_alu_or_null(val);
   if (!top || top->op != nir_op_ior)
      return;

   const nir_def *a = &load[0]->def, *b = &load[1]->def;
   const nir_def *lo_sum = NULL;    /* iadd(iand(a,0xFFFF), iand(b,0xFFFF)) */
   const nir_def *hi_result = NULL; /* iadd(iadd(ushr(a,16), ushr(b,16)), carry) */

   for (unsigned k = 0; k < 2; k++) {
      const nir_def *s0 = top->src[k].src.ssa;
      const nir_def *s1 = top->src[1 - k].src.ssa;

      hi_result = as_ishl_const(s0, 16);
      const nir_def *lo_inner = as_iand_const(s1, 0xFFFF);
      if (!hi_result || !lo_inner)
         continue;

      /* lo_inner must be an iadd -- that is the lo_sum node. */
      const nir_alu_instr *loa = nir_def_as_alu_or_null(lo_inner);
      if (!loa || loa->op != nir_op_iadd)
         continue;

      /* lo_sum = iadd(iand(a, 0xFFFF), iand(b, 0xFFFF)) */
      const nir_def *la = as_iand_const(loa->src[0].src.ssa, 0xFFFF);
      const nir_def *lb = as_iand_const(loa->src[1].src.ssa, 0xFFFF);
      if (!la || !lb)
         continue;
      if (!((la == a && lb == b) || (la == b && lb == a)))
         continue;

      lo_sum = lo_inner;
      break;
   }
   if (!lo_sum || !hi_result)
      return;

   /* hi_result = iadd(inner_pair, carry) or iadd(carry, inner_pair).
    * carry = ushr(lo_sum, 16); inner_pair = iadd(ushr(a,16), ushr(b,16)). */
   const nir_alu_instr *hi_top = nir_def_as_alu_or_null(hi_result);
   if (!hi_top || hi_top->op != nir_op_iadd)
      return;

   bool hi_ok = false;
   for (unsigned k = 0; k < 2; k++) {
      const nir_def *carry_src = as_ushr_const(hi_top->src[k].src.ssa, 16);
      if (carry_src != lo_sum)
         continue;
      /* Other operand: iadd(ushr(a,16), ushr(b,16)). */
      const nir_alu_instr *hi_pair = nir_def_as_alu_or_null(hi_top->src[1 - k].src.ssa);
      if (!hi_pair || hi_pair->op != nir_op_iadd)
         continue;
      const nir_def *ha = as_ushr_const(hi_pair->src[0].src.ssa, 16);
      const nir_def *hb = as_ushr_const(hi_pair->src[1].src.ssa, 16);
      if (!ha || !hb)
         continue;
      if (!((ha == a && hb == b) || (ha == b && hb == a)))
         continue;
      hi_ok = true;
      break;
   }
   if (!hi_ok)
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_q16_16_add = true;
}

void
r300_nir_detect_qdiv_pattern(const nir_shader *s,
                             struct r300_compute_qdiv_pattern *out)
{
   out->is_qdiv              = false;
   out->input_a_ssbo_binding = 0;
   out->input_b_ssbo_binding = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *load[2] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 2, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 2 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]))
      return;

   /* a / b = a * inv(b), inv(b) = conj(b) * (1/dot(b,b)).  Loads in declaration
    * order: a = dividend, b = divisor. */
   const nir_def *a = &load[0]->def, *b = &load[1]->def;

   /* Find the reciprocal r = 1 / dot(b,b).  A single self-dot reciprocal is the
    * signature of quaternion division -- the octonion ODIV reciprocates a sum of
    * two self-dots, so the single term distinguishes the dim-4 divide.  Only a
    * divide produces it; matching it plus the Hamilton product of a against the
    * scaled conjugate makes the admission sound. */
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
            if (odiv_is_self_dot(alu->src[1].src.ssa, b))
               r = &alu->def;
         }
      }
   }
   if (!r)
      return;

   /* The inverse ib = conj(b) * r.xxxx: conj(b) is the Hamilton row-0 permutation
    * of b (w positive, x/y/z negated) and r broadcasts across all four lanes. */
   const nir_def *ib = NULL;
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
            bool ident = true;
            for (unsigned k = 0; k < 4; k++)
               if (other->swizzle[k] != k)
                  ident = false;
            if (ident && omul_match_perm(other->src.ssa, b, r300_hamilton_rows[0]))
               ib = &mul->def;
         }
      }
   }
   if (!ib)
      return;

   /* out = a * ib, the Hamilton product (four sign-permuted DP4s). */
   if (!qmul_match(store[0]->src[0].ssa, a, ib))
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(load[1]->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(load[1]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_qdiv = true;
}

/* Resolve a scalar integer def to a constant, walking the imul / ishl / iadd of
 * constants that the SSBO address lowering leaves UNFOLDED.  The classify path
 * runs copy-prop/dce/cse but not constant folding, so m[k]'s byte offset arrives
 * as index*16 (an imul or ishl of small constants), not a folded const -- exactly
 * the MAT4VEC matrix-row offsets. */
static bool
mat4vec_resolve_u32(const nir_def *d, uint32_t *out)
{
   if (nir_def_is_const(d)) {
      *out = nir_def_as_load_const(d)->value[0].u32;
      return true;
   }
   const nir_alu_instr *a = nir_def_as_alu_or_null(d);
   if (!a)
      return false;
   uint32_t x, y;
   if ((a->op == nir_op_imul || a->op == nir_op_ishl || a->op == nir_op_iadd) &&
       mat4vec_resolve_u32(a->src[0].src.ssa, &x) &&
       mat4vec_resolve_u32(a->src[1].src.ssa, &y)) {
      *out = a->op == nir_op_imul ? x * y
           : a->op == nir_op_ishl ? x << y
           :                        x + y;
      return true;
   }
   return false;
}

/* Decompose a Vulkan SSBO byte offset into a runtime base def plus a constant
 * delta.  The descriptor lowering builds each matrix-row offset as
 * iadd(descriptor_base, rowindex*16): descriptor_base is the buffer's runtime
 * base offset (a load_vulkan_descriptor .y component) shared by every access to
 * that buffer, and rowindex*16 folds to a constant through the imul/ishl walk.
 * A fully constant offset (no descriptor base, e.g. a 0-based test kernel)
 * returns base = NULL so the caller can still match it. */
static bool
mat4vec_offset_split(const nir_def *d, const nir_def **base, uint32_t *delta)
{
   const nir_alu_instr *a = nir_def_as_alu_or_null(d);
   if (a && a->op == nir_op_iadd) {
      uint32_t cx, cy;
      bool rx = mat4vec_resolve_u32(a->src[0].src.ssa, &cx);
      bool ry = mat4vec_resolve_u32(a->src[1].src.ssa, &cy);
      if (rx && ry) { *base = NULL;               *delta = cx + cy; return true; }
      if (ry)       { *base = a->src[0].src.ssa;  *delta = cy;      return true; }
      if (rx)       { *base = a->src[1].src.ssa;  *delta = cx;      return true; }
      return false;
   }
   uint32_t c;
   if (mat4vec_resolve_u32(d, &c)) {
      *base = NULL;
      *delta = c;
      return true;
   }
   return false;
}

void
r300_nir_detect_mat4vec_pattern(const nir_shader *s,
                                struct r300_compute_mat4vec_pattern *out)
{
   out->is_mat4vec          = false;
   out->matrix_ssbo_binding = 0;
   out->vertex_ssbo_binding = 1;
   out->output_ssbo_binding = 2;

   const nir_intrinsic_instr *load[5] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 5, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 5 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]))
      return;

   /* Store value = vec4 of four fdot4 lanes, each a (matrix row) . v with both
    * operands identity-swizzled (the transform permutes nothing -- unlike the
    * Hamilton product -- so a non-identity swizzle would change the dot). */
   const nir_alu_instr *vec = nir_def_as_alu_or_null(store[0]->src[0].ssa);
   if (!vec || vec->op != nir_op_vec4)
      return;
   const nir_alu_instr *dot[4];
   for (unsigned lane = 0; lane < 4; lane++) {
      dot[lane] = nir_def_as_alu_or_null(vec->src[lane].src.ssa);
      if (!dot[lane] || dot[lane]->op != nir_op_fdot4)
         return;
      for (unsigned k = 0; k < 2; k++)
         for (unsigned c = 0; c < 4; c++)
            if (dot[lane]->src[k].swizzle[c] != c)
               return;
   }

   /* The vertex v is the def common to all four dots; each matrix row is the
    * other operand of its dot (fdot is commutative, so accept either order). */
   const nir_def *v = NULL;
   for (unsigned k = 0; k < 2 && !v; k++) {
      const nir_def *cand = dot[0]->src[k].src.ssa;
      bool in_all = true;
      for (unsigned lane = 1; lane < 4; lane++)
         if (dot[lane]->src[0].src.ssa != cand &&
             dot[lane]->src[1].src.ssa != cand)
            in_all = false;
      if (in_all)
         v = cand;
   }
   if (!v)
      return;
   const nir_def *rowdef[4];
   for (unsigned lane = 0; lane < 4; lane++)
      rowdef[lane] = (dot[lane]->src[0].src.ssa == v) ? dot[lane]->src[1].src.ssa
                                                      : dot[lane]->src[0].src.ssa;

   /* Map v and the four rows back to their load_ssbo intrinsics. */
   const nir_intrinsic_instr *vload = NULL, *rload[4] = {NULL, NULL, NULL, NULL};
   for (unsigned j = 0; j < 5; j++) {
      if (&load[j]->def == v)
         vload = load[j];
      for (unsigned lane = 0; lane < 4; lane++)
         if (&load[j]->def == rowdef[lane])
            rload[lane] = load[j];
   }
   if (!vload)
      return;
   for (unsigned lane = 0; lane < 4; lane++)
      if (!rload[lane])
         return;

   /* The four rows must come from ONE buffer (the matrix) and row k must sit at
    * byte offset base + k*16, so the dispatch -- which wraps texel k at offset
    * k*16 into output lane k -- matches the kernel's row-to-lane mapping.  The
    * binding handle is an opaque post-descriptor-lowering value (a mov of the
    * load_vulkan_descriptor .x component), not a constant, so require the four
    * row bindings to be the SAME ssa def and resolve the binding positionally in
    * the dispatch.  The byte offset is iadd(descriptor_base, k*16): descriptor_
    * base is the buffer's runtime base (the .y descriptor component) shared by
    * every row, while k*16 folds to a constant -- so split each offset into that
    * shared base plus a constant delta and require the bases to agree. */
   const nir_def *mbind_def = rload[0]->src[0].ssa;
   const nir_def *obase = NULL;   /* shared runtime SSBO base offset of the matrix */
   for (unsigned lane = 0; lane < 4; lane++) {
      if (rload[lane]->src[0].ssa != mbind_def)
         return;
      const nir_def *base = NULL;
      uint32_t delta = 0;
      if (!mat4vec_offset_split(rload[lane]->src[1].ssa, &base, &delta) ||
          delta != lane * 16u || (lane && base != obase))
         return;
      if (lane == 0)
         obase = base;
   }

   if (nir_src_is_const(rload[0]->src[0]))
      out->matrix_ssbo_binding = nir_src_as_uint(rload[0]->src[0]);
   if (nir_src_is_const(vload->src[0]))
      out->vertex_ssbo_binding = nir_src_as_uint(vload->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_mat4vec = true;
}

/* QFMUL: out[gid] = a[gid] * s, a per-element vec4 quaternion times a BROADCAST
 * scalar.  Two loads, one store; the store value is an fmul whose operands are a
 * 4-component identity-swizzled load (the quaternion) and a 1-component
 * splat-swizzled load (the scalar).  The 1-component load is the broadcast s
 * (one value for every element) and belongs in the fragment constant file, the
 * way MAT4VEC's broadcast matrix does. */
void
r300_nir_detect_qfmul_pattern(const nir_shader *s,
                              struct r300_compute_qfmul_pattern *out)
{
   out->is_qfmul            = false;
   out->scalar_ssbo_binding = 0;
   out->quat_ssbo_binding   = 1;
   out->output_ssbo_binding = 2;
   out->scalar_ssbo_binding_valid = false;
   out->quat_ssbo_binding_valid   = false;
   out->output_ssbo_binding_valid = false;

   const nir_intrinsic_instr *load[2] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 2, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 2 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store_is_full_width(store[0]))
      return;

   const nir_alu_instr *mul = nir_def_as_alu_or_null(store[0]->src[0].ssa);
   if (!mul || mul->op != nir_op_fmul)
      return;

   /* One operand is the 4-component identity-swizzled quaternion, the other the
    * 1-component splat-swizzled (component 0 four times) broadcast scalar. */
   const nir_def *quat_def = NULL, *scal_def = NULL;
   for (unsigned k = 0; k < 2; k++) {
      const nir_def *d = mul->src[k].src.ssa;
      if (d->num_components == 4 && d->bit_size == 32) {
         bool ident = true;
         for (unsigned c = 0; c < 4; c++)
            if (mul->src[k].swizzle[c] != c)
               ident = false;
         if (ident)
            quat_def = d;
      } else if (d->num_components == 1 && d->bit_size == 32) {
         bool splat = true;
         for (unsigned c = 0; c < 4; c++)
            if (mul->src[k].swizzle[c] != 0)
               splat = false;
         if (splat)
            scal_def = d;
      }
   }
   if (!quat_def || !scal_def)
      return;

   const nir_intrinsic_instr *quat_load = NULL, *scal_load = NULL;
   for (unsigned j = 0; j < 2; j++) {
      if (&load[j]->def == quat_def)
         quat_load = load[j];
      if (&load[j]->def == scal_def)
         scal_load = load[j];
   }
   if (!quat_load || !scal_load)
      return;
   if (mul->def.bit_size != 32 || store[0]->src[0].ssa->bit_size != 32 ||
       quat_load->def.num_components != 4 || quat_load->def.bit_size != 32 ||
       scal_load->def.num_components != 1 || scal_load->def.bit_size != 32)
      return;

   const nir_def *scalar_offset_base = NULL;
   uint32_t scalar_offset_delta = 0;
   if (!mat4vec_offset_split(scal_load->src[1].ssa, &scalar_offset_base,
                             &scalar_offset_delta) ||
       scalar_offset_delta != 0)
      return;

   if (nir_src_is_const(scal_load->src[0])) {
      out->scalar_ssbo_binding = nir_src_as_uint(scal_load->src[0]);
      out->scalar_ssbo_binding_valid = true;
   }
   if (nir_src_is_const(quat_load->src[0])) {
      out->quat_ssbo_binding = nir_src_as_uint(quat_load->src[0]);
      out->quat_ssbo_binding_valid = true;
   }
   if (nir_src_is_const(store[0]->src[1])) {
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
      out->output_ssbo_binding_valid = true;
   }
   out->is_qfmul = true;
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

   /* Constant-operand comp_swap admit-precheck (CAS ROUTE A): same
    * admit-on-shape pattern, keyed on ssbo_atomic_swap with both data
    * operands compile-time constants.  Mutually exclusive with the other
    * two flags by intrinsic (they admit ssbo_atomic, never the swap). */
   struct r300_compute_cas_pattern cas = {0};
   r300_nir_detect_cas_pattern(s, &cas);
   const bool admit_ssbo_atomic_swap_cas = cas.is_cas;

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
                  if (admit_ssbo_atomic_swap_cas &&
                      intr->intrinsic == nir_intrinsic_ssbo_atomic_swap) {
                     continue;  /* the constant-operand CAS verb handles it */
                  }
                  reject(out, R300_COMPUTE_REJECT_GENERAL_ATOMIC, name);
                  return;
               }
               if (strstr(name, "shared") != NULL) {
                  reject(out, R300_COMPUTE_REJECT_SHARED_MEMORY, name);
                  return;
               }
               if (intr->intrinsic == nir_intrinsic_store_ssbo) {
                  const char *store_detail = NULL;
                  if (!store_ssbo_address_is_supported(intr, &store_detail)) {
                     reject(out, R300_COMPUTE_REJECT_RW_STORAGE, store_detail);
                     return;
                  }
               }
               /* store_ssbo can lower only when its buffer handle is a
                * constant/test binding or the Vulkan descriptor handle, and
                * its byte offset is constant or derived from the global
                * invocation coordinates.  Data-dependent storage-buffer
                * addressing is arbitrary scatter on this substrate even
                * though the NIR opcode is still store_ssbo. */
               if (is_arbitrary_scatter(intr->intrinsic)) {
                  reject(out, R300_COMPUTE_REJECT_ARBITRARY_SCATTER, name);
                  return;
               }
               if (is_image_store(intr->intrinsic)) {
                  reject(out, R300_COMPUTE_REJECT_IMAGE_STORE, name);
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
     "the substrate has texture-load input and one RB3D color export; store_ssbo requires a canonical buffer handle and coordinate offset" },
   { R300_COMPUTE_REJECT_FP64, "fp64",
     "the fragment ALU is FP24 (s1e7m16); no double-precision path exists" },
   { R300_COMPUTE_REJECT_FP16, "fp16",
     "r300 has no native FP16 support; only emulated virtual-FP16 delegates are admissible" },
   { R300_COMPUTE_REJECT_ARBITRARY_SCATTER, "arbitrary-scatter",
     "store_global / store_global_2x32 writes to an arbitrary pointer; the substrate has no pointer-addressed write path" },
   { R300_COMPUTE_REJECT_IMAGE_STORE, "image-store",
     "image_store / image_deref_store / bindless_image_store; the substrate export is a single 2D RT, not a random-access texel write" },
   { R300_COMPUTE_REJECT_UNKNOWN_SHAPE, "unknown-shape",
     "kernel admitted classification but matched no raster-verb pattern at dispatch; dispatch is a silent no-op" },
   };

   const struct r300_compute_reject_row *
   r300_compute_reject_lookup(enum r300_compute_reject reason)
   {
   /* One row per enum value: this guard fails the build if a reason is added to
   * the enum without a registry row -- the divergence the registry prevents.
   * STATIC_ASSERT is a do/while statement, so it lives inside a function. */
   STATIC_ASSERT(ARRAY_SIZE(r300_compute_reject_registry) ==
                R300_COMPUTE_REJECT_UNKNOWN_SHAPE + 1);
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

void
r300_nir_detect_ieee16_classify(const nir_shader *s,
                                struct r300_compute_ieee16_classify_pattern *out)
{
   out->is_ieee16_classify   = false;
   out->input_ssbo_binding   = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *load[1] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 1, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 1 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;

   /* Find the ldexp ALU opcode representing the classify placeholder */
   bool found_ldexp = false;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *v = nir_instr_as_alu(instr);
            if (v->op == nir_op_ldexp && v->def.num_components == 4) {
               found_ldexp = true;
               break;
            }
         }
         if (found_ldexp)
            break;
      }
   }
   if (!found_ldexp)
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_ieee16_classify = true;
}

void
r300_nir_detect_ieee16_mul(const nir_shader *s,
                           struct r300_compute_ieee16_mul_pattern *out)
{
   out->is_ieee16_mul        = false;
   out->input_ssbo_binding   = 0;
   out->output_ssbo_binding  = 0;

   const nir_intrinsic_instr *load[1] = {0}, *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 1, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 1 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;

   /* Find the fpow ALU opcode representing the multiply placeholder */
   bool found_fpow = false;
   nir_foreach_function_impl (impl, s) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            const nir_alu_instr *v = nir_instr_as_alu(instr);
            if (v->op == nir_op_fpow && v->def.num_components == 4) {
               found_fpow = true;
               break;
            }
         }
         if (found_fpow)
            break;
      }
   }
   if (!found_fpow)
      return;

   if (nir_src_is_const(load[0]->src[0]))
      out->input_ssbo_binding = nir_src_as_uint(load[0]->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_ieee16_mul = true;
}

/* Constant-fill (CONSTFILL) detector.  Recognises the degenerate shape
 *
 *     out_buffer[gid] = C;     // C is a compile-time constant, no loads
 *
 * which is the limiting case of the identity-map where the stored value is
 * independent of gid and independent of any memory read.  R3V replays this
 * as a CPU buffer fill, so the store offset must name the same contiguous
 * element slots the replay writes.
 *
 * Detection invariants:
 *   1. Exactly 1 store_ssbo, 0 load_ssbo, 0 ssbo_atomic.
 *   2. No loop and no if-branch (the constant is unconditional; a conditional
 *      constant store is the predicated-store shape, not CONSTFILL).
 *   3. The store's byte offset is the flat global invocation index times 4.
 *   4. The store's value SSA def is produced by a nir_instr_type_load_const
 *      instruction -- a NIR compile-time immediate, not a loaded value.
 *
 * Discriminator from every prior admitted shape:
 *   load_count == 0 rules out identity-map (1 load), unary-map (1 load),
 *   binary-map (2 loads), multitap (>= 3 loads), dp4/qmul/qrotate and all
 *   their derivatives (>= 2 loads), blend-acc / ZPASS (1 atomic each), and
 *   multipass (1 load + a loop).  The absent atomic rules out the reduction
 *   shapes; the absent loop/if rules out multipass and predicated-store.
 *
 * const_value[0..3] carries the four uint8_t octets of a scalar uint32_t in
 * host byte order (R=byte 0, G=byte 1, B=byte 2, A=byte 3).
 *
 * output_ssbo_binding is 0 when the post-explicit_io store_ssbo binding source
 * is not a constant; the orchestrator's positional fallback recovers it
 * (binding 0 = output). */
void
r300_nir_detect_const_fill_pattern(const nir_shader *s,
                                   struct r300_compute_const_fill_pattern *out)
{
   out->is_const_fill             = false;
   out->output_ssbo_binding       = 0;
   out->output_ssbo_binding_valid = false;
   out->const_value[0]            = 0;
   out->const_value[1]            = 0;
   out->const_value[2]            = 0;
   out->const_value[3]            = 0;
   out->value_components          = 0;
   out->value_bit_size            = 0;

   const nir_intrinsic_instr *store[1] = {0};
   /* A single-slot dummy array; collect_loads_stores will not write to it
    * because max_loads=0 makes the write guard `if (*nload < max_loads)`
    * permanently false -- *nload still increments as a counter so the
    * post-call `nload != 0` discriminator rejects any load_ssbo.  A
    * non-NULL pointer avoids pointer-arithmetic UB in the collector. */
   const nir_intrinsic_instr *dummy_load[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, dummy_load, 0, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 0 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store[0])
      return;

   /* The store's value source must be produced by a load_const instruction
    * (a NIR compile-time immediate).  Any other producer (an ALU op, a load
    * intrinsic, or a system value) means the value is not a pure compile-time
    * constant and is not the CONSTFILL degenerate shape. */
   const nir_def *val = store[0]->src[0].ssa;
   if (!val || !nir_def_is_const(val))
      return;

   /* Full write mask: the replay writes each scalar element as a whole 32-bit
    * slot.  A partial-mask store cannot be transported faithfully because the
    * unmasked bytes would receive the constant fill bytes. */
   if (nir_intrinsic_has_write_mask(store[0]) &&
       nir_intrinsic_write_mask(store[0]) != BITFIELD_MASK(store[0]->num_components))
      return;

   struct r300_compute_index_pattern ip;
   r300_nir_classify_index_consumption(s, &ip);
   if (!ip.store_offset_valid || !ip.store_offset_global_invocation_only ||
       ip.store_offset_stride != 4 || ip.store_offset_offset != 0)
      return;

   const nir_load_const_instr *lc = nir_def_as_load_const(val);
   const uint8_t bit_size = val->bit_size;
   if (store[0]->num_components != 1 || bit_size != 32)
      return;

   const uint32_t raw = lc->value[0].u32;
   out->const_value[0] = (uint8_t)(raw >>  0);
   out->const_value[1] = (uint8_t)(raw >>  8);
   out->const_value[2] = (uint8_t)(raw >> 16);
   out->const_value[3] = (uint8_t)(raw >> 24);

   if (nir_src_is_const(store[0]->src[1])) {
      out->output_ssbo_binding       = nir_src_as_uint(store[0]->src[1]);
      out->output_ssbo_binding_valid = true;
   }
   out->value_components = store[0]->num_components;
   out->value_bit_size   = bit_size;
   out->is_const_fill    = true;
}

/* Invocation-index consumption classifier.
 *
 * Walks the SSA use chains of every invocation-index intrinsic and reports
 * whether any chain escapes the addressing path (load_ssbo / store_ssbo /
 * load_ubo offset operands) into a stored VALUE.  Address-only consumption is
 * carried by raster texel position at replay time, so it needs no FP24 index
 * materialization; value consumption must materialize a * gid + b in the
 * FP24 fragment ALU and is bounded by the exact-integer ceiling at dispatch
 * (r300_grid_strided_index_exact).
 *
 * The walk tracks an affine state (value = a * gid + b) through the
 * recognized transparent ops: mov / vecN composition, int-float conversions,
 * iadd / fadd with a constant, imul / amul / fmul with a constant, and ishl
 * by a constant.  Any other producer invalidates the affine state; an
 * invalid-affine def reaching a stored value classifies VALUE_GENERAL.  The
 * walk is bounded; overflow of the visited set classifies VALUE_GENERAL,
 * never a weaker class. */

#define R300_INDEX_WALK_MAX_DEFS 256u

/* Per-component affine state: value = ax * id.x + ay * id.y + az * id.z + b.
 * A vec_seed entry is a vecN invocation-id def whose channel c carries
 * s * unit(c) + b; extracting channel c turns it into the scalar state.
 * The scalar algebra is closed under the transparent ops: constant multiply
 * scales all four coefficients, constant add lands in b, and the sum of two
 * tracked states adds componentwise (the sum of affines is affine). */
struct index_walk_entry {
   const nir_def *def;
   bool affine_valid;
   bool global_only;
   bool vec_seed;
   uint64_t s;
   uint64_t ax, ay, az, b;
};

struct index_walk_state {
   struct index_walk_entry queue[R300_INDEX_WALK_MAX_DEFS];
   unsigned head, tail;
   const nir_def *visited[R300_INDEX_WALK_MAX_DEFS];
   struct index_walk_entry states[R300_INDEX_WALK_MAX_DEFS];
   unsigned nvisited;
   bool overflow;
   bool address_use;
   bool value_use_general;
   bool value_use_affine;
   uint64_t value_ax, value_ay, value_az, value_b;
   bool uses_component_y;
   bool uses_component_z;
};

/* A zero coefficient erases the index source before the source-identity gate:
 * local_id * 0 and workgroup_id * 0 are constants, not local/workgroup reads. */
static void
index_entry_normalize_source_identity(struct index_walk_entry *e)
{
   if (!e->affine_valid)
      return;

   if (e->vec_seed) {
      if (e->s == 0)
         e->global_only = true;
      return;
   }

   if (e->ax == 0 && e->ay == 0 && e->az == 0)
      e->global_only = true;
}

static bool
index_walk_push(struct index_walk_state *st, const struct index_walk_entry *e)
{
   struct index_walk_entry normalized = *e;
   index_entry_normalize_source_identity(&normalized);

   for (unsigned i = 0; i < st->nvisited; i++) {
      if (st->visited[i] == normalized.def)
         return true; /* first reaching state wins; DAG revisits are rare */
   }
   if (st->nvisited >= R300_INDEX_WALK_MAX_DEFS ||
       st->tail >= R300_INDEX_WALK_MAX_DEFS) {
      st->overflow = true;
      return false;
   }
   st->states[st->nvisited] = normalized;
   st->visited[st->nvisited++] = normalized.def;
   st->queue[st->tail++] = normalized;
   return true;
}

static const struct index_walk_entry *
index_walk_lookup(const struct index_walk_state *st, const nir_def *def)
{
   for (unsigned i = 0; i < st->nvisited; i++) {
      if (st->visited[i] == def)
         return &st->states[i];
   }
   return NULL;
}

/* Constant operand of a two-source ALU op as a non-negative integer.  Float
 * ops accept only integral values; the affine bound math is integer
 * exactness math. */
static bool
index_walk_const_operand(const nir_alu_instr *alu, unsigned other,
                         bool is_float, uint64_t *out)
{
   if (!nir_src_is_const(alu->src[other].src))
      return false;
   if (is_float) {
      const double v = nir_src_as_float(alu->src[other].src);
      if (!isfinite(v) || v < 0.0 || v > (double)UINT32_MAX)
         return false;
      const uint64_t iv = (uint64_t)v;
      if (v != (double)iv)
         return false;
      *out = iv;
   } else {
      const int64_t v = nir_src_as_int(alu->src[other].src);
      if (v < 0)
         return false;
      *out = (uint64_t)v;
   }
   return true;
}

static bool
index_entry_in_range(const struct index_walk_entry *e)
{
   return e->ax <= UINT32_MAX && e->ay <= UINT32_MAX && e->az <= UINT32_MAX &&
          e->b <= UINT32_MAX && e->s <= UINT32_MAX;
}

/* Resolve the affine state an ALU operand contributes: a scalar entry
 * passes through; a vec_seed entry reading a SINGLE channel c becomes the
 * scalar state s * unit(c) + b.  Mixed-channel reads of a vec_seed stay
 * vector-shaped and are handled by the vec lane in the caller. */
static bool
index_operand_scalar_state(const struct index_walk_entry *in,
                           const nir_alu_instr *alu, unsigned operand,
                           struct index_walk_entry *out)
{
   *out = *in;
   if (!in->vec_seed)
      return true;
   const unsigned nread = nir_ssa_alu_instr_src_components(alu, operand);
   const uint8_t c0 = alu->src[operand].swizzle[0];
   for (unsigned ch = 1; ch < nread; ch++)
      if (alu->src[operand].swizzle[ch] != c0)
         return false; /* mixed channels: not a scalar extraction */
   out->vec_seed = false;
   out->ax = c0 == 0 ? in->s : 0;
   out->ay = c0 == 1 ? in->s : 0;
   out->az = c0 >= 2 ? in->s : 0;
   /* b carries over unchanged. */
   return true;
}

static bool
index_intrinsic_produces_id(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_global_invocation_id:
   case nir_intrinsic_load_global_invocation_index:
   case nir_intrinsic_load_local_invocation_id:
   case nir_intrinsic_load_local_invocation_index:
   case nir_intrinsic_load_workgroup_id:
      return true;
   default:
      return false;
   }
}

static bool
index_intrinsic_is_global_invocation(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_global_invocation_id:
   case nir_intrinsic_load_global_invocation_index:
      return true;
   default:
      return false;
   }
}

/* Base offsets the system-value lowering adds to the invocation id.  The
 * replay substrate does not implement vkCmdDispatchBase, so the base is the
 * zero vector; tracking it as the zero seed lets global_id + base fold to
 * the identity instead of poisoning the chain.  If DispatchBase ever lands,
 * the dispatch-time guard must add the recorded base into the
 * materialized-value bound before this stays sound. */
static bool
index_intrinsic_is_zero_base(const nir_intrinsic_instr *intr)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_load_base_global_invocation_id:
   case nir_intrinsic_load_base_workgroup_id:
      return true;
   default:
      return false;
   }
}

/* Callback for the non-ALU, non-intrinsic consumer sweep: any tracked def
 * feeding a phi / tex / jump has no derivable bound. */
static bool
index_src_is_tracked_cb(nir_src *src, void *data)
{
   struct index_walk_state *st = data;
   if (index_walk_lookup(st, src->ssa))
      st->value_use_general = true;
   return true;
}

void
r300_nir_classify_index_consumption(const nir_shader *s,
                                    struct r300_compute_index_pattern *out)
{
   memset(out, 0, sizeof(*out));
   out->consumption = R300_COMPUTE_INDEX_NONE;

   /* Linear program-order scan.  Admitted kernels are single-block straight
    * lines (the classifier rejects loops and conditionals before any verb
    * matches), and SSA places every operand before its use, so one pass
    * resolves every ALU's affine state from already-resolved operands -- no
    * worklist, no visit-order sensitivity.  Tracked defs carry either a
    * valid state or POISON (index-derived through an unsupported op);
    * poison reaching a stored value classifies VALUE_GENERAL. */
   struct index_walk_state st;
   memset(&st, 0, sizeof(st));

   bool any_index = false;
   bool value_general = false;
   bool value_affine = false;
   bool address_use = false;
   uint64_t vax = 0, vay = 0, vaz = 0, vb = 0;
   bool value_global_only = false;
   bool store_offset_affine = false;
   bool store_offset_general = false;
   bool store_offset_global_only = false;
   uint64_t oax = 0, oay = 0, oaz = 0, ob = 0;

   nir_foreach_function_impl(impl, (nir_shader *)s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_intrinsic) {
               const nir_intrinsic_instr *intr =
                  nir_instr_as_intrinsic(instr);
               struct index_walk_entry seed;
               memset(&seed, 0, sizeof(seed));
               if (index_intrinsic_is_zero_base(intr) ||
                   index_intrinsic_produces_id(intr)) {
                  seed.def = &intr->def;
                  seed.affine_valid = true;
                  if (index_intrinsic_is_zero_base(intr)) {
                     seed.vec_seed = true;
                     seed.s = 0;
                     seed.global_only = true;
                  } else {
                     any_index = true;
                     seed.global_only =
                        index_intrinsic_is_global_invocation(intr);
                     if (intr->def.num_components > 1) {
                        seed.vec_seed = true;
                        seed.s = 1;
                     } else {
                        /* Scalar linear-index intrinsics seed the x lane;
                         * multi-dimensional dispatches of kernels built on
                         * them are caught by the replay's shape check. */
                        seed.ax = 1;
                     }
                  }
                  index_walk_push(&st, &seed);
                  continue;
               }
               /* Consumption sites: classify tracked operands. */
               if (intr->intrinsic == nir_intrinsic_store_ssbo) {
                  const struct index_walk_entry *v =
                     index_walk_lookup(&st, intr->src[0].ssa);
                  const struct index_walk_entry *o =
                     index_walk_lookup(&st, intr->src[2].ssa);
                  if (o)
                     address_use = true;
                  if (o) {
                     if (o->affine_valid && !o->vec_seed &&
                         index_entry_in_range(o)) {
                        if (store_offset_affine &&
                            (oax != o->ax || oay != o->ay ||
                             oaz != o->az || ob != o->b)) {
                           store_offset_general = true;
                        } else {
                           store_offset_global_only =
                              store_offset_affine ?
                              (store_offset_global_only && o->global_only) :
                              o->global_only;
                           store_offset_affine = true;
                           oax = o->ax;
                           oay = o->ay;
                           oaz = o->az;
                           ob = o->b;
                        }
                     } else {
                        store_offset_general = true;
                     }
                  }
                  if (v) {
                     if (v->affine_valid && !v->vec_seed &&
                         index_entry_in_range(v)) {
                        if (value_affine &&
                            (vax != v->ax || vay != v->ay || vaz != v->az ||
                             vb != v->b)) {
                           value_general = true;
                        } else {
                           value_global_only =
                              value_affine ? (value_global_only && v->global_only)
                                           : v->global_only;
                           value_affine = true;
                           vax = v->ax;
                           vay = v->ay;
                           vaz = v->az;
                           vb = v->b;
                        }
                     } else {
                        value_general = true;
                     }
                  }
                  continue;
               }
               if (intr->intrinsic == nir_intrinsic_load_ssbo ||
                   intr->intrinsic == nir_intrinsic_load_ubo) {
                  for (unsigned i = 0;
                       i < nir_intrinsic_infos[intr->intrinsic].num_srcs; i++)
                     if (index_walk_lookup(&st, intr->src[i].ssa))
                        address_use = true;
                  continue;
               }
               if (intr->intrinsic == nir_intrinsic_ssbo_atomic ||
                   intr->intrinsic == nir_intrinsic_ssbo_atomic_swap) {
                  /* Buffer index and byte offset are ADDRESSING, carried by
                   * texel position at replay like any load/store; the data
                   * operands (value, and compare for the swap) would
                   * materialize the index and have no admitted carrier. */
                  for (unsigned i = 0;
                       i < nir_intrinsic_infos[intr->intrinsic].num_srcs;
                       i++) {
                     if (!index_walk_lookup(&st, intr->src[i].ssa))
                        continue;
                     if (i < 2)
                        address_use = true;
                     else
                        value_general = true;
                  }
                  continue;
               }
               /* Any other intrinsic consuming a tracked def: no bound. */
               for (unsigned i = 0;
                    i < nir_intrinsic_infos[intr->intrinsic].num_srcs; i++)
                  if (index_walk_lookup(&st, intr->src[i].ssa))
                     value_general = true;
               continue;
            }

            if (instr->type != nir_instr_type_alu) {
               nir_foreach_src((nir_instr *)instr, index_src_is_tracked_cb,
                               &st);
               continue;
            }

            const nir_alu_instr *alu = nir_instr_as_alu(instr);
            const unsigned ninputs = nir_op_infos[alu->op].num_inputs;

            /* Resolve operand states; record channel-consumption flags. */
            const struct index_walk_entry *in[4] = { NULL, NULL, NULL, NULL };
            bool any_tracked = false;
            for (unsigned i = 0; i < ninputs && i < 4; i++) {
               in[i] = index_walk_lookup(&st, alu->src[i].src.ssa);
               if (!in[i])
                  continue;
               any_tracked = true;
               const unsigned nread =
                  nir_ssa_alu_instr_src_components(alu, i);
               for (unsigned ch = 0; ch < nread; ch++) {
                  if (alu->src[i].swizzle[ch] == 1)
                     st.uses_component_y = true;
                  else if (alu->src[i].swizzle[ch] >= 2)
                     st.uses_component_z = true;
               }
            }
            if (!any_tracked)
               continue;
            if (ninputs > 4) {
               st.value_use_general = true;
               continue;
            }

            struct index_walk_entry r;
            memset(&r, 0, sizeof(r));
            r.def = &alu->def;
            r.affine_valid = false; /* poison unless a lane below validates */

            /* Vector lane: per-channel seeds survive identity moves and the
             * sum of two seeds (the system-value lowering's id + base). */
            const bool in0_vec = in[0] && in[0]->affine_valid &&
                                 in[0]->vec_seed;
            const bool in1_vec = ninputs > 1 && in[1] && in[1]->affine_valid &&
                                 in[1]->vec_seed;
            if ((alu->op == nir_op_mov || alu->op == nir_op_u2f32 ||
                 alu->op == nir_op_i2f32 || alu->op == nir_op_f2u32 ||
                 alu->op == nir_op_f2i32 || alu->op == nir_op_u2u32 ||
                 alu->op == nir_op_i2i32) &&
                in0_vec && alu->def.num_components > 1) {
               bool identity = true;
               for (unsigned ch = 0; ch < alu->def.num_components; ch++)
                  if (alu->src[0].swizzle[ch] != ch)
                     identity = false;
               if (identity) {
                  r = *in[0];
                  r.def = &alu->def;
               }
               index_walk_push(&st, &r);
               continue;
            }
            if ((alu->op == nir_op_iadd || alu->op == nir_op_fadd) &&
                in0_vec && in1_vec && alu->def.num_components > 1) {
               bool identity = true;
               for (unsigned ch = 0; ch < alu->def.num_components; ch++) {
                  if (alu->src[0].swizzle[ch] != ch ||
                      alu->src[1].swizzle[ch] != ch)
                     identity = false;
               }
               if (identity) {
                  r = *in[0];
                  r.def = &alu->def;
                  r.s += in[1]->s;
                  r.b += in[1]->b;
                  r.global_only = in[0]->global_only && in[1]->global_only;
                  r.affine_valid = index_entry_in_range(&r);
               }
               index_walk_push(&st, &r);
               continue;
            }

            /* Scalar lane: resolve each tracked operand to its scalar
             * state (single-channel extraction of a vec seed included). */
            struct index_walk_entry sc[2];
            bool sc_ok[2] = { false, false };
            for (unsigned i = 0; i < 2 && i < ninputs; i++) {
               if (in[i] && in[i]->affine_valid)
                  sc_ok[i] =
                     index_operand_scalar_state(in[i], alu, i, &sc[i]);
            }

            switch (alu->op) {
            case nir_op_mov:
            case nir_op_u2f32:
            case nir_op_i2f32:
            case nir_op_f2u32:
            case nir_op_f2i32:
            case nir_op_u2u32:
            case nir_op_i2i32:
               if (sc_ok[0]) {
                  r = sc[0];
                  r.def = &alu->def;
                  r.affine_valid = true;
               }
               break;
            case nir_op_iadd:
            case nir_op_fadd: {
               const unsigned t = sc_ok[0] ? 0 : 1;
               if (!sc_ok[t])
                  break;
               struct index_walk_entry sum = sc[t];
               sum.def = &alu->def;
               uint64_t c;
               if (in[t ^ 1]) {
                  if (!sc_ok[t ^ 1])
                     break; /* tracked but unresolvable: poison */
                  sum.ax += sc[t ^ 1].ax;
                  sum.ay += sc[t ^ 1].ay;
                  sum.az += sc[t ^ 1].az;
                  sum.b += sc[t ^ 1].b;
                  sum.global_only = sc[t].global_only && sc[t ^ 1].global_only;
               } else if (index_walk_const_operand(alu, t ^ 1,
                                                   alu->op == nir_op_fadd,
                                                   &c)) {
                  sum.b += c;
                  sum.global_only = sc[t].global_only;
               } else {
                  break;
               }
               sum.affine_valid = index_entry_in_range(&sum);
               r = sum;
               break;
            }
            case nir_op_imul:
            case nir_op_amul:
            case nir_op_fmul: {
               const unsigned t = sc_ok[0] ? 0 : 1;
               uint64_t c;
               if (!sc_ok[t] || in[t ^ 1] ||
                   !index_walk_const_operand(alu, t ^ 1,
                                             alu->op == nir_op_fmul, &c))
                  break;
               r = sc[t];
               r.def = &alu->def;
               r.ax *= c;
               r.ay *= c;
               r.az *= c;
               r.b *= c;
               r.affine_valid = index_entry_in_range(&r);
               break;
            }
            case nir_op_ishl: {
               uint64_t c;
               if (!sc_ok[0] || in[1] ||
                   !index_walk_const_operand(alu, 1, false, &c) || c >= 32)
                  break;
               r = sc[0];
               r.def = &alu->def;
               r.ax <<= c;
               r.ay <<= c;
               r.az <<= c;
               r.b <<= c;
               r.affine_valid = index_entry_in_range(&r);
               break;
            }
            default:
               if (nir_op_is_vec(alu->op) && alu->def.num_components == 1 &&
                   sc_ok[0]) {
                  /* Lane composition preserves the carried lane. */
                  r = sc[0];
                  r.def = &alu->def;
                  r.affine_valid = true;
               }
               break;
            }
            index_walk_push(&st, &r);
         }
      }
   }
   if (!any_index)
      return;

   out->uses_component_y = st.uses_component_y;
   out->uses_component_z = st.uses_component_z;
   if (store_offset_affine && !store_offset_general) {
      out->store_offset_valid = true;
      out->store_offset_global_invocation_only = store_offset_global_only;
      out->store_offset_stride = (uint32_t)oax;
      out->store_offset_stride_y = (uint32_t)oay;
      out->store_offset_stride_z = (uint32_t)oaz;
      out->store_offset_offset = (uint32_t)ob;
   }
   if (st.overflow || value_general || st.value_use_general) {
      out->consumption = R300_COMPUTE_INDEX_VALUE_GENERAL;
      return;
   }
   if (value_affine) {
      out->stride_valid = true;
      out->stride = (uint32_t)vax;
      out->stride_y = (uint32_t)vay;
      out->stride_z = (uint32_t)vaz;
      out->offset = (uint32_t)vb;
      out->affine_global_invocation_only = value_global_only;
      out->consumption = (vay || vaz) ? R300_COMPUTE_INDEX_VALUE_AFFINE_3D
                                      : R300_COMPUTE_INDEX_VALUE_AFFINE;
      return;
   }
   (void)address_use;
   out->consumption = R300_COMPUTE_INDEX_ADDRESS_ONLY;
}

/* Affine-iota detector: out[gid] = stride * gid + offset with zero loads.
 * The shape gate is structural (one full-width single-component 32-bit
 * integer store_ssbo, no load / atomic / loop / conditional); the affinity
 * and its coefficients come from the index-consumption classifier, which
 * already proves the stored value is an affine chain of the invocation
 * index with in-range constant coefficients.  A float store is rejected:
 * the RGBA8 byte-decomposition carrier writes the little-endian integer,
 * not an IEEE-754 bit pattern. */
void
r300_nir_detect_affine_iota_pattern(const nir_shader *s,
                                    struct r300_compute_affine_iota_pattern *out)
{
   memset(out, 0, sizeof(*out));

   const nir_intrinsic_instr *store[1] = {0};
   const nir_intrinsic_instr *dummy_load[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, dummy_load, 0, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 0 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store[0] || !store[0]->src[0].ssa)
      return;

   const nir_def *val = store[0]->src[0].ssa;
   if (val->num_components != 1 || val->bit_size != 32)
      return;
   if (nir_intrinsic_has_write_mask(store[0]) &&
       nir_intrinsic_write_mask(store[0]) !=
          BITFIELD_MASK(store[0]->num_components))
      return;
   if (intrinsic_base_type_is_float(store[0], nir_type_invalid))
      return;

   struct r300_compute_index_pattern ip;
   r300_nir_classify_index_consumption(s, &ip);
   if ((ip.consumption != R300_COMPUTE_INDEX_VALUE_AFFINE &&
        ip.consumption != R300_COMPUTE_INDEX_VALUE_AFFINE_3D) ||
       !ip.stride_valid || !ip.affine_global_invocation_only)
      return;
   if (!ip.store_offset_valid || !ip.store_offset_global_invocation_only ||
       ip.store_offset_stride != 4 || ip.store_offset_offset != 0)
      return;
   if (ip.consumption == R300_COMPUTE_INDEX_VALUE_AFFINE &&
       (ip.store_offset_stride_y || ip.store_offset_stride_z))
      return;

   if (nir_src_is_const(store[0]->src[1])) {
      out->output_ssbo_binding       = nir_src_as_uint(store[0]->src[1]);
      out->output_ssbo_binding_valid = true;
   }
   out->stride         = ip.stride;
   out->stride_y       = ip.stride_y;
   out->stride_z       = ip.stride_z;
   out->offset         = ip.offset;
   out->output_offset_stride   = ip.store_offset_stride;
   out->output_offset_stride_y = ip.store_offset_stride_y;
   out->output_offset_stride_z = ip.store_offset_stride_z;
   out->output_offset_offset   = ip.store_offset_offset;
   out->is_affine_iota = true;
}

/* Multilimb u32 multiply detector: the binary-map imul shape narrowed to
 * single-component 32-bit integer operands.  One store_ssbo whose value is
 * an imul (or amul) of exactly the two load_ssbo defs.  Bindings are read
 * off constant sources where available; the orchestrator's positional
 * fallback (a = first compute-visible STORAGE_BUFFER, b = second, out = third)
 * recovers them otherwise. */
void
r300_nir_detect_multilimb_mul_pattern(const nir_shader *s,
                                      struct r300_compute_multilimb_mul_pattern *out)
{
   memset(out, 0, sizeof(*out));

   const nir_intrinsic_instr *load[2] = {0};
   const nir_intrinsic_instr *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 2, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 2 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store[0] || !store[0]->src[0].ssa || !load[0] || !load[1])
      return;

   const nir_def *val = store[0]->src[0].ssa;
   if (val->num_components != 1 || val->bit_size != 32)
      return;
   if (intrinsic_base_type_is_float(store[0], nir_type_invalid))
      return;

   const nir_alu_instr *alu = nir_def_as_alu_or_null(val);
   if (!alu || (alu->op != nir_op_imul && alu->op != nir_op_amul))
      return;
   const nir_def *sa = alu->src[0].src.ssa;
   const nir_def *sb = alu->src[1].src.ssa;
   const bool direct = sa == &load[0]->def && sb == &load[1]->def;
   const bool swapped = sa == &load[1]->def && sb == &load[0]->def;
   if (!direct && !swapped)
      return;

   const nir_intrinsic_instr *la = direct ? load[0] : load[1];
   const nir_intrinsic_instr *lb = direct ? load[1] : load[0];
   if (nir_src_is_const(la->src[0]))
      out->input_a_ssbo_binding = nir_src_as_uint(la->src[0]);
   if (nir_src_is_const(lb->src[0]))
      out->input_b_ssbo_binding = nir_src_as_uint(lb->src[0]);
   if (nir_src_is_const(store[0]->src[1]))
      out->output_ssbo_binding = nir_src_as_uint(store[0]->src[1]);
   out->is_multilimb_mul = true;
}

/* Constant-operand compare-and-swap detector.  Exactly one
 * ssbo_atomic_swap with ATOMIC_OP cmpxchg whose compare (src[2]) and new
 * (src[3]) operands are 32-bit compile-time constants, exactly one
 * store_ssbo whose stored value is the atomic's def (the returned old),
 * zero load_ssbo, no loop or conditional.  The guard and result bindings
 * come off constant sources where available; the orchestrator's positional
 * fallback (guard = first compute-visible STORAGE_BUFFER, result = second)
 * recovers them otherwise. */
void
r300_nir_detect_cas_pattern(const nir_shader *s,
                            struct r300_compute_cas_pattern *out)
{
   memset(out, 0, sizeof(*out));

   const nir_intrinsic_instr *swap = NULL;
   const nir_intrinsic_instr *store = NULL;
   unsigned nswap = 0, nstore = 0, nload = 0, nother = 0;
   nir_foreach_function_impl(impl, (nir_shader *)s) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_ssbo_atomic_swap) {
               swap = intr;
               nswap++;
            } else if (intr->intrinsic == nir_intrinsic_store_ssbo) {
               store = intr;
               nstore++;
            } else if (intr->intrinsic == nir_intrinsic_load_ssbo) {
               nload++;
            } else if (strstr(nir_intrinsic_infos[intr->intrinsic].name,
                              "atomic")) {
               nother++;
            }
         }
      }
   }
   if (nswap != 1 || nstore != 1 || nload != 0 || nother != 0)
      return;
   if (nir_intrinsic_atomic_op(swap) != nir_atomic_op_cmpxchg)
      return;
   if (swap->def.bit_size != 32 || swap->def.num_components != 1)
      return;
   if (!nir_src_is_const(swap->src[2]) || !nir_src_is_const(swap->src[3]))
      return;
   /* The stored value must be the atomic's returned old. */
   if (store->src[0].ssa != &swap->def)
      return;

   out->expect    = (uint32_t)nir_src_as_uint(swap->src[2]);
   out->value_new = (uint32_t)nir_src_as_uint(swap->src[3]);
   if (nir_src_is_const(swap->src[0])) {
      out->guard_ssbo_binding = (uint32_t)nir_src_as_uint(swap->src[0]);
      out->guard_binding_valid = true;
   }
   if (nir_src_is_const(store->src[1])) {
      out->result_ssbo_binding = (uint32_t)nir_src_as_uint(store->src[1]);
      out->result_binding_valid = true;
   }
   out->is_cas = true;
}

/* Resolve a byte-offset def to a per-component affine of the invocation id
 * plus at most one opaque descriptor-base term:
 * off = ax * id.x + ay * id.y + az * id.z + b (+ base).  Mirrors the
 * scalar lanes of the index-consumption scan recursively over the offset
 * chain; anything outside the transparent op set fails.  The single base
 * term absorbs the post-explicit-io descriptor add (load_vulkan_descriptor
 * component + 0). */
struct log4_affine {
   uint64_t ax, ay, az, b;
   bool has_base;
};

static bool
log4_resolve_offset(const nir_def *def, unsigned channel, unsigned depth,
                    struct log4_affine *out)
{
   if (depth > 12 || channel > 3)
      return false;
   memset(out, 0, sizeof(*out));

   if (nir_def_instr(def)->type == nir_instr_type_load_const) {
      const nir_load_const_instr *lc = nir_def_as_load_const((nir_def *)def);
      out->b = lc->value[channel].u32;
      return true;
   }
   if (nir_def_instr(def)->type == nir_instr_type_intrinsic) {
      const nir_intrinsic_instr *intr =
         nir_instr_as_intrinsic(nir_def_instr(def));
      if (index_intrinsic_is_zero_base(intr))
         return true; /* the zero vector */
      if (index_intrinsic_produces_id(intr)) {
         /* The consumer's swizzle channel selects the unit lane. */
         out->ax = channel == 0;
         out->ay = channel == 1;
         out->az = channel >= 2;
         return true;
      }
      if (intr->intrinsic == nir_intrinsic_load_vulkan_descriptor) {
         out->has_base = true;
         return true;
      }
      return false;
   }
   if (nir_def_instr(def)->type != nir_instr_type_alu)
      return false;

   const nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(def));
   struct log4_affine a, b;
   /* Each recursion threads the channel through the operand's swizzle, so
    * a vec-channel read at ANY use edge (imul 2, id.y; iadd id, base; a
    * bare mov) resolves the right lane. */
   switch (alu->op) {
   case nir_op_mov:
   case nir_op_u2u32:
   case nir_op_i2i32:
      if (!log4_resolve_offset(alu->src[0].src.ssa,
                               alu->src[0].swizzle[channel], depth + 1, &a))
         return false;
      *out = a;
      return true;
   case nir_op_iadd:
      if (!log4_resolve_offset(alu->src[0].src.ssa,
                               alu->src[0].swizzle[channel], depth + 1, &a) ||
          !log4_resolve_offset(alu->src[1].src.ssa,
                               alu->src[1].swizzle[channel], depth + 1, &b))
         return false;
      if (a.has_base && b.has_base)
         return false;
      out->ax = a.ax + b.ax;
      out->ay = a.ay + b.ay;
      out->az = a.az + b.az;
      out->b = a.b + b.b;
      out->has_base = a.has_base || b.has_base;
      return true;
   case nir_op_imul:
   case nir_op_amul: {
      unsigned ci = 2;
      if (nir_src_is_const(alu->src[1].src))
         ci = 1;
      else if (nir_src_is_const(alu->src[0].src))
         ci = 0;
      if (ci == 2)
         return false;
      const int64_t cv = nir_src_as_int(alu->src[ci].src);
      if (cv < 0)
         return false;
      if (!log4_resolve_offset(alu->src[ci ^ 1].src.ssa,
                               alu->src[ci ^ 1].swizzle[channel], depth + 1,
                               &a) ||
          a.has_base)
         return false;
      out->ax = a.ax * (uint64_t)cv;
      out->ay = a.ay * (uint64_t)cv;
      out->az = a.az * (uint64_t)cv;
      out->b = a.b * (uint64_t)cv;
      return true;
   }
   case nir_op_ishl: {
      if (!nir_src_is_const(alu->src[1].src))
         return false;
      const uint64_t c = nir_src_as_uint(alu->src[1].src);
      if (c >= 32 ||
          !log4_resolve_offset(alu->src[0].src.ssa,
                               alu->src[0].swizzle[channel], depth + 1, &a) ||
          a.has_base)
         return false;
      out->ax = a.ax << c;
      out->ay = a.ay << c;
      out->az = a.az << c;
      out->b = a.b << c;
      return true;
   }
   default:
      return false;
   }
}

/* Flatten a sum tree under the >> 2 into its leaves: the four load defs
 * plus one constant 2.  Any other leaf fails. */
static bool
log4_collect_sum(const nir_def *def, const nir_intrinsic_instr *load[4],
                 bool seen[4], bool *seen_two, unsigned depth)
{
   if (depth > 8)
      return false;
   if (nir_def_instr(def)->type == nir_instr_type_load_const) {
      if (*seen_two ||
          nir_def_as_load_const((nir_def *)def)->value[0].u32 != 2)
         return false;
      *seen_two = true;
      return true;
   }
   if (nir_def_instr(def)->type == nir_instr_type_intrinsic) {
      for (unsigned i = 0; i < 4; i++) {
         if (def == &load[i]->def) {
            if (seen[i])
               return false;
            seen[i] = true;
            return true;
         }
      }
      return false;
   }
   if (nir_def_instr(def)->type != nir_instr_type_alu)
      return false;
   const nir_alu_instr *alu = nir_instr_as_alu(nir_def_instr(def));
   if (alu->op != nir_op_iadd)
      return false;
   return log4_collect_sum(alu->src[0].src.ssa, load, seen, seen_two,
                           depth + 1) &&
          log4_collect_sum(alu->src[1].src.ssa, load, seen, seen_two,
                           depth + 1);
}

static bool
log4_same_ssbo_source(nir_src a, nir_src b)
{
   if (nir_src_is_const(a) || nir_src_is_const(b))
      return nir_src_is_const(a) && nir_src_is_const(b) &&
             nir_src_as_uint(a) == nir_src_as_uint(b);

   return a.ssa && b.ssa && a.ssa == b.ssa;
}

void
r300_nir_detect_log4_pool_pattern(const nir_shader *s,
                                  struct r300_compute_log4_pool_pattern *out)
{
   memset(out, 0, sizeof(*out));

   const nir_intrinsic_instr *load[4] = {0};
   const nir_intrinsic_instr *store[1] = {0};
   unsigned nload, nstore, natomic;
   bool has_loop, in_if;
   collect_loads_stores(s, load, 4, &nload, store, 1, &nstore, &natomic,
                        &has_loop, &in_if);
   if (nload != 4 || nstore != 1 || natomic != 0 || has_loop || in_if)
      return;
   if (!store[0] || !store[0]->src[0].ssa)
      return;
   const nir_def *val = store[0]->src[0].ssa;
   if (val->num_components != 1 || val->bit_size != 32)
      return;
   for (unsigned i = 0; i < 4; i++) {
      if (!load[i] || load[i]->def.num_components != 1 ||
          load[i]->def.bit_size != 32)
         return;
      if (i != 0 &&
          !log4_same_ssbo_source(load[i]->src[0], load[0]->src[0]))
         return;
   }

   /* The half-up form: (sum of the four loads + 2) >> 2. */
   const nir_alu_instr *shr = nir_def_as_alu_or_null(val);
   if (!shr || shr->op != nir_op_ushr ||
       !nir_src_is_const(shr->src[1].src) ||
       nir_src_as_uint(shr->src[1].src) != 2)
      return;
   bool seen[4] = { false, false, false, false };
   bool seen_two = false;
   if (!log4_collect_sum(shr->src[0].src.ssa, load, seen, &seen_two, 0) ||
       !seen_two || !seen[0] || !seen[1] || !seen[2] || !seen[3])
      return;

   /* Load offsets: shared strides (8, 8W), base deltas {0, 4, 4W, 4W + 4}. */
   struct log4_affine off[4];
   for (unsigned i = 0; i < 4; i++) {
      if (!log4_resolve_offset(load[i]->src[1].ssa, 0, 0, &off[i]))
         return;
      if (off[i].az != 0)
         return;
   }
   const uint64_t sx = off[0].ax, sy = off[0].ay;
   if (sx != 8 || sy == 0 || (sy & 7) != 0)
      return;
   for (unsigned i = 1; i < 4; i++)
      if (off[i].ax != sx || off[i].ay != sy)
         return;
   const uint64_t base = off[0].b < off[1].b ? (off[0].b < off[2].b
                            ? (off[0].b < off[3].b ? off[0].b : off[3].b)
                            : (off[2].b < off[3].b ? off[2].b : off[3].b))
                                             : (off[1].b < off[2].b
                            ? (off[1].b < off[3].b ? off[1].b : off[3].b)
                            : (off[2].b < off[3].b ? off[2].b : off[3].b));
   if (base != 0)
      return;
   const uint64_t half_row = sy / 2; /* 4W bytes */
   bool want[4] = { false, false, false, false };
   for (unsigned i = 0; i < 4; i++) {
      const uint64_t d = off[i].b - base;
      unsigned slot = 4;
      if (d == 0)
         slot = 0;
      else if (d == 4)
         slot = 1;
      else if (d == half_row)
         slot = 2;
      else if (d == half_row + 4)
         slot = 3;
      if (slot == 4 || want[slot])
         return;
      want[slot] = true;
   }
   const uint64_t row_w = sy / 8; /* W elements: sy = 8W bytes */

   /* Store offset: strides (4, 4 * W/2) -- the half-extent output grid. */
   struct log4_affine so;
   if (!log4_resolve_offset(store[0]->src[2].ssa, 0, 0, &so) || so.az != 0 ||
       so.b != 0 || so.ax != 4 || so.ay != 2 * row_w)
      return;

   out->row_w = (uint32_t)row_w;
   if (nir_src_is_const(load[0]->src[0])) {
      out->input_ssbo_binding = (uint32_t)nir_src_as_uint(load[0]->src[0]);
      out->input_binding_valid = true;
   }
   if (nir_src_is_const(store[0]->src[1])) {
      out->output_ssbo_binding =
         (uint32_t)nir_src_as_uint(store[0]->src[1]);
      out->output_binding_valid = true;
   }
   out->is_log4_pool = true;
}
