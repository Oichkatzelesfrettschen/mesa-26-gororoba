/*
 * SPDX-License-Identifier: MIT
 *
 * CPU vertex-job IR: the compiler's immutable lowering output that the
 * CPU executor decodes.  The job describes one vertex program over a
 * vec4 temporary file; the executor runs it once per vertex.
 */

#ifndef R300_VERTEX_JOB_H
#define R300_VERTEX_JOB_H

#include <stdbool.h>
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
/* Attribute slots the job can read, one per vertex-input location; the
 * count is the Vulkan maxVertexInputAttributes the physical device
 * publishes, so every admitted pipeline's locations fit. */
#define R300_VERTEX_JOB_MAX_INPUTS 16

/* Arithmetic executes in IEEE-754 binary32 under the executor's
 * round-to-nearest, denormal-preserving environment.  FMAD commits the
 * product to binary32 before the add.  FFMA rounds only the combined
 * multiply-add result.  DP4 sums its four products in component order,
 * ((x + y) + z) + w, and broadcasts the scalar to all four destination
 * lanes.
 */
enum r300_vertex_job_opcode {
   /* dst = logical vec4 of input attribute slot src0 for the current
    * vertex, gathered from that slot's stream through the
    * r300_vertex_format_semantics selectors of input_format_ids[src0]. */
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
   /* dst = temp[src0] * temp[src1] + temp[src2], per lane, one fused
    * rounding. */
   R300_VERTEX_JOB_OP_FFMA,
   /* dst.xyzw = dot4(temp[src0], temp[src1]). */
   R300_VERTEX_JOB_OP_DP4,
   /* carrier vec4 of the current vertex = temp[src0].  Exactly one
    * per job, as its final instruction. */
   R300_VERTEX_JOB_OP_STORE_POSITION,
   /* varying vec4 of the current vertex = temp[src0]: the second
    * record vector, the TEX0 varying the consumer's RS routes to US
    * input 0.  At most one per job, before the position store; a job
    * that carries it writes eight-dword records. */
   R300_VERTEX_JOB_OP_STORE_VARYING,
};

struct r300_vertex_job_instruction {
   uint8_t opcode;
   uint8_t dst;
   uint8_t src0;
   uint8_t src1;
   uint8_t src2;
};

struct r300_vertex_job {
   /* enum r300_vertex_format_id per attribute slot; a slot no
    * LOAD_INPUT reads stays R300_VERTEX_FORMAT_INVALID (zero), and a
    * slot one reads carries the bound attribute's format. */
   int input_format_ids[R300_VERTEX_JOB_MAX_INPUTS];
   uint32_t instruction_count;
   struct r300_vertex_job_instruction
      instructions[R300_VERTEX_JOB_MAX_INSTRUCTIONS];
   uint32_t constant_count;
   uint32_t constants[R300_VERTEX_JOB_MAX_CONSTANTS][4];
};

/* The carrier record the job writes per vertex: the position vec4, then
 * the varying vec4 when the job stores one.  The VAP fetch of the same
 * stream declares the record as one FLOAT_4 element per vector, so the
 * dword count here is the VAP_VTX_SIZE the consumer programs.
 */
#define R300_VERTEX_JOB_POSITION_DWORDS 4u
#define R300_VERTEX_JOB_VARYING_DWORDS 4u

static inline bool
r300_vertex_job_has_varying(const struct r300_vertex_job *job)
{
   for (uint32_t i = 0; i < job->instruction_count; i++) {
      if (job->instructions[i].opcode == R300_VERTEX_JOB_OP_STORE_VARYING)
         return true;
   }
   return false;
}

/* The attribute slots the job's LOAD_INPUT instructions read, one bit
 * per slot: the streams an executor must bind and bound-prove. */
static inline uint32_t
r300_vertex_job_input_mask(const struct r300_vertex_job *job)
{
   uint32_t mask = 0;
   for (uint32_t i = 0; i < job->instruction_count; i++) {
      if (job->instructions[i].opcode == R300_VERTEX_JOB_OP_LOAD_INPUT &&
          job->instructions[i].src0 < R300_VERTEX_JOB_MAX_INPUTS)
         mask |= 1u << job->instructions[i].src0;
   }
   return mask;
}

static inline uint32_t
r300_vertex_job_record_dwords(const struct r300_vertex_job *job)
{
   return R300_VERTEX_JOB_POSITION_DWORDS +
          (r300_vertex_job_has_varying(job) ? R300_VERTEX_JOB_VARYING_DWORDS
                                             : 0u);
}

#endif /* R300_VERTEX_JOB_H */
