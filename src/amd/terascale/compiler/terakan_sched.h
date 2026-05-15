/*
 * SPDX-License-Identifier: MIT
 *
 * terakan_sched.h — Phase 0 Architecture Contract for C2-C6 Scheduler Track
 *
 * This header defines the C interface between the Terakan Vulkan driver
 * and the VLIW5 scheduler/packer/RA pipeline. It sits alongside the
 * existing SFN C++ backend and will eventually replace it.
 *
 * The implementation enforces the 9 Physical Laws of TeraScale-2 Scheduling
 * (see HARMONIZED_SCHEDULER_ARCHITECTURE.md).
 *
 * Integration seam: terakan_shader_sfn.cpp selects between SFN and
 * the new scheduler via TERAKAN_USE_NEW_SCHED environment variable.
 */

#ifndef TERAKAN_SCHED_H
#define TERAKAN_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * Forward declarations
 * ====================================================================== */
struct nir_shader;
struct terakan_sched_context;

/* ======================================================================
 * Section 1: IR Instruction Types (C2)
 *
 * TeraScale-specific instruction representation carrying slot legality,
 * modifier folding, and PV/PS-eligible operand annotations.
 * ====================================================================== */

enum terakan_ir_class {
   TERAKAN_IR_ALU,      /* ALU operation (vec4 + trans slots) */
   TERAKAN_IR_TEX,      /* Texture fetch */
   TERAKAN_IR_VTX,      /* Vertex/memory fetch (VFETCH) */
   TERAKAN_IR_CF,       /* Control flow */
   TERAKAN_IR_EXPORT,   /* Export (SX) */
   TERAKAN_IR_RAT,      /* Random access target (UAV) */
   TERAKAN_IR_LDS,      /* Local data share */
   TERAKAN_IR_BARRIER,  /* Clause boundary barrier (Gap 9) */
};

/* Source operand — unified for GPR, KCACHE, literal, PV/PS */
enum terakan_ir_src_type {
   TERAKAN_SRC_NONE,
   TERAKAN_SRC_GPR,        /* General purpose register */
   TERAKAN_SRC_KCACHE,     /* Constant cache line.component */
   TERAKAN_SRC_LITERAL,    /* Inline 32-bit literal */
   TERAKAN_SRC_PV,         /* Previous vector (inter-bundle bypass) */
   TERAKAN_SRC_PS,         /* Previous scalar (inter-bundle bypass) */
   TERAKAN_SRC_SPECIAL,    /* AR, predicate, etc. */
};

struct terakan_ir_src {
   enum terakan_ir_src_type type;
   uint16_t reg;          /* GPR number or KCACHE addr */
   uint8_t  chan;          /* xyzw channel (0-3) */
   uint8_t  kcache_bank;  /* KCACHE bank (0-1) */
   uint32_t literal_val;  /* inline literal value */
   bool     negate;
   bool     abs_val;
   bool     rel;          /* relative addressing via AR */
};

struct terakan_ir_dst {
   uint16_t reg;
   uint8_t  chan;
   bool     write_mask;
   bool     rel;
   uint8_t  clamp;
};

/* Flags for special instruction properties */
enum terakan_ir_flags {
   TERAKAN_IR_FLAG_PRED_SET     = (1u << 0),
   TERAKAN_IR_FLAG_KILL         = (1u << 1),
   TERAKAN_IR_FLAG_MOVA         = (1u << 2),
   TERAKAN_IR_FLAG_LDS          = (1u << 3),
   TERAKAN_IR_FLAG_EXPORT_DONE  = (1u << 4),
   TERAKAN_IR_FLAG_FP64         = (1u << 5),  /* consumes 2 vec slots */
   TERAKAN_IR_FLAG_SET_CF_IDX   = (1u << 6),  /* X-slot only */
   TERAKAN_IR_FLAG_EOC          = (1u << 7),  /* end of clause marker */
};

struct terakan_ir_instr {
   uint32_t id;              /* unique SSA-like ID */
   enum terakan_ir_class ir_class;
   uint16_t opcode;          /* terakan_alu_op from machine model */
   uint8_t  slot_mask;       /* legal slots from C1 tables */
   uint8_t  nsrc;            /* number of sources (1-3) */
   struct terakan_ir_src src[3];
   struct terakan_ir_dst dst;
   uint32_t flags;           /* terakan_ir_flags bitmask */

   /* Scheduling metadata (filled by C3/C5) */
   int32_t  critical_path_height;
   int32_t  schedule_cycle;    /* -1 if unscheduled */
   int8_t   assigned_slot;     /* -1 if unassigned */
   uint8_t  bank_class;        /* assigned bank class for RA (Gap 8) */

   /* Linked list for instruction pools */
   struct terakan_ir_instr *next;
   struct terakan_ir_instr *prev;
};

/* ======================================================================
 * Section 2: Dependency Graph Edge Types (C3)
 * ====================================================================== */

enum terakan_dep_type {
   TERAKAN_DEP_DATA,           /* RAW: def → use */
   TERAKAN_DEP_WAR_COISSUE,   /* WAR: co-issuable in same bundle (Law 6) */
   TERAKAN_DEP_WAW,           /* WAW: write ordering */
   TERAKAN_DEP_RESOURCE,      /* KCACHE / special unit resource conflict */
   TERAKAN_DEP_BARRIER,       /* Hard clause boundary (Gap 9) */
   TERAKAN_DEP_SEQUENCE,      /* Program-order memory ordering */
};

struct terakan_dep_edge {
   struct terakan_ir_instr *from;
   struct terakan_ir_instr *to;
   enum terakan_dep_type type;
   uint8_t  latency;         /* minimum cycles between from→to */
   bool     coissue_legal;   /* true if WAR edge allows same-bundle */
};

/* ======================================================================
 * Section 3: Resource State Machine (C4 Port Allocator)
 *
 * Tracks the 5-step constraint cascade for the active bundle.
 * ====================================================================== */

struct terakan_bundle_state {
   /* Slot occupancy bitmask (5 bits: XYZWT) */
   uint8_t  slots_used;

   /* GPR read-port tracking: [cycle][channel] = GPR_id or -1 */
   int16_t  gpr_read[3][4];
   uint8_t  num_unique_gprs;

   /* KCACHE clause-level locks (max 2 lines per CF_ALU) */
   uint32_t kcache_line[2];     /* 256-byte aligned addresses */
   uint8_t  kcache_lines_used;

   /* T-slot literal lock */
   bool     t_locked_by_literal;
   uint32_t literal_value;
   uint8_t  num_literals;

   /* Special unit counters (max 1 each per bundle) */
   uint8_t  num_pred_set;
   uint8_t  num_kill;
   uint8_t  num_mova;
   uint8_t  num_lds;

   /* PV/PS forwarding from previous bundle */
   uint8_t  pv_valid;          /* bitmask: which PV.xyzw are live */
   bool     ps_valid;
   uint16_t pv_gpr;            /* GPR that PV aliases */
   uint16_t ps_gpr;            /* GPR that PS aliases */

   /* Instruction count in current bundle */
   uint8_t  num_instrs;
};

/* ======================================================================
 * Section 4: Clause State (C5 Global Scheduler)
 * ====================================================================== */

struct terakan_clause_state {
   enum terakan_ir_class clause_type; /* CF_ALU, CF_TEX, CF_VTX */
   uint16_t instr_count;              /* instructions in active clause */
   uint16_t max_clause_instrs;        /* 128 for ALU */
   uint16_t live_gpr_count;           /* current live GPRs */
   uint16_t occupancy_gpr_threshold;  /* break clause if exceeded */
   uint32_t kcache_line[2];           /* clause-locked KCACHE lines */
   uint8_t  kcache_lines_used;
};

/* ======================================================================
 * Section 5: Bank-Constrained RA Interface (C5.5, Gap 8)
 * ====================================================================== */

struct terakan_bank_constraint {
   uint32_t ssa_a;    /* SSA ID of first value */
   uint32_t ssa_b;    /* SSA ID of second value */
   /* Constraint: physical_reg(a) % 4 != physical_reg(b) % 4 */
};

/* ======================================================================
 * Section 6: Public API — C Interface for Terakan Vulkan Driver
 * ====================================================================== */

/* Create scheduler context (per-shader compilation) */
struct terakan_sched_context *
terakan_sched_create(void *mem_ctx);

void
terakan_sched_destroy(struct terakan_sched_context *ctx);

/* C2: Lower NIR to terakan IR */
bool
terakan_sched_lower_nir(struct terakan_sched_context *ctx,
                        struct nir_shader *nir);

/* C3: Build dependency graph with BARRIER nodes */
bool
terakan_sched_build_dep_graph(struct terakan_sched_context *ctx);

/* C4+C5: Interleaved schedule + pack (single pass) */
bool
terakan_sched_schedule_and_pack(struct terakan_sched_context *ctx);

/* C5.5: Bank-constrained register allocation */
bool
terakan_sched_regalloc(struct terakan_sched_context *ctx);

/* C6: Post-RA repair (PV/PS fixup, minimal) */
bool
terakan_sched_post_ra_repair(struct terakan_sched_context *ctx);

/* Emit bytecode (assembler integration) */
bool
terakan_sched_emit_bytecode(struct terakan_sched_context *ctx,
                            uint32_t **bytecode_out,
                            size_t *bytecode_size_out);

/* C4 Port Allocator: 5-step constraint cascade
 * Returns true if instr can legally join the current bundle. */
bool
terakan_bundle_can_add(const struct terakan_bundle_state *bundle,
                       const struct terakan_clause_state *clause,
                       const struct terakan_ir_instr *instr);

/* C4: Add instruction to bundle (call after can_add returns true) */
void
terakan_bundle_add(struct terakan_bundle_state *bundle,
                   struct terakan_clause_state *clause,
                   struct terakan_ir_instr *instr);

/* C4: Reset bundle state for next cycle */
void
terakan_bundle_advance(struct terakan_bundle_state *prev,
                       struct terakan_bundle_state *next);

/* Debug: dump scheduled IR */
void
terakan_sched_dump(const struct terakan_sched_context *ctx, FILE *f);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_SCHED_H */
