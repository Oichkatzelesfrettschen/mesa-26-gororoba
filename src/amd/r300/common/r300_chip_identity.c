/*
 * SPDX-License-Identifier: MIT
 */

#include "r300_chip_identity.h"

#include "util/macros.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

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

const struct r300_platform_identity r300_platform_vostro1000 = {
   .pci_vendor = R300_PCI_VENDOR_ATI,
   .pci_device = R300_PCI_DEVICE_RS48X_5974,
   .subsystem_vendor = 0x1028,
   .subsystem_device = 0x022a,
   .dmi_product_name = "Vostro 1000",
   .firmware_chip_name = "RS485/M",
   .firmware_product_name = "ATI Radeon Xpress 1150",
   .die_name = "RS485",
   .part_name = "RS485M",
   .product_name = "Radeon Xpress 1150",
   .historical_alias = "rs482",
   .platform_id = R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M,
   .runtime_match_basis = R300_PLATFORM_MATCH_PCI_SUBSYSTEM_DMI,
   .identity_evidence = R300_PLATFORM_EVIDENCE_OPTION_ROM_STRING |
                        R300_PLATFORM_EVIDENCE_PRODUCT_STRING |
                        R300_PLATFORM_EVIDENCE_RETAINED_ROM_DIGEST,
   .specimen_facts = &r300_vostro1000_rs485m_specimen_facts,
};

static const struct r300_platform_identity *const r300_platform_rows[] = {
   &r300_platform_vostro1000,
};

/* DMI product names carry firmware padding ("Vostro   1000 "), so the
 * comparison folds every blank run to one and ignores the edges. */
static bool
dmi_product_name_matches(const char *expected, const char *actual)
{
   const char *e = expected;
   const char *a = actual;
   while (*a == ' ')
      a++;
   while (*e != '\0') {
      if (*e == ' ') {
         if (*a != ' ')
            return false;
         while (*e == ' ')
            e++;
         while (*a == ' ')
            a++;
         continue;
      }
      if (*e != *a)
         return false;
      e++;
      a++;
   }
   while (*a == ' ')
      a++;
   return *a == '\0';
}

bool
r300_platform_identity_lookup(uint16_t pci_vendor, uint16_t pci_device,
                              uint16_t subsystem_vendor,
                              uint16_t subsystem_device,
                              const char *dmi_product_name,
                              const struct r300_platform_identity **out)
{
   if (dmi_product_name == NULL || out == NULL)
      return false;
   for (size_t i = 0; i < ARRAY_SIZE(r300_platform_rows); i++) {
      const struct r300_platform_identity *row = r300_platform_rows[i];
      if (row->pci_vendor == pci_vendor && row->pci_device == pci_device &&
          row->subsystem_vendor == subsystem_vendor &&
          row->subsystem_device == subsystem_device &&
          dmi_product_name_matches(row->dmi_product_name, dmi_product_name)) {
         *out = row;
         return true;
      }
   }
   return false;
}

/* Field values cross-checked against their macro homes by the
 * r300-chip-identity test: the FP24 ceiling against r300_grid_fold.h and
 * the cache registers against r300_reg.h.
 */
/* Measurements made on the attended board: its identity as the device
 * reports it, and the cache publication registers as it leaves them at
 * rest.  A second board carrying 1002:5974 is a different specimen and
 * reaches none of this. */
const struct r300_specimen_facts r300_vostro1000_rs485m_specimen_facts = {
   .platform_id = R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M,
   .pci_revision = 0x00,
   .subsystem_vendor = 0x1028,
   .subsystem_device = 0x022a,
   .dstcache_ctlstat_at_rest = 0x00000002,
   .zcache_ctlstat_at_rest = 0x00000001,
};

const struct r300_family_facts r300_rs4xx_igp_family_facts = {
   .vertex_engine_absent = true,
   .uma_framebuffer_from_nb_tom = true,
   .gart_requires_snoop_disable = true,
   .fp24_exact_int_ceiling = 1u << 17,
   .dp4_limb_ceiling_bits = 7,
   .us_program_depth = 64,
   .sampler_dimension_max = 2048,
   .render_span_max = 2560,
   .tiled_row_max = 2048,
   .point_size_max = 64,
   .hw_line_width_max = 8,
   .dstcache_ctlstat_reg = 0x4e4c,
   .zcache_ctlstat_reg = 0x4f18,
   .video_decode_engine_absent = true,
   .claim_basis = R300_FAMILY_CLAIM_SOURCE_DERIVED,
   .observed_on = R300_PLATFORM_ID_DELL_VOSTRO1000_RS485M,
};

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
      identity->family_facts =
         row->family == CHIP_RS480 ? &r300_rs4xx_igp_family_facts : NULL;
      return true;
   }
   return false;
}


enum r300_platform_id
r300_platform_id_resolve(uint16_t pci_vendor, uint16_t pci_device,
                         uint16_t subsystem_vendor, uint16_t subsystem_device,
                         const char *dmi_product_name)
{
   const struct r300_platform_identity *row = NULL;
   if (!r300_platform_identity_lookup(pci_vendor, pci_device,
                                      subsystem_vendor, subsystem_device,
                                      dmi_product_name, &row))
      return R300_PLATFORM_ID_NONE;
   return row->platform_id;
}
