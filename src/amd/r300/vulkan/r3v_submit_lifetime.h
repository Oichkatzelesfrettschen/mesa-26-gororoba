/*
 * Copyright 2026 Mesa3D authors
 * SPDX-License-Identifier: MIT
 *
 * Submit cleanup ordering for Gallium replay.
 */

#ifndef R3V_SUBMIT_LIFETIME_H
#define R3V_SUBMIT_LIFETIME_H

#include <stdbool.h>

typedef void (*r3v_submit_lifetime_action)(void *data);

struct r3v_submit_lifetime_ops {
   r3v_submit_lifetime_action drain;
   r3v_submit_lifetime_action release;
};

/* A replay error can follow GPU work emitted by an earlier command.  The
 * pending-work state, rather than the final VkResult, controls whether the
 * resource owner must wait before releasing its transient references. */
static inline void
r3v_submit_lifetime_finish(bool gpu_pending,
                           const struct r3v_submit_lifetime_ops *ops,
                           void *data)
{
   if (gpu_pending)
      ops->drain(data);
   ops->release(data);
}

/* Render-pass load-op clears enqueue GPU writes even when no draw follows.
 * Keep depth/stencil and color clears in the same pending-work contract. */
static inline bool
r3v_submit_lifetime_render_pass_has_clear(bool depth_clear, bool color_clear)
{
   return depth_clear || color_clear;
}

#endif /* R3V_SUBMIT_LIFETIME_H */
