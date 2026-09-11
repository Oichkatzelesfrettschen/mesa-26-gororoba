/*
 * SPDX-License-Identifier: MIT
 *
 * The radeon r300 CS parser's PACKET0 register authority, as data.
 *
 * A type-0 packet reaches r100_cs_parse_packet0 with the r300 safe
 * bitmap.  Two mechanisms decide the register run, in this order:
 *
 *    reg run top >= 0x4f80            -EINVAL before any check runs
 *    bitmap bit clear (safe listed)   admits with no further check
 *    bitmap bit set                   r300_packet0_check judges it:
 *                                     a case handles it, and the
 *                                     default arm reports
 *                                     "Forbidden register"
 *
 * mkregtable.c builds the bitmap from reg_srcs/r300: table_build
 * memsets every word to 0xff and then XOR-clears the bit of each listed
 * offset, and r100_cs_parse_packet0 calls the check function when
 * auth[j] & m is set.  A listed register therefore carries a clear bit
 * and skips the check, which makes the two tables below disjoint by
 * construction -- r300_kernel_packet0_authority_self_check asserts that
 * disjointness, so a transcription that lands a register in both is a
 * test failure rather than a silent widening.
 *
 * The bound is the bitmap's own extent: table_build sizes the table as
 * ((offset_max >> 2) + 31) / 32 words over an offset_max of 0x4f60,
 * the last register named in the reg_srcs header line, giving 159 words
 * and a first unreachable register of 0x4f80.
 *
 * Provenance, radeon-unified 0.8.19 as deployed on the Vostro 1000 at
 * /usr/src/radeon-unified-0.8.19/radeon/:
 *
 *    reg_srcs/r300 sha256
 *    c688dca95a806f1e218e9951295f997fc1dca5cc63481d48801ef6193fe6e81a
 *    r300.c sha256
 *    b78e379f88b63fd25ccf4686542eddef008e898b20716848dd8b365b0243781a
 *    r100.c sha256
 *    f213b47956b6ee471c2078e118f18ae433cf5b08f6be0f0a3f866fda574f1653
 *    mkregtable.c sha256
 *    e8a7a177e265b8a46ef678412eeecdf33f745cf9236d8f2b033b7f8ad8f1d404
 *
 * The safe list is that reg_srcs/r300 verbatim, offset and name per
 * row.  The checked list is the case labels of the r300_packet0_check
 * switch, which r300.c spells as bare hex apart from the register
 * macros resolved here from the same tree's headers.  Both are
 * mechanically derived:
 *
 *    grep -oE '^(0x[0-9a-fA-F]+) ' reg_srcs/r300
 *    awk 'NR>=732 && NR<=1378' r300.c | grep -oE '^\tcase [^:]+:'
 *
 * The authority is the kernel's, so this file is test data: the driver
 * asks r300_zb_hyperz_admission.c about a stream, and the cross-check
 * test holds every register a ZMASK plan writes against the union here.
 */

#ifndef R300_KERNEL_PACKET0_AUTHORITY_H
#define R300_KERNEL_PACKET0_AUTHORITY_H

#include <stdbool.h>
#include <stdint.h>

/* The first register r100_cs_parse_packet0 rejects for lying outside the
 * safe bitmap's extent. */
#define R300_KERNEL_PACKET0_REGISTER_LIMIT 0x4f80u

/* The bitmap word count table_build derives, which the limit follows
 * from as R300_KERNEL_PACKET0_REGISTER_LIMIT / (32 * 4). */
#define R300_KERNEL_PACKET0_BITMAP_WORDS 159u

struct r300_kernel_register {
   uint32_t offset;
   /* The name reg_srcs/r300 carries, or NULL where r300.c spells the
    * case label as a bare offset. */
   const char *name;
};

/* reg_srcs/r300: every register the bitmap admits without a check. */
const struct r300_kernel_register *
r300_kernel_safe_registers(uint32_t *count_out);

/* The r300_packet0_check case labels: every register the bitmap routes
 * to the check, which the check then admits, gates, or rejects. */
const struct r300_kernel_register *
r300_kernel_checked_registers(uint32_t *count_out);

bool r300_kernel_register_safe_listed(uint32_t reg);
bool r300_kernel_register_checked(uint32_t reg);

/* Whether a PACKET0 write of reg reaches r300_packet0_check without
 * hitting its default arm or the bitmap bound.  A checked register may
 * still refuse on HyperZ ownership, chip family, or a malformed value;
 * r300_zb_hyperz_admission.c carries the ownership half. */
bool r300_kernel_admits_packet0_register(uint32_t reg);

/* Both tables ascending and duplicate-free, disjoint from each other,
 * every offset inside the bitmap bound or named as outside it, and the
 * counts the derivation produced.  Returns 0 or -EINVAL. */
int r300_kernel_packet0_authority_self_check(void);

#endif /* R300_KERNEL_PACKET0_AUTHORITY_H */
