/*
 * SPDX-License-Identifier: MIT
 */

#ifndef R3V_PHYSICAL_DEVICE_H
#define R3V_PHYSICAL_DEVICE_H

#include "vk_physical_device.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "wsi_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _drmDevice;
struct r3v_instance;

struct r3v_physical_device {
   struct vk_physical_device vk;

   /* Identity from the DRM probe.  pci_device_id is R3V_PCI_DEVICE_ID_RS48X
    * or R3V_PCI_DEVICE_ID_RS482M. */
   uint32_t pci_device_id;
   uint32_t pci_vendor_id;

   /* Render-node fd kept open to validate the DRM device.  Released
    * when the physical device is destroyed. */
   int render_node_fd;

   /* DRM node device IDs for VK_EXT_physical_device_drm.  Zink's
    * display-device selection (zink_get_display_device) matches the EGL DRM
    * fd's render major/minor against drmRenderMajor/drmRenderMinor, so without
    * these the pdev is rejected before any feature check. */
   bool    has_primary_node;
   int64_t primary_node_major;
   int64_t primary_node_minor;
   int64_t render_node_major;
   int64_t render_node_minor;

   /* The verb ledger's compute-queue claim, evaluated once at
    * physical-device creation so the advertised queue flags stay
    * consistent across queries even if the environment changes
    * mid-process. */
   bool compute_queue_claimed;


   /* Timeline sync type emulated on top of the binary cpu_sync.  radeon has no
    * DRM_CAP_SYNCOBJ (see below), so there is no hardware timeline; the binary
    * sync is signalled only after the queue's GPU fence retires, so a timeline
    * point reached through this emulation reflects real GPU completion. */
   struct vk_sync_timeline_type timeline_sync_type;

   /* NULL-terminated sync type table assigned to vk.supported_sync_types.
    * r3v uses a CPU-side binary sync; the radeon DRM driver does not
    * support DRM_CAP_SYNCOBJ (confirmed on kernel 6.18 radeon driver).  Slot 0
    * is the binary sync, slot 1 the timeline emulation above. */
   const struct vk_sync_type *sync_types[3];

   /* r3v_init_wsi routes this device through the render-node fd by
    * default, the DRM/DRI3 path; R3V_WSI_SW=1 switches it to the Mesa
    * common WSI software mode (the lavapipe pattern), CPU-reachable
    * swapchain images presented through the xcb-shm path with no
    * dma-buf, DRM modifier, or external-memory support required of the
    * radeon winsys.  vk.wsi_device points here after r3v_init_wsi. */
   struct wsi_device wsi_device;
};

VK_DEFINE_HANDLE_CASTS(r3v_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)

VkResult r3v_physical_device_try_create_for_drm(struct vk_instance *instance,
                                                   struct _drmDevice *device,
                                                   struct vk_physical_device **out);

void r3v_physical_device_destroy(struct vk_physical_device *device);

/* The format-feature grant switch: r3v_GetPhysicalDeviceFormatProperties2
 * and r3v_CreateBufferView (r3v_native_object.c) both read this table, so
 * a format's queried bufferFeatures and its buffer-view admission stay
 * derived from the same source. */
void r3v_get_format_properties(const struct r3v_physical_device *device,
                                VkFormat vk_format,
                                VkFormatProperties3 *properties);

#ifdef __cplusplus
}
#endif

#endif /* R3V_PHYSICAL_DEVICE_H */
