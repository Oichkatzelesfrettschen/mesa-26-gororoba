/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_chip_identity.h"

#include "util/macros.h"

#include <assert.h>
#include <stddef.h>

/* One row per r300_pci_ids entry; the CHIPSET expansion keeps this table
 * and the id header in lockstep, so a new PCI id lands here without a
 * hand-written row.
 */
struct r300_chip_identity_row {
   uint16_t pci_device;
   enum radeon_family family;
};

static const struct r300_chip_identity_row r300_chip_identity_rows[] = {
#define CHIPSET(pci_id, name, chipfamily) \
   { pci_id, CHIP_##chipfamily },
#include "pci_ids/r300_pci_ids.h"
#undef CHIPSET
};

static bool
r300_die_class_from_family(enum radeon_family family,
                           enum r300_die_class *die_class)
{
   switch (family) {
   case CHIP_R300:
   case CHIP_R350:
      *die_class = R300_DIE_CLASS_R300;
      return true;
   case CHIP_RV350:
   case CHIP_RV370:
   case CHIP_RV380:
      *die_class = R300_DIE_CLASS_RV350;
      return true;
   case CHIP_RS400:
   case CHIP_RC410:
   case CHIP_RS480:
      *die_class = R300_DIE_CLASS_RS400_IGP;
      return true;
   case CHIP_R420:
   case CHIP_R423:
   case CHIP_R430:
   case CHIP_R480:
   case CHIP_R481:
   case CHIP_RV410:
      *die_class = R300_DIE_CLASS_R400;
      return true;
   case CHIP_RS600:
   case CHIP_RS690:
   case CHIP_RS740:
      *die_class = R300_DIE_CLASS_RS600_IGP;
      return true;
   case CHIP_RV515:
   case CHIP_R520:
   case CHIP_RV530:
   case CHIP_R580:
   case CHIP_RV560:
   case CHIP_RV570:
      *die_class = R300_DIE_CLASS_R500;
      return true;
   default:
      return false;
   }
}

bool
r300_chip_identity_lookup(uint16_t pci_vendor, uint16_t pci_device,
                          struct r300_chip_identity *identity)
{
   if (pci_vendor != R300_PCI_VENDOR_ATI || identity == NULL)
      return false;

   for (size_t i = 0; i < ARRAY_SIZE(r300_chip_identity_rows); i++) {
      const struct r300_chip_identity_row *row = &r300_chip_identity_rows[i];
      if (row->pci_device != pci_device)
         continue;
      enum r300_die_class die_class;
      if (!r300_die_class_from_family(row->family, &die_class))
         return false;
      identity->pci_device = row->pci_device;
      identity->family = row->family;
      identity->die_class = die_class;
      return true;
   }
   return false;
}
