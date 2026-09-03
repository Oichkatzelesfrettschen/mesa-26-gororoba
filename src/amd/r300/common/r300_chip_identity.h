/*
 * SPDX-License-Identifier: MIT
 *
 * R300-class chip identity: the PCI-keyed family and die-class table.
 */

#ifndef R300_CHIP_IDENTITY_H
#define R300_CHIP_IDENTITY_H

#include "amd/common/amd_family.h"

#include <stdbool.h>
#include <stdint.h>

#define R300_PCI_VENDOR_ATI 0x1002

/* 1002:5974 is one PCI device id shared by RS482 and RS485 (the PCI ID
 * repository names it "RS482/RS485 [Radeon Xpress 1100/1150]"), so the id
 * names the RS48x die class and never one of the two products.
 * include/pci_ids/r300_pci_ids.h is the id source; this spelling exists so
 * identity comparisons bind to one constant.
 */
#define R300_PCI_DEVICE_RS48X_5974 0x5974

/* 1002:5975 is RS482M (Mobility Radeon Xpress 200) in the same RS480
 * family row set.
 */
#define R300_PCI_DEVICE_RS482M_5975 0x5975

/* The product a 1002:5974 specimen carries resolves through the platform:
 * the board's PCI subsystem id and DMI product name select the row.
 *
 * 1002:5974 is shared with the desktop RS482 (Radeon Xpress 1100), so the
 * PCI id alone does not name the part.  The board's video BIOS does: the
 * option-ROM string table of the Dell Vostro 1000 (subsystem 1028:022a,
 * DMI "Vostro 1000") carries "RS485/M BR#26605" and "ATI Radeon Xpress
 * 1150", which is the mobile RS485.  That string is the chip's own name
 * for itself and outranks the shared id, so the attended target is RS485M
 * and RS482 names a part this platform does not carry.
 *
 * The kernel enumerates the whole RS400-class family as CHIP_RS480 and
 * the GL renderer string is "ATI RS480"; both are owner-spelled and stay
 * verbatim.
 */
/* A shared PCI id names a die class and not a part, so a platform row
 * records which observations agree on it: the strongest identity is the
 * agreement among the id, the board's subsystem id, its DMI product, and
 * the option-ROM strings. */
enum r300_platform_identity_basis {
   R300_PLATFORM_IDENTITY_BASIS_PCI_ONLY = 0,
   R300_PLATFORM_IDENTITY_BASIS_PCI_SUBSYSTEM_DMI,
   R300_PLATFORM_IDENTITY_BASIS_FIRMWARE_STRING,
   R300_PLATFORM_IDENTITY_BASIS_PCI_SUBSYSTEM_DMI_AND_FIRMWARE,
};

struct r300_platform_identity {
   uint16_t pci_vendor;
   uint16_t pci_device;
   uint16_t subsystem_vendor;
   uint16_t subsystem_device;
   /* DMI product name as the firmware spells it, trailing blanks
    * excluded. */
   const char *dmi_product_name;
   /* The chip name the board's own video BIOS carries, verbatim from the
    * option-ROM string table.  ATI writes one name for the die and its
    * mobile variant together, so this string is never normalized. */
   const char *firmware_chip_name;
   /* The marketing product the same ROM names, verbatim. */
   const char *firmware_product_name;
   /* The die the ROM names, which is the scope a register file, a
    * bitfield table, and an ISA rule hold over: those facts are shared by
    * the desktop and mobile parts cut from it. */
   const char *die_name;
   /* The part this board carries, which is the scope a capture, a
    * receipt, and an evidence bundle hold over: a measurement is made on
    * one part, and this platform's is the mobile one. */
   const char *part_name;
   /* The marketing product name in the spelling durable names use. */
   const char *product_name;
   /* The compatibility token older externally retained artifacts carry.
    * It is not the platform's current part identity. */
   const char *historical_alias;
   /* Which observations agree on the row.  A platform resolved from the
    * shared PCI id alone is not resolved. */
   enum r300_platform_identity_basis identity_basis;
};

extern const struct r300_platform_identity r300_platform_vostro1000;

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


/* Measured facts for the RS480 die class (RS480/RS482/RS485, one shared
 * register database across 1002:5954/5955/5974/5975).  Every row is exact
 * RS482 (1002:5974) evidence unless its comment widens the claim; the
 * commit history and the steinmarder-r300 findings named per field carry
 * the provenance.  The die classes other than RS480 carry no facts row,
 * so the one struct is the finite map and a separate quirk header would
 * duplicate it.
 */
struct r300_die_facts {
   /* The PVS/SE_TCL vertex transform engine is absent from the die, so
    * every vertex route is TCL bypass over produced window-space records
    * (finding 2026-05-31-rs482-has-tcl-gate-and-hardware-unit-map.md).
    */
   bool vertex_engine_absent;

   /* The UMA framebuffer carveout is sized from NB_TOM, and the GART
    * route is a noncoherent HyperTransport link that is ready only with
    * snooping disabled, so host reads of device output take an explicit
    * cache sync (findings 2026-06-03-rs482-sideport-flag-uma-framebuffer
    * .md; the host-read invalidate rule remains hypothesized).
    */
   bool uma_framebuffer_from_nb_tom;
   bool gart_requires_snoop_disable;

   /* FP24 (s1e7m16) fragment ALU: exact integer window and the DP4
    * multi-limb multiply limb-width ceiling, with a 64-instruction US
    * program depth (findings 2026-05-31-rs482-floating-point-engine-
    * synthesis.md, 2026-05-29-rs482-dp4-vectorized-multilimb-multiply-
    * limb-width-ceiling.md).
    */
   uint32_t fp24_exact_int_ceiling;
   uint32_t dp4_limb_ceiling_bits;
   uint32_t us_program_depth;

   /* Dimension ceilings: sampler 2048 (R300_TX_HEIGHTMASK), render span
    * 2560, tiled row 2048; larger surfaces are composed, not native
    * (finding 2026-07-18-r2vb-auto-single-canary-producer-slot-row-
    * ceiling.md; the sampler ceiling is the R300_TX_HEIGHTMASK encoding
    * measured through the tile-stitch sampler probe).  Point size tops
    * at 64 and the hardware line-width
    * range ends at 8 (finding 2026-06-20-rs482-point-line-size-limit-
    * is-not-the-colorbuffer-dimension.md).
    */
   uint32_t sampler_dimension_max;
   uint32_t render_span_max;
   uint32_t tiled_row_max;
   uint32_t point_size_max;
   uint32_t hw_line_width_max;

   /* Cache publication registers written in the first-draw contract and
    * every R2VB publication tail; at rest they read 0x00000002 and
    * 0x00000001 (findings 2026-08-19-rs480-rb3d-dstcache-ctlstat-armed-
    * debut.md, 2026-08-20-rs480-zb-zcache-ctlstat-armed-debut.md,
    * silicon).
    */
   uint32_t dstcache_ctlstat_reg;
   uint32_t zcache_ctlstat_reg;
   uint32_t dstcache_ctlstat_at_rest;
   uint32_t zcache_ctlstat_at_rest;

   /* No video decode engine; texture sampling covers packed 4:2:2 only,
    * so planar 4:2:0 is absent (docs/hardware/r3v-implementation-
    * boundaries.md).
    */
   bool video_decode_engine_absent;

   /* The attended 1002:5974 specimen: PCI revision and subsystem read
    * from the target (lspci/setpci, revision 0x00, subsystem 1028:022a).
    * Specimen identity, not a die-class key; r300_platform_identity
    * resolves the product.
    */
   uint8_t specimen_pci_revision;
   uint16_t specimen_subsystem_vendor;
   uint16_t specimen_subsystem_device;
};

struct r300_chip_identity {
   uint16_t pci_device;
   enum radeon_family family;
   enum r300_die_class die_class;
   /* Non-null only where measured facts exist for the die; the RS480
    * family rows share the r300_rs480_die_facts record.
    */
   const struct r300_die_facts *die_facts;
};

/* The RS480-class facts record, measured on the 1002:5974 Vostro 1000
 * specimen. */
extern const struct r300_die_facts r300_rs480_die_facts;

/* Resolve a PCI vendor/device pair against the r300_pci_ids table.  An id
 * outside the table, a vendor other than ATI, or a null output refuses, so
 * an unknown chip admits nothing.
 */
bool r300_chip_identity_lookup(uint16_t pci_vendor, uint16_t pci_device,
                               struct r300_chip_identity *identity);

/* Resolve a platform row from the PCI tuple and the DMI product name.
 * Every field must match its row; a tuple outside the table, a null
 * name, or a null output refuses, so an unmatched board names no
 * product.
 */
bool r300_platform_identity_lookup(uint16_t pci_vendor, uint16_t pci_device,
                                   uint16_t subsystem_vendor,
                                   uint16_t subsystem_device,
                                   const char *dmi_product_name,
                                   const struct r300_platform_identity **out);

#endif /* R300_CHIP_IDENTITY_H */
