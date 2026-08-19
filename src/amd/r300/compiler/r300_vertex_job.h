/*
 * SPDX-License-Identifier: MIT
 *
 * CPU vertex-job IR: the compiler's immutable lowering output that the
 * CPU executor decodes.  The job describes one vertex program over a
 * vec4 temporary file; the executor runs it once per vertex.
 */

#ifndef R300_VERTEX_JOB_H
#define R300_VERTEX_JOB_H

#include <stdint.h>

/* The register file is vec4-shaped like the VAP PVS it stands in for;
 * the limits bound validation, not silicon.  Constants and register
 * lanes are 32-bit patterns, not host floats, so data movement is a
 * byte copy and NaN payloads, denormals, and negative zero survive
 * every non-arithmetic instruction.
 */
#define R300_VERTEX_JOB_MAX_INSTRUCTIONS 64
#define R300_VERTEX_JOB_MAX_TEMPS 16
#define R300_VERTEX_JOB_MAX_CONSTANTS 32

/* Arithmetic executes in host IEEE-754 binary32, one rounding per
 * operator.  FMAD is the two-rounding multiply-then-add -- the K8
 * target's SSE2/SSE3 substrate carries no fused operator, and the
 * scalar authority pins the same double rounding so a later SIMD
 * kernel can be proven bit-identical.  DP4 sums its four products in
 * component order, ((x + y) + z) + w, and broadcasts the scalar to
 * all four destination lanes.
 */
enum r300_vertex_job_opcode {
   /* dst = logical vec4 of input attribute src0 for the current
    * vertex, gathered through the r300_vertex_format_semantics
    * selectors.  The single-binding subset admits attribute 0 only. */
   R300_VERTEX_JOB_OP_LOAD_INPUT = 0,
   /* dst = constants[src0] (bit copy). */
   R300_VERTEX_JOB_OP_LOAD_CONSTANT,
   /* dst = temp[src0] (bit copy). */
   R300_VERTEX_JOB_OP_MOV,
   /* dst = temp[src0] + temp[src1], per lane. */
   R300_VERTEX_JOB_OP_FADD,
   /* dst = temp[src0] * temp[src1], per lane. */
   R300_VERTEX_JOB_OP_FMUL,
   /* dst = temp[src0] * temp[src1] + temp[src2], per lane, two
    * roundings. */
   R300_VERTEX_JOB_OP_FMAD,
   /* dst.xyzw = dot4(temp[src0], temp[src1]). */
   R300_VERTEX_JOB_OP_DP4,
   /* carrier vec4 of the current vertex = temp[src0].  Exactly one
    * per job, as its final instruction. */
   R300_VERTEX_JOB_OP_STORE_POSITION,
};

struct r300_vertex_job_instruction {
   uint8_t opcode;
   uint8_t dst;
   uint8_t src0;
   uint8_t src1;
   uint8_t src2;
};

struct r300_vertex_job {
   /* enum r300_vertex_format_id of the one bound attribute stream. */
   int input_format_id;
   uint32_t instruction_count;
   struct r300_vertex_job_instruction
      instructions[R300_VERTEX_JOB_MAX_INSTRUCTIONS];
   uint32_t constant_count;
   uint32_t constants[R300_VERTEX_JOB_MAX_CONSTANTS][4];
};

#endif /* R300_VERTEX_JOB_H */
