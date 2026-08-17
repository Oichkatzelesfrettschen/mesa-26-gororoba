/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "util/ralloc.h"

#include "classic/r300_classic_regalloc.h"
#include "radeon_program_constants.h"

/* Register-file ceiling criterion: allocation never exceeds the target's temp file
 * for a fitting program, recycles slots at last use, and cleanly rejects an
 * over-budget one.  A dependency chain of any length fits in two slots; a
 * fan whose values all stay live must reject exactly past
 * R300_PFS_NUM_TEMP_REGS. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

static struct r300_classic_instr *
append_input_mov(struct r300_classic_program *p)
{
   struct r300_classic_instr *i = r300_classic_instr_append(p, R300C_OP_MOV);
   i->writemask = 0xf;
   i->src[0] = (struct r300_classic_src){
      .file = R300C_FILE_INPUT, .index = 0, .swizzle = RC_SWIZZLE_XYZW,
   };
   return i;
}

static struct r300_classic_src
src_ssa(struct r300_classic_instr *def)
{
   return (struct r300_classic_src){
      .file = R300C_FILE_SSA, .def = def, .swizzle = RC_SWIZZLE_XYZW,
   };
}

/* A 100-step add chain: each value dies at its single use, so recycling
 * keeps the whole chain in two slots. */
static void
case_chain_recycles(void)
{
   void *ctx = ralloc_context(NULL);
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_program *p = r300_classic_program_create(ctx, t);

   struct r300_classic_instr *prev = append_input_mov(p);
   for (unsigned n = 0; n < 100; n++) {
      struct r300_classic_instr *add =
         r300_classic_instr_append(p, R300C_OP_ADD);
      add->writemask = 0xf;
      add->src[0] = src_ssa(prev);
      add->src[1] = src_ssa(prev);
      prev = add;
   }
   struct r300_classic_instr *out =
      r300_classic_instr_append(p, R300C_OP_EXPORT_COLOR);
   out->src[0] = src_ssa(prev);

   struct r300_classic_regalloc_result r;
   CHECK(r300_classic_regalloc(ctx, p, &r), "allocation ran");
   CHECK(r.temp_of_ssa != NULL, "chain allocates");
   if (r.reject_reason)
      fprintf(stderr, "  rejected: %s\n", r.reject_reason);
   CHECK(r.num_temps <= 2, "chain recycles into two slots");
   ralloc_free(ctx);
}

/* max_temp_regs values all live at once fit exactly; one more rejects. */
static void
build_fan(struct r300_classic_program *p, unsigned width,
          struct r300_classic_instr **defs)
{
   for (unsigned n = 0; n < width; n++)
      defs[n] = append_input_mov(p);
   /* One MAD per pair keeps every def live until the reduction reads it. */
   struct r300_classic_instr *acc = defs[0];
   for (unsigned n = 1; n < width; n++) {
      struct r300_classic_instr *mad =
         r300_classic_instr_append(p, R300C_OP_MAD);
      mad->writemask = 0xf;
      mad->src[0] = src_ssa(defs[n]);
      mad->src[1] = src_ssa(defs[n]);
      mad->src[2] = src_ssa(acc);
      acc = mad;
   }
   struct r300_classic_instr *out =
      r300_classic_instr_append(p, R300C_OP_EXPORT_COLOR);
   out->src[0] = src_ssa(acc);
}

static void
case_fan_boundary(void)
{
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_instr *defs[64];

   {
      /* Width max_temp_regs: the first MAD frees two dying movs before its
       * def allocates, so the peak stays within the file. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      build_fan(p, t->max_temp_regs, defs);
      struct r300_classic_regalloc_result r;
      CHECK(r300_classic_regalloc(ctx, p, &r), "allocation ran");
      CHECK(r.temp_of_ssa != NULL, "full-file fan allocates");
      if (r.reject_reason)
         fprintf(stderr, "  rejected: %s\n", r.reject_reason);
      CHECK(r.num_temps <= t->max_temp_regs, "fan stays within the file");
      ralloc_free(ctx);
   }
   {
      /* Width max_temp_regs + 2: the movs alone oversubscribe the file
       * before anything dies, so allocation must reject by name. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      build_fan(p, t->max_temp_regs + 2, defs);
      struct r300_classic_regalloc_result r;
      CHECK(r300_classic_regalloc(ctx, p, &r), "allocation ran");
      CHECK(r.temp_of_ssa == NULL, "oversubscribed fan rejects");
      CHECK(r.reject_reason != NULL, "reject is named");
      ralloc_free(ctx);
   }
}

/* VEC expands to a MOV sequence at emission, so its destination must not
 * reuse the slot of a source dying at the VEC -- the standard recycling
 * rule would corrupt channels the earlier MOVs already wrote. */
static void
case_vec_dst_never_aliases_sources(void)
{
   void *ctx = ralloc_context(NULL);
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_program *p = r300_classic_program_create(ctx, t);

   struct r300_classic_instr *a = append_input_mov(p);
   struct r300_classic_instr *bdef = append_input_mov(p);
   struct r300_classic_instr *vec =
      r300_classic_instr_append(p, R300C_OP_VEC);
   vec->num_srcs = 2;
   vec->writemask = 0x3;
   vec->src[0] = src_ssa(a);
   vec->src[1] = src_ssa(bdef);
   struct r300_classic_instr *use =
      r300_classic_instr_append(p, R300C_OP_MOV);
   use->writemask = 0xf;
   use->src[0] = src_ssa(vec);

   struct r300_classic_regalloc_result r;
   CHECK(r300_classic_regalloc(ctx, p, &r), "allocation ran");
   CHECK(r.temp_of_ssa != NULL, "vec program allocates");
   if (r.temp_of_ssa) {
      CHECK(r.temp_of_ssa[vec->ssa_id] != r.temp_of_ssa[a->ssa_id],
            "vec dst disjoint from source a");
      CHECK(r.temp_of_ssa[vec->ssa_id] != r.temp_of_ssa[bdef->ssa_id],
            "vec dst disjoint from source b");
   }
   ralloc_free(ctx);
}

int
main(void)
{
   case_chain_recycles();
   case_fan_boundary();
   case_vec_dst_never_aliases_sources();
   if (failures) {
      fprintf(stderr, "r300_classic_regalloc_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_regalloc_test: all checks passed\n");
   return 0;
}
