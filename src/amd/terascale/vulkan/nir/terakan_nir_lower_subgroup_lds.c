/*
 * SPDX-License-Identifier: MIT
 *
 * LDS-emulated cross-lane subgroup primitives for Palm / Wrestler
 * (CHIP_PALM, Evergreen / TeraScale-2 VLIW5).
 *
 * Hardware reality (per AMD Evergreen-Family Instruction Set Architecture):
 * the TeraScale-2 VLIW5 ALU has no native cross-lane communication.  Wave64
 * lanes execute lockstep within a SIMD but cannot read each other's GPRs
 * directly.  Cross-lane data movement is only possible through:
 *
 *   - Local Data Share (LDS, 32 KB per SIMD), with atomic-OR/AND/ADD/MIN/MAX
 *     and synchronous barrier (the LDS_ATOMIC_<op>_RET family in the
 *     Evergreen ISA, e.g. LDS_OR_RET).
 *   - MEM_RAT atomic ops through the L2 cache (slower, serializes at the
 *     cache lock -- viable for ballot-style reductions, not used here).
 *   - Generic VFETCH from a workgroup-scratch buffer (slower fallback).
 *
 * This pass lowers the cross-lane subgroup intrinsics to LDS sequences.
 * The foundational primitive is nir_intrinsic_ballot; once ballot is in
 * place, Mesa's nir_lower_subgroups decomposes vote_any/vote_all/reduce
 * into ballot + scalar ALU ops.
 *
 * LDS layout for ballot (wave64):
 *
 *   reserve 8 bytes (2 DWORDs) of shared memory per subgroup-using call
 *   site, initialized to 0 before the ballot.
 *
 *   Each lane writes its 1-bit predicate into bit `lane_id` of the slot:
 *     if (predicate) shared_atomic_or(slot + (lane_id / 32) * 4,
 *                                     1u << (lane_id % 32))
 *
 *   After a workgroup barrier, all lanes read the 8-byte slot as a uvec2
 *   and reset the slot to 0 for the next ballot.
 *
 * Broadcast layout (read_first_invocation, read_invocation):
 *
 *   Reserve 1 DWORD (or one slot of the value's bit_size).  The selected
 *   source lane writes; barrier; all lanes read.  Lane selection is "the
 *   first set bit of the active execution mask" for read_first_invocation
 *   and the source-lane operand for read_invocation -- emit a per-lane
 *   "I am the source" predicate check and the write is guarded.
 *
 * The pass runs after nir_lower_explicit_io (which has already converted
 * any shader-declared shared variables to load_shared / store_shared
 * intrinsics), allocates its own offsets within shader_info.shared_size,
 * and emits load_shared / store_shared / shared_atomic intrinsics that
 * the existing terakan compute LDS path handles.
 */

#include "terakan_nir.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"

#include <assert.h>
#include <stdint.h>

/* Wave size on Evergreen / TeraScale-2 VLIW5 (Palm / Wrestler, CHIP_PALM)
 * is fixed at 64 lanes per SIMD per the AMD Evergreen-Family Instruction
 * Set Architecture.  Keep these as named constants because every lowering
 * helper below derives slot offsets, lane masks, and per-lane bit indices
 * from them. */
#define TERAKAN_SUBGROUP_SIZE      64u
#define TERAKAN_SUBGROUP_LOG2_SIZE 6u

/* LDS allocation tracking.  Two policies coexist:
 *
 * 1. Per-call-site stacked slots for ballot:
 *    Each ballot lowering reserves an 8-byte slot at the tail of
 *    shared_size.  Slots from different ballot call sites do not
 *    collide; per-call-site allocation guarantees one slot is owned
 *    by exactly one ballot expansion.
 *
 * 2. Single per-shader lane-indexed region for broadcast variants
 *    (read_first_invocation, read_invocation, shuffle):
 *    One subgroup_size * 4 byte region allocated ONCE per shader,
 *    addressed as region + lane_id * 4.  Each lane writes its own
 *    value to its own slot (no contention, no atomic, no
 *    nir_push_if), one workgroup barrier, all lanes read the source
 *    lane's slot.  Independent per-lane stores defeat both LDS bank
 *    contention and the SFN VLIW5 scheduler's GROUP_BARRIER
 *    slot-pinning pressure that compounded with the
 *    per-call-site-loop variant.  Region allocated lazily on first
 *    broadcast intrinsic so shaders with only ballot pay no extra
 *    LDS cost.
 */
struct subgroup_lds_alloc {
   uint32_t shared_size_bytes;
   uint32_t broadcast_region_offset;
};

#define TERAKAN_NO_BROADCAST_REGION UINT32_MAX
#define TERAKAN_BROADCAST_REGION_BYTES (TERAKAN_SUBGROUP_SIZE * 4u)

/* Wave64 lane-id derivation shared by every ballot and broadcast
 * variant.  Computed from gl_LocalInvocationIndex rather than via
 * nir_load_subgroup_invocation -- the r600 SFN backend reports
 * "Unsupported instruction" on the latter since historic r600
 * Gallium always advertised subgroup_size = 1. */
static inline nir_def *
build_lane_id(nir_builder *b)
{
   return nir_iand_imm(b, nir_load_local_invocation_index(b),
                       TERAKAN_SUBGROUP_SIZE - 1u);
}

static uint32_t
alloc_lds_slot(struct subgroup_lds_alloc *alloc,
               uint32_t size_bytes, uint32_t align_bytes)
{
   alloc->shared_size_bytes = ALIGN_POT(alloc->shared_size_bytes, align_bytes);
   uint32_t const offset = alloc->shared_size_bytes;
   alloc->shared_size_bytes += size_bytes;
   return offset;
}

/* Lower nir_intrinsic_ballot to a sequence of:
 *
 *    lane_bit = 1u << (gl_LocalInvocationIndex & 31)
 *    word     = gl_LocalInvocationIndex >> 5
 *    if (predicate)
 *       shared_atomic_or(BALLOT_BASE + word*4, lane_bit)
 *    workgroup_memory_barrier()
 *    result.x = load_shared(BALLOT_BASE + 0)
 *    result.y = load_shared(BALLOT_BASE + 4)
 *    workgroup_memory_barrier()
 *    if (lane_id == 0)
 *       store_shared(BALLOT_BASE + 0, 0)
 *       store_shared(BALLOT_BASE + 4, 0)
 *    workgroup_memory_barrier()
 *
 * The second barrier + reset is required because the same workgroup may
 * execute another ballot afterwards on the same LDS slot.  Per AMD
 * Evergreen-Family Instruction Set Architecture, LDS_ATOMIC_OR_RET
 * combines lane bits into the slot in a single LDS-bank-arbitrated
 * cycle; the barrier ensures all atomic-OR'd contributions are visible
 * before the load.
 *
 * Bit width: produces a uvec2 with subgroup_size = 64.  Higher-tier ops
 * (subgroupBallotBitCount, subgroupBallotFindLSB/MSB) are decomposed by
 * nir_lower_subgroups using bit_count / find_lsb on this uvec2.
 */
static nir_def *
lower_ballot(nir_builder *b, nir_intrinsic_instr *intrin,
             struct subgroup_lds_alloc *alloc)
{
   /* Per-call-site LDS slot: single-use, so no post-load reset is
    * needed.  Eight bytes covers the wave64 uvec2 ballot result. */
   uint32_t const slot_off = alloc_lds_slot(alloc, 8, 4);

   nir_def *predicate = intrin->src[0].ssa;
   if (predicate->bit_size != 1)
      predicate = nir_ine_imm(b, predicate, 0);

   /* The SFN backend has no native load_subgroup_invocation lowering
    * (r600 historically reported subgroup_size = 1, see
    * lower_load_subgroup_invocation).  Inline the Wave64 lowering here
    * because nir_shader_intrinsics_pass only walks the instruction
    * list as captured at entry -- any nir_load_subgroup_invocation
    * emitted from this pass would not be revisited. */
   nir_def *lane_id =
      nir_iand_imm(b, nir_load_local_invocation_index(b),
                   TERAKAN_SUBGROUP_SIZE - 1u);
   nir_def *word_off = nir_imul_imm(b, nir_ushr_imm(b, lane_id, 5), 4);
   nir_def *lane_bit = nir_ishl(b, nir_imm_int(b, 1),
                                nir_iand_imm(b, lane_id, 31));
   nir_def *base = nir_imm_int(b, slot_off);

   /* Per AMD Evergreen-Family Instruction Set Architecture section
    * 2.6.2, the per-SIMD LDS region is "shared memory" with no
    * hardware zero-initialisation contract at workgroup launch --
    * each shader must initialise the LDS region it reads.  Our
    * single-barrier all-lane atomic-OR relies on the slot starting
    * at zero, so emit an explicit clear FIRST.
    *
    * Clear shape: every lane does LDS_AND with 0 on both DWORDs of
    * the ballot slot.  AND-with-0 is idempotent (N lockstep lanes
    * converging on 0 just store 0) and Wave64-lockstep-safe for the
    * LDS bank-arbitration cycle (the ISA documents the 32 integer
    * atomic units as "unordered" but the final state is
    * commutative-associative for AND/OR).  Two atomic_and ops per
    * slot (one per 32-bit half of the wave64 ballot uvec2) plus one
    * workgroup barrier so the zero state is globally visible before
    * the subsequent atomic_or.  The clear path replaces the earlier
    * lane-0 + push_if approach that compounded GROUP_BARRIER
    * slot-pinning rejections in the SFN VLIW5 scheduler. */
   nir_def *zero = nir_imm_int(b, 0);
   nir_shared_atomic(b, 32, base, zero, .base = 0,
                     .atomic_op = nir_atomic_op_iand);
   nir_shared_atomic(b, 32, base, zero, .base = 4,
                     .atomic_op = nir_atomic_op_iand);

   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
               .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL,
               .memory_modes = nir_var_mem_shared);

   /* Every lane participates in the atomic-OR, contributing either
    * its lane bit or zero.  Issuing the LDS_ATOMIC_OR_RET from every
    * lane lockstep is what the AMD Evergreen-Family Instruction Set
    * Architecture LDS bank-arbitration cycle expects, and it lets
    * the SFN scheduler pack the atomic without a surrounding
    * control-flow predicate -- the original push_if/pop_if pair
    * created an extra basic-block boundary that compounded the
    * GROUP_BARRIER slot-pinning rejections seen at
    * sfn_instr_alu.h:90 in the three-barrier variant. */
   nir_def *atomic_addr = nir_iadd(b, base, word_off);
   nir_def *or_value = nir_bcsel(b, predicate, lane_bit, nir_imm_int(b, 0));
   nir_shared_atomic(b, 32, atomic_addr, or_value,
                     .atomic_op = nir_atomic_op_ior);

   /* Workgroup barrier so all lane contributions are drained before
    * the load.  Per-call-site LDS slot allocation means no
    * subsequent ballot can observe a residual; no trailing barrier
    * needed. */
   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
               .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL,
               .memory_modes = nir_var_mem_shared);

   nir_def *lo = nir_load_shared(b, 1, 32, base, .base = 0, .align_mul = 8);
   nir_def *hi = nir_load_shared(b, 1, 32, base, .base = 4, .align_mul = 8);
   return nir_vec2(b, lo, hi);
}

/* Lower nir_intrinsic_read_first_invocation: the value from the first
 * active lane is broadcast to all active lanes.  Implementation:
 *
 *   For each 32-bit chunk of the value (vector components are
 *   independent LDS slots):
 *
 *     slot starts at 0 (LDS allocation zeroes by construction since
 *     alloc_lds_slot extends shared_size_bytes -- the kernel + radeon
 *     ucode zero new LDS pages on workgroup launch).
 *     contribution = (lane == 0) ? value_chunk : 0
 *     LDS_ATOMIC_OR_RET(slot, contribution)
 *
 *     workgroup_memory_barrier
 *     result_chunk = load_shared(slot)
 *
 * Identical shape to the simplified lower_ballot: every lane
 * participates in the LDS atomic so the AMD Evergreen-Family
 * Instruction Set Architecture LDS bank-arbitration cycle sees Wave64
 * lockstep, no nir_push_if / pop_if boundary creates a basic-block
 * that competes with surrounding GROUP_BARRIER slot pinning, and
 * only one workgroup barrier per call site.  Per-call-site LDS slot
 * allocation makes the post-load reset / trailing barrier
 * unnecessary.
 *
 * Sub-32-bit values (8, 16 bit) are widened with nir_u2u32 before the
 * atomic and narrowed back with nir_u2u<bit_size> after the load.
 * Float values are bitcast through uint storage.  Multi-component
 * vectors split into independent slots so the atomic-OR semantics
 * stay valid per chunk.  64-bit chunks decompose into two 32-bit
 * halves.
 */
/* Lane-indexed broadcast.
 *
 * Allocate ONE per-shader region of size TERAKAN_BROADCAST_REGION_BYTES
 * = subgroup_size * 4.  For each 32-bit chunk of the broadcast value:
 *
 *   store_shared(region + lane_id * 4, value_chunk)   -- ALL lanes
 *   workgroup_barrier
 *   result_chunk = load_shared(region + src_lane * 4) -- ALL lanes
 *
 * Per the AMD Evergreen-Family Instruction Set Architecture section
 * 2.6.2 (Local Data Share), the per-SIMD 32 KB LDS region has 32
 * banks and is "a data exchange machine for the work-items of a
 * work-group".  Lane-indexed addresses mean each lane writes a
 * bank-disjoint slot for itself:
 *
 *   - No atomic op.  Lane addresses never collide.  LDS bank
 *     arbitration stays contention-free.
 *   - No nir_push_if.  Every lane is a writer with its own value,
 *     so no basic-block boundary contends with the surrounding
 *     GROUP_BARRIER slot pinning that the SFN VLIW5 scheduler
 *     enforces on Palm / Wrestler (CHIP_PALM, Evergreen /
 *     TeraScale-2 VLIW5).
 *   - One workgroup barrier per 32-bit chunk.  The region is
 *     allocated ONCE per shader and reused across every broadcast
 *     call site, so an N-iteration broadcast loop costs N plain
 *     stores + N barriers + N plain loads -- independent of the
 *     per-call-site allocation depth that the earlier atomic-OR
 *     shape compounded with.
 *
 * Property: forall lane : Fin SUBGROUP_SIZE,
 * region[lane] = lane.value after the per-lane store, and
 * reader.result = region[src_lane] after the workgroup barrier
 * drains every store.
 *
 * LDS cost: TERAKAN_BROADCAST_REGION_BYTES = 256 bytes per shader
 * (64 lanes on Wave64).  Net cheaper than the per-call-site
 * stacked-slot policy once a shader contains more than 64
 * broadcast call sites.
 *
 * Sub-32-bit values widen via nir_u2u32 / narrow back via
 * nir_u2u<bit_size>; 64-bit chunks split / recombine via
 * nir_unpack_64_2x32_split_* / nir_pack_64_2x32_split.
 */
static nir_def *
emit_broadcast_via_lds(nir_builder *b, nir_def *value, nir_def *src_lane,
                       struct subgroup_lds_alloc *alloc)
{
   unsigned const bit_size = value->bit_size;
   unsigned const num_components = value->num_components;
   bool const is_64 = (bit_size == 64);
   unsigned const chunks_per_comp = is_64 ? 2u : 1u;
   unsigned const total_chunks = num_components * chunks_per_comp;

   /* Lazily allocate the per-shader lane-indexed region on first
    * broadcast.  A shader that uses only ballot pays no extra LDS
    * cost. */
   if (alloc->broadcast_region_offset == TERAKAN_NO_BROADCAST_REGION) {
      alloc->broadcast_region_offset =
         alloc_lds_slot(alloc, TERAKAN_BROADCAST_REGION_BYTES, 4u);
   }
   uint32_t const region_off = alloc->broadcast_region_offset;

   nir_def *region_base = nir_imm_int(b, region_off);
   nir_def *lane_id = build_lane_id(b);
   nir_def *lane_addr =
      nir_iadd(b, region_base, nir_imul_imm(b, lane_id, 4));
   nir_def *src_addr =
      nir_iadd(b, region_base, nir_imul_imm(b, src_lane, 4));

   /* Reinterpret value as 32-bit chunks. */
   nir_def *u32_chunks[NIR_MAX_VEC_COMPONENTS * 2];
   for (unsigned c = 0; c < num_components; ++c) {
      nir_def *comp = nir_channel(b, value, c);
      if (bit_size < 32) {
         u32_chunks[c] = nir_u2u32(b, comp);
      } else if (bit_size == 32) {
         u32_chunks[c] = nir_mov(b, comp);
      } else {
         u32_chunks[c * 2 + 0] = nir_unpack_64_2x32_split_x(b, comp);
         u32_chunks[c * 2 + 1] = nir_unpack_64_2x32_split_y(b, comp);
      }
   }

   /* Iterate over chunks.  Each chunk reuses the same lane-indexed
    * region: barrier between chunks so the prior chunk's store is
    * captured into out_chunks before this chunk overwrites the
    * region. */
   nir_def *out_chunks[NIR_MAX_VEC_COMPONENTS * 2];
   for (unsigned i = 0; i < total_chunks; ++i) {
      nir_store_shared(b, u32_chunks[i], lane_addr, .base = 0,
                       .align_mul = 4, .write_mask = 0x1);

      nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
                  .memory_scope = SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = nir_var_mem_shared);

      out_chunks[i] = nir_load_shared(b, 1, 32, src_addr,
                                      .base = 0, .align_mul = 4);
   }

   /* Reassemble the original type. */
   nir_def *out_components[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < num_components; ++c) {
      if (!is_64) {
         if (bit_size < 32) {
            out_components[c] = (bit_size == 16)
               ? nir_u2u16(b, out_chunks[c])
               : nir_u2u8(b, out_chunks[c]);
         } else {
            out_components[c] = out_chunks[c];
         }
      } else {
         out_components[c] =
            nir_pack_64_2x32_split(b, out_chunks[c * 2 + 0],
                                   out_chunks[c * 2 + 1]);
      }
   }
   return nir_vec(b, out_components, num_components);
}

static nir_def *
lower_read_first_invocation(nir_builder *b, nir_intrinsic_instr *intrin,
                            struct subgroup_lds_alloc *alloc)
{
   nir_def *value = intrin->src[0].ssa;
   return emit_broadcast_via_lds(b, value, nir_imm_int(b, 0), alloc);
}

/* nir_intrinsic_read_invocation: broadcast value from a dynamic
 * source lane (intrin->src[1]) to all lanes.  Under the
 * lane-indexed shape each reader picks region[src_lane] directly --
 * exact even when src_lane is per-reader divergent. */
static nir_def *
lower_read_invocation(nir_builder *b, nir_intrinsic_instr *intrin,
                      struct subgroup_lds_alloc *alloc)
{
   nir_def *value = intrin->src[0].ssa;
   nir_def *src_lane = intrin->src[1].ssa;
   return emit_broadcast_via_lds(b, value, src_lane, alloc);
}

/* nir_intrinsic_shuffle: per-lane source.  Under the lane-indexed
 * shape this is exact: write region[lane_id] = value, barrier, read
 * region[per_lane_src_lane]. */
static nir_def *
lower_shuffle(nir_builder *b, nir_intrinsic_instr *intrin,
              struct subgroup_lds_alloc *alloc)
{
   nir_def *value = intrin->src[0].ssa;
   nir_def *src_lane = intrin->src[1].ssa;
   return emit_broadcast_via_lds(b, value, src_lane, alloc);
}

/* Butterfly LDS reduction.
 *
 * For subgroup reductions (subgroupAdd / Min / Max / And / Or / Xor /
 * etc.) the goal is to compute op(value across all lanes) and
 * broadcast the result to every lane.  The classic shape on
 * hardware without native cross-lane ALU is the butterfly /
 * recursive-halving pattern: at each level k = 0..log2(N)-1, lane L
 * exchanges values with lane L XOR (1 << k) and applies the
 * associative operator.  After log2(N) hops every lane holds the
 * same op-reduction of all N input values.
 *
 * On Wave64 (TERAKAN_SUBGROUP_LOG2_SIZE = 6) the cost is 6 LDS
 * store-barrier-load triples regardless of input value -- bounded
 * GROUP_BARRIER pressure that the SFN VLIW5 scheduler on Palm /
 * Wrestler can pack without slot-pinning crisis.
 *
 * Reuses the per-shader broadcast region as the exchange surface:
 * region + lane_id * 4 is each lane's outgoing slot, and partner
 * reads via region + (lane_id ^ step) * 4.  Allocation is shared
 * with emit_broadcast_via_lds; the region is sized for one chunk
 * per lane and chunked-iterated for wider / multi-component
 * values.
 *
 * Per AMD Evergreen-Family Instruction Set Architecture section
 * 2.6.2 (Local Data Share), the 32 banks support contention-free
 * lane-disjoint stores; the workgroup barrier between levels
 * drains each level's writes before the next level reads.
 *
 * Operator dispatch is parameterised on a nir_op:
 *
 *   nir_op_iadd / nir_op_imin / nir_op_imax / nir_op_umin /
 *   nir_op_umax / nir_op_iand / nir_op_ior / nir_op_ixor /
 *   nir_op_fadd / nir_op_fmin / nir_op_fmax / nir_op_fmul
 *
 * 32-bit values reduce in one chunk; 64-bit values split into hi
 * and lo halves via nir_pack_64_2x32_split, reduce each half
 * independently (associative+commutative operators), and the
 * caller re-packs.  Sub-32-bit operators widen with nir_u2u32 / etc
 * around the reduction and narrow back after.
 */
static nir_def *
apply_reduce_op(nir_builder *b, nir_op op, nir_def *a, nir_def *b_val)
{
   switch (op) {
   case nir_op_iadd: return nir_iadd(b, a, b_val);
   case nir_op_imin: return nir_imin(b, a, b_val);
   case nir_op_imax: return nir_imax(b, a, b_val);
   case nir_op_umin: return nir_umin(b, a, b_val);
   case nir_op_umax: return nir_umax(b, a, b_val);
   case nir_op_iand: return nir_iand(b, a, b_val);
   case nir_op_ior:  return nir_ior(b, a, b_val);
   case nir_op_ixor: return nir_ixor(b, a, b_val);
   case nir_op_fadd: return nir_fadd(b, a, b_val);
   case nir_op_fmin: return nir_fmin(b, a, b_val);
   case nir_op_fmax: return nir_fmax(b, a, b_val);
   case nir_op_fmul: return nir_fmul(b, a, b_val);
   case nir_op_imul: return nir_imul(b, a, b_val);
   default:
      /* Caller has filtered unsupported ops. */
      assert(!"unhandled subgroup reduce op");
      return a;
   }
}

static nir_def *
emit_reduce_chunk_via_butterfly(nir_builder *b, nir_def *chunk_value,
                                nir_op op, struct subgroup_lds_alloc *alloc)
{
   if (alloc->broadcast_region_offset == TERAKAN_NO_BROADCAST_REGION) {
      alloc->broadcast_region_offset =
         alloc_lds_slot(alloc, TERAKAN_BROADCAST_REGION_BYTES, 4u);
   }
   nir_def *region_base = nir_imm_int(b, alloc->broadcast_region_offset);
   nir_def *lane_id = build_lane_id(b);
   nir_def *lane_addr =
      nir_iadd(b, region_base, nir_imul_imm(b, lane_id, 4));

   nir_def *current = chunk_value;
   for (unsigned level = 0; level < TERAKAN_SUBGROUP_LOG2_SIZE; ++level) {
      uint32_t const step = 1u << level;
      nir_def *partner_lane = nir_ixor(b, lane_id, nir_imm_int(b, step));
      nir_def *partner_addr =
         nir_iadd(b, region_base, nir_imul_imm(b, partner_lane, 4));

      nir_store_shared(b, current, lane_addr, .base = 0,
                       .align_mul = 4, .write_mask = 0x1);

      nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
                  .memory_scope = SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = nir_var_mem_shared);

      nir_def *partner_value = nir_load_shared(b, 1, 32, partner_addr,
                                               .base = 0, .align_mul = 4);
      current = apply_reduce_op(b, op, current, partner_value);

      /* Barrier between levels so the next level's stores don't
       * race the prior level's loads.  6 barriers total for Wave64. */
      nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
                  .memory_scope = SCOPE_WORKGROUP,
                  .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = nir_var_mem_shared);
   }
   return current;
}

static nir_def *
emit_reduce_via_butterfly(nir_builder *b, nir_def *value, nir_op op,
                          struct subgroup_lds_alloc *alloc)
{
   unsigned const bit_size = value->bit_size;
   unsigned const num_components = value->num_components;

   nir_def *out_components[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < num_components; ++c) {
      nir_def *comp = nir_channel(b, value, c);

      if (bit_size < 32) {
         nir_def *widened = nir_u2u32(b, comp);
         nir_def *reduced =
            emit_reduce_chunk_via_butterfly(b, widened, op, alloc);
         out_components[c] = (bit_size == 16)
            ? nir_u2u16(b, reduced) : nir_u2u8(b, reduced);
      } else if (bit_size == 32) {
         out_components[c] =
            emit_reduce_chunk_via_butterfly(b, comp, op, alloc);
      } else {
         /* 64-bit: split, reduce each half, repack.  This is correct
          * for bitwise ops (and / or / xor) and for integer add when
          * carries are absorbed by the underlying type, but not in
          * general (sub-word carry would cross the boundary).  For
          * now restrict 64-bit ARITHMETIC advertisement accordingly
          * via the operation-bit filter in the subgroup_supported_*
          * properties. */
         nir_def *lo = nir_unpack_64_2x32_split_x(b, comp);
         nir_def *hi = nir_unpack_64_2x32_split_y(b, comp);
         nir_def *lo_r = emit_reduce_chunk_via_butterfly(b, lo, op, alloc);
         nir_def *hi_r = emit_reduce_chunk_via_butterfly(b, hi, op, alloc);
         out_components[c] = nir_pack_64_2x32_split(b, lo_r, hi_r);
      }
   }
   return nir_vec(b, out_components, num_components);
}

/* Lower nir_intrinsic_load_subgroup_invocation, load_subgroup_id and
 * load_num_subgroups for compute stages on Wave64.
 *
 * The r600 SFN backend has no native lowering for the per-lane
 * "which subgroup-local lane am I?" system value because the original
 * r600 Gallium driver only ever advertised subgroup_size = 1 and the
 * intrinsic folded to a constant before reaching SFN.  Once Terakan
 * advertises Wave64 subgroups for CTS, the SFN dispatch path sees
 * load_subgroup_invocation as an "Unsupported instruction".
 *
 * On Wave64 with subgroup_size = 64 the per-lane mapping is purely a
 * compute-system-value rearrangement of gl_LocalInvocationIndex:
 *
 *   subgroup_invocation = local_invocation_index & (subgroup_size - 1)
 *   subgroup_id         = local_invocation_index >> log2(subgroup_size)
 *   num_subgroups       = (workgroup_size + subgroup_size - 1) / subgroup_size
 *                        -- represented as a divide of the linearized
 *                           workgroup size since dynamic 3D workgroup
 *                           sizes are folded earlier.
 *
 * These constants are visible to NIR through shader_info, so we can fold
 * load_num_subgroups when the workgroup size is fully known at compile
 * time.  Otherwise we emit the runtime arithmetic.
 */
static nir_def *
lower_load_subgroup_invocation(nir_builder *b)
{
   nir_def *local_index = nir_load_local_invocation_index(b);
   return nir_iand_imm(b, local_index, TERAKAN_SUBGROUP_SIZE - 1u);
}

static nir_def *
lower_load_subgroup_id(nir_builder *b)
{
   nir_def *local_index = nir_load_local_invocation_index(b);
   return nir_ushr_imm(b, local_index, TERAKAN_SUBGROUP_LOG2_SIZE);
}

static nir_def *
lower_load_num_subgroups(nir_builder *b, nir_shader *shader)
{
   if (shader->info.workgroup_size_variable ||
       shader->info.workgroup_size[0] == 0 ||
       shader->info.workgroup_size[1] == 0 ||
       shader->info.workgroup_size[2] == 0) {
      /* Variable workgroup size: compute (linearized_size + 63) / 64 at
       * runtime.  load_workgroup_size yields uvec3; linearize. */
      nir_def *wg = nir_load_workgroup_size(b);
      nir_def *linear = nir_imul(b, nir_channel(b, wg, 0),
                                  nir_imul(b, nir_channel(b, wg, 1),
                                           nir_channel(b, wg, 2)));
      nir_def *plus = nir_iadd_imm(b, linear, TERAKAN_SUBGROUP_SIZE - 1u);
      return nir_ushr_imm(b, plus, TERAKAN_SUBGROUP_LOG2_SIZE);
   }
   uint32_t const linear = shader->info.workgroup_size[0] *
                            shader->info.workgroup_size[1] *
                            shader->info.workgroup_size[2];
   uint32_t const num = (linear + TERAKAN_SUBGROUP_SIZE - 1u) /
                         TERAKAN_SUBGROUP_SIZE;
   return nir_imm_int(b, (int)num);
}

static bool
lower_subgroup_lds_instr(nir_builder *b, nir_intrinsic_instr *intrin,
                        void *data)
{
   struct subgroup_lds_alloc *alloc = data;
   nir_def *result = NULL;

   b->cursor = nir_before_instr(&intrin->instr);

   switch (intrin->intrinsic) {
   case nir_intrinsic_ballot:
      result = lower_ballot(b, intrin, alloc);
      break;
   case nir_intrinsic_read_first_invocation:
      result = lower_read_first_invocation(b, intrin, alloc);
      break;
   case nir_intrinsic_read_invocation:
      result = lower_read_invocation(b, intrin, alloc);
      break;
   case nir_intrinsic_shuffle:
      result = lower_shuffle(b, intrin, alloc);
      break;
   case nir_intrinsic_reduce: {
      nir_op const op = nir_intrinsic_reduction_op(intrin);
      switch (op) {
      case nir_op_iadd: case nir_op_imin: case nir_op_imax:
      case nir_op_umin: case nir_op_umax:
      case nir_op_iand: case nir_op_ior:  case nir_op_ixor:
      case nir_op_fadd: case nir_op_fmin: case nir_op_fmax:
      case nir_op_fmul: case nir_op_imul:
         result = emit_reduce_via_butterfly(b, intrin->src[0].ssa, op, alloc);
         break;
      default:
         /* Unsupported reduce op -- let nir_lower_subgroups handle it
          * (or leave the intrinsic unlowered to surface as SFN
          * "Unsupported instruction" so the gap is visible). */
         return false;
      }
      break;
   }
   case nir_intrinsic_load_subgroup_invocation:
      result = lower_load_subgroup_invocation(b);
      break;
   case nir_intrinsic_load_subgroup_id:
      result = lower_load_subgroup_id(b);
      break;
   case nir_intrinsic_load_num_subgroups:
      result = lower_load_num_subgroups(b, b->shader);
      break;
   default:
      return false;
   }

   nir_def_rewrite_uses(&intrin->def, result);
   nir_instr_remove(&intrin->instr);
   return true;
}

bool
terakan_nir_lower_subgroup_lds(nir_shader *shader)
{
   /* LDS-emulated subgroup ops are only meaningful in compute shaders.
    * Graphics stages may use subgroupBroadcastFirst etc. but the
    * sync-via-workgroup-LDS model does not apply outside compute, and
    * the driver advertises subgroup ops compute-only anyway (see
    * terakan_physical_device.c subgroupSupportedStages =
    * VK_SHADER_STAGE_COMPUTE_BIT).  Affected silicon for the LDS path
    * is Evergreen / TeraScale-2 VLIW5 (Cedar / Cypress / Hemlock /
    * Palm / Wrestler, CHIP_PALM is the empirical reproducer) plus
    * Northern Islands / TeraScale-3 VLIW4 (Barts / Cayman / Aruba).
    */
   if (shader->info.stage != MESA_SHADER_COMPUTE &&
       shader->info.stage != MESA_SHADER_KERNEL)
      return false;

   struct subgroup_lds_alloc alloc = {
      .shared_size_bytes = shader->info.shared_size,
      .broadcast_region_offset = TERAKAN_NO_BROADCAST_REGION,
   };

   bool const progress = nir_shader_intrinsics_pass(
      shader, lower_subgroup_lds_instr,
      nir_metadata_loop_analysis,
      &alloc);

   if (progress) {
      shader->info.shared_size = alloc.shared_size_bytes;
   }
   return progress;
}
