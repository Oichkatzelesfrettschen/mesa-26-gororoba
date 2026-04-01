/*
 * terakan_shader_debug.h — TERAKAN_DEBUG=shaders/perf shader analysis helpers.
 *
 * Must be valid C (included from terakan_shader_sfn.cpp via C linkage).
 */
#pragma once
#include <stdint.h>
#include "compiler/shader_enums.h"

struct r600_bytecode;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Walk the CF/ALU linked list (must be called BEFORE r600_bytecode_clear).
 * Prints per-stage VLIW utilisation, GPR count, bundle-size histogram.
 *
 * Controlled by debug_flags:
 *   TERAKAN_DEBUG_SHADERS — always print
 *   TERAKAN_DEBUG_PERF    — print only when utilisation < 70%
 */
void terakan_shader_debug_vliw_stats(struct r600_bytecode const *bc,
                                     mesa_shader_stage stage,
                                     uint64_t debug_flags);

#ifdef __cplusplus
}
#endif
