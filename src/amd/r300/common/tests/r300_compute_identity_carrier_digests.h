/*
 * SPDX-License-Identifier: MIT
 *
 * Retained RS485M (1002:5974) identity of the reference compute identity
 * carrier pass: the BLAKE3 of the little-endian dword stream
 * r300_compute_identity_carrier_reference_emit() emits -- sixteen F32_4
 * records (one 64-invocation workgroup) fetched from a one-page input at
 * offset zero and written as one C4_32_FP slot row at offset zero of a
 * one-page output -- with its dword count, the stream an attended run
 * submitted on silicon and the device delivered the output equal to the
 * input for (steinmarder-r300 bundle r3v-native-compute-identity-
 * carrier-cell-first-delivery-rs482).  Every consumer (emitter test,
 * manifest writer, kernel replay, the driver's admission and the harness
 * arms) compares against these literals, so a drift here is a drift of
 * the pass's bytes against the retained silicon identity.
 */
#ifndef R300_COMPUTE_IDENTITY_CARRIER_DIGESTS_H
#define R300_COMPUTE_IDENTITY_CARRIER_DIGESTS_H
#define R300_COMPUTE_IDENTITY_CARRIER_IB_DWORDS 316u
#define R300_COMPUTE_IDENTITY_CARRIER_IB_BLAKE3 \
   "650ae8ddbdfbc7b9d0ca2d306a5c1e92348352aea756de4b238a2537fe259fb7"
#endif /* R300_COMPUTE_IDENTITY_CARRIER_DIGESTS_H */
