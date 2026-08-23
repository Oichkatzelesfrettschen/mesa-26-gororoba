/*
 * SPDX-License-Identifier: MIT
 *
 * Compute identity map on the R2VB producer carrier: the raster lowering
 * of out[i] = in[i] over 32-bit storage words, in which the fetched
 * producer pass reads the input buffer as F32_4 records through the VAP
 * and writes the output buffer as one C4_32_FP slot row through the
 * color backend.  The US datapath narrows to FP24, so the route promises
 * the identity inside the FP24 fixed-point window alone and refuses a
 * word outside it before any device work.
 */

#ifndef R300_COMPUTE_IDENTITY_CARRIER_H
#define R300_COMPUTE_IDENTITY_CARRIER_H

#include "r300_r2vb_fetched_producer.h"
#include "r300_r2vb_producer_pass.h"

#include <stdint.h>

/* One slot carries four words: a record is one F32_4 fetch and one
 * C4_32_FP export, so the dispatch's invocation count is a multiple of
 * four and the record count is the single-row producer's count. */
#define R300_COMPUTE_IDENTITY_CARRIER_RECORD_DWORDS 4u
#define R300_COMPUTE_IDENTITY_CARRIER_RECORD_BYTES 16u
#define R300_COMPUTE_IDENTITY_CARRIER_MAX_RECORDS R300_R2VB_PRODUCER_MAX_COUNT

/* The output offset keeps the 32-byte alignment every retained carrier
 * offset has carried, so the RB3D_COLOROFFSET0 payload stays inside the
 * qualified alignment class; the input offset is dword-granular, the
 * VBPNTR pointer's unit. */
#define R300_COMPUTE_IDENTITY_CARRIER_OUTPUT_ALIGN 32u
#define R300_COMPUTE_IDENTITY_CARRIER_INPUT_ALIGN 4u

struct r300_compute_identity_carrier_params {
   uint32_t record_count;
   /* The output buffer: byte offset of record 0 inside its BO and the
    * BO size; the color backend's bound is pitch * 16 bytes from the
    * offset, the single-row layout's pitch rounding included. */
   uint32_t carrier_offset;
   uint64_t carrier_bo_size_bytes;
   /* The input buffer: byte offset of record 0 inside its BO and the
    * BO size; records fetch at a 16-byte stride. */
   uint32_t source_offset;
   uint64_t source_bo_size_bytes;
   /* The driver-owned slot BO: record_count slot positions from
    * slot_offset_bytes. */
   uint32_t slot_offset_bytes;
   uint64_t slot_bo_size_bytes;
};

/* The single-row layout for record_count records, or -EINVAL outside
 * 1..R300_COMPUTE_IDENTITY_CARRIER_MAX_RECORDS. */
int r300_compute_identity_carrier_layout(
   uint32_t record_count, struct r300_r2vb_producer_layout *out);

/* The carrier bytes the layout's color bound covers from the output
 * offset: pitch_pixels * 16. */
uint64_t r300_compute_identity_carrier_output_bytes(
   const struct r300_r2vb_producer_layout *layout);

/* Emits the pass: the first-draw contract at the slot-row extent, the
 * producer prologue with the output as the color target, the
 * varying-passthrough US program, the fetched draw body over the slot
 * and input arrays, and the publication tail -- three relocation sites
 * in stream order: carrier (output, written), slot (read), source
 * (input, read).  Returns 0 or a negative errno: -EINVAL for a count
 * outside the ceiling or a misaligned offset, -ERANGE when the output
 * bound, the input array, or the slot array leaves its BO.  The caller
 * owns the returned IB. */
int r300_compute_identity_carrier_emit(
   const struct r300_compute_identity_carrier_params *params,
   struct r300_r2vb_fetched_producer_ib *out);

/* The reference pass: sixteen records (one 64-invocation workgroup of
 * the reference identity-map kernel) at offset zero in one-page BOs,
 * the digest every pre-hardware consumer pins. */
#define R300_COMPUTE_IDENTITY_CARRIER_REFERENCE_RECORDS 16u
int r300_compute_identity_carrier_reference_emit(
   struct r300_r2vb_fetched_producer_ib *out);

/* The host model of the delivery: the FP24 identity over record_count
 * F32_4 records of input words into out (record_count * 4 dwords).
 * Inside the FP24 fixed-point window the result is the bit copy the CPU
 * compute route writes, so the two routes agree byte for byte; a word
 * outside the window refuses with -EDOM before any write, -ENOSPC when
 * out_dwords is short, -EINVAL on a null argument or zero count. */
int r300_compute_identity_carrier_expected(const uint32_t *input,
                                           uint32_t record_count,
                                           uint32_t *out,
                                           uint32_t out_dwords);

#endif /* R300_COMPUTE_IDENTITY_CARRIER_H */
