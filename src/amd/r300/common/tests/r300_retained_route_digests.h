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

/* The public draw surface's own stream at the reference target: the
 * render-shape cell over the reference geometry with the constant the
 * admitted fragment module wrote -- the reference module's
 * vec4(0, 1, 0, 1) -- so the executed constant is the pipeline's rather
 * than the emitter's baked oracle color.  The stream differs from the
 * retained CPU route in the four R300_PFS_PARAM_0 payloads alone and
 * keeps its dword count.  The constant mechanism -- the four payloads
 * the shape writes and the UNORM8 value they render -- carries the
 * attended render-shape procedure's constant arm as its silicon
 * witness; this stream's own identity is the host composition below.
 */
#define R300_MODULE_CONSTANT_CPU_ROUTE_IB_DWORDS \
   R300_RETAINED_CPU_ROUTE_IB_DWORDS
#define R300_MODULE_CONSTANT_CPU_ROUTE_IB_BLAKE3 \
   "3a3443902879a0c86e30bc6e1d2ba69c933a306902838040363239b80bd60030"

/* GPU route: the composed public R2VB producer stream from
 * r300_r2vb_public_route_reference_compose(), producer then consumer. */
#define R300_RETAINED_GPU_ROUTE_IB_DWORDS 547u
#define R300_RETAINED_GPU_ROUTE_CONSUMER_START_DWORDS 316u
#define R300_RETAINED_GPU_ROUTE_IB_BLAKE3 \
   "17c98b55d72d4836f51a4cf2a993fe3e3d41f41318b3b82cdb5178eb4f8c7164"

#endif /* R300_RETAINED_ROUTE_DIGESTS_H */
