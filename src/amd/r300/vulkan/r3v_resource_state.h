/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_RESOURCE_STATE_H
#define R3V_RESOURCE_STATE_H

#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-image resource state for layout tracking.
 *
 * RS482/RS485 carries no auxiliary compression surfaces (no CMASK, no HTILE,
 * no DCC) -- those features postdate the R3xx generation entirely.  Layout
 * transitions on this target therefore have no aux-surface decompression step;
 * they reduce to a pipe_context CS flush at the barrier boundary plus this
 * bookkeeping update.  The flush is documented in r3v_queue.c where
 * R3V_CMD_PIPELINE_BARRIER is replayed.
 *
 * The field is updated at replay time (not at record time) so the ledger
 * reflects the layout visible to subsequent commands in the same submit.
 * Future users: image state validation, compute-layout emit paths, and
 * multi-submit ordering checks that need more than a submit-boundary flush. */
struct r3v_resource_state {
   VkImageLayout layout;
};

#ifdef __cplusplus
}
#endif

#endif /* R3V_RESOURCE_STATE_H */
