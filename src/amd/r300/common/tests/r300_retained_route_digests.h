/*
 * SPDX-License-Identifier: MIT
 *
 * Retained RS482 (1002:5974) route identities: the BLAKE3 of each
 * little-endian dword stream an attended route-dispatch-timing run
 * submitted on silicon, with the dword counts and the consumer split the
 * composed stream declared.  A test that emits or submits either route
 * compares against these literals, so an emitter, composer, or submit-path
 * change that moves the submitted bytes reports as a digest movement
 * against the retained silicon identity.
 */

#ifndef R300_RETAINED_ROUTE_DIGESTS_H
#define R300_RETAINED_ROUTE_DIGESTS_H

/* CPU route: the 64x64 TCL-bypass triangle reference cell, the bytes
 * r300_tcl_bypass_triangle_reference_emit() emits and the consumer slice
 * of the composed public route. */
#define R300_RETAINED_CPU_ROUTE_IB_DWORDS 231u
#define R300_RETAINED_CPU_ROUTE_IB_BLAKE3 \
   "ddbb5e9e38257994a5433a3e0af1cf0da094acb8fd6c1b7d7f09916fa3d41821"

/* GPU route: the composed public R2VB producer stream from
 * r300_r2vb_public_route_reference_compose(), producer then consumer. */
#define R300_RETAINED_GPU_ROUTE_IB_DWORDS 547u
#define R300_RETAINED_GPU_ROUTE_CONSUMER_START_DWORDS 316u
#define R300_RETAINED_GPU_ROUTE_IB_BLAKE3 \
   "17c98b55d72d4836f51a4cf2a993fe3e3d41f41318b3b82cdb5178eb4f8c7164"

#endif /* R300_RETAINED_ROUTE_DIGESTS_H */
