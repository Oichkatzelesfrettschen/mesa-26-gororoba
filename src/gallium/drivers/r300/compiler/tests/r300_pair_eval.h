/*
 * SPDX-License-Identifier: MIT
 *
 * Value-level CPU evaluator over the scheduled RGB/Alpha pair form, with a
 * per-source-class read-model profile and a deterministic schedule
 * serializer.  Test-subtree utility: the production driver never links it.
 */

#ifndef R300_PAIR_EVAL_H
#define R300_PAIR_EVAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "radeon_compiler.h"
#include "radeon_program_pair.h"
#include "r300_us_source_read.h"

/* One read model per source-operand class.  The sign-flip discriminator
 * measured the input and temporary classes; the constant, texture, and
 * presubtract classes stay R300_SOURCE_READ_UNMODELED until a silicon cell
 * measures them, and an unmodeled class delivering a negative value makes
 * the evaluator return an indeterminate verdict instead of silently
 * evaluating identity. */
struct r300_pair_eval_profile {
   enum r300_source_read_model input;
   enum r300_source_read_model temporary;
   enum r300_source_read_model constant;
   enum r300_source_read_model texture;
   enum r300_source_read_model presubtract;
};

struct r300_pair_eval {
   float temps[64][4];
   float inputs[8][4];
   float outputs[4][4];
   float depth;
   const struct rc_constant_list *consts;
   const char *error;
   struct r300_pair_eval_profile profile;
};

/* All classes read as ideal FP32 (the all-zero profile, so a memset
 * evaluator carries it by default). */
struct r300_pair_eval_profile r300_pair_eval_profile_identity(void);

/* The measured RS482 US profile: input and temporary reads apply the
 * negative-predecessor transform; constant, texture, and presubtract stay
 * unmodeled and fail closed on negative values. */
struct r300_pair_eval_profile r300_pair_eval_profile_rs48x_measured(void);

/* Decode one channel of a resolved register through a pair swizzle,
 * including the ZERO/ONE/HALF swizzle constants. */
float r300_pair_decode_channel(const float *reg, unsigned swz);

/* Evaluate a fully paired program.  Returns false with e->error set on an
 * unhandled opcode, an out-of-range constant, or an unmodeled negative
 * source read (indeterminate verdict). */
bool r300_pair_eval_program(struct r300_pair_eval *e,
                            struct radeon_compiler *c);

/* Deterministic ASCII JSON serialization of a scheduled pair program:
 * stable instruction order, stable RGB-then-Alpha lane order, stable
 * source-slot order, explicit unused fields, no pointer values, schema
 * version 2.  The profile stamps each register-file class's read model into
 * the source records. */
void r300_pair_schedule_serialize(FILE *out, struct radeon_compiler *c,
                                  const struct r300_pair_eval_profile *p);

/* FNV-1a 64-bit digests.  The semantic hash covers the serialized schedule
 * bytes; the constant hash covers each entry's type, use mask, and active
 * union payload in list order; the program hash covers caller-supplied
 * hardware code words (the compiled r300_fragment_program_code stream). */
uint64_t r300_pair_schedule_semantic_hash(struct radeon_compiler *c,
                                          const struct r300_pair_eval_profile *p);
uint64_t r300_pair_constant_list_hash(const struct rc_constant_list *consts);
uint64_t r300_pair_program_words_hash(const uint32_t *words, size_t count);

#endif /* R300_PAIR_EVAL_H */
