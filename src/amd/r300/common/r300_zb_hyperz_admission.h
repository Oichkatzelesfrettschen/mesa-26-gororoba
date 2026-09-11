/*
 * SPDX-License-Identifier: MIT
 *
 * HyperZ ownership admission for R300-class command streams.
 *
 * The radeon CS parser grants the HyperZ block to one file descriptor at a
 * time.  RADEON_INFO_WANT_HYPERZ records the owner in hyperz_filp, and
 * r300_packet0_check judges every submission against it:
 *
 *    register / packet                        non-owner disposition
 *    ZB_BW_CNTL   HIZ_ENABLE, RD_COMP_ENABLE,
 *                 WR_COMP_ENABLE, FAST_FILL_ENABLE   reject
 *    ZB_ZMASK_OFFSET, ZB_ZMASK_PITCH,
 *    ZB_HIZ_OFFSET, ZB_HIZ_PITCH     nonzero        reject
 *    GB_Z_PEQ_CONFIG                 nonzero        reject (RV350 and later;
 *                                                   every value below it)
 *    SC_HYPERZ    bit 0                             cleared in the stream
 *    PACKET3 3D_CLEAR_HIZ, 3D_CLEAR_ZMASK           reject
 *
 * This table is the same rule one layer earlier.  A stream is scanned
 * before it reaches the ioctl: with ownership held every row admits, and
 * without it every row refuses by name, including the SC_HYPERZ bit the
 * kernel would silently clear, because a silently cleared enable is a
 * draw that ran without the state its recording assumed.  The kernel's
 * rejections stay exactly as they are; the refusal here is the one that
 * lets a caller acquire ownership and resubmit instead of losing the
 * device.
 *
 * Ownership is one of three mechanisms the parser applies to a PACKET0
 * register, and the other two answer before it:
 *
 *    reg run top >= 0x4f80   r100_cs_parse_packet0 rejects the packet
 *                            for lying outside the safe bitmap's extent
 *    reg absent from both    r300_packet0_check reaches its default arm
 *    reg_srcs/r300 and the   and reports "Forbidden register", whatever
 *    check's case labels     the ownership
 *    otherwise               the table above judges it
 *
 * ZB_ZMASK_WRINDEX (0x4f38) and ZB_ZMASK_RDINDEX (0x4f40) land in the
 * second row, which an RS485M measured: the first ZMASK submission
 * through the public lifecycle route carried a zero write to 0x4f38 and
 * the parser reported "Forbidden register 0x4F38 in cs at 14
 * (val=00000000)" with DRM_RADEON_CS answering EINVAL.  The rows for
 * them refuse with no ownership question asked, so a caller reads a
 * stream it must rewrite rather than an acquire it should retry.
 *
 * The scope of an ADMIT is this table: the stream carries no HyperZ
 * write ownership refuses and no register these rows forbid.  The
 * kernel's full PACKET0 authority is the union of reg_srcs/r300 and the
 * r300_packet0_check case labels.  That union is data in
 * r300_kernel_packet0_authority.c, and the ZMASK cross-check test holds
 * every register a plan writes against it.
 *
 * Source: r300_packet0_check and r300_packet3_check in
 * drivers/gpu/drm/radeon/r300.c; r100_cs_parse_packet0 in r100.c;
 * table_build in mkregtable.c; radeon_info_ioctl
 * RADEON_INFO_WANT_HYPERZ in radeon_kms.c.
 */

#ifndef R300_ZB_HYPERZ_ADMISSION_H
#define R300_ZB_HYPERZ_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>

enum r300_zb_hyperz_ownership {
   R300_ZB_HYPERZ_UNOWNED = 0,
   R300_ZB_HYPERZ_OWNED,
};

/* What the kernel does to a non-owner's write of the row. */
enum r300_zb_hyperz_kernel_disposition {
   R300_ZB_HYPERZ_KERNEL_REJECTS = 0,
   R300_ZB_HYPERZ_KERNEL_CLEARS_SILENTLY,
};

enum r300_zb_hyperz_row_kind {
   R300_ZB_HYPERZ_ROW_PACKET0 = 0,
   R300_ZB_HYPERZ_ROW_PACKET3,
   /* A register the parser opens for no client: every write refuses,
    * every value, both ownership states. */
   R300_ZB_HYPERZ_ROW_PACKET0_FORBIDDEN,
};

struct r300_zb_hyperz_row {
   enum r300_zb_hyperz_row_kind kind;
   /* PACKET0: the register; PACKET3: the opcode in header position. */
   uint32_t key;
   /* PACKET0: the bits ownership gates; a value with none of them set is
    * not a HyperZ write.  A PACKET3 row and a forbidden row judge no
    * bits and carry zero. */
   uint32_t gated_mask;
   enum r300_zb_hyperz_kernel_disposition disposition;
   const char *name;
   const char *kernel_rule;
};

const struct r300_zb_hyperz_row *r300_zb_hyperz_rows(uint32_t *count_out);

enum r300_zb_hyperz_verdict {
   R300_ZB_HYPERZ_ADMIT = 0,
   /* The word is a HyperZ write and ownership is not held. */
   R300_ZB_HYPERZ_REFUSE_OWNERSHIP,
   /* The stream is malformed before the scan could judge it. */
   R300_ZB_HYPERZ_REFUSE_STREAM,
   /* The write addresses a register r300_packet0_check answers from its
    * default arm.  An acquire changes nothing; the stream is rewritten
    * or it is refused. */
   R300_ZB_HYPERZ_REFUSE_FORBIDDEN_REGISTER,
   /* The write addresses a register at or above
    * R300_ZB_HYPERZ_PACKET0_REGISTER_LIMIT, which r100_cs_parse_packet0
    * rejects before r300_packet0_check runs. */
   R300_ZB_HYPERZ_REFUSE_REGISTER_RANGE,
   R300_ZB_HYPERZ_VERDICT_COUNT,
};

/* The safe bitmap's extent, and so the first register no PACKET0 run
 * may reach: table_build in mkregtable.c sizes the r300 table as
 * ((offset_max >> 2) + 31) / 32 words over the 0x4f60 the reg_srcs/r300
 * header names, giving 159 words over registers 0 through 0x4f7c.
 * r100_cs_parse_packet0 tests the run's top register against the extent
 * and rejects the packet with -EINVAL; the scan here reports the first
 * register of the run that crosses, so the two refuse the same streams.
 */
#define R300_ZB_HYPERZ_PACKET0_REGISTER_LIMIT 0x4f80u

/* One register write judged against the three mechanisms, in the order
 * the parser applies them: a register at or above the bitmap extent
 * answers REFUSE_REGISTER_RANGE, a forbidden row answers
 * REFUSE_FORBIDDEN_REGISTER for every value and both ownership states,
 * and a gated row answers on ownership.  A register this table does not
 * name, or a gated register whose gated bits are all clear, admits.
 */
enum r300_zb_hyperz_verdict
r300_zb_hyperz_admit_register(uint32_t reg, uint32_t value,
                              enum r300_zb_hyperz_ownership ownership,
                              const struct r300_zb_hyperz_row **row_out);

/* The first HyperZ site in a stream that ownership refuses. */
struct r300_zb_hyperz_site {
   uint32_t ib_index;
   uint32_t reg_or_opcode;
   uint32_t value;
   const struct r300_zb_hyperz_row *row;
};

/* Walks the stream the way radeon_cs_packet_parse frames it: type-0
 * headers name a register run (ONE_REG_WR repeats the register), type-2
 * headers are one-dword fillers, type-3 headers name an opcode with a
 * payload.  Returns ADMIT when no HyperZ write is present or ownership is
 * held; REFUSE_OWNERSHIP with *site at the first gated write otherwise;
 * REFUSE_STREAM for a header that runs past the end or a type-1 packet. */
enum r300_zb_hyperz_verdict
r300_zb_hyperz_admit_stream(const uint32_t *ib, uint32_t ib_size_dwords,
                            enum r300_zb_hyperz_ownership ownership,
                            struct r300_zb_hyperz_site *site);

const char *r300_zb_hyperz_verdict_name(enum r300_zb_hyperz_verdict v);

/* Table self-consistency: keys unique per kind and never a gated and a
 * forbidden row at once, every gated PACKET0 row gated on at least one
 * bit, every forbidden row gating none, every row named with a kernel
 * rule.  Returns 0 or -EINVAL. */
int r300_zb_hyperz_rows_self_check(void);

#endif /* R300_ZB_HYPERZ_ADMISSION_H */
