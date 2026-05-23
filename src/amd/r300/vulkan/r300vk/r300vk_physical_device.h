/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PHYSICAL_DEVICE_H
#define R300VK_PHYSICAL_DEVICE_H

#include "vk_physical_device.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _drmDevice;
struct r300vk_instance;

struct r300vk_physical_device {
   struct vk_physical_device vk;

   /* Identity from the DRM probe.  pci_device_id is R300VK_PCI_DEVICE_ID_RS482
    * or R300VK_PCI_DEVICE_ID_RS485. */
   uint32_t pci_device_id;
   uint32_t pci_vendor_id;

   /* Render-node fd kept open to validate the DRM device.  Released
    * when the physical device is destroyed. */
   int render_node_fd;
};

VK_DEFINE_HANDLE_CASTS(r300vk_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

VkResult r300vk_physical_device_try_create_for_drm(struct vk_instance *instance,
                                                   struct _drmDevice *device,
                                                   struct vk_physical_device **out);

void r300vk_physical_device_destroy(struct vk_physical_device *device);

#ifdef __cplusplus
}
#endif

#endif /* R300VK_PHYSICAL_DEVICE_H */
