/*
 * SPDX-License-Identifier: MIT
 *
 * Direct SPIR-V admission for the compute-job IR: a fail-closed
 * word-stream reader that recognizes the admitted module shapes from
 * SPIR-V itself, with no intermediate compiler representation.  The
 * admitted grammar is an opcode allowlist plus a symbolic walk of the
 * straight-line entry function, so every unrecognized construct
 * refuses by name before any job field publishes.
 */

#ifndef R300_COMPUTE_SPIRV_H
#define R300_COMPUTE_SPIRV_H

#include "r300_compute_job.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lowers an admitted SPIR-V compute module to the job IR.  The
 * admitted subset is the elementwise identity map: one store of one
 * loaded 32-bit word, both addressed by the flattened global
 * invocation index into runtime arrays of 4-byte-stride words at
 * member offset zero, against two distinct storage-buffer bindings on
 * descriptor set 0.  Shared memory, barriers, atomics, arithmetic on
 * the address, control flow, and every opcode outside the recognized
 * grammar refuse with *reason naming the construct; the job is
 * unspecified on refusal.
 * entry_name binds the OpEntryPoint literal byte for byte.
 */
bool r300_compute_job_from_spirv(const uint32_t *words, size_t word_count,
                                 const char *entry_name,
                                 struct r300_compute_job *job,
                                 const char **reason);

#endif /* R300_COMPUTE_SPIRV_H */
