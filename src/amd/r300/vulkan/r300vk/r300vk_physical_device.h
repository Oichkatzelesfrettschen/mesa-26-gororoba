/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PHYSICAL_DEVICE_H
#define R300VK_PHYSICAL_DEVICE_H

#include "vk_physical_device.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _drmDevice;
struct pipe_screen;
struct r300vk_instance;
struct radeon_winsys;

struct r300vk_physical_device {
   struct vk_physical_device vk;

   /* Identity from the DRM probe.  pci_device_id is R300VK_PCI_DEVICE_ID_RS482
    * or R300VK_PCI_DEVICE_ID_RS485. */
   uint32_t pci_device_id;
   uint32_t pci_vendor_id;

   /* Render-node fd kept open to validate the DRM device.  Released
    * when the physical device is destroyed. */
   int render_node_fd;

   /* Experimental hybrid-compute queue gate, read once from the environment
    * at physical-device creation.  Caching it keeps the advertised queue
    * flags consistent across queries even if the environment changes
    * mid-process. */
   bool hybrid_compute_enabled;

#ifdef R300VK_GALLIUM_BACKEND
   /* Gallium r300g oracle used for physical-device format queries.
    * The screen owns its radeon_winsys reference and is destroyed before
    * the render-node fd is closed. */
   struct radeon_winsys *rws;
   struct pipe_screen *screen;
#endif

   /* Timeline sync type emulated on top of the binary cpu_sync.  radeon has no
    * DRM_CAP_SYNCOBJ (see below), so there is no hardware timeline; the binary
    * sync is signalled only after the queue's GPU fence retires, so a timeline
    * point reached through this emulation reflects real GPU completion. */
   struct vk_sync_timeline_type timeline_sync_type;

   /* NULL-terminated sync type table assigned to vk.supported_sync_types.
    * r300vk uses a CPU-side binary sync; the radeon DRM driver does not
    * support DRM_CAP_SYNCOBJ (confirmed on kernel 6.18 radeon driver).  Slot 0
    * is the binary sync, slot 1 the timeline emulation above. */
   const struct vk_sync_type *sync_types[3];
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
