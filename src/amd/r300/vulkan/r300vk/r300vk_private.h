/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#ifndef R300VK_PRIVATE_H
#define R300VK_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define R300VK_VENDOR_ID_ATI 0x1002

/* RS482 (Radeon Xpress 200M, IGP) and RS485 (Radeon Xpress 1100/1150,
 * mobile IGP).  Both report GL_RENDERER="ATI RS480"; both route the
 * vertex stage through Gallium Draw SW TCL because num_vert_fpus == 0
 * for the RS480 family in r300_parse_chipset(). */
#define R300VK_PCI_DEVICE_ID_RS482 0x5974
#define R300VK_PCI_DEVICE_ID_RS485 0x5975

#define R300VK_API_VERSION VK_MAKE_API_VERSION(0, 1, 0, VK_HEADER_VERSION)

static inline bool
r300vk_pci_device_id_is_supported(uint32_t pci_device_id)
{
   switch (pci_device_id) {
   case R300VK_PCI_DEVICE_ID_RS482:
   case R300VK_PCI_DEVICE_ID_RS485:
      return true;
   default:
      return false;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* R300VK_PRIVATE_H */
