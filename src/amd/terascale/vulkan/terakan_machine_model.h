/*
 * Copyright 2025 Eirikr
 * SPDX-License-Identifier: MIT
 *
 * terakan_machine_model.h — TeraScale-2 (Evergreen) VLIW5 Machine Model
 *
 * Static data tables defining the ISA machine model for the Phase 8 formal
 * instruction scheduler. All slot legality masks are for Evergreen (EG) and
 * derived from the proven r600 SFN implementation (sfn_alu_defines.cpp,
 * unit_mask[2]) cross-referenced with the Evergreen ISA Reference (Nov 2011).
 *
 * This is DATA DEFINITION only — no scheduling algorithms.
 */

#ifndef TERAKAN_MACHINE_MODEL_H
#define TERAKAN_MACHINE_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * Section 1: VLIW5 Slot Definitions
 *
 * ISA Ref Table 4.1 — Instruction Slots in an Instruction Group
 *   Slot 0: ALU.X   (vector element X)
 *   Slot 1: ALU.Y   (vector element Y)
 *   Slot 2: ALU.Z   (vector element Z)
 *   Slot 3: ALU.W   (vector element W)
 *   Slot 4: ALU.Trans (transcendental/scalar)
 *   Slot 5-6: literal constant slots (not instruction slots)
 * ====================================================================== */

enum terakan_vliw_slot {
   TERAKAN_SLOT_X     = 0,
   TERAKAN_SLOT_Y     = 1,
   TERAKAN_SLOT_Z     = 2,
   TERAKAN_SLOT_W     = 3,
   TERAKAN_SLOT_T     = 4,
   TERAKAN_SLOT_COUNT = 5,
};

/* Bitmasks — match r600 SFN convention (x=1,y=2,z=4,w=8,t=16) */
#define TERAKAN_SLOT_MASK_X    (1u << TERAKAN_SLOT_X)   /* 0x01 */
#define TERAKAN_SLOT_MASK_Y    (1u << TERAKAN_SLOT_Y)   /* 0x02 */
#define TERAKAN_SLOT_MASK_Z    (1u << TERAKAN_SLOT_Z)   /* 0x04 */
#define TERAKAN_SLOT_MASK_W    (1u << TERAKAN_SLOT_W)   /* 0x08 */
#define TERAKAN_SLOT_MASK_T    (1u << TERAKAN_SLOT_T)   /* 0x10 */

#define TERAKAN_SLOT_MASK_VEC  (TERAKAN_SLOT_MASK_X | TERAKAN_SLOT_MASK_Y | \
                                TERAKAN_SLOT_MASK_Z | TERAKAN_SLOT_MASK_W) /* 0x0F */
#define TERAKAN_SLOT_MASK_ANY  (TERAKAN_SLOT_MASK_VEC | TERAKAN_SLOT_MASK_T)  /* 0x1F */
#define TERAKAN_SLOT_MASK_NONE 0u

/* ======================================================================
 * Section 2: ALU Opcode Enumeration
 *
 * Opcodes match the Evergreen ISA hardware encoding. The naming convention
 * follows r600_isa.h: op{nsrc}_{name} where nsrc = 0,1,2 for OP2 format
 * and 3 for OP3 format.
 * ====================================================================== */

enum terakan_alu_op {
   /* --- OP2 format: 0-source (pseudo/sync) --- */
   TERAKAN_OP0_NOP                      = 26,
   TERAKAN_OP0_GROUP_BARRIER             = 84,
   TERAKAN_OP0_GROUP_SEQ_BEGIN           = 85,
   TERAKAN_OP0_GROUP_SEQ_END            = 86,
   TERAKAN_OP0_PRED_SET_CLR             = 38,
   TERAKAN_OP0_STORE_FLAGS              = 218,
   TERAKAN_OP0_LDS_1A                   = 220,
   TERAKAN_OP0_LDS_1A1D                 = 221,
   TERAKAN_OP0_LDS_2A                   = 223,

   /* --- OP2 format: 1-source --- */
   TERAKAN_OP1_BCNT_INT                 = 170,
   TERAKAN_OP1_BCNT_ACCUM_PREV_INT      = 182,
   TERAKAN_OP1_BFREV_INT                = 81,
   TERAKAN_OP1_CEIL                     = 18,
   TERAKAN_OP1_COS                      = 142,
   TERAKAN_OP1_EXP_IEEE                 = 129,
   TERAKAN_OP1_FLOOR                    = 20,
   TERAKAN_OP1_FLT_TO_INT               = 80,
   TERAKAN_OP1_FLT_TO_UINT              = 154,
   TERAKAN_OP1_FLT_TO_INT_RPI           = 176,
   TERAKAN_OP1_FLT_TO_INT_FLOOR         = 177,
   TERAKAN_OP1_FLT16_TO_FLT32           = 163,
   TERAKAN_OP1_FLT32_TO_FLT16           = 162,
   TERAKAN_OP1_FLT32_TO_FLT64           = 29,
   TERAKAN_OP1_FLT64_TO_FLT32           = 28,
   TERAKAN_OP1_FRACT                    = 16,
   TERAKAN_OP1_FRACT_64                 = 198,
   TERAKAN_OP1_FREXP_64                 = 196,
   TERAKAN_OP1_INT_TO_FLT               = 155,
   TERAKAN_OP1_LDEXP_64                 = 197,
   TERAKAN_OP1_INTERP_LOAD_P0           = 224,
   TERAKAN_OP1_INTERP_LOAD_P10          = 125,
   TERAKAN_OP1_INTERP_LOAD_P20          = 126,
   TERAKAN_OP1_LOAD_STORE_FLAGS         = 219,
   TERAKAN_OP1_LOG_CLAMPED              = 130,
   TERAKAN_OP1_LOG_IEEE                 = 131,
   TERAKAN_OP1_MAX4                     = 193,
   TERAKAN_OP1_MBCNT_32HI_INT           = 179,
   TERAKAN_OP1_MBCNT_32LO_ACCUM_PREV_INT = 183,
   TERAKAN_OP1_MOV                      = 25,
   TERAKAN_OP1_MOVA_INT                 = 204,
   TERAKAN_OP1_NOT_INT                  = 51,
   TERAKAN_OP1_OFFSET_TO_FLT            = 180,
   TERAKAN_OP1_PRED_SET_INV             = 36,
   TERAKAN_OP1_PRED_SET_RESTORE         = 39,
   TERAKAN_OP1_SET_CF_IDX0              = 88,
   TERAKAN_OP1_SET_CF_IDX1              = 89,
   TERAKAN_OP1_RECIP_CLAMPED            = 132,
   TERAKAN_OP1_RECIP_FF                 = 133,
   TERAKAN_OP1_RECIP_IEEE               = 134,
   TERAKAN_OP1_RECIPSQRT_CLAMPED        = 135,
   TERAKAN_OP1_RECIPSQRT_FF             = 136,
   TERAKAN_OP1_RECIPSQRT_IEEE           = 137,
   TERAKAN_OP1_RECIP_INT                = 147,
   TERAKAN_OP1_RECIP_UINT               = 148,
   TERAKAN_OP1_RECIP_64                 = 149,
   TERAKAN_OP1_RECIP_CLAMPED_64         = 150,
   TERAKAN_OP1_RECIPSQRT_64             = 151,
   TERAKAN_OP1_RECIPSQRT_CLAMPED_64     = 152,
   TERAKAN_OP1_RNDNE                    = 19,
   TERAKAN_OP1_SQRT_IEEE                = 138,
   TERAKAN_OP1_SIN                      = 141,
   TERAKAN_OP1_TRUNC                    = 17,
   TERAKAN_OP1_SQRT_64                  = 153,
   TERAKAN_OP1_UBYTE0_FLT               = 164,
   TERAKAN_OP1_UBYTE1_FLT               = 165,
   TERAKAN_OP1_UBYTE2_FLT               = 166,
   TERAKAN_OP1_UBYTE3_FLT               = 167,
   TERAKAN_OP1_UINT_TO_FLT              = 156,
   TERAKAN_OP1_FFBH_UINT                = 171,
   TERAKAN_OP1_FFBL_INT                 = 172,
   TERAKAN_OP1_FFBH_INT                 = 173,
   TERAKAN_OP1_FLT_TO_UINT4             = 174,

   /* --- OP2 format: 2-source --- */
   TERAKAN_OP2_ADD                      = 0,
   TERAKAN_OP2_MUL                      = 1,
   TERAKAN_OP2_MUL_IEEE                 = 2,
   TERAKAN_OP2_MAX                      = 3,
   TERAKAN_OP2_MIN                      = 4,
   TERAKAN_OP2_MAX_DX10                 = 5,
   TERAKAN_OP2_MIN_DX10                 = 6,
   TERAKAN_OP2_SETE                     = 8,
   TERAKAN_OP2_SETGT                    = 9,
   TERAKAN_OP2_SETGE                    = 10,
   TERAKAN_OP2_SETNE                    = 11,
   TERAKAN_OP2_SETE_DX10                = 12,
   TERAKAN_OP2_SETGT_DX10               = 13,
   TERAKAN_OP2_SETGE_DX10               = 14,
   TERAKAN_OP2_SETNE_DX10               = 15,
   TERAKAN_OP2_ASHR_INT                 = 21,
   TERAKAN_OP2_LSHR_INT                 = 22,
   TERAKAN_OP2_LSHL_INT                 = 23,
   TERAKAN_OP2_MUL_64                   = 27,
   TERAKAN_OP2_PRED_SETGT_UINT          = 30,
   TERAKAN_OP2_PRED_SETGE_UINT          = 31,
   TERAKAN_OP2_PRED_SETE                = 32,
   TERAKAN_OP2_PRED_SETGT               = 33,
   TERAKAN_OP2_PRED_SETGE               = 34,
   TERAKAN_OP2_PRED_SETNE               = 35,
   TERAKAN_OP2_PRED_SET_POP             = 37,
   TERAKAN_OP2_PRED_SETE_PUSH           = 40,
   TERAKAN_OP2_PRED_SETGT_PUSH          = 41,
   TERAKAN_OP2_PRED_SETGE_PUSH          = 42,
   TERAKAN_OP2_PRED_SETNE_PUSH          = 43,
   TERAKAN_OP2_KILLE                    = 44,
   TERAKAN_OP2_KILLGT                   = 45,
   TERAKAN_OP2_KILLGE                   = 46,
   TERAKAN_OP2_KILLNE                   = 47,
   TERAKAN_OP2_AND_INT                  = 48,
   TERAKAN_OP2_OR_INT                   = 49,
   TERAKAN_OP2_XOR_INT                  = 50,
   TERAKAN_OP2_ADD_INT                  = 52,
   TERAKAN_OP2_SUB_INT                  = 53,
   TERAKAN_OP2_MAX_INT                  = 54,
   TERAKAN_OP2_MIN_INT                  = 55,
   TERAKAN_OP2_MAX_UINT                 = 56,
   TERAKAN_OP2_MIN_UINT                 = 57,
   TERAKAN_OP2_SETE_INT                 = 58,
   TERAKAN_OP2_SETGT_INT                = 59,
   TERAKAN_OP2_SETGE_INT                = 60,
   TERAKAN_OP2_SETNE_INT                = 61,
   TERAKAN_OP2_SETGT_UINT               = 62,
   TERAKAN_OP2_SETGE_UINT               = 63,
   TERAKAN_OP2_KILLGT_UINT              = 64,
   TERAKAN_OP2_KILLGE_UINT              = 65,
   TERAKAN_OP2_PREDE_INT                = 66,
   TERAKAN_OP2_PRED_SETGT_INT           = 67,
   TERAKAN_OP2_PRED_SETGE_INT           = 68,
   TERAKAN_OP2_PRED_SETNE_INT           = 69,
   TERAKAN_OP2_KILLE_INT                = 70,
   TERAKAN_OP2_KILLGT_INT               = 71,
   TERAKAN_OP2_KILLGE_INT               = 72,
   TERAKAN_OP2_KILLNE_INT               = 73,
   TERAKAN_OP2_PRED_SETE_PUSH_INT       = 74,
   TERAKAN_OP2_PRED_SETGT_PUSH_INT      = 75,
   TERAKAN_OP2_PRED_SETGE_PUSH_INT      = 76,
   TERAKAN_OP2_PRED_SETNE_PUSH_INT      = 77,
   TERAKAN_OP2_PRED_SETLT_PUSH_INT      = 78,
   TERAKAN_OP2_PRED_SETLE_PUSH_INT      = 79,
   TERAKAN_OP2_ADDC_UINT                = 82,
   TERAKAN_OP2_SUBB_UINT                = 83,
   TERAKAN_OP2_SET_MODE                 = 87,
   TERAKAN_OP2_SET_LDS_SIZE             = 90,
   TERAKAN_OP2_BFM_INT                  = 160,
   TERAKAN_OP2_MULLO_INT                = 143,
   TERAKAN_OP2_MULHI_INT                = 144,
   TERAKAN_OP2_MULLO_UINT               = 145,
   TERAKAN_OP2_MULHI_UINT               = 146,
   TERAKAN_OP2_DOT_IEEE                 = 175,
   TERAKAN_OP2_MULHI_UINT24             = 178,
   TERAKAN_OP2_MUL_UINT24               = 181,
   TERAKAN_OP2_SETE_64                  = 184,
   TERAKAN_OP2_SETNE_64                 = 185,
   TERAKAN_OP2_SETGT_64                 = 186,
   TERAKAN_OP2_SETGE_64                 = 187,
   TERAKAN_OP2_MIN_64                   = 188,
   TERAKAN_OP2_MAX_64                   = 189,
   TERAKAN_OP2_DOT4                     = 190,
   TERAKAN_OP2_DOT4_IEEE                = 191,
   TERAKAN_OP2_CUBE                     = 192,
   TERAKAN_OP2_PRED_SETGT_64            = 199,
   TERAKAN_OP2_PRED_SETE_64             = 200,
   TERAKAN_OP2_PRED_SETGE_64            = 201,
   TERAKAN_OP2_MUL_64_VEC               = 202,
   TERAKAN_OP2_ADD_64                   = 203,
   TERAKAN_OP2_SAD_ACCUM_PREV_UINT      = 207,
   TERAKAN_OP2_DOT                      = 208,
   TERAKAN_OP2_MUL_PREV                 = 209,
   TERAKAN_OP2_MUL_IEEE_PREV            = 210,
   TERAKAN_OP2_ADD_PREV                 = 211,
   TERAKAN_OP2_MULADD_PREV              = 212,
   TERAKAN_OP2_MULADD_IEEE_PREV         = 213,
   TERAKAN_OP2_INTERP_XY                = 214,
   TERAKAN_OP2_INTERP_ZW                = 215,
   TERAKAN_OP2_INTERP_X                 = 216,
   TERAKAN_OP2_INTERP_Z                 = 217,

   /* --- OP3 format: 3-source --- */
   TERAKAN_OP3_BFE_UINT                 = 4,
   TERAKAN_OP3_BFE_INT                  = 5,
   TERAKAN_OP3_BFI_INT                  = 6,
   TERAKAN_OP3_FMA                      = 7,
   TERAKAN_OP3_CNDNE_64                 = 9,
   TERAKAN_OP3_FMA_64                   = 10,
   TERAKAN_OP3_LERP_UINT                = 11,
   TERAKAN_OP3_BIT_ALIGN_INT            = 12,
   TERAKAN_OP3_BYTE_ALIGN_INT           = 13,
   TERAKAN_OP3_SAD_ACCUM_UINT           = 14,
   TERAKAN_OP3_SAD_ACCUM_HI_UINT        = 15,
   TERAKAN_OP3_MULADD_UINT24            = 16,
   TERAKAN_OP3_LDS_IDX_OP               = 17,
   TERAKAN_OP3_MULADD                   = 20,
   TERAKAN_OP3_MULADD_M2                = 21,
   TERAKAN_OP3_MULADD_M4                = 22,
   TERAKAN_OP3_MULADD_D2                = 23,
   TERAKAN_OP3_MULADD_IEEE              = 24,
   TERAKAN_OP3_CNDE                     = 25,
   TERAKAN_OP3_CNDGT                    = 26,
   TERAKAN_OP3_CNDGE                    = 27,
   TERAKAN_OP3_CNDE_INT                 = 28,
   TERAKAN_OP3_CNDGT_INT                = 29,
   TERAKAN_OP3_CNDGE_INT                = 30,
   TERAKAN_OP3_MUL_LIT                  = 31,

   TERAKAN_ALU_OP_COUNT                 = 256,
};

/* ======================================================================
 * Section 3: Operation Categories and Flags
 * ====================================================================== */

enum terakan_op_category {
   TERAKAN_CAT_ALU       = 0,  /* Generic ALU (ADD, MUL, MOV, etc.) */
   TERAKAN_CAT_TRANS     = 1,  /* Transcendental (SIN, COS, RCP, etc.) */
   TERAKAN_CAT_REDUCTION = 2,  /* DOT4, CUBE, MAX4 — must occupy all 4 vec slots */
   TERAKAN_CAT_PRED      = 3,  /* Predicate set/push/kill */
   TERAKAN_CAT_LDS       = 4,  /* LDS operations */
   TERAKAN_CAT_INTERP    = 5,  /* Interpolation ops */
   TERAKAN_CAT_SPECIAL   = 6,  /* MOVA, GROUP_BARRIER, SET_CF_IDX, etc. */
   TERAKAN_CAT_DP64      = 7,  /* Double-precision (2 or 4 slot) */
};

/* Op flags — combinable bitmask for co-issue and scheduling constraints */
#define TERAKAN_OPFLAG_NONE          0u
#define TERAKAN_OPFLAG_PRED_SET      (1u << 0)  /* Computes predicate (exclusive) */
#define TERAKAN_OPFLAG_KILL          (1u << 1)  /* Kill instruction (exclusive) */
#define TERAKAN_OPFLAG_REDUCTION     (1u << 2)  /* Must occupy all 4 vec slots */
#define TERAKAN_OPFLAG_MOVA          (1u << 3)  /* Writes address register */
#define TERAKAN_OPFLAG_CAN_SRCMOD    (1u << 4)  /* Supports src NEG/ABS modifiers */
#define TERAKAN_OPFLAG_CAN_CLAMP     (1u << 5)  /* Supports output clamp [0,1] */
#define TERAKAN_OPFLAG_FP64          (1u << 6)  /* Double-precision (multi-slot) */
#define TERAKAN_OPFLAG_LDS           (1u << 7)  /* LDS operation */

/* ======================================================================
 * Section 4: Per-Opcode Slot Legality Table
 *
 * Each entry describes one ALU opcode. The slot_mask_eg field encodes which
 * VLIW5 slots are legal on Evergreen hardware. These masks come from the
 * r600 SFN alu_ops table (unit_mask[2], the EG column).
 *
 * Abbreviations in comments:
 *   A = Any slot (VEC|T, 0x1F)   V = Vector only (0x0F)
 *   T = Trans only (0x10)        X = Special/pseudo (0x01)
 * ====================================================================== */

struct terakan_op_info {
   const char   *name;
   uint8_t       slot_mask_eg;   /* Which VLIW5 slots this op is legal in */
   uint8_t       num_srcs;       /* Number of source operands (0-3) */
   uint8_t       category;       /* enum terakan_op_category */
   uint8_t       flags;          /* TERAKAN_OPFLAG_* bitmask */
   uint8_t       vec_lanes;      /* Vec slots consumed (4=reduction, 2=FP64 paired, 0=normal) */
};

/*
 * Lookup table indexed by hardware opcode.
 *
 * OP2 (opcodes 0..255) and OP3 (opcodes 0..31) share different namespaces
 * in hardware. We provide separate tables for each format.
 */

static const struct terakan_op_info terakan_op2_table[] = {
   /* opcode  0 */ [0]   = { "ADD",                TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  1 */ [1]   = { "MUL",                TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  2 */ [2]   = { "MUL_IEEE",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  3 */ [3]   = { "MAX",                TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  4 */ [4]   = { "MIN",                TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  5 */ [5]   = { "MAX_DX10",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  6 */ [6]   = { "MIN_DX10",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  8 */ [8]   = { "SETE",               TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode  9 */ [9]   = { "SETGT",              TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 10 */ [10]  = { "SETGE",              TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 11 */ [11]  = { "SETNE",              TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 12 */ [12]  = { "SETE_DX10",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 13 */ [13]  = { "SETGT_DX10",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 14 */ [14]  = { "SETGE_DX10",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 15 */ [15]  = { "SETNE_DX10",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 16 */ [16]  = { "FRACT",              TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 17 */ [17]  = { "TRUNC",              TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 18 */ [18]  = { "CEIL",               TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 19 */ [19]  = { "RNDNE",              TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 20 */ [20]  = { "FLOOR",              TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 21 */ [21]  = { "ASHR_INT",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 22 */ [22]  = { "LSHR_INT",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 23 */ [23]  = { "LSHL_INT",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 25 */ [25]  = { "MOV",                TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 26 */ [26]  = { "NOP",                TERAKAN_SLOT_MASK_ANY, 0, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 27 */ [27]  = { "MUL_64",             TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 28 */ [28]  = { "FLT64_TO_FLT32",     TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 29 */ [29]  = { "FLT32_TO_FLT64",     TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },  /* FP64: even-start pair (X→XY or Z→ZW) */
   /* opcode 30 */ [30]  = { "PRED_SETGT_UINT",    TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 31 */ [31]  = { "PRED_SETGE_UINT",    TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 32 */ [32]  = { "PRED_SETE",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 33 */ [33]  = { "PRED_SETGT",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 34 */ [34]  = { "PRED_SETGE",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 35 */ [35]  = { "PRED_SETNE",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 36 */ [36]  = { "PRED_SET_INV",       TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 37 */ [37]  = { "PRED_SET_POP",       TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 38 */ [38]  = { "PRED_SET_CLR",       TERAKAN_SLOT_MASK_ANY, 0, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 39 */ [39]  = { "PRED_SET_RESTORE",   TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 40 */ [40]  = { "PRED_SETE_PUSH",     TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 41 */ [41]  = { "PRED_SETGT_PUSH",    TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 42 */ [42]  = { "PRED_SETGE_PUSH",    TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 43 */ [43]  = { "PRED_SETNE_PUSH",    TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 44 */ [44]  = { "KILLE",              TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 45 */ [45]  = { "KILLGT",             TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 46 */ [46]  = { "KILLGE",             TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 47 */ [47]  = { "KILLNE",             TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL | TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 48 */ [48]  = { "AND_INT",            TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 49 */ [49]  = { "OR_INT",             TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 50 */ [50]  = { "XOR_INT",            TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 51 */ [51]  = { "NOT_INT",            TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 52 */ [52]  = { "ADD_INT",            TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 53 */ [53]  = { "SUB_INT",            TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 54 */ [54]  = { "MAX_INT",            TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 55 */ [55]  = { "MIN_INT",            TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 56 */ [56]  = { "MAX_UINT",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 57 */ [57]  = { "MIN_UINT",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 58 */ [58]  = { "SETE_INT",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 59 */ [59]  = { "SETGT_INT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 60 */ [60]  = { "SETGE_INT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 61 */ [61]  = { "SETNE_INT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 62 */ [62]  = { "SETGT_UINT",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 63 */ [63]  = { "SETGE_UINT",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 64 */ [64]  = { "KILLGT_UINT",        TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL, 0 },
   /* opcode 65 */ [65]  = { "KILLGE_UINT",        TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL, 0 },
   /* opcode 66 */ [66]  = { "PREDE_INT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 67 */ [67]  = { "PRED_SETGT_INT",     TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 68 */ [68]  = { "PRED_SETGE_INT",     TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 69 */ [69]  = { "PRED_SETNE_INT",     TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 70 */ [70]  = { "KILLE_INT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL, 0 },
   /* opcode 71 */ [71]  = { "KILLGT_INT",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL, 0 },
   /* opcode 72 */ [72]  = { "KILLGE_INT",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL, 0 },
   /* opcode 73 */ [73]  = { "KILLNE_INT",         TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_KILL, 0 },
   /* opcode 74 */ [74]  = { "PRED_SETE_PUSH_INT", TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 75 */ [75]  = { "PRED_SETGT_PUSH_INT",TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 76 */ [76]  = { "PRED_SETGE_PUSH_INT",TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 77 */ [77]  = { "PRED_SETNE_PUSH_INT",TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 78 */ [78]  = { "PRED_SETLT_PUSH_INT",TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 79 */ [79]  = { "PRED_SETLE_PUSH_INT",TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET, 0 },
   /* opcode 80 */ [80]  = { "FLT_TO_INT",         TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },   /* EG: vector-only (sfn) */
   /* opcode 81 */ [81]  = { "BFREV_INT",          TERAKAN_SLOT_MASK_ANY, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 82 */ [82]  = { "ADDC_UINT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 83 */ [83]  = { "SUBB_UINT",          TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 84 */ [84]  = { "GROUP_BARRIER",      0x01,                  0, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },         /* X-slot only (pseudo) */
   /* opcode 85 */ [85]  = { "GROUP_SEQ_BEGIN",    TERAKAN_SLOT_MASK_ANY, 0, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 86 */ [86]  = { "GROUP_SEQ_END",      TERAKAN_SLOT_MASK_ANY, 0, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 87 */ [87]  = { "SET_MODE",           TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 88 */ [88]  = { "SET_CF_IDX0",        TERAKAN_SLOT_MASK_X,   1, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_MOVA, 0 },  /* X-slot only, same co-issue restriction as MOVA */
   /* opcode 89 */ [89]  = { "SET_CF_IDX1",        TERAKAN_SLOT_MASK_X,   1, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_MOVA, 0 },  /* X-slot only, same co-issue restriction as MOVA */
   /* opcode 90 */ [90]  = { "SET_LDS_SIZE",       TERAKAN_SLOT_MASK_ANY, 2, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },

   /* --- High opcodes (transcendental/scalar, starting at 129) --- */
   /* opcode 125 */ [125] = { "INTERP_LOAD_P10",   TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 126 */ [126] = { "INTERP_LOAD_P20",   TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 129 */ [129] = { "EXP_IEEE",          TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 130 */ [130] = { "LOG_CLAMPED",       TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 131 */ [131] = { "LOG_IEEE",          TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 132 */ [132] = { "RECIP_CLAMPED",     TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 133 */ [133] = { "RECIP_FF",          TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 134 */ [134] = { "RECIP_IEEE",        TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 135 */ [135] = { "RECIPSQRT_CLAMPED", TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 136 */ [136] = { "RECIPSQRT_FF",      TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 137 */ [137] = { "RECIPSQRT_IEEE",    TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 138 */ [138] = { "SQRT_IEEE",         TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 141 */ [141] = { "SIN",               TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 142 */ [142] = { "COS",               TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 143 */ [143] = { "MULLO_INT",         TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 144 */ [144] = { "MULHI_INT",         TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 145 */ [145] = { "MULLO_UINT",        TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 146 */ [146] = { "MULHI_UINT",        TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 147 */ [147] = { "RECIP_INT",         TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 148 */ [148] = { "RECIP_UINT",        TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 149 */ [149] = { "RECIP_64",          TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 150 */ [150] = { "RECIP_CLAMPED_64",  TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 151 */ [151] = { "RECIPSQRT_64",      TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 152 */ [152] = { "RECIPSQRT_CLAMPED_64",TERAKAN_SLOT_MASK_T, 2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 153 */ [153] = { "SQRT_64",           TERAKAN_SLOT_MASK_T,   2, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 0 },
   /* opcode 154 */ [154] = { "FLT_TO_UINT",       TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 155 */ [155] = { "INT_TO_FLT",        TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 156 */ [156] = { "UINT_TO_FLT",       TERAKAN_SLOT_MASK_T,   1, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 160 */ [160] = { "BFM_INT",           TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 162 */ [162] = { "FLT32_TO_FLT16",    TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 163 */ [163] = { "FLT16_TO_FLT32",    TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 164 */ [164] = { "UBYTE0_FLT",        TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 165 */ [165] = { "UBYTE1_FLT",        TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 166 */ [166] = { "UBYTE2_FLT",        TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 167 */ [167] = { "UBYTE3_FLT",        TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 170 */ [170] = { "BCNT_INT",          TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 171 */ [171] = { "FFBH_UINT",         TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 172 */ [172] = { "FFBL_INT",          TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 173 */ [173] = { "FFBH_INT",          TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 174 */ [174] = { "FLT_TO_UINT4",      TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 175 */ [175] = { "DOT_IEEE",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,      TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 176 */ [176] = { "FLT_TO_INT_RPI",    TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 177 */ [177] = { "FLT_TO_INT_FLOOR",  TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 178 */ [178] = { "MULHI_UINT24",      TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 179 */ [179] = { "MBCNT_32HI_INT",    TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 180 */ [180] = { "OFFSET_TO_FLT",     TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 181 */ [181] = { "MUL_UINT24",        TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 182 */ [182] = { "BCNT_ACCUM_PREV_INT",TERAKAN_SLOT_MASK_VEC,1, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 183 */ [183] = { "MBCNT_32LO_ACCUM_PREV_INT",TERAKAN_SLOT_MASK_VEC,1,TERAKAN_CAT_ALU,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 184 */ [184] = { "SETE_64",           TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 185 */ [185] = { "SETNE_64",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 186 */ [186] = { "SETGT_64",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 187 */ [187] = { "SETGE_64",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 188 */ [188] = { "MIN_64",            TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 189 */ [189] = { "MAX_64",            TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 190 */ [190] = { "DOT4",              TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_REDUCTION,TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_REDUCTION, 4 },
   /* opcode 191 */ [191] = { "DOT4_IEEE",         TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_REDUCTION,TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_REDUCTION, 4 },
   /* opcode 192 */ [192] = { "CUBE",              TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_REDUCTION,TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_REDUCTION, 4 },
   /* opcode 193 */ [193] = { "MAX4",              TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_REDUCTION,TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_REDUCTION, 4 },
   /* FP64 ops consume 2 contiguous vec slots (even-start pair: X→XY or Z→ZW) */
   /* opcode 196 */ [196] = { "FREXP_64",          TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 197 */ [197] = { "LDEXP_64",          TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 198 */ [198] = { "FRACT_64",          TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 199 */ [199] = { "PRED_SETGT_64",     TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 200 */ [200] = { "PRED_SETE_64",      TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 201 */ [201] = { "PRED_SETGE_64",     TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_PRED,  TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 202 */ [202] = { "MUL_64_VEC",        TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 203 */ [203] = { "ADD_64",            TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 204 */ [204] = { "MOVA_INT",          0x01,                  1, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_MOVA, 0 },         /* X-slot only (pseudo) */
   /* opcode 207 */ [207] = { "SAD_ACCUM_PREV_UINT",TERAKAN_SLOT_MASK_VEC,2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 208 */ [208] = { "DOT",               TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,      TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 209 */ [209] = { "MUL_PREV",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 210 */ [210] = { "MUL_IEEE_PREV",     TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 211 */ [211] = { "ADD_PREV",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 212 */ [212] = { "MULADD_PREV",       TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 213 */ [213] = { "MULADD_IEEE_PREV",  TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
   /* opcode 214 */ [214] = { "INTERP_XY",         TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 215 */ [215] = { "INTERP_ZW",         TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 216 */ [216] = { "INTERP_X",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 217 */ [217] = { "INTERP_Z",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 218 */ [218] = { "STORE_FLAGS",       TERAKAN_SLOT_MASK_VEC, 0, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 219 */ [219] = { "LOAD_STORE_FLAGS",  TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_SPECIAL,TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 220 */ [220] = { "LDS_1A",            TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_LDS,   TERAKAN_OPFLAG_LDS, 0 },
   /* opcode 221 */ [221] = { "LDS_1A1D",          TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_LDS,   TERAKAN_OPFLAG_LDS, 0 },
   /* opcode 223 */ [223] = { "LDS_2A",            TERAKAN_SLOT_MASK_VEC, 2, TERAKAN_CAT_LDS,   TERAKAN_OPFLAG_LDS, 0 },
   /* opcode 224 */ [224] = { "INTERP_LOAD_P0",    TERAKAN_SLOT_MASK_VEC, 1, TERAKAN_CAT_INTERP,TERAKAN_OPFLAG_NONE, 0 },
};

/* OP3 table — separate namespace (5-bit opcode, high bits of ALU_INST nonzero) */
static const struct terakan_op_info terakan_op3_table[] = {
   /* opcode  4 */ [4]  = { "BFE_UINT",          TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode  5 */ [5]  = { "BFE_INT",           TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode  6 */ [6]  = { "BFI_INT",           TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode  7 */ [7]  = { "FMA",               TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode  9 */ [9]  = { "CNDNE_64",          TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 10 */ [10] = { "FMA_64",            TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_DP64,  TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP | TERAKAN_OPFLAG_FP64, 2 },
   /* opcode 11 */ [11] = { "LERP_UINT",         TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 12 */ [12] = { "BIT_ALIGN_INT",     TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 13 */ [13] = { "BYTE_ALIGN_INT",    TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 14 */ [14] = { "SAD_ACCUM_UINT",    TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 15 */ [15] = { "SAD_ACCUM_HI_UINT", TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 16 */ [16] = { "MULADD_UINT24",     TERAKAN_SLOT_MASK_VEC, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 17 */ [17] = { "LDS_IDX_OP",        0x01,                  3, TERAKAN_CAT_LDS,   TERAKAN_OPFLAG_LDS, 0 },    /* X-slot only (pseudo) */
   /* opcode 20 */ [20] = { "MULADD",            TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 21 */ [21] = { "MULADD_M2",         TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 22 */ [22] = { "MULADD_M4",         TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 23 */ [23] = { "MULADD_D2",         TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 24 */ [24] = { "MULADD_IEEE",       TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_CAN_SRCMOD | TERAKAN_OPFLAG_CAN_CLAMP, 0 },
   /* opcode 25 */ [25] = { "CNDE",              TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 26 */ [26] = { "CNDGT",             TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 27 */ [27] = { "CNDGE",             TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 28 */ [28] = { "CNDE_INT",          TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 29 */ [29] = { "CNDGT_INT",         TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 30 */ [30] = { "CNDGE_INT",         TERAKAN_SLOT_MASK_ANY, 3, TERAKAN_CAT_ALU,   TERAKAN_OPFLAG_NONE, 0 },
   /* opcode 31 */ [31] = { "MUL_LIT",           TERAKAN_SLOT_MASK_T,   3, TERAKAN_CAT_TRANS, TERAKAN_OPFLAG_CAN_SRCMOD, 0 },
};

#define TERAKAN_OP2_TABLE_SIZE (sizeof(terakan_op2_table) / sizeof(terakan_op2_table[0]))
#define TERAKAN_OP3_TABLE_SIZE (sizeof(terakan_op3_table) / sizeof(terakan_op3_table[0]))

/* ======================================================================
 * Section 5: Latency Matrix
 *
 * Latency is measured in ALU instruction groups (the scheduling quantum).
 * Within an ALU clause, the PV/PS bypass makes all ALU→ALU dependencies
 * 1-cycle. Cross-clause latencies are approximate and depend on cache state.
 *
 * ISA Ref §4.11: Hardware auto-substitutes PV/PS for adjacent-group GPR
 * dependencies. No compiler NOP needed except for GPR-indexed edge cases.
 * ====================================================================== */

enum terakan_clause_type {
   TERAKAN_CLAUSE_ALU  = 0,
   TERAKAN_CLAUSE_VTX  = 1,
   TERAKAN_CLAUSE_TEX  = 2,
   TERAKAN_CLAUSE_LDS  = 3,
   TERAKAN_CLAUSE_CF   = 4,
   TERAKAN_CLAUSE_TYPE_COUNT = 5,
};

struct terakan_latency_info {
   uint16_t group_latency;   /* Intra-clause: groups until result available */
   uint16_t clause_latency;  /* Inter-clause: approx cycles for clause type */
};

/*
 * group_latency:  ALU→ALU = 1 (PV/PS bypass); Trans→ALU = 1 (PS bypass)
 * clause_latency: estimated minimum cycles for the clause type overall
 */
static const struct terakan_latency_info terakan_latencies[] = {
   [TERAKAN_CLAUSE_ALU] = { .group_latency = 1,   .clause_latency = 1   },
   [TERAKAN_CLAUSE_VTX] = { .group_latency = 0,   .clause_latency = 150 },
   [TERAKAN_CLAUSE_TEX] = { .group_latency = 0,   .clause_latency = 150 },
   [TERAKAN_CLAUSE_LDS] = { .group_latency = 0,   .clause_latency = 20  },
   [TERAKAN_CLAUSE_CF]  = { .group_latency = 0,   .clause_latency = 40  },
};

/* ======================================================================
 * Section 6: Issue Group Constraints
 *
 * An instruction group (VLIW5 bundle) must satisfy ALL these constraints.
 * ISA Ref §4.3, §4.7.4–4.7.9, §4.8.1.1, §4.8.3.1
 * ====================================================================== */

struct terakan_group_limits {
   uint8_t max_vec_slots;        /* Max ALU.[X,Y,Z,W] instructions per group */
   uint8_t max_trans_slots;      /* Max ALU.Trans instructions per group */
   uint8_t max_slots_total;      /* Max total instructions per group */
   uint8_t max_gpr_read_ports;   /* Per-element GPR read ports per cycle */
   uint8_t num_read_cycles;      /* Number of operand-fetch cycles (BANK_SWIZZLE) */
   uint8_t max_kcache_banks;     /* Kcache banks per clause (EG=2, Cayman=4) */
   uint8_t max_kcache_lines_per_bank; /* Cache lines lockable per bank */
   uint8_t max_kcache_consts_per_line; /* Constants per cache line */
   uint8_t max_cfile_reads;      /* Max distinct constant-file reads per group */
   uint8_t max_literals;         /* Max literal constant dwords per group */
   uint8_t max_literal_slots;    /* Max literal slots (64-bit each) per group */
   uint8_t max_pred_per_group;   /* Max PRED_SET* ops per group */
   uint8_t max_kill_per_group;   /* Max KILL ops per group */
   uint8_t max_mova_per_group;   /* Max MOVA* ops per group */
   uint16_t max_alu_clause_slots; /* Max slots per ALU clause */
};

static const struct terakan_group_limits terakan_eg_limits = {
   .max_vec_slots               = 4,
   .max_trans_slots              = 1,
   .max_slots_total              = 5,   /* 4 vec + 1 trans */
   .max_gpr_read_ports           = 3,   /* 3 read ports per element memory */
   .num_read_cycles              = 3,   /* Cycles 0, 1, 2 for BANK_SWIZZLE */
   .max_kcache_banks             = 2,   /* EG has 2 kcache banks (Cayman has 4) */
   .max_kcache_lines_per_bank    = 2,   /* Lock 1 or 2 lines per bank */
   .max_kcache_consts_per_line   = 16,  /* 16 vec4 constants per cache line */
   .max_cfile_reads              = 4,   /* 4 distinct constant-file elements per group */
   .max_literals                 = 4,   /* 4 dwords (X,Y,Z,W) of literal data */
   .max_literal_slots            = 2,   /* Slots 5 and 6 in the instruction group */
   .max_pred_per_group           = 1,   /* At most 1 PRED_SET* per group */
   .max_kill_per_group           = 1,   /* At most 1 KILL per group */
   .max_mova_per_group           = 1,   /* At most 1 MOVA per group (ISA §4.9.5) */
   .max_alu_clause_slots         = 128, /* Max slots in an ALU clause */
};

/* ======================================================================
 * Section 7: Read-Port / Bank-Swizzle Constraints
 *
 * ISA Ref §4.7.7–4.7.8: Each element memory (X,Y,Z,W) has 3 read ports.
 * Operands are loaded across 3 cycles. BANK_SWIZZLE controls mapping.
 *
 * Key rules:
 * - Cannot read 2+ values from the same GPR element on the same cycle
 * - Special case: src0==src1 from same GPR.elem shares one port (squaring)
 * - ALU.Trans shares read ports with ALU.[XYZW], opportunistic loading
 * - PV/PS have no cycle restrictions
 * - Constants can be accessed on any cycle
 * ====================================================================== */

/* BANK_SWIZZLE values for ALU.[X,Y,Z,W] — maps srcN to CYCLEN_GPR */
enum terakan_vec_bank_swizzle {
   TERAKAN_VEC_012 = 0,   /* src0=C0, src1=C1, src2=C2 (identity) */
   TERAKAN_VEC_021 = 1,   /* src0=C0, src1=C2, src2=C1 */
   TERAKAN_VEC_120 = 2,   /* src0=C1, src1=C2, src2=C0 */
   TERAKAN_VEC_102 = 3,   /* src0=C1, src1=C0, src2=C2 */
   TERAKAN_VEC_201 = 4,   /* src0=C2, src1=C0, src2=C1 */
   TERAKAN_VEC_210 = 5,   /* src0=C2, src1=C1, src2=C0 */
   TERAKAN_VEC_SWIZZLE_COUNT = 6,
};

/* BANK_SWIZZLE values for ALU.Trans — different interpretation */
enum terakan_scl_bank_swizzle {
   TERAKAN_SCL_210 = 0,   /* src0=C2, src1=C1, src2=C0 */
   TERAKAN_SCL_122 = 1,   /* src0=C1, src1=C2, src2=C2 */
   TERAKAN_SCL_212 = 2,   /* src0=C2, src1=C1, src2=C2 */
   TERAKAN_SCL_221 = 3,   /* src0=C2, src1=C2, src2=C1 */
   TERAKAN_SCL_SWIZZLE_COUNT = 4,
};

/*
 * Cycle mapping tables for BANK_SWIZZLE validation.
 * vec_swizzle_cycle[swiz][src_idx] = cycle number for that operand.
 */
static const uint8_t terakan_vec_swizzle_cycle[6][3] = {
   /* VEC_012 */ { 0, 1, 2 },
   /* VEC_021 */ { 0, 2, 1 },
   /* VEC_120 */ { 1, 2, 0 },
   /* VEC_102 */ { 1, 0, 2 },
   /* VEC_201 */ { 2, 0, 1 },
   /* VEC_210 */ { 2, 1, 0 },
};

static const uint8_t terakan_scl_swizzle_cycle[4][3] = {
   /* SCL_210 */ { 2, 1, 0 },
   /* SCL_122 */ { 1, 2, 2 },
   /* SCL_212 */ { 2, 1, 2 },
   /* SCL_221 */ { 2, 2, 1 },
};

/* ======================================================================
 * Section 8: Bypass Edges (PV/PS Forwarding)
 *
 * ISA Ref §4.6.2, §4.11: Result forwarding paths that avoid stalls.
 *
 * PV (Previous Vector): 4-element register holding ALU.[X,Y,Z,W] results
 *   from the prior instruction group. PV.X = slot X result, etc.
 * PS (Previous Scalar): scalar register holding ALU.Trans result from
 *   the prior instruction group.
 *
 * Rules:
 * - PV/PS only valid within the same ALU clause (cleared at clause start)
 * - NOP invalidates PV and PS
 * - Hardware auto-substitutes PV/PS when adjacent groups have GPR deps
 * - No bypass for GPR-indexed writes followed by non-indexed reads
 *   (compiler must insert NOP or reorder)
 * ====================================================================== */

enum terakan_bypass_type {
   TERAKAN_BYPASS_NONE      = 0,  /* No forwarding; must wait for GPR write */
   TERAKAN_BYPASS_PV        = 1,  /* Result forwarded via PV register */
   TERAKAN_BYPASS_PS        = 2,  /* Result forwarded via PS register */
   TERAKAN_BYPASS_PV_PRED   = 3,  /* PV with predication override */
};

struct terakan_bypass_edge {
   uint8_t from_slot;       /* Producing slot (TERAKAN_SLOT_*) */
   uint8_t bypass_type;     /* enum terakan_bypass_type */
   uint8_t latency_groups;  /* Groups until result available (always 1) */
};

/* All 5 slots produce results available in 1 group via PV or PS */
static const struct terakan_bypass_edge terakan_bypass_edges[] = {
   { TERAKAN_SLOT_X, TERAKAN_BYPASS_PV, 1 },
   { TERAKAN_SLOT_Y, TERAKAN_BYPASS_PV, 1 },
   { TERAKAN_SLOT_Z, TERAKAN_BYPASS_PV, 1 },
   { TERAKAN_SLOT_W, TERAKAN_BYPASS_PV, 1 },
   { TERAKAN_SLOT_T, TERAKAN_BYPASS_PS, 1 },
};

#define TERAKAN_BYPASS_EDGE_COUNT \
   (sizeof(terakan_bypass_edges) / sizeof(terakan_bypass_edges[0]))

/* ======================================================================
 * Section 9: Export / CF Ordering Rules
 *
 * ISA Ref §3.4.1, Table 3.3:
 * - EXPORT_PIXEL: MRT outputs 0-7 and computed Z (61). Last pixel export
 *   must use EXPORT_DONE to signal HW.
 * - EXPORT_POS: position outputs 60-63. Last position export uses
 *   EXPORT_DONE.
 * - EXPORT_PARAM: parameter cache outputs 0-31. Dense-packed starting at
 *   index 0. Last parameter export uses EXPORT_DONE.
 *
 * Ordering:
 * - Each export type's DONE instruction must be the final one of that type.
 * - Parameter exports must be dense (0, 1, 2, ...) with no gaps.
 * - A shader must export at least one value (even if masked) for each
 *   required output type to terminate correctly.
 * ====================================================================== */

enum terakan_export_type {
   TERAKAN_EXPORT_PIXEL   = 0,
   TERAKAN_EXPORT_POS     = 1,
   TERAKAN_EXPORT_PARAM   = 2,
   TERAKAN_EXPORT_TYPE_COUNT = 3,
};

struct terakan_export_limits {
   uint8_t max_pixel_exports;    /* Max MRT targets */
   uint8_t max_pos_exports;      /* Max position exports (60-63) */
   uint8_t max_param_exports;    /* Max parameter cache entries */
   uint8_t pixel_z_index;        /* ARRAY_BASE for computed Z export */
};

static const struct terakan_export_limits terakan_eg_export_limits = {
   .max_pixel_exports   = 8,     /* MRT0-MRT7 */
   .max_pos_exports     = 4,     /* POS0-POS3 (ARRAY_BASE 60-63) */
   .max_param_exports   = 32,    /* PARAM0-PARAM31 */
   .pixel_z_index       = 61,    /* CF_PIXEL_Z */
};

/* ======================================================================
 * Section 10: Co-Issue Exclusion Rules
 *
 * ISA Ref §4.8.1.1, §4.8.2.2, §4.8.3.1:
 * Certain operations are mutually exclusive within a single group.
 * ====================================================================== */

/*
 * Co-issue exclusion matrix:
 * - Two PRED_SET* ops cannot be co-issued
 * - Two KILL ops cannot be co-issued
 * - PRED_SET* and KILL cannot be co-issued
 * - At most 1 MOVA* per group
 * - At most 1 transcendental (T-slot) per group
 * - Reduction ops (DOT4, CUBE, MAX4) must fill all 4 vec slots
 * - MOVA* must not co-issue with AR-indexed ops in any slot
 */

static inline bool
terakan_ops_can_coissue(const struct terakan_op_info *a,
                        const struct terakan_op_info *b)
{
   if (!a || !b)
      return false;

   /* PRED_SET and KILL are mutually exclusive with each other */
   const uint8_t exclusive_mask = TERAKAN_OPFLAG_PRED_SET | TERAKAN_OPFLAG_KILL;
   if ((a->flags & exclusive_mask) && (b->flags & exclusive_mask))
      return false;

   /* A reduction occupies all 4 vec slots — only a T-only op may co-issue */
   if ((a->flags & TERAKAN_OPFLAG_REDUCTION) &&
       (b->slot_mask_eg & TERAKAN_SLOT_MASK_VEC))
      return false;
   if ((b->flags & TERAKAN_OPFLAG_REDUCTION) &&
       (a->slot_mask_eg & TERAKAN_SLOT_MASK_VEC))
      return false;

   return true;
}

/* ======================================================================
 * Section 11: Helper Functions (inline)
 * ====================================================================== */

static inline const struct terakan_op_info *
terakan_get_op2_info(unsigned opcode)
{
   if (opcode >= TERAKAN_OP2_TABLE_SIZE)
      return NULL;
   const struct terakan_op_info *info = &terakan_op2_table[opcode];
   return info->name ? info : NULL;
}

static inline const struct terakan_op_info *
terakan_get_op3_info(unsigned opcode)
{
   if (opcode >= TERAKAN_OP3_TABLE_SIZE)
      return NULL;
   const struct terakan_op_info *info = &terakan_op3_table[opcode];
   return info->name ? info : NULL;
}

static inline bool
terakan_op_can_slot(const struct terakan_op_info *info,
                    enum terakan_vliw_slot slot)
{
   if (!info)
      return false;
   return (info->slot_mask_eg & (1u << slot)) != 0;
}

static inline bool
terakan_op_is_trans_only(const struct terakan_op_info *info)
{
   return info && info->slot_mask_eg == TERAKAN_SLOT_MASK_T;
}

static inline bool
terakan_op_is_x_only(const struct terakan_op_info *info)
{
   return info && info->slot_mask_eg == TERAKAN_SLOT_MASK_X;
}

static inline bool
terakan_op_is_vec_only(const struct terakan_op_info *info)
{
   return info && info->slot_mask_eg == TERAKAN_SLOT_MASK_VEC;
}

static inline bool
terakan_op_is_reduction(const struct terakan_op_info *info)
{
   return info && (info->flags & TERAKAN_OPFLAG_REDUCTION) != 0;
}

static inline bool
terakan_op_is_kill(const struct terakan_op_info *info)
{
   return info && (info->flags & TERAKAN_OPFLAG_KILL) != 0;
}

static inline bool
terakan_op_is_pred_set(const struct terakan_op_info *info)
{
   return info && (info->flags & TERAKAN_OPFLAG_PRED_SET) != 0;
}

/* ======================================================================
 * Section 12: Group Constraint Checker Prototype
 *
 * Full validation requires tracking per-cycle read port allocation and
 * BANK_SWIZZLE selection. These are declared here; implementation in
 * terakan_machine_model.c when the scheduler is built (C2-C6).
 * ====================================================================== */

struct terakan_group_state {
   uint8_t  slot_used;            /* Bitmask of occupied slots */
   uint8_t  num_trans;            /* Number of trans ops placed */
   uint8_t  num_pred;             /* Number of PRED_SET* ops placed */
   uint8_t  num_kill;             /* Number of KILL ops placed */
   uint8_t  num_mova;             /* Number of MOVA* ops placed */
   bool     has_reduction;        /* True if a reduction op is placed */
   bool     has_lds_op;           /* True if an LDS op is placed (max 1 per group) */
   uint8_t  num_literals;         /* Literal dwords used (0-4; each of 2 literal slots holds 2 dwords) */
   uint8_t  num_kcache_banks;     /* Kcache banks in use */

   /*
    * GPR read port tracking: gpr_read[cycle][elem] = GPR address (-1 = free).
    * Each element (X,Y,Z,W) has 3 read ports corresponding to 3 cycles.
    */
   int16_t  gpr_read[3][4];      /* [cycle][element] = GPR addr or -1 */
};

/* Initialize an empty group state */
static inline void
terakan_group_state_init(struct terakan_group_state *gs)
{
   gs->slot_used     = 0;
   gs->num_trans     = 0;
   gs->num_pred      = 0;
   gs->num_kill      = 0;
   gs->num_mova      = 0;
   gs->has_reduction = false;
   gs->has_lds_op    = false;
   gs->num_literals  = 0;
   gs->num_kcache_banks = 0;
   for (int c = 0; c < 3; c++)
      for (int e = 0; e < 4; e++)
         gs->gpr_read[c][e] = -1;
}

/*
 * Check if an instruction can be added to the current group.
 * Returns true if legal, false if any constraint would be violated.
 * Detailed implementation deferred to terakan_machine_model.c (C2).
 *
 * Parameters:
 *   gs       — current group state
 *   info     — op info for the instruction to add
 *   slot     — target slot
 *   limits   — hardware limits
 */
bool terakan_group_can_add(const struct terakan_group_state *gs,
                           const struct terakan_op_info *info,
                           enum terakan_vliw_slot slot,
                           const struct terakan_group_limits *limits);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_MACHINE_MODEL_H */
