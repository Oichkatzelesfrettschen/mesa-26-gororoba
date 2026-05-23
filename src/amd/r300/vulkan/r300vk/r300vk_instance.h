/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_INSTANCE_H
#define R300VK_INSTANCE_H

#include "vk_instance.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
   /* Print loader-boundary diagnostics: instance create, ICD ABI
    * negotiation result, and physical-device enumeration outcome.
    * Activate with R300VK_DEBUG=startup. */
   R300VK_DEBUG_STARTUP = (uint64_t)1 << 0
};

struct r300vk_instance {
   struct vk_instance vk;

   uint64_t debug_flags;
};

VK_DEFINE_HANDLE_CASTS(r300vk_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)

VkResult r300vk_instance_init(struct r300vk_instance *instance,
                              const VkInstanceCreateInfo *create_info,
                              const VkAllocationCallbacks *allocator);

void r300vk_instance_finish(struct r300vk_instance *instance);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_INSTANCE_H */
