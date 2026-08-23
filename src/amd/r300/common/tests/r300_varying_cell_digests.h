/*
 * SPDX-License-Identifier: MIT
 *
 * The varying TCL-bypass triangle cell's no-submit identity: the BLAKE3
 * of the little-endian dword stream r300_tcl_bypass_triangle_varying_
 * reference_emit() emits -- position-plus-varying records through the
 * pass-through fragment binary -- with its dword count.  Every emitter
 * and submit-path consumer compares against these literals, so a change
 * that moves the cell's bytes reports as a digest movement against the
 * identity an attended run declares.
 */

#ifndef R300_VARYING_CELL_DIGESTS_H
#define R300_VARYING_CELL_DIGESTS_H

#define R300_VARYING_CELL_IB_DWORDS 236u
#define R300_VARYING_CELL_IB_BLAKE3 \
   "49856ce9b9dc5f6caaa5f259d77d205c4b7f8dda42123c3aad674da0a5ad12e2"

#endif /* R300_VARYING_CELL_DIGESTS_H */
