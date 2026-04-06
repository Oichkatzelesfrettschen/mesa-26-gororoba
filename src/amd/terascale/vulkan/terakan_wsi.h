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

#ifndef TERAKAN_WSI_H
#define TERAKAN_WSI_H

#include "terakan_physical_device.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_bo;
struct terakan_wsi_hw_wait;

uint32_t terakan_wsi_hw_wait_load_value(struct terakan_wsi_hw_wait const * hw_wait);
void terakan_wsi_hw_wait_ref(struct terakan_wsi_hw_wait * hw_wait);
void terakan_wsi_hw_wait_unref(struct terakan_wsi_hw_wait * hw_wait);

void terakan_wsi_finish(struct terakan_physical_device * physical_device);

VkResult terakan_wsi_init(struct terakan_physical_device * physical_device);

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_WSI_H */
