/*
 * Copyright © 2024 Vitaliy Triang3l Kuzmin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef TERAKAN_DRAW_H
#define TERAKAN_DRAW_H

#include "terakan_command_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* When is_meta_draw is true (internal meta draws: clear, blit, query),
 * INV_TC | INV_SH | INV_VC are preserved only for pure read-cache
 * invalidation barriers (SYNC_PFP_TO_ME + INV_* only). Barriers that also
 * include producer-side flush actions are consumed once.
 */
void terakan_before_hw_draw(struct terakan_gfx_command_writer * command_writer,
                             bool is_meta_draw);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_DRAW_H */
