/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_NIR_SSA_CUT_H
#define R300_NIR_SSA_CUT_H

#include "nir.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Producer-neutral single-block SSA cut/frontier analysis: given a NIR
 * program whose entrypoint is a single basic block, rank candidate
 * instruction-index cuts by carry size and balance
 * (r300_mp_find_cuts), and for a given cut compute the def set that
 * crosses it, reduced to modifier-chain bases and classified as
 * float/int/bool1 (r300_mp_collect, r300_mp_carry_base,
 * r300_mp_producer_type, r300_mp_classify_carry).  The analysis knows
 * nothing about how a crossing value is transported to a consumer of
 * the cut: r300_fs.c rides it to split an over-budget fragment program
 * into two RGBA8-scratch-target passes, and the R2VB producer-budget
 * escape rides the same cut/frontier facts to split an over-budget
 * vertex producer. */

#define R300_MP_MAX_CARRY_COMPS 8
#define R300_MP_MAX_CANDIDATES 6

enum r300_mp_carry_type {
    R300_MP_CARRY_FLOAT,
    /* Integer and bool32 values carry through numeric-float transport:
     * small integers are exact in the fixed-point window and bool32's
     * 0/0xffffffff is exactly the signed pair 0/-1. */
    R300_MP_CARRY_INT,
    /* A 1-bit boolean def (a comparison result feeding bcsel conditions)
     * carries as b2f32 0.0/1.0 and reconstitutes through fneu-zero. */
    R300_MP_CARRY_BOOL1,
};

/* Logical value transport selected for the R2VB FP32 carry. Integer
 * transports are admitted only when range analysis proves that every carried
 * component survives the R300 FP24 conversion and FP32 storage round trip.
 * The producer and all post-cut consumers must agree on the logical type. */
enum r300_mp_r2vb_transport {
    R300_MP_R2VB_INVALID,
    R300_MP_R2VB_FLOAT,
    R300_MP_R2VB_SINT,
    R300_MP_R2VB_UINT,
    R300_MP_R2VB_BOOL1,
    R300_MP_R2VB_BOOL32,
};

struct r300_mp_partition {
    unsigned cut_index;
    unsigned num_bases;
    nir_def *bases[R300_MP_MAX_CARRY_COMPS];
    enum r300_mp_carry_type base_type[R300_MP_MAX_CARRY_COMPS];
    enum r300_mp_r2vb_transport r2vb_transport[R300_MP_MAX_CARRY_COMPS];
    unsigned total_comps;
};

nir_def *
r300_mp_carry_base(nir_def *def, unsigned depth);

nir_alu_type
r300_mp_producer_type(nir_def *def, unsigned depth);

bool
r300_mp_classify_carry(nir_def *def, enum r300_mp_carry_type *out);

bool
r300_mp_collect(nir_block *block, unsigned cut_index,
                struct r300_mp_partition *p);

unsigned
r300_mp_find_cuts(nir_shader *nir, struct r300_mp_partition *cands,
                  unsigned max_cands);

/* R2VB producer budget-escape (carry-BO split): the fragment-ALU vertex
 * producer transports the cut-crossing values through one FP32x4 carry BO
 * instead of the FS multipass RGBA8 hi/lo scratch. Float and boolean carries
 * retain their values. Signed and unsigned integer carries are admitted only
 * when every component is proven inside the exact R300 FP24 integer domain
 * and every post-cut consumer agrees with the producer's logical type. Pass A
 * uses i2f32/u2f32; pass B directly uses the ftrunc/ffloor form produced by
 * r300's integer lowering so its flat carry input bypasses interpolation-only
 * epsilon adjustment. The carry fits four scalar components.
 *
 * r300_mp_find_vec4_cut ranks the single-block cuts (r300_mp_find_cuts, which
 * orders smallest carry first) and returns the first whose crossing set fits
 * one vec4 -- total_comps <= 4.  r300_mp_build_carry_pass_a clones the
 * position-pass producer FS, keeps the computation up to the cut, and replaces
 * the position store with one vec4 that packs each carried base through its
 * typed conversion, with unused lanes set to 0.0, into FRAG_RESULT_DATA0.
 * r300_mp_build_pos_pass_b clones the same FS, adds a flat shader input at
 * VARYING_SLOT_VAR0 + num_in fed from the carry BO, reconstitutes each base
 * from its carry components through the matching exact numeric form and
 * rewrites the base's uses so the pre-cut half dead-codes away
 * while the position output survives.  Both halves are pure NIR; the caller
 * measures each against the emitted-slot admission oracle and adopts the split
 * only when both fit. */
/* Why a transport selection declined a partition.  MIXED_SIGNEDNESS covers a
 * carried integer whose producer and post-cut consumers disagree on one
 * signed/unsigned/boolean class; the range declines cover a component whose
 * proven bounds leave the R300 FP24 exact-integer window. */
enum r300_mp_transport_decline {
    R300_MP_TRANSPORT_OK = 0,
    R300_MP_TRANSPORT_MIXED_SIGNEDNESS,
    R300_MP_TRANSPORT_SIGNED_RANGE,
    R300_MP_TRANSPORT_UNSIGNED_RANGE,
};

/* Resolve the FP32-carry transport for every base of a candidate partition.
 * Returns true with every r2vb_transport slot filled, or false with *decline
 * (when non-NULL) naming the first failing class.  range_ht caches NIR range
 * analysis across candidates of the same shader. */
bool
r300_mp_select_r2vb_transport(nir_shader *shader,
                              struct r300_mp_partition *part,
                              struct hash_table *range_ht,
                              enum r300_mp_transport_decline *decline);

bool
r300_mp_find_vec4_cut(nir_shader *nir, struct r300_mp_partition *out);

nir_shader *
r300_mp_build_carry_pass_a(nir_shader *src, const struct r300_mp_partition *part);

nir_shader *
r300_mp_build_pos_pass_b(nir_shader *src, const struct r300_mp_partition *part,
                         unsigned num_in);

#ifdef __cplusplus
}
#endif

#endif /* R300_NIR_SSA_CUT_H */
