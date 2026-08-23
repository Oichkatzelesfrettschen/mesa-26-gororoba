/*
 * SPDX-License-Identifier: MIT
 *
 * The varying TCL-bypass triangle cell's retained RS482 (1002:5974)
 * identity: the BLAKE3 of the little-endian dword stream
 * r300_tcl_bypass_triangle_varying_reference_emit() emits --
 * position-plus-varying records through the pass-through fragment
 * binary -- with its dword count, the stream an attended run submitted
 * on silicon and the target delivered the varying gradient for
 * (steinmarder-r300 bundle r3v-native-varying-triangle-cell-first-
 * delivery-rs482).  Every emitter and submit-path consumer compares
 * against these literals, so a change that moves the cell's bytes
 * reports as a digest movement against the retained silicon identity.
 */

#ifndef R300_VARYING_CELL_DIGESTS_H
#define R300_VARYING_CELL_DIGESTS_H

#define R300_VARYING_CELL_IB_DWORDS 236u
#define R300_VARYING_CELL_IB_BLAKE3 \
   "49856ce9b9dc5f6caaa5f259d77d205c4b7f8dda42123c3aad674da0a5ad12e2"

#endif /* R300_VARYING_CELL_DIGESTS_H */
