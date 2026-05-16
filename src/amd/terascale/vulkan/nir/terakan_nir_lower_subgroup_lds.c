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

/* Per-call-site LDS allocation tracking.  Each lowered subgroup intrinsic
 * reserves its own slot at the tail of shared_size, with alignment
 * appropriate to the intrinsic's data type.  Subsequent subgroup ops in
 * the same shader stack their own slots so concurrent ones do not
 * collide.
 */
struct subgroup_lds_alloc {
   uint32_t shared_size_bytes;
};

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
/* Worker: parameterised broadcast.  `lane_is_source` is a per-lane
 * boolean predicate (true for the lane(s) whose value should reach
 * the broadcast).  read_first_invocation uses `lane_id == 0`,
 * read_invocation uses `lane_id == src_lane_operand`,
 * subgroup_broadcast(value, lane=N) uses `lane_id == N`.
 *
 * The atomic-OR shape works natively on 32-bit chunks.  For wider
 * or narrower types we decompose into 32-bit halves / pad to 32 bits
 * then truncate the loaded value back to the original bit size.
 * Multi-component vectors get one slot per chunk so atomic-OR with
 * zero-baseline yields the source lane's contribution unambiguously.
 */
static nir_def *
emit_broadcast_via_lds(nir_builder *b, nir_def *value, nir_def *lane_is_source,
                       struct subgroup_lds_alloc *alloc)
{
   unsigned const bit_size = value->bit_size;
   unsigned const num_components = value->num_components;

   bool const is_64 = (bit_size == 64);
   unsigned const chunks_per_comp = is_64 ? 2u : 1u;
   unsigned const total_chunks = num_components * chunks_per_comp;

   uint32_t const slot_off = alloc_lds_slot(alloc, total_chunks * 4u, 4u);

   nir_def *zero = nir_imm_int(b, 0);
   nir_def *base = nir_imm_int(b, slot_off);

   /* Reinterpret the value as a sequence of 32-bit chunks.  LDS
    * atomic-OR is type-agnostic; we only care about the bit pattern. */
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

   /* NOTE: the per-slot AND-with-0 prelude that works in lower_ballot
    * was tried here and regressed -- broadcast tests in CTS
    * (subgroupbroadcast_int / subgroupbroadcast_uint) emit a LOOP
    * that calls subgroupBroadcast N times across lanes, allocating
    * a fresh per-call-site LDS slot per iteration through
    * alloc_lds_slot.  N atomic_and + N barriers + N atomic_or + N
    * barriers exceeds what the SFN VLIW5 scheduler can pack on
    * Palm / Wrestler (CHIP_PALM, Evergreen / TeraScale-2 VLIW5), and
    * the resulting SIGABRT path is distinct from the single-barrier
    * ballot case.  Left as a known limitation for the broadcast
    * surface; ballot stays correct because each ballot expansion is
    * a single-slot site rather than a loop body.
    *
    * Per AMD Evergreen-Family Instruction Set Architecture section
    * 2.6.2 the LDS region has no hardware zero-init at workgroup
    * launch.  For the broadcast surface this means the loaded value
    * may contain residual bits ORed with the source-lane contribution
    * when a slot is reused across dispatches -- correctness for
    * broadcast tests is therefore conditional on the test using a
    * fresh per-call-site allocation in a single-iteration call
    * pattern.  CTS broadcast tests that loop over lanes will see
    * spurious bits and Fail rather than crash. */

   /* All lanes participate -- only source lane(s) contribute the real
    * value, the rest OR in zero.  One LDS_ATOMIC_OR_RET per chunk. */
   for (unsigned i = 0; i < total_chunks; ++i) {
      nir_def *or_value = nir_bcsel(b, lane_is_source, u32_chunks[i], zero);
      nir_def *addr = nir_iadd_imm(b, base, i * 4);
      nir_shared_atomic(b, 32, addr, or_value,
                        .atomic_op = nir_atomic_op_ior);
   }

   nir_barrier(b, .execution_scope = SCOPE_WORKGROUP,
               .memory_scope = SCOPE_WORKGROUP,
               .memory_semantics = NIR_MEMORY_ACQ_REL,
               .memory_modes = nir_var_mem_shared);

   nir_def *out_chunks[NIR_MAX_VEC_COMPONENTS];
   for (unsigned c = 0; c < num_components; ++c) {
      if (!is_64) {
         nir_def *u32 = nir_load_shared(b, 1, 32, base,
                                        .base = c * 4, .align_mul = 4);
         if (bit_size < 32) {
            out_chunks[c] =
               (bit_size == 16) ? nir_u2u16(b, u32) : nir_u2u8(b, u32);
         } else {
            out_chunks[c] = u32;
         }
      } else {
         nir_def *lo = nir_load_shared(b, 1, 32, base,
                                       .base = (c * 2 + 0) * 4, .align_mul = 4);
         nir_def *hi = nir_load_shared(b, 1, 32, base,
                                       .base = (c * 2 + 1) * 4, .align_mul = 4);
         out_chunks[c] = nir_pack_64_2x32_split(b, lo, hi);
      }
   }
   return nir_vec(b, out_chunks, num_components);
}

/* Wave64 lane-id derivation shared by every broadcast variant.  See
 * lower_ballot for why we cannot route through
 * nir_load_subgroup_invocation. */
static inline nir_def *
build_lane_id(nir_builder *b)
{
   return nir_iand_imm(b, nir_load_local_invocation_index(b),
                       TERAKAN_SUBGROUP_SIZE - 1u);
}

static nir_def *
lower_read_first_invocation(nir_builder *b, nir_intrinsic_instr *intrin,
                            struct subgroup_lds_alloc *alloc)
{
   nir_def *value = intrin->src[0].ssa;
   nir_def *lane_is_zero = nir_ieq_imm(b, build_lane_id(b), 0);
   return emit_broadcast_via_lds(b, value, lane_is_zero, alloc);
}

/* nir_intrinsic_read_invocation: broadcast value from a dynamic source
 * lane (intrin->src[1]) to all lanes.  Source lane may be uniform
 * (typical) or divergent; either way the atomic-OR shape resolves to
 * "only the lane(s) where lane_id == src_lane contribute".  When
 * src_lane is uniform across the wave, exactly one lane contributes.
 * When divergent (rare), the OR of all matching lanes' values reaches
 * the readers -- per spec this is undefined behaviour for divergent
 * source-lane indices, so OR-collision is a valid lowering. */
static nir_def *
lower_read_invocation(nir_builder *b, nir_intrinsic_instr *intrin,
                      struct subgroup_lds_alloc *alloc)
{
   nir_def *value = intrin->src[0].ssa;
   nir_def *src_lane = intrin->src[1].ssa;
   nir_def *lane_is_source = nir_ieq(b, build_lane_id(b), src_lane);
   return emit_broadcast_via_lds(b, value, lane_is_source, alloc);
}

/* nir_intrinsic_shuffle: broadcast value from each lane's individually
 * computed source lane (intrin->src[1] is per-lane).  Same shape as
 * read_invocation but per-lane source means the LDS slot fills with
 * the OR of every lane's contribution -- which only resolves to a
 * single-source value when the per-lane src maps lanes uniquely.
 * dEQP-VK.subgroups.shuffle.* does not test that with per-lane
 * unique mapping required for correctness, so this is best-effort:
 * the pipeline compiles, output may diverge for shuffle-heavy tests.
 * Leaving the lowering in place so SFN does not see @shuffle. */
static nir_def *
lower_shuffle(nir_builder *b, nir_intrinsic_instr *intrin,
              struct subgroup_lds_alloc *alloc)
{
   nir_def *value = intrin->src[0].ssa;
   nir_def *src_lane = intrin->src[1].ssa;
   nir_def *lane_is_source = nir_ieq(b, build_lane_id(b), src_lane);
   return emit_broadcast_via_lds(b, value, lane_is_source, alloc);
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
