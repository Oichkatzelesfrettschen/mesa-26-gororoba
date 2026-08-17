/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef R300_CAPABILITIES_H
#define R300_CAPABILITIES_H

#include <stdbool.h>

/* The size of a compressed tile. Each compressed tile takes 2 bits
 * in the ZMASK RAM, so there is always 16 tiles per one dword. */
enum r300_zmask_compression {
   R300_ZCOMP_4X4 = 4,
   R300_ZCOMP_8X8 = 8,
};

/* Structure containing all the possible information about a specific Radeon
 * in the R3xx, R4xx, and R5xx families. */
struct r300_capabilities {
   /* Chipset family */
   int family;
   /* The number of vertex floating-point units */
   unsigned num_vert_fpus;
   /* The number of texture units. */
   unsigned num_tex_units;
   /* Whether or not TCL is physically present. */
   bool has_hardware_tcl;
   /* Whether or not hardware TCL is enabled. Debug options may disable it. */
   bool has_tcl;
   /* Some chipsets do not have HiZ RAM - other have varying amounts. */
   int hiz_ram;
   /* Some chipsets have zmask ram per pipe some don't. */
   int zmask_ram;
   /* CMASK is for MSAA colorbuffer compression and fast clear. */
   bool has_cmask;
   /* Compression mode for ZMASK. */
   enum r300_zmask_compression z_compress;
   /* Whether or not this is RV350 or newer, including all r400 and r500
    * chipsets. The differences compared to the oldest r300 chips are:
    * - Blend LTE/GTE thresholds
    * - Better MACRO_SWITCH in texture tiling
    * - Half float vertex
    * - More HyperZ optimizations */
   bool is_rv350;
   /* Whether or not this is R400. The differences compared their rv350
    * cousins are:
    * - Extended fragment shader registers
    * - 3DC texture compression (RGTC2) */
   bool is_r400;
   /* R300_HB_R400_US on an RS48x part: the fragment-shader command-stream
    * emit treats the US block as R400-class (R400_US_CODE_BANK/EXT writes,
    * extended-address registers when the program needs code banks) while
    * is_r400 stays false, so texture-format admission and the vertex
    * compiler keep their R300-class configuration.  Set only by
    * r300_hb_r400_us_init; execution is unproven and under test. */
   bool hb_r400_us;
   /* R300_HB_R400_US=2 additionally lifts the fragment compiler to the full
    * R400 envelope (64 temps, 512 ALU/TEX slots, r390_mode code banks).  Kept
    * separate from the emission probe: the envelope changes register
    * allocation even for programs that fit R300 limits, so the two effects
    * must be testable independently. */
   bool hb_r400_us_envelope;
   /* R300_HB_R400_US=3 lifts only the ALU/TEX slot count to 512 and the
    * r390_mode code-bank emission, while the temp file stays at the proven
    * R300 size of 32.  A >64-instruction shader that stays within 32 temps
    * then exercises code banks without touching the unproven upper temp file
    * (regs 32..63), which is the safer first silicon rung.  Diagnostic only
    * on RS48x: bank instructions execute, but a live temporary written in
    * bank 0 is not usable in bank 1 (constants survive the boundary), so the
    * envelope raise is a compile-envelope probe, not a dependent-chain
    * escape past 64 slots. */
   bool hb_r400_us_alu_only;
   /* Whether or not this is an RV515 or newer; R500s have many differences
    * that require extra consideration, compared to their rv350 cousins:
    * - Extra bit of width and height on texture sizes
    * - Blend color is split across two registers
    * - Universal Shader (US) block used for fragment shaders
    * - FP16 blending and multisampling
    * - Full RGTC texture compression
    * - 24-bit depth textures
    * - Stencil back-face reference value
    * - Ability to render up to 2^24 - 1 vertices with signed index offset */
   bool is_r500;
   /* Whether or not the second pixel pipe is accessed with the high bit */
   bool high_second_pipe;
   /* DXTC texture swizzling. */
   bool dxtc_swizzle;
   /* Whether R500_US_FORMAT0_0 exists (R520-only and DRM-dependent). */
   bool has_us_format;
};

#endif /* R300_CAPABILITIES_H */
