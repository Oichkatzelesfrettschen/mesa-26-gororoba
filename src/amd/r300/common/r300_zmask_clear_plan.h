/*
 * SPDX-License-Identifier: MIT
 *
 * ZMASK bind and clear plan: the dwords a ZMASK cell appends after the
 * ordinary depth control cell.
 *
 * The ladder separates the mechanisms a fast Z clear composes.  Stage A
 * is the ordinary depth cell with no HyperZ word at all, stage B adds
 * the ownership acquire and nothing else, stage C binds the ZMASK RAM
 * and clears it with compression left off, and stage D turns FAST_FILL
 * on so the cleared RAM answers the depth reads.  Each stage's stream is
 * the previous stream plus the words its own mechanism needs, so a
 * verdict names one mechanism.
 */

#ifndef R300_ZMASK_CLEAR_PLAN_H
#define R300_ZMASK_CLEAR_PLAN_H

#include "r300_zmask_layout.h"

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
};

/* Longest append: the ZMASK_OFFSET and ZMASK_PITCH run, the two index
 * registers, GB_Z_PEQ_CONFIG, ZB_BW_CNTL, and the four-dword
 * 3D_CLEAR_ZMASK packet.
 */
#define R300_ZMASK_CLEAR_PLAN_MAX_DWORDS 16u

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

/* Builds the append for one stage.  A stage that binds ZMASK refuses
 * with -EINVAL when the layout does not fit, matching
 * r300_fast_zclear_allowed, which returns false on a zero ZMASK dword
 * count; a pitch and a clear count of zero would otherwise describe a
 * bind of nothing.  An unknown stage is -EINVAL.
 */
int r300_zmask_clear_plan_build(enum r300_zmask_clear_stage stage,
                                const struct r300_zmask_layout *layout,
                                struct r300_zmask_clear_plan *out);

const char *r300_zmask_clear_stage_name(enum r300_zmask_clear_stage stage);

#endif /* R300_ZMASK_CLEAR_PLAN_H */
