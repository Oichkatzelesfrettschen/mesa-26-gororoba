/*
 * SPDX-License-Identifier: MIT
 *
 * Every register a ZMASK stream writes, held against the kernel's own
 * PACKET0 authority.
 *
 * The authority is two tables the radeon CS parser composes: a register
 * listed in reg_srcs/r300 admits with no check, and a register absent
 * from it reaches r300_packet0_check, where a case label judges it and
 * the default arm reports "Forbidden register".  A register at or above
 * the bitmap's extent never reaches the check at all.  So a stream is
 * admissible only where every PACKET0 register it writes lies in the
 * union of the two lists, and that union is what
 * r300_kernel_packet0_authority.c carries as data.
 *
 * The walk here reads the kernel data rather than
 * r300_zb_hyperz_admission.c, so the two tables are independent
 * readings of one rule: every plan stream is judged by both, and the two
 * must agree.  A plan that regains the ZMASK RAM index ports fails both.
 *
 * Every check reaches main through a count or a bool that main compares
 * against the shape it expects, and every call that fills an output runs
 * in an if rather than inside an assert.  So the verdict holds under a
 * compile that discards assertions, and the asserts serve to name which
 * check failed.
 */
#undef NDEBUG

#include "r300_kernel_packet0_authority.h"

#include "r300_chipset.h"
#include "r300_pm4_builder.h"
#include "r300_reg.h"
#include "r300_zb_depth_surface.h"
#include "r300_zb_hyperz_admission.h"
#include "r300_zmask_clear_plan.h"
#include "r300_zmask_layout.h"
#include "r300_zmask_materialize_plan.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

/* Header fields as radeon_cs_packet_parse reads them. */
#define PKT_TYPE(h) (((h) >> 30) & 0x3u)
#define PKT_COUNT(h) ((((h) >> 16) & 0x3fffu) + 1u)
#define PKT0_REG(h) (((h) & 0x1fffu) << 2)
#define PKT3_OPCODE(h) ((h) & 0xff00u)

/* Why a stream fails the authority, so a known-bad names its mechanism
 * instead of merely failing. */
enum crosscheck_verdict {
   CROSSCHECK_ADMIT = 0,
   /* A PACKET0 register outside the union of the two kernel lists. */
   CROSSCHECK_FORBIDDEN_REGISTER,
   /* A PACKET0 register at or above the safe bitmap's extent. */
   CROSSCHECK_REGISTER_RANGE,
   /* A type-3 opcode outside the two a ZMASK plan emits. */
   CROSSCHECK_UNKNOWN_OPCODE,
   /* A header running past the end of the stream, or a type-1 packet. */
   CROSSCHECK_MALFORMED,
};

struct crosscheck_site {
   uint32_t ib_index;
   uint32_t reg_or_opcode;
};

/* The two opcodes a ZMASK plan emits: 3D_CLEAR_ZMASK, which
 * r300_packet3_check authorizes for an owner, and the type-3 NOP that
 * carries a BO reference.  The scope is a plan stream rather than a whole
 * command stream, so the draw-path opcodes an application also submits --
 * 3D_LOAD_VBPNTR, 3D_DRAW_VBUF_2 -- lie outside it by construction and
 * r300_packet3_check stays the authority for them. */
static bool
opcode_authorized(uint32_t opcode)
{
   return opcode == R300_PACKET3_3D_CLEAR_ZMASK ||
          opcode == R300_PM4_PACKET3_NOP;
}

static enum crosscheck_verdict
crosscheck_stream(const uint32_t *ib, uint32_t dwords,
                  struct crosscheck_site *site)
{
   struct crosscheck_site scratch;
   if (site == NULL)
      site = &scratch;
   site->ib_index = 0u;
   site->reg_or_opcode = 0u;

   uint32_t i = 0u;
   while (i < dwords) {
      const uint32_t header = ib[i];
      const uint32_t type = PKT_TYPE(header);
      if (type == 2u) {
         i += 1u;
         continue;
      }
      if (type == 1u) {
         site->ib_index = i;
         return CROSSCHECK_MALFORMED;
      }
      const uint32_t count = PKT_COUNT(header);
      if (count > dwords - 1u - i) {
         site->ib_index = i;
         return CROSSCHECK_MALFORMED;
      }
      if (type == 0u) {
         const uint32_t base = PKT0_REG(header);
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0u;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = one_reg ? base : base + 4u * k;
            site->ib_index = i + 1u + k;
            site->reg_or_opcode = reg;
            if (reg >= R300_KERNEL_PACKET0_REGISTER_LIMIT)
               return CROSSCHECK_REGISTER_RANGE;
            if (!r300_kernel_admits_packet0_register(reg))
               return CROSSCHECK_FORBIDDEN_REGISTER;
         }
      } else {
         const uint32_t opcode = PKT3_OPCODE(header);
         if (!opcode_authorized(opcode)) {
            site->ib_index = i;
            site->reg_or_opcode = opcode;
            return CROSSCHECK_UNKNOWN_OPCODE;
         }
      }
      i += 1u + count;
   }
   return CROSSCHECK_ADMIT;
}

/* The admission model reading the same stream, which must agree: a
 * stream the kernel data admits carries no forbidden and no
 * out-of-range register for the driver's own table either.  Ownership is
 * held, so the gated rows answer ADMIT and the two new verdicts stand
 * alone. */
static bool
cross_tie(const uint32_t *ib, uint32_t dwords)
{
   struct crosscheck_site site;
   const enum crosscheck_verdict kernel =
      crosscheck_stream(ib, dwords, &site);
   assert(kernel == CROSSCHECK_ADMIT);
   struct r300_zb_hyperz_site model_site;
   const enum r300_zb_hyperz_verdict model =
      r300_zb_hyperz_admit_stream(ib, dwords, R300_ZB_HYPERZ_OWNED,
                                  &model_site);
   assert(model == R300_ZB_HYPERZ_ADMIT);
   return kernel == CROSSCHECK_ADMIT && model == R300_ZB_HYPERZ_ADMIT;
}

/* The 64x64 Z24 reference level on one RS480 pipe, resolved at the block
 * the caller names. */
static bool
reference_layout(enum r300_zmask_compression block,
                 struct r300_zmask_layout *out)
{
   const struct r300_zmask_layout_params params = {
      .stride_in_pixels = 64,
      .height = 64,
      .depth_bytes_per_pixel = 4,
      .is_depth_or_stencil = true,
      .microtile = true,
      .macrotile = true,
      .num_samples = 1,
      .zcomp8x8_capable = true,
      .pipes = 1,
      .zmask_ram_dwords_per_pipe = RV3xx_ZMASK_SIZE,
   };
   if (r300_zmask_layout_compute_at_block(&params, block, out) != 0)
      return false;
   assert(out->fits_zmask_ram);
   return out->fits_zmask_ram;
}

/* Every stage of the ladder, at every block the stage admits, plus the
 * fast-clear stream the value plan prefixes. */
static uint32_t
check_clear_plans(void)
{
   uint32_t streams = 0u;
   struct r300_zmask_layout four, eight;
   if (!reference_layout(R300_ZCOMP_4X4, &four) ||
       !reference_layout(R300_ZCOMP_8X8, &eight))
      return 0u;

   for (int s = R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY;
        s <= R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED; s++) {
      const enum r300_zmask_clear_stage stage =
         (enum r300_zmask_clear_stage)s;
      for (int b = 0; b <= 1; b++) {
         const enum r300_zmask_compression block =
            b == 0 ? R300_ZCOMP_4X4 : R300_ZCOMP_8X8;
         if (!r300_zmask_clear_stage_admits_block(stage, block))
            continue;
         const struct r300_zmask_layout *layout =
            block == R300_ZCOMP_8X8 ? &eight : &four;
         struct r300_zmask_clear_plan plan;
         const int built =
            r300_zmask_clear_plan_build_at_block(stage, block, layout, &plan);
         assert(built == 0);
         if (built == 0 && cross_tie(plan.words, plan.dword_count))
            streams++;
      }
   }

   struct r300_zmask_clear_plan fast;
   const int fast_built = r300_zmask_fast_clear_plan_build(
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical, &four, 0x800000u,
      0u, &fast);
   assert(fast_built == 0);
   if (fast_built == 0 && cross_tie(fast.words, fast.dword_count))
      streams++;
   return streams;
}

/* The materializer's three streams: the prefix that binds the metadata,
 * the suffix that flushes and unbinds it, and the whole read plan. */
static uint32_t
check_materialize_plans(void)
{
   struct r300_zmask_layout four;
   if (!reference_layout(R300_ZCOMP_4X4, &four))
      return 0u;
   struct r300_zmask_materialize_plan prefix;
   const int prefix_built = r300_zmask_materialize_prefix(
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical, &four, 0x800000u,
      0u, &prefix);
   assert(prefix_built == 0);
   if (prefix_built != 0)
      return 0u;
   uint32_t streams = cross_tie(prefix.words, prefix.dword_count) ? 1u : 0u;

   struct r300_zmask_materialize_plan whole;
   const int whole_built = r300_zmask_fast_clear_read_plan(
      &r300_zb_depth_surface_rs485m_z24_macrotiled_logical, &four, 0x800000u,
      0u, &whole);
   assert(whole_built == 0);
   if (whole_built != 0)
      return streams;
   streams += cross_tie(whole.words, whole.dword_count) ? 1u : 0u;
   /* The suffix on its own, which the whole plan appends. */
   streams += cross_tie(r300_zmask_materialize_end_words(&whole),
                        r300_zmask_materialize_end_dword_count(&whole))
                 ? 1u
                 : 0u;
   return streams;
}

/* The stream the RS485M refused, reassembled: the stage C append with
 * the two index writes spliced back in at the position the bind run left
 * them, which is the fifteen-dword form the measured submission carried.
 * The checker rejects it at the first index register, and the admission
 * model rejects it at the same word, so the fix is what moved both
 * verdicts rather than the plan changing shape underneath them.
 */
static bool
check_prefix_plan_known_bad(void)
{
   struct r300_zmask_layout four;
   if (!reference_layout(R300_ZCOMP_4X4, &four))
      return false;
   struct r300_zmask_clear_plan plan;
   if (r300_zmask_clear_plan_build(R300_ZMASK_CLEAR_STAGE_BIND_CLEAR, &four,
                                   &plan) != 0)
      return false;
   assert(plan.dword_count == 11u);
   if (plan.dword_count != 11u)
      return false;

   uint32_t stream[16];
   uint32_t n = 0u;
   /* The bind run: header and its two payload dwords. */
   for (uint32_t i = 0; i < 3u; i++)
      stream[n++] = plan.words[i];
   stream[n++] = CP_PACKET0(R300_ZB_ZMASK_WRINDEX, 0);
   stream[n++] = 0u;
   stream[n++] = CP_PACKET0(R300_ZB_ZMASK_RDINDEX, 0);
   stream[n++] = 0u;
   for (uint32_t i = 3u; i < plan.dword_count; i++)
      stream[n++] = plan.words[i];
   assert(n == 15u);

   struct crosscheck_site site;
   const enum crosscheck_verdict kernel = crosscheck_stream(stream, n, &site);
   assert(kernel == CROSSCHECK_FORBIDDEN_REGISTER);
   assert(site.reg_or_opcode == R300_ZB_ZMASK_WRINDEX && site.ib_index == 4u);

   struct r300_zb_hyperz_site model_site;
   const enum r300_zb_hyperz_verdict model = r300_zb_hyperz_admit_stream(
      stream, n, R300_ZB_HYPERZ_OWNED, &model_site);
   assert(model == R300_ZB_HYPERZ_REFUSE_FORBIDDEN_REGISTER);
   assert(model_site.reg_or_opcode == R300_ZB_ZMASK_WRINDEX &&
          model_site.ib_index == 4u && model_site.value == 0u);

   /* The same stream without the splice is the plan that ships, and both
    * readings admit it. */
   return kernel == CROSSCHECK_FORBIDDEN_REGISTER &&
          site.reg_or_opcode == R300_ZB_ZMASK_WRINDEX &&
          site.ib_index == 4u &&
          model == R300_ZB_HYPERZ_REFUSE_FORBIDDEN_REGISTER &&
          model_site.value == 0u &&
          cross_tie(plan.words, plan.dword_count);
}

/* The two index registers on their own, at the value and the ownership
 * the refusal was measured with: each fails the kernel data on the
 * forbidden-register mechanism and the admission model on its own row.
 */
static uint32_t
check_known_bad(void)
{
   uint32_t confirmed = 0u;
   const uint32_t keys[2] = { R300_ZB_ZMASK_WRINDEX, R300_ZB_ZMASK_RDINDEX };
   for (uint32_t k = 0; k < 2u; k++) {
      /* CP_PACKET0 carries the payload run as count - 1, so each of
       * these headers names one payload dword. */
      const uint32_t stream[4] = {
         CP_PACKET0(R300_ZB_ZMASK_OFFSET, 0), 0u,
         CP_PACKET0(keys[k], 0), 0u,
      };
      struct crosscheck_site site;
      const enum crosscheck_verdict kernel =
         crosscheck_stream(stream, 4u, &site);
      assert(kernel == CROSSCHECK_FORBIDDEN_REGISTER);
      assert(site.reg_or_opcode == keys[k] && site.ib_index == 3u);
      assert(!r300_kernel_admits_packet0_register(keys[k]));

      struct r300_zb_hyperz_site model_site;
      const enum r300_zb_hyperz_verdict model = r300_zb_hyperz_admit_stream(
         stream, 4u, R300_ZB_HYPERZ_OWNED, &model_site);
      assert(model == R300_ZB_HYPERZ_REFUSE_FORBIDDEN_REGISTER);
      assert(model_site.reg_or_opcode == keys[k] && model_site.value == 0u);

      if (kernel == CROSSCHECK_FORBIDDEN_REGISTER &&
          site.reg_or_opcode == keys[k] && site.ib_index == 3u &&
          !r300_kernel_admits_packet0_register(keys[k]) &&
          !r300_kernel_register_safe_listed(keys[k]) &&
          !r300_kernel_register_checked(keys[k]) &&
          model == R300_ZB_HYPERZ_REFUSE_FORBIDDEN_REGISTER &&
          model_site.reg_or_opcode == keys[k] && model_site.value == 0u)
         confirmed++;
   }

   /* A register above the bitmap's extent, which the parser rejects
    * before the check runs. */
   const uint32_t out_of_range[2] = {
      CP_PACKET0(R300_KERNEL_PACKET0_REGISTER_LIMIT, 0), 0u,
   };
   struct crosscheck_site site;
   const enum crosscheck_verdict range =
      crosscheck_stream(out_of_range, 2u, &site);
   assert(range == CROSSCHECK_REGISTER_RANGE);
   struct r300_zb_hyperz_site model_site;
   const enum r300_zb_hyperz_verdict range_model =
      r300_zb_hyperz_admit_stream(out_of_range, 2u, R300_ZB_HYPERZ_OWNED,
                                  &model_site);
   assert(range_model == R300_ZB_HYPERZ_REFUSE_REGISTER_RANGE);
   if (range == CROSSCHECK_REGISTER_RANGE &&
       range_model == R300_ZB_HYPERZ_REFUSE_REGISTER_RANGE)
      confirmed++;

   /* An opcode no ZMASK stream may carry. */
   const uint32_t bad_opcode[2] = {
      CP_PACKET3(R300_PACKET3_3D_CLEAR_HIZ, 0), 0u,
   };
   const enum crosscheck_verdict opcode =
      crosscheck_stream(bad_opcode, 2u, &site);
   assert(opcode == CROSSCHECK_UNKNOWN_OPCODE);
   if (opcode == CROSSCHECK_UNKNOWN_OPCODE)
      confirmed++;

   /* The checker admits the stream these known-bads differ from, so the
    * verdicts above rest on the register and the opcode alone. */
   const uint32_t good[4] = {
      CP_PACKET0(R300_ZB_ZMASK_OFFSET, 0), 0u,
      CP_PACKET0(R300_ZB_DEPTHCLEARVALUE, 0), 0u,
   };
   const enum crosscheck_verdict admit = crosscheck_stream(good, 4u, &site);
   assert(admit == CROSSCHECK_ADMIT);
   if (admit == CROSSCHECK_ADMIT)
      confirmed++;
   return confirmed;
}

/* The two kernel lists, and the disjointness that states the bitmap's
 * polarity: a listed register carries a clear bit and skips the check,
 * so it can never also be a case label. */
static bool
check_authority_tables(void)
{
   const int self_check = r300_kernel_packet0_authority_self_check();
   assert(self_check == 0);
   if (self_check != 0)
      return false;
   uint32_t safe_count = 0u, checked_count = 0u;
   const struct r300_kernel_register *safe =
      r300_kernel_safe_registers(&safe_count);
   const struct r300_kernel_register *checked =
      r300_kernel_checked_registers(&checked_count);
   assert(safe_count == 685u && checked_count == 142u);

   /* Every row of both tables, counted so the walk reaches the verdict:
    * a safe-listed register admits and carries no case label, and a case
    * label is absent from the safe list and admits exactly when it lies
    * inside the bitmap extent -- the shared check carries r500 labels the
    * r300 bitmap cannot reach. */
   uint32_t safe_held = 0u, checked_held = 0u;
   for (uint32_t i = 0; i < safe_count; i++) {
      const bool held = r300_kernel_admits_packet0_register(safe[i].offset) &&
                        !r300_kernel_register_checked(safe[i].offset);
      assert(held);
      safe_held += held ? 1u : 0u;
   }
   for (uint32_t i = 0; i < checked_count; i++) {
      const bool held =
         !r300_kernel_register_safe_listed(checked[i].offset) &&
         r300_kernel_admits_packet0_register(checked[i].offset) ==
            (checked[i].offset < R300_KERNEL_PACKET0_REGISTER_LIMIT);
      assert(held);
      checked_held += held ? 1u : 0u;
   }

   /* The registers the ZMASK path depends on, by the mechanism that
    * admits each one:
    *
    *    ZB_DEPTHCLEARVALUE  0x4f28  safe listed
    *    ZB_ZCACHE_CTLSTAT   0x4f18  safe listed
    *    ZB_ZMASK_OFFSET     0x4f30  case label, ownership gated
    *    ZB_ZMASK_PITCH      0x4f34  case label, ownership gated
    *    ZB_BW_CNTL          0x4f1c  case label, ownership gated
    *    GB_Z_PEQ_CONFIG     0x4028  case label, ownership gated
    *    ZB_ZMASK_WRINDEX    0x4f38  neither
    *    ZB_ZMASK_RDINDEX    0x4f40  neither
    */
   assert(r300_kernel_register_safe_listed(R300_ZB_DEPTHCLEARVALUE));
   assert(r300_kernel_register_safe_listed(R300_ZB_ZCACHE_CTLSTAT));
   assert(r300_kernel_register_checked(R300_ZB_ZMASK_OFFSET));
   assert(r300_kernel_register_checked(R300_ZB_ZMASK_PITCH));
   assert(r300_kernel_register_checked(R300_ZB_BW_CNTL));
   assert(r300_kernel_register_checked(R300_GB_Z_PEQ_CONFIG));
   assert(!r300_kernel_admits_packet0_register(R300_ZB_ZMASK_WRINDEX));
   assert(!r300_kernel_admits_packet0_register(R300_ZB_ZMASK_RDINDEX));

   return safe_count == 685u && checked_count == 142u &&
          safe_held == safe_count && checked_held == checked_count &&
          r300_kernel_register_safe_listed(R300_ZB_DEPTHCLEARVALUE) &&
          r300_kernel_register_safe_listed(R300_ZB_ZCACHE_CTLSTAT) &&
          r300_kernel_register_checked(R300_ZB_ZMASK_OFFSET) &&
          r300_kernel_register_checked(R300_ZB_ZMASK_PITCH) &&
          r300_kernel_register_checked(R300_ZB_BW_CNTL) &&
          r300_kernel_register_checked(R300_GB_Z_PEQ_CONFIG) &&
          !r300_kernel_admits_packet0_register(R300_ZB_ZMASK_WRINDEX) &&
          !r300_kernel_admits_packet0_register(R300_ZB_ZMASK_RDINDEX);
}

/* The shape the run must produce.  Eight clear streams: six stages with
 * the compressed-write stage built at both blocks it admits, plus the
 * fast-clear value plan.  Three materialize streams: the prefix, the
 * whole read plan, and the suffix.  Five known-bads: the two index
 * registers, the out-of-range write, the unauthorized opcode, and the
 * control stream that differs from them only in the register it names.
 * A check a reading refuses goes uncounted, so a mismatch fails through
 * the return value.
 */
#define EXPECTED_CLEAR_STREAMS 8u
#define EXPECTED_MATERIALIZE_STREAMS 3u
#define EXPECTED_KNOWN_BAD_CONFIRMATIONS 5u

int
main(void)
{
   const bool authority = check_authority_tables();
   const int hyperz_rows = r300_zb_hyperz_rows_self_check();
   const int clear_stages = r300_zmask_clear_stages_self_check();
   assert(hyperz_rows == 0);
   assert(clear_stages == 0);
   const uint32_t clear_streams = check_clear_plans();
   const uint32_t materialize_streams = check_materialize_plans();
   const uint32_t known_bad = check_known_bad();
   const bool prefix_plan = check_prefix_plan_known_bad();

   if (!authority || hyperz_rows != 0 || clear_stages != 0 || !prefix_plan ||
       clear_streams != EXPECTED_CLEAR_STREAMS ||
       materialize_streams != EXPECTED_MATERIALIZE_STREAMS ||
       known_bad != EXPECTED_KNOWN_BAD_CONFIRMATIONS) {
      printf("r300 zmask kernel register cross-check: authority %d, rows %d, "
             "stages %d, prefix plan %d, clear %u of %u, materialize %u of "
             "%u, known-bad %u of %u\n",
             (int)authority, hyperz_rows, clear_stages, (int)prefix_plan,
             clear_streams, EXPECTED_CLEAR_STREAMS, materialize_streams,
             EXPECTED_MATERIALIZE_STREAMS, known_bad,
             EXPECTED_KNOWN_BAD_CONFIRMATIONS);
      return 1;
   }
   printf("r300 zmask kernel register cross-check: %u clear streams, %u "
          "materialize streams, %u known-bads against %u safe and %u "
          "checked registers\n",
          clear_streams, materialize_streams, known_bad, 685u, 142u);
   return 0;
}
