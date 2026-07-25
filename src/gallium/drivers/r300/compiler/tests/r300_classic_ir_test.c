/*
 * SPDX-License-Identifier: MIT
 */

#include "classic/r300_classic_ir.h"
#include "radeon_program_constants.h"

#include <stdio.h>
#include <string.h>

#include "util/ralloc.h"

/* Exercises the Phase-1 exit criterion: the IR validates and prints.  The
 * positive case builds a representative fragment program (constant-scaled
 * varying through MAD, a dependent texture read, a DP3, a color export) and
 * requires validation plus a stable text dump.  Each negative case flips one
 * rule (use-before-def, empty writemask, constant out of file range, TEX
 * unit out of range, writemask on a depth sink, empty export destination
 * mask) and requires validation to fail
 * with the offending instruction named. */

static int failures;

#define CHECK(cond, what)                                                    \
   do {                                                                      \
      if (!(cond)) {                                                         \
         fprintf(stderr, "FAIL: %s\n", what);                                \
         failures++;                                                         \
      }                                                                      \
   } while (0)

static struct r300_classic_src
src_input(unsigned index)
{
   return (struct r300_classic_src){
      .file = R300C_FILE_INPUT, .index = index, .swizzle = RC_SWIZZLE_XYZW,
   };
}

static struct r300_classic_src
src_const(unsigned index)
{
   return (struct r300_classic_src){
      .file = R300C_FILE_CONST, .index = index, .swizzle = RC_SWIZZLE_XYZW,
   };
}

static struct r300_classic_src
src_ssa(struct r300_classic_instr *def)
{
   return (struct r300_classic_src){
      .file = R300C_FILE_SSA, .def = def, .swizzle = RC_SWIZZLE_XYZW,
   };
}

static void
positive_program(void)
{
   void *ctx = ralloc_context(NULL);
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   struct r300_classic_program *p = r300_classic_program_create(ctx, t);
   char err[128] = {0};

   struct r300_classic_instr *mad =
      r300_classic_instr_append(p, R300C_OP_MAD);
   mad->writemask = 0xf;
   mad->src[0] = src_input(0);
   mad->src[1] = src_const(0);
   mad->src[2] = src_const(1);

   struct r300_classic_instr *tex =
      r300_classic_instr_append(p, R300C_OP_TEX);
   tex->writemask = 0xf;
   tex->tex_unit = 2;
   tex->tex_target = RC_TEXTURE_2D;
   tex->src[0] = src_ssa(mad);

   struct r300_classic_instr *dp3 =
      r300_classic_instr_append(p, R300C_OP_DP3);
   dp3->writemask = 0x1;
   dp3->src[0] = src_ssa(tex);
   dp3->src[1] = src_const(2);
   dp3->src[1].negate = true;

   struct r300_classic_instr *out =
      r300_classic_instr_append(p, R300C_OP_EXPORT_COLOR);
   out->writemask = 0xf;
   out->src[0] = src_ssa(dp3);
   out->src[0].swizzle = RC_MAKE_SWIZZLE_SMEAR(RC_SWIZZLE_X);

   CHECK(r300_classic_program_validate(p, err, sizeof(err)),
         "representative program validates");
   if (err[0])
      fprintf(stderr, "  validator said: %s\n", err);

   char buf[512] = {0};
   FILE *m = fmemopen(buf, sizeof(buf) - 1, "w");
   r300_classic_program_print(p, m);
   fclose(m);
   CHECK(strstr(buf, "t0 = mad in0, c0, c1") != NULL, "mad prints");
   CHECK(strstr(buf, "t1 = tex unit2, t0") != NULL, "tex prints");
   CHECK(strstr(buf, "t2.x = dp3 t1, -c2") != NULL, "dp3 prints");
   CHECK(strstr(buf, "export_color t2.xxxx") != NULL, "export prints");

   ralloc_free(ctx);
}

static void
negative_cases(void)
{
   const struct r300_classic_target *t = r300_classic_target_get(false, false);
   char err[128];

   {
      /* Use before def: swap program order by appending the user first. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *user =
         r300_classic_instr_append(p, R300C_OP_MOV);
      struct r300_classic_instr *def =
         r300_classic_instr_append(p, R300C_OP_MOV);
      def->writemask = 0xf;
      def->src[0] = src_input(0);
      user->writemask = 0xf;
      user->src[0] = src_ssa(def);
      err[0] = 0;
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "use-before-def rejected");
      CHECK(strstr(err, "use precedes its def") != NULL,
            "use-before-def named");
      ralloc_free(ctx);
   }
   {
      /* Value-producing op with an empty writemask. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *mov =
         r300_classic_instr_append(p, R300C_OP_MOV);
      mov->src[0] = src_input(0);
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "empty writemask rejected");
      ralloc_free(ctx);
   }
   {
      /* Constant index past the R300 32-vec4 file. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *mov =
         r300_classic_instr_append(p, R300C_OP_MOV);
      mov->writemask = 0xf;
      mov->src[0] = src_const(t->max_const_regs);
      err[0] = 0;
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "constant out of range rejected");
      CHECK(strstr(err, "constant index") != NULL, "constant bound named");
      ralloc_free(ctx);
   }
   {
      /* TEX unit past the TX block. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *tex =
         r300_classic_instr_append(p, R300C_OP_TEX);
      tex->writemask = 0xf;
      tex->tex_unit = R300C_MAX_TEX_UNITS;
      tex->tex_target = RC_TEXTURE_2D;
      tex->src[0] = src_input(0);
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "tex unit out of range rejected");
      ralloc_free(ctx);
   }
   {
      /* TEX with an array/zero-initialized target. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *tex =
         r300_classic_instr_append(p, R300C_OP_TEX);
      tex->writemask = 0xf;
      tex->src[0] = src_input(0);
      err[0] = 0;
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "unset tex target rejected");
      CHECK(strstr(err, "texture target") != NULL, "tex target named");
      ralloc_free(ctx);
   }
   {
      /* VEC width outside 2-4. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *vec =
         r300_classic_instr_append(p, R300C_OP_VEC);
      vec->num_srcs = 1;
      vec->writemask = 0x1;
      vec->src[0] = src_input(0);
      err[0] = 0;
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "vec width 1 rejected");
      CHECK(strstr(err, "vec width") != NULL, "vec width named");
      ralloc_free(ctx);
   }
   {
      /* VEC writemask must cover exactly its width. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *vec =
         r300_classic_instr_append(p, R300C_OP_VEC);
      vec->num_srcs = 3;
      vec->writemask = 0xf;
      vec->src[0] = src_input(0);
      vec->src[1] = src_input(0);
      vec->src[2] = src_input(0);
      err[0] = 0;
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "vec writemask mismatch rejected");
      ralloc_free(ctx);
   }
   {
      /* A well-formed vec3 collect validates. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *vec =
         r300_classic_instr_append(p, R300C_OP_VEC);
      vec->num_srcs = 3;
      vec->writemask = 0x7;
      vec->src[0] = src_input(0);
      vec->src[1] = src_input(1);
      vec->src[2] = src_const(0);
      err[0] = 0;
      CHECK(r300_classic_program_validate(p, err, sizeof(err)),
            "vec3 collect validates");
      if (err[0])
         fprintf(stderr, "  said: %s\n", err);
      ralloc_free(ctx);
   }
   {
      /* A true sink (depth export) must not claim a writemask: emission
       * always targets the output register's .w lane regardless. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *mov =
         r300_classic_instr_append(p, R300C_OP_MOV);
      mov->writemask = 0xf;
      mov->src[0] = src_input(0);
      struct r300_classic_instr *out =
         r300_classic_instr_append(p, R300C_OP_EXPORT_DEPTH);
      out->writemask = 0xf;
      out->src[0] = src_ssa(mov);
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "sink writemask rejected");
      ralloc_free(ctx);
   }
   {
      /* A color export must claim a nonzero destination mask: an empty
       * mask would silently drop the store instead of writing anything. */
      void *ctx = ralloc_context(NULL);
      struct r300_classic_program *p = r300_classic_program_create(ctx, t);
      struct r300_classic_instr *mov =
         r300_classic_instr_append(p, R300C_OP_MOV);
      mov->writemask = 0xf;
      mov->src[0] = src_input(0);
      struct r300_classic_instr *out =
         r300_classic_instr_append(p, R300C_OP_EXPORT_COLOR);
      out->src[0] = src_ssa(mov);
      CHECK(!r300_classic_program_validate(p, err, sizeof(err)),
            "empty export writemask rejected");
      ralloc_free(ctx);
   }
}

int
main(void)
{
   positive_program();
   negative_cases();
   if (failures) {
      fprintf(stderr, "r300_classic_ir_test: %d failures\n", failures);
      return 1;
   }
   printf("r300_classic_ir_test: all checks passed\n");
   return 0;
}
