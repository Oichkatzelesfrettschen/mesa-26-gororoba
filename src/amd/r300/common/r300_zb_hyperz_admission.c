/* SPDX-License-Identifier: MIT */

#include "r300_zb_hyperz_admission.h"
#include "r300_reg.h"

#include <errno.h>
#include <string.h>

/* PACKET3 opcodes as r300_reg.h spells them: the opcode occupies bits
 * 15:8 of the header and the constants carry it in place. */
static const struct r300_zb_hyperz_row rows[] = {
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_ZB_BW_CNTL,
     R300_HIZ_ENABLE | R300_RD_COMP_ENABLE | R300_WR_COMP_ENABLE |
        R300_FAST_FILL_ENABLE,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "ZB_BW_CNTL HyperZ enables",
     "r300_packet0_check case 0x4f1c: HIZ, RD_COMP, WR_COMP, FAST_FILL "
     "reject for a non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_ZB_ZMASK_OFFSET, 0xffffffffu,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "ZB_ZMASK_OFFSET",
     "r300_packet0_check case 0x4f30: nonzero rejects for a non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_ZB_ZMASK_PITCH, 0xffffffffu,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "ZB_ZMASK_PITCH",
     "r300_packet0_check case 0x4f34: nonzero rejects for a non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_ZB_HIZ_OFFSET, 0xffffffffu,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "ZB_HIZ_OFFSET",
     "r300_packet0_check case 0x4f44: nonzero rejects for a non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_ZB_HIZ_PITCH, 0xffffffffu,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "ZB_HIZ_PITCH",
     "r300_packet0_check case 0x4f54: nonzero rejects for a non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_GB_Z_PEQ_CONFIG, 0xffffffffu,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "GB_Z_PEQ_CONFIG",
     "r300_packet0_check case 0x4028: nonzero rejects for a non-owner; "
     "any value rejects below CHIP_RV350" },
   { R300_ZB_HYPERZ_ROW_PACKET0, R300_SC_HYPERZ, R300_SC_HYPERZ_ENABLE,
     R300_ZB_HYPERZ_KERNEL_CLEARS_SILENTLY, "SC_HYPERZ enable",
     "r300_packet0_check case 0x43a4: bit 0 cleared in the stream for a "
     "non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET3, R300_PACKET3_3D_CLEAR_HIZ, 0u,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "PACKET3 3D_CLEAR_HIZ",
     "r300_packet3_check PACKET3_3D_CLEAR_HIZ: rejects for a non-owner" },
   { R300_ZB_HYPERZ_ROW_PACKET3, R300_PACKET3_3D_CLEAR_ZMASK, 0u,
     R300_ZB_HYPERZ_KERNEL_REJECTS, "PACKET3 3D_CLEAR_ZMASK",
     "r300_packet3_check PACKET3_3D_CLEAR_ZMASK: rejects for a non-owner" },
};

#define ROW_COUNT (sizeof(rows) / sizeof(rows[0]))

const struct r300_zb_hyperz_row *
r300_zb_hyperz_rows(uint32_t *count_out)
{
   if (count_out != NULL)
      *count_out = (uint32_t)ROW_COUNT;
   return rows;
}

const char *
r300_zb_hyperz_verdict_name(enum r300_zb_hyperz_verdict v)
{
   switch (v) {
   case R300_ZB_HYPERZ_ADMIT:
      return "admit";
   case R300_ZB_HYPERZ_REFUSE_OWNERSHIP:
      return "HyperZ write without ownership";
   case R300_ZB_HYPERZ_REFUSE_STREAM:
      return "malformed stream";
   }
   return NULL;
}

static const struct r300_zb_hyperz_row *
find_row(enum r300_zb_hyperz_row_kind kind, uint32_t key)
{
   for (size_t i = 0; i < ROW_COUNT; i++) {
      if (rows[i].kind == kind && rows[i].key == key)
         return &rows[i];
   }
   return NULL;
}

enum r300_zb_hyperz_verdict
r300_zb_hyperz_admit_register(uint32_t reg, uint32_t value,
                              enum r300_zb_hyperz_ownership ownership,
                              const struct r300_zb_hyperz_row **row_out)
{
   const struct r300_zb_hyperz_row *row =
      find_row(R300_ZB_HYPERZ_ROW_PACKET0, reg);
   if (row_out != NULL)
      *row_out = NULL;
   if (row == NULL || (value & row->gated_mask) == 0u)
      return R300_ZB_HYPERZ_ADMIT;
   if (row_out != NULL)
      *row_out = row;
   return ownership == R300_ZB_HYPERZ_OWNED ? R300_ZB_HYPERZ_ADMIT
                                            : R300_ZB_HYPERZ_REFUSE_OWNERSHIP;
}

/* Header fields as radeon_cs_packet_parse reads them. */
#define PKT_TYPE(h) (((h) >> 30) & 0x3u)
#define PKT_COUNT(h) ((((h) >> 16) & 0x3fffu) + 1u)
#define PKT0_REG(h) (((h) & 0x1fffu) << 2)
#define PKT3_OPCODE(h) ((h) & 0xff00u)

enum r300_zb_hyperz_verdict
r300_zb_hyperz_admit_stream(const uint32_t *ib, uint32_t ib_size_dwords,
                            enum r300_zb_hyperz_ownership ownership,
                            struct r300_zb_hyperz_site *site)
{
   struct r300_zb_hyperz_site scratch;
   if (site == NULL)
      site = &scratch;
   memset(site, 0, sizeof(*site));
   if (ib == NULL && ib_size_dwords != 0u)
      return R300_ZB_HYPERZ_REFUSE_STREAM;

   uint32_t i = 0u;
   while (i < ib_size_dwords) {
      const uint32_t header = ib[i];
      const uint32_t type = PKT_TYPE(header);

      if (type == 2u) {
         i += 1u;
         continue;
      }
      if (type == 1u)
         return R300_ZB_HYPERZ_REFUSE_STREAM;

      const uint32_t count = PKT_COUNT(header);
      /* The payload must lie inside the stream: i + count < size in a
       * form that cannot wrap. */
      if (count > ib_size_dwords - 1u - i)
         return R300_ZB_HYPERZ_REFUSE_STREAM;

      if (type == 0u) {
         const uint32_t base = PKT0_REG(header);
         const bool one_reg = (header & RADEON_ONE_REG_WR) != 0u;
         for (uint32_t k = 0; k < count; k++) {
            const uint32_t reg = one_reg ? base : base + 4u * k;
            const uint32_t value = ib[i + 1u + k];
            const struct r300_zb_hyperz_row *row = NULL;
            if (r300_zb_hyperz_admit_register(reg, value, ownership, &row) ==
                R300_ZB_HYPERZ_REFUSE_OWNERSHIP) {
               site->ib_index = i + 1u + k;
               site->reg_or_opcode = reg;
               site->value = value;
               site->row = row;
               return R300_ZB_HYPERZ_REFUSE_OWNERSHIP;
            }
         }
      } else {
         const uint32_t opcode = PKT3_OPCODE(header);
         const struct r300_zb_hyperz_row *row =
            find_row(R300_ZB_HYPERZ_ROW_PACKET3, opcode);
         if (row != NULL && ownership != R300_ZB_HYPERZ_OWNED) {
            site->ib_index = i;
            site->reg_or_opcode = opcode;
            site->value = header;
            site->row = row;
            return R300_ZB_HYPERZ_REFUSE_OWNERSHIP;
         }
      }
      i += 1u + count;
   }
   return R300_ZB_HYPERZ_ADMIT;
}

int
r300_zb_hyperz_rows_self_check(void)
{
   for (size_t i = 0; i < ROW_COUNT; i++) {
      const struct r300_zb_hyperz_row *r = &rows[i];
      if (r->name == NULL || r->kernel_rule == NULL)
         return -EINVAL;
      if (r->kind == R300_ZB_HYPERZ_ROW_PACKET0 && r->gated_mask == 0u)
         return -EINVAL;
      if (r->kind == R300_ZB_HYPERZ_ROW_PACKET3 &&
          (r->key & ~0xff00u) != 0u)
         return -EINVAL;
      for (size_t j = 0; j < i; j++) {
         if (rows[j].kind == r->kind && rows[j].key == r->key)
            return -EINVAL;
      }
   }
   return 0;
}
