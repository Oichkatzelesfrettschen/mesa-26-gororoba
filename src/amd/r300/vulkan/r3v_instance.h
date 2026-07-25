/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_INSTANCE_H
#define R3V_INSTANCE_H

#include "vk_instance.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
   /* Print loader-boundary diagnostics: instance create, ICD ABI
    * negotiation result, and physical-device enumeration outcome.
    * Activate with R3V_DEBUG=startup. */
   R3V_DEBUG_STARTUP = (uint64_t)1 << 0
};

struct r3v_instance {
   struct vk_instance vk;

   uint64_t debug_flags;
};

VK_DEFINE_HANDLE_CASTS(r3v_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)

VkResult r3v_instance_init(struct r3v_instance *instance,
                              const VkInstanceCreateInfo *create_info,
                              const VkAllocationCallbacks *allocator);

void r3v_instance_finish(struct r3v_instance *instance);

#ifdef __cplusplus
}
#endif

#endif /* R3V_INSTANCE_H */
