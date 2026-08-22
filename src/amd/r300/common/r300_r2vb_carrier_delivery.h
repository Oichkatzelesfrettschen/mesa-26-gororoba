/*
 * SPDX-License-Identifier: MIT
 *
 * Host model of R2VB carrier delivery for the F32 identity plan: the
 * producer pass that copies application vertex records into the native
 * TCL-bypass carrier through the fragment path.
 */

#ifndef R300_R2VB_CARRIER_DELIVERY_H
#define R300_R2VB_CARRIER_DELIVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "amd/r300/common/r300_vertex_stream.h"

/* The R2VB producer routes every attribute through the US fragment
 * datapath: the input registers, the copy, and the interpolator carry
 * s1e7m16 (FP24), and the C4_32_FP color target widens the result back
 * to binary32.  A binary32 value therefore survives delivery byte-exact
 * only when it is a fixed point of that round trip, and the delivery
 * model admits non-negative binary32 values that are already fixed points
 * of the shared FP24 quantizer: positive zero and normal values from the
 * measured minimum through the measured finite maximum with the low 7
 * mantissa bits clear.  The RS48x source-read model canonicalizes negative
 * zero and steps negative nonzero values toward zero, so negative values
 * refuse.  Infinities, NaNs, denormals, off-grid values, and values outside
 * the measured normal range also refuse because quantization changes their
 * bits.  On the admitted domain the round trip is the identity, so the
 * delivery is a verbatim copy there and the model needs no rounding
 * arithmetic.
 */
bool r300_r2vb_fp24_identity_admits(uint32_t bits);

/* Delivers vertex_count F32_4, F32_3, or F32_2 records starting at
 * first_vertex into the carrier, the same contract as
 * r300_cpu_vertex_gather: bounds prove in the gather's wrap-free
 * divide form before any read, a stride below the record size refuses
 * the overlapping binding, -ENOSPC on carrier overrun, -EINVAL on any
 * other format --
 * F32_1's synthesized Y stays a CPU-route shape until its identity
 * control exists.  Source components are little-endian binary32 bytes and
 * must admit into the FP24 fixed-point domain above; an out-of-domain
 * component refuses with -EDOM before any carrier write, so the caller's
 * rollback authority selects the CPU route instead of receiving bytes the
 * silicon producer would not reproduce.  The lanes past the source record
 * synthesize as the gather does -- Z as +0.0, W as 1.0 -- values the
 * producer embeds host-side, both FP24 fixed points by construction,
 * so the synthesis needs no admission scan.  Returns 0 on success.
 */
int r300_r2vb_identity_deliver(
   int format_id, const struct r300_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

#endif /* R300_R2VB_CARRIER_DELIVERY_H */
