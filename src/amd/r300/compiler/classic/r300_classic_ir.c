/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_classic_ir.h"

#include <string.h>

#include "util/ralloc.h"
#include "util/bitset.h"

#include "../radeon_program_constants.h"

struct r300_classic_op_info {
   const char *name;
   unsigned num_srcs;
   /* Produces an SSA value; the writemask names which channels of that
    * value are live and must be nonzero. */
   bool has_def;
   /* Carries a destination write mask without producing an SSA value:
    * R300C_OP_EXPORT_COLOR's writemask is the store's destination channels
    * (see r300_classic_ir.h), so two masked exports to the same color
    * attachment can each cover a disjoint subset instead of one clobbering
    * the other.  Mutually exclusive with has_def. */
   bool has_output_mask;
};

/* Arity and def-ness per opcode; the RC_OPCODE each row maps to at emission
 * shares its name (MOV -> RC_OPCODE_MOV, EXPORT_* -> the output-register
 * write nir_to_rc models the same way). */
static const struct r300_classic_op_info op_info[R300C_OP_COUNT] = {
   [R300C_OP_MOV]          = {"mov", 1, true, false},
   [R300C_OP_ADD]          = {"add", 2, true, false},
   [R300C_OP_MUL]          = {"mul", 2, true, false},
   [R300C_OP_MAD]          = {"mad", 3, true, false},
   [R300C_OP_DP2]          = {"dp2", 2, true, false},
   [R300C_OP_DP3]          = {"dp3", 2, true, false},
   [R300C_OP_DP4]          = {"dp4", 2, true, false},
   [R300C_OP_MIN]          = {"min", 2, true, false},
   [R300C_OP_MAX]          = {"max", 2, true, false},
   [R300C_OP_FRC]          = {"frc", 1, true, false},
   [R300C_OP_ROUND]        = {"round", 1, true, false},
   [R300C_OP_RCP]          = {"rcp", 1, true, false},
   [R300C_OP_RSQ]          = {"rsq", 1, true, false},
   [R300C_OP_EX2]          = {"ex2", 1, true, false},
   [R300C_OP_LG2]          = {"lg2", 1, true, false},
   [R300C_OP_SIN]          = {"sin", 1, true, false},
   [R300C_OP_COS]          = {"cos", 1, true, false},
   [R300C_OP_POW]          = {"pow", 2, true, false},
   [R300C_OP_CMP]          = {"cmp", 3, true, false},
   [R300C_OP_DDX]          = {"ddx", 1, true, false},
   [R300C_OP_DDY]          = {"ddy", 1, true, false},
   /* Variable arity (2-4); selection sets num_srcs to the vector width and
    * the validator checks the range instead of this table row. */
   [R300C_OP_VEC]          = {"vec", 0, true, false},
   [R300C_OP_TEX]          = {"tex", 1, true, false},
   [R300C_OP_TXB]          = {"txb", 1, true, false},
   [R300C_OP_TXP]          = {"txp", 1, true, false},
   [R300C_OP_KIL]          = {"kil", 1, false, false},
   [R300C_OP_KILP]         = {"kilp", 0, false, false},
   [R300C_OP_EXPORT_COLOR] = {"export_color", 1, false, true},
   [R300C_OP_EXPORT_DEPTH] = {"export_depth", 1, false, false},
};

const char *
r300_classic_op_name(enum r300_classic_op op)
{
   return op_info[op].name;
}

unsigned
r300_classic_op_num_srcs(enum r300_classic_op op)
{
   return op_info[op].num_srcs;
}

bool
r300_classic_op_has_def(enum r300_classic_op op)
{
   return op_info[op].has_def;
}

struct r300_classic_program *
r300_classic_program_create(void *mem_ctx,
                            const struct r300_classic_target *target)
{
   struct r300_classic_program *p =
      rzalloc(mem_ctx, struct r300_classic_program);
   if (!p)
      return NULL;
   p->target = target;
   list_inithead(&p->instrs);
   return p;
}

struct r300_classic_instr *
r300_classic_instr_append(struct r300_classic_program *p,
                          enum r300_classic_op op)
{
   struct r300_classic_instr *i = rzalloc(p, struct r300_classic_instr);
   if (!i)
      return NULL;
   i->op = op;
   i->ssa_id = p->next_ssa_id++;
   i->num_srcs = op_info[op].num_srcs;
   for (unsigned s = 0; s < i->num_srcs; s++)
      i->src[s].swizzle = RC_SWIZZLE_XYZW;
   list_addtail(&i->link, &p->instrs);
   return i;
}

static bool
fail(char *err, size_t err_size, const struct r300_classic_instr *i,
     const char *msg)
{
   /* r300_classic_program_validate reports an out-of-range opcode by calling
    * here with i->op >= R300C_OP_COUNT, so op_info[i->op] would read past the
    * table.  Name the opcode only when it indexes a real entry. */
   const char *op_name =
      i->op < R300C_OP_COUNT ? op_info[i->op].name : "?";
   if (err && err_size)
      snprintf(err, err_size, "t%u (%s): %s", i->ssa_id, op_name, msg);
   return false;
}

static bool
swizzle_is_alu_selects(unsigned swizzle)
{
   for (unsigned c = 0; c < 4; c++) {
      unsigned s = GET_SWZ(swizzle, c);
      if (s > RC_SWIZZLE_ONE)
         return false;
   }
   return true;
}

bool
r300_classic_program_validate(const struct r300_classic_program *p,
                              char *err, size_t err_size)
{
   BITSET_WORD *defined =
      rzalloc_array(NULL, BITSET_WORD, BITSET_WORDS(p->next_ssa_id + 1));
   if (!defined)
      return false;

   bool ok = true;
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      if (i->op >= R300C_OP_COUNT) {
         ok = fail(err, err_size, i, "unknown opcode");
         break;
      }
      const struct r300_classic_op_info *info = &op_info[i->op];

      if (i->op == R300C_OP_VEC) {
         if (i->num_srcs < 2 || i->num_srcs > 4) {
            ok = fail(err, err_size, i, "vec width outside 2-4");
            break;
         }
         if (i->writemask != BITFIELD_MASK(i->num_srcs)) {
            ok = fail(err, err_size, i,
                      "vec writemask does not cover its width");
            break;
         }
      } else if (i->num_srcs != info->num_srcs) {
         ok = fail(err, err_size, i, "source count does not match opcode");
         break;
      }
      if (info->has_def && !(i->writemask & 0xf)) {
         ok = fail(err, err_size, i, "value-producing op with empty writemask");
         break;
      }
      if (info->has_output_mask && !(i->writemask & 0xf)) {
         ok = fail(err, err_size, i, "export with empty destination mask");
         break;
      }
      if (!info->has_def && !info->has_output_mask && i->writemask) {
         ok = fail(err, err_size, i, "sink op with nonzero writemask");
         break;
      }
      const bool is_tex = i->op == R300C_OP_TEX || i->op == R300C_OP_TXB ||
                          i->op == R300C_OP_TXP;
      if (is_tex && i->tex_unit >= R300C_MAX_TEX_UNITS) {
         ok = fail(err, err_size, i, "texture unit out of range");
         break;
      }
      if (i->op == R300C_OP_EXPORT_COLOR && i->export_index >= 4) {
         ok = fail(err, err_size, i, "color export index outside 0-3");
         break;
      }
      /* The TX block samples 1D/2D/3D/CUBE/RECT; array targets (the enum
       * values below RC_TEXTURE_CUBE, including the zero-initialized
       * RC_TEXTURE_2D_ARRAY) never come from selection. */
      if (is_tex &&
          (i->tex_target < RC_TEXTURE_CUBE || i->tex_target > RC_TEXTURE_1D)) {
         ok = fail(err, err_size, i, "texture target outside 1D/2D/3D/CUBE/RECT");
         break;
      }

      for (unsigned s = 0; s < i->num_srcs && ok; s++) {
         const struct r300_classic_src *src = &i->src[s];
         if (!swizzle_is_alu_selects(src->swizzle)) {
            ok = fail(err, err_size, i, "swizzle uses a non-ALU select");
            break;
         }
         switch (src->file) {
         case R300C_FILE_SSA:
            if (!src->def) {
               ok = fail(err, err_size, i, "SSA source with no def");
               break;
            }
            if (src->def->ssa_id >= p->next_ssa_id ||
                !BITSET_TEST(defined, src->def->ssa_id)) {
               ok = fail(err, err_size, i, "use precedes its def");
               break;
            }
            break;
         case R300C_FILE_CONST:
            if (src->index >= p->target->max_const_regs) {
               ok = fail(err, err_size, i, "constant index exceeds the file");
               break;
            }
            break;
         case R300C_FILE_INPUT:
         case R300C_FILE_STATE:
            break;
         default:
            ok = fail(err, err_size, i, "unknown source file");
            break;
         }
      }
      if (!ok)
         break;

      if (info->has_def)
         BITSET_SET(defined, i->ssa_id);
   }

   ralloc_free(defined);
   return ok;
}

static void
print_swizzle(unsigned swizzle, FILE *f)
{
   static const char sel_name[] = {'x', 'y', 'z', 'w', '0', '1', '?', '?'};
   if (swizzle == RC_SWIZZLE_XYZW)
      return;
   fputc('.', f);
   for (unsigned c = 0; c < 4; c++)
      fputc(sel_name[GET_SWZ(swizzle, c) & 0x7], f);
}

static void
print_src(const struct r300_classic_src *src, FILE *f)
{
   if (src->negate)
      fputc('-', f);
   if (src->abs)
      fputc('|', f);
   switch (src->file) {
   case R300C_FILE_SSA:
      fprintf(f, "t%u", src->def ? src->def->ssa_id : ~0u);
      break;
   case R300C_FILE_INPUT:
      fprintf(f, "in%u", src->index);
      break;
   case R300C_FILE_CONST:
      fprintf(f, "c%u", src->index);
      break;
   case R300C_FILE_STATE:
      fprintf(f, "s%u", src->index);
      break;
   }
   if (src->abs)
      fputc('|', f);
   print_swizzle(src->swizzle, f);
}

void
r300_classic_program_print(const struct r300_classic_program *p, FILE *f)
{
   fprintf(f, "classic-r300 program, pfs_class=%d\n", p->target->pfs_class);
   list_for_each_entry (struct r300_classic_instr, i, &p->instrs, link) {
      const struct r300_classic_op_info *info = &op_info[i->op];
      fputs("  ", f);
      if (info->has_def) {
         fprintf(f, "t%u", i->ssa_id);
         if (i->writemask != 0xf) {
            fputc('.', f);
            for (unsigned c = 0; c < 4; c++)
               if (i->writemask & (1u << c))
                  fputc("xyzw"[c], f);
         }
         fputs(" = ", f);
      }
      fputs(info->name, f);
      if (i->op == R300C_OP_TEX)
         fprintf(f, " unit%u,", i->tex_unit);
      for (unsigned s = 0; s < i->num_srcs; s++) {
         fputc(s ? ',' : ' ', f);
         if (s)
            fputc(' ', f);
         print_src(&i->src[s], f);
      }
      fputc('\n', f);
   }
}
