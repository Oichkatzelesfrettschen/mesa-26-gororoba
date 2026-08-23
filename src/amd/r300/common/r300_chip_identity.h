/*
 * SPDX-License-Identifier: MIT
 *
 * R300-class chip identity: the PCI-keyed family and die-class table.
 */

#ifndef R300_CHIP_IDENTITY_H
#define R300_CHIP_IDENTITY_H

#include "amd_family.h"

#include <stdbool.h>
#include <stdint.h>

#define R300_PCI_VENDOR_ATI 0x1002

/* RS482 (Radeon Xpress 200M, 1002:5974): the RS480-family IGP the native
 * Vulkan implementation targets.  include/pci_ids/r300_pci_ids.h is the id
 * source; this spelling exists so identity comparisons bind to one constant.
 */
#define R300_PCI_DEVICE_RS482 0x5974

/* Die classes group families by the silicon blocks capability decisions
 * key on.  The RS400-class IGPs (RS400, RC410, RS480/RS482/RS485) share an
 * R300-generation raster core with the vertex transform engine absent; the
 * RS600-class IGPs pair an R400-generation core with the same absence.
 */
enum r300_die_class {
   R300_DIE_CLASS_R300,
   R300_DIE_CLASS_RV350,
   R300_DIE_CLASS_R400,
   R300_DIE_CLASS_RS400_IGP,
   R300_DIE_CLASS_RS600_IGP,
   R300_DIE_CLASS_R500,
};

struct r300_chip_identity {
   uint16_t pci_device;
   enum radeon_family family;
   enum r300_die_class die_class;
};

/* Resolve a PCI vendor/device pair against the r300_pci_ids table.  An id
 * outside the table, a vendor other than ATI, or a null output refuses, so
 * an unknown chip admits nothing.
 */
bool r300_chip_identity_lookup(uint16_t pci_vendor, uint16_t pci_device,
                               struct r300_chip_identity *identity);

#endif /* R300_CHIP_IDENTITY_H */
