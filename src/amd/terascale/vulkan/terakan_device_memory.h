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

#ifndef TERAKAN_DEVICE_MEMORY_H
#define TERAKAN_DEVICE_MEMORY_H

#include "terakan_bo.h"

#include "vk_device_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

struct terakan_dmabuf_carrier;

struct terakan_device_memory {
   struct vk_device_memory vk;

   struct terakan_bo * bo;

   /* Optional non-owning carrier pointer.  Set when a dma-buf
    * import path attaches a Palm external-sync carrier to this
    * device-memory's BO via terakan_dmabuf_carrier_attach().
    * terakan_FreeMemory() destroys the carrier (which clears the
    * BO's atomic carrier pointer) BEFORE freeing the BO so the
    * BO's owning struct outlives its carrier. */
   struct terakan_dmabuf_carrier * carrier;
};

VK_DEFINE_NONDISP_HANDLE_CASTS(terakan_device_memory, vk.base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)

#ifdef __cplusplus
}
#endif

#endif /* TERAKAN_DEVICE_MEMORY_H */
