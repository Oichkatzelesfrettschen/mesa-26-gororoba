/*
 * SPDX-License-Identifier: MIT
 *
 * ZMASK bind and clear plan: the dwords a ZMASK cell appends after the
 * ordinary depth control cell.
 *
 * The ladder separates the mechanisms a fast Z clear composes.  Stage A
 * is the ordinary depth cell with no HyperZ word at all, stage B adds
 * the ownership acquire and nothing else, stage C binds the ZMASK RAM
 * and clears it with compression left off, stage D turns FAST_FILL on so
 * the cleared RAM answers the depth reads, stage E adds RD_COMP_ENABLE
 * so the pipe decompresses a tile the metadata calls compressed, and
 * stage F adds WR_COMP_ENABLE so a passing fragment writes one back.
 * Each stage's stream is the previous stream plus the words its own
 * mechanism needs, so a verdict names one mechanism.
 *
 * The two compressed stages reproduce r300_update_hyperz's own two
 * groups: the decompression path sets FAST_FILL_ENABLE | RD_COMP_ENABLE
 * and the in-use path sets FAST_FILL_ENABLE | RD_COMP_ENABLE |
 * WR_COMP_ENABLE, so each stage is a group Gallium already emits rather
 * than a bit combination this ladder invents.
 */

#ifndef R300_ZMASK_CLEAR_PLAN_H
#define R300_ZMASK_CLEAR_PLAN_H

#include "r300_zmask_layout.h"
#include "r300_zb_depth_surface.h"

#include <stdbool.h>
#include <stdint.h>

enum r300_zmask_clear_stage {
   /* Ordinary depth, HyperZ absent: the append is empty. */
   R300_ZMASK_CLEAR_STAGE_DEPTH_ONLY = 0,
   /* Ownership acquired, HyperZ registers untouched. */
   R300_ZMASK_CLEAR_STAGE_OWNERSHIP_ONLY,
   /* ZMASK bound at its tile size and cleared, compression and fast
    * fill off. */
   R300_ZMASK_CLEAR_STAGE_BIND_CLEAR,
   /* Stage C plus ZB_BW_CNTL FAST_FILL_ENABLE. */
   R300_ZMASK_CLEAR_STAGE_FAST_FILL,
   /* Stage D plus RD_COMP_ENABLE: Gallium's decompression group, which
    * r300_update_hyperz emits under zmask_decompress. */
   R300_ZMASK_CLEAR_STAGE_READ_COMPRESSED,
   /* Stage E plus WR_COMP_ENABLE: Gallium's in-use group, which
    * r300_update_hyperz emits under zmask_in_use. */
   R300_ZMASK_CLEAR_STAGE_WRITE_COMPRESSED,
};

/* Longest append: the ZMASK_OFFSET and ZMASK_PITCH run, the two index
 * registers, GB_Z_PEQ_CONFIG, ZB_BW_CNTL, and the four-dword
 * 3D_CLEAR_ZMASK packet.
 */
#define R300_ZMASK_CLEAR_PLAN_MAX_DWORDS 20u

struct r300_zmask_clear_plan {
   uint32_t words[R300_ZMASK_CLEAR_PLAN_MAX_DWORDS];
   uint32_t dword_count;
   /* The queue calls the HyperZ acquire before submitting this stream.
    * Stage B sets it over an empty append, so an acquire failure and a
    * register-path failure land at different stages.
    */
   bool requires_hyperz_ownership;
   /* The append carries a word the kernel gates on ownership, so the
    * stream refuses without it.
    */
   bool writes_hyperz_registers;
};

/* What a stage can demonstrate about the ZMASK, in the order the ladder
 * reaches it.  A class names an observation no lower class explains, so
 * a run that reaches a class carries evidence for that class alone.
 *
 *    class                    stage  the observation it explains
 *    FAST_CLEAR_SUBSTITUTION    D    a zeroed tile answers depth reads
 *                                    with ZB_DEPTHCLEARVALUE in place of
 *                                    the word depth memory holds
 *    COMPRESSED_READ            E    a tile the metadata calls compressed
 *                                    decompresses to contents a uniform
 *                                    clear cannot produce
 *    COMPRESSED_WRITE           F    a passing fragment writes a
 *                                    compressed tile back, so depth
 *                                    memory and the metadata both change
 *
 * The API contract the classes serve: an image in an optimal layout is
 * stored in an implementation-dependent opaque layout (Vulkan Resource
 * Creation, Image Layouts), so a compressed ZMASK representation is
 * admissible storage for the same logical image; a layout transition
 * whose old layout matches the current layout preserves the contents of
 * the range and happens inside a memory dependency (Vulkan
 * Synchronization and Cache Control, Image Layout Transitions), so the
 * compressed representation must survive synchronization, update,
 * switching, and transfer as the same image, and the materialized bytes
 * are the bridge back to the ordinary uncompressed model.  The register
 * facts below rest on AMD Radeon R5xx Acceleration and the AMD R3xx 3D
 * register reference instead; the two authorities answer different
 * questions and neither substitutes for the other.
 */
enum r300_zmask_evidence_class {
   R300_ZMASK_EVIDENCE_FAST_CLEAR_SUBSTITUTION = 0,
   R300_ZMASK_EVIDENCE_COMPRESSED_READ,
   R300_ZMASK_EVIDENCE_COMPRESSED_WRITE,
   R300_ZMASK_EVIDENCE_CLASS_COUNT,
};

const char *r300_zmask_evidence_class_name(enum r300_zmask_evidence_class c);

/* The class a stage can demonstrate, written through out_class.  Stages
 * A through C establish no ZMASK read at all -- A and B append nothing
 * and C leaves every enable clear -- so they name no class and the call
 * answers false with out_class untouched.
 */
bool r300_zmask_clear_stage_evidence_class(
   enum r300_zmask_clear_stage stage,
   enum r300_zmask_evidence_class *out_class);

/* The ZB_BW_CNTL word a stage writes, or zero for a stage that writes
 * the register at all only from its binding path.  A stage outside the
 * enumeration answers zero. */
uint32_t r300_zmask_clear_stage_bw_cntl(enum r300_zmask_clear_stage stage);

/* Rule 8 of the fast-clear notes in r300_blit.c: FASTFILL must not run
 * with compressed reads disabled while compressed writes are enabled,
 * so FAST_FILL_ENABLE | WR_COMP_ENABLE without RD_COMP_ENABLE is
 * -EINVAL and every other combination is 0.  Every builder runs its
 * stage's word through this before emitting, so the ladder cannot reach
 * the forbidden combination whatever a caller asks for.
 */
int r300_zmask_clear_bw_cntl_check(uint32_t zb_bw_cntl);

/* The stage table held to its own rules: every stage names a ZB_BW_CNTL
 * word rule 8 admits, the two compressed stages carry exactly the two
 * groups r300_update_hyperz emits, and every stage that names an
 * evidence class enables the mechanism the class observes.  Returns 0 or
 * -EINVAL. */
int r300_zmask_clear_stages_self_check(void);

/* The compression block a stage pins.  A stage that leaves both
 * RD_COMP_ENABLE and WR_COMP_ENABLE clear reads and writes depth memory
 * uncompressed, and the R5xx acceleration guide requires 4x4 plane
 * equations while compression is disabled, so stages A through E answer
 * R300_ZCOMP_4X4 and the GA and the ZB agree on the plane-equation
 * format.  Stage F enables compressed writes, which is where 8x8 first
 * becomes admissible; it answers 4x4 as well, because the 4x4-versus-8x8
 * disagreement between the guide and in-tree r300_update_hyperz is a
 * recorded conflict with no silicon observation on either side, and a
 * pinned default keeps the unsettled question out of the stream a caller
 * gets by default.  r300_zmask_clear_stage_admits_block is what opens 8x8
 * at stage F.
 */
enum r300_zmask_compression r300_zmask_clear_stage_block(
   enum r300_zmask_clear_stage stage);

/* Whether a stage may program a block.  Stage F admits both blocks and
 * every other stage admits R300_ZCOMP_4X4 alone, so the caller that
 * wants 8x8 states the block explicitly at the one stage whose
 * configuration makes 8x8 meaningful. */
bool r300_zmask_clear_stage_admits_block(enum r300_zmask_clear_stage stage,
                                         enum r300_zmask_compression block);

/* Builds the append for one stage.  A stage that binds ZMASK refuses
 * with -EINVAL when the layout does not fit, matching
 * r300_fast_zclear_allowed, which returns false on a zero ZMASK dword
 * count; a pitch and a clear count of zero would otherwise describe a
 * bind of nothing.  An unknown stage is -EINVAL.
 *
 * A binding stage additionally refuses a layout whose metadata count exceeds
 * its nonzero RAM capacity or whose block size disagrees with
 * r300_zmask_clear_stage_block.  GB_Z_PEQ_CONFIG and the
 * 3D_CLEAR_ZMASK dword count both follow the block size, so a layout
 * computed at one block paired with a stage that programs the other
 * writes a plane-equation format the clear coverage contradicts: the
 * 64x64 reference level clears four dwords at 8x8 and sixteen at 4x4,
 * and a 4x4 register over an 8x8-sized clear leaves three quarters of
 * the metadata the surface needs untouched.  Resolving the layout
 * through r300_zmask_layout_compute_at_block with the stage's own block
 * makes the pair agree by construction, and this refusal holds the two
 * together for a caller that assembles them another way.  Every refusal
 * leaves the caller's output unchanged.
 */
int r300_zmask_clear_plan_build(enum r300_zmask_clear_stage stage,
                                const struct r300_zmask_layout *layout,
                                struct r300_zmask_clear_plan *out);

/* The same build at a block the caller names.  The block must be one
 * r300_zmask_clear_stage_admits_block admits for the stage and must be
 * the block the layout was resolved at; either disagreement is -EINVAL.
 * r300_zmask_clear_plan_build is this call at the stage's pinned block.
 */
int r300_zmask_clear_plan_build_at_block(
   enum r300_zmask_clear_stage stage, enum r300_zmask_compression block,
   const struct r300_zmask_layout *layout,
   struct r300_zmask_clear_plan *out);

int r300_zmask_fast_clear_plan_build(
   const struct r300_zb_depth_surface *surface,
   const struct r300_zmask_layout *layout, uint32_t depth_code,
   uint32_t stencil, struct r300_zmask_clear_plan *out);

const char *r300_zmask_clear_stage_name(enum r300_zmask_clear_stage stage);

#endif /* R300_ZMASK_CLEAR_PLAN_H */
