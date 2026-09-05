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
 * Source: r300_packet0_check and r300_packet3_check in
 * drivers/gpu/drm/radeon/r300.c; radeon_info_ioctl RADEON_INFO_WANT_HYPERZ
 * in radeon_kms.c.
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
};

struct r300_zb_hyperz_row {
   enum r300_zb_hyperz_row_kind kind;
   /* PACKET0: the register; PACKET3: the opcode in header position. */
   uint32_t key;
   /* PACKET0: the bits ownership gates; a value with none of them set is
    * not a HyperZ write.  Unused for a PACKET3 row. */
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
};

/* One register write judged against ownership.  A register outside the
 * table, or a gated register whose gated bits are all clear, admits. */
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

/* Table self-consistency: keys unique per kind, every PACKET0 row gated
 * on at least one bit, every row named with a kernel rule.  Returns 0 or
 * -EINVAL. */
int r300_zb_hyperz_rows_self_check(void);

#endif /* R300_ZB_HYPERZ_ADMISSION_H */
