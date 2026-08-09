/*
 * SPDX-License-Identifier: MIT
 *
 * Host model of R2VB carrier delivery for the F32_4 identity plan: the
 * producer pass that copies application vertex records into the native
 * TCL-bypass carrier through the fragment path.
 */

#ifndef R300_R2VB_CARRIER_DELIVERY_H
#define R300_R2VB_CARRIER_DELIVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "amd/r300/cpu/r300_cpu_vertex.h"

/* The R2VB producer routes every attribute through the US fragment
 * datapath: the input registers, the copy, and the interpolator carry
 * s1e7m16 (FP24), and the C4_32_FP color target widens the result back
 * to binary32.  A binary32 value therefore survives delivery byte-exact
 * only when it is a fixed point of that round trip, and the delivery
 * model admits exactly that domain: +-0, +-Inf, and normals whose low 7
 * mantissa bits are zero with unbiased exponent in [-62, 63] -- the
 * s1e7m16 normal range under bias 63.  NaN refuses because the 23-bit
 * payload truncates to 16 bits, denormals refuse because binary32
 * denormals sit below the FP24 normal range, and values that would land
 * on FP24 denormals refuse as a conservative fail-closed cut even where
 * silicon might preserve them.  On the admitted domain the round trip
 * is the identity, so the delivery is a verbatim copy there and the
 * model needs no rounding arithmetic.
 */
bool r300_r2vb_f32_4_identity_admits(uint32_t bits);

/* Delivers vertex_count F32_4 records starting at first_vertex into the
 * carrier, the same contract as r300_cpu_vertex_gather: bounds prove in
 * 64-bit arithmetic before any write, -ENOSPC on carrier overrun,
 * -EINVAL on a format other than F32_4.  Every component must admit
 * into the FP24 fixed-point domain above; an out-of-domain component
 * refuses with -EDOM before any carrier write, so the caller's rollback
 * authority selects the CPU route instead of receiving bytes the
 * silicon producer would not reproduce.  Returns 0 on success.
 */
int r300_r2vb_f32_4_identity_deliver(
   int format_id, const struct r300_cpu_vertex_stream *stream,
   uint32_t first_vertex, uint32_t vertex_count, uint32_t *carrier,
   uint32_t carrier_dwords);

#endif /* R300_R2VB_CARRIER_DELIVERY_H */
