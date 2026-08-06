/* SPDX-License-Identifier: MIT */

/* The public-surface command tables the sweeps measure against.  The bodies
 * are generated from vk.xml by r3v_native_entrypoint_audit.py, so the surface
 * the sweeps check and the surface the policy audits come from one parse.
 */

#ifndef R3V_NATIVE_SURFACE_H
#define R3V_NATIVE_SURFACE_H

#include "util/macros.h"

#include <stdint.h>

enum r3v_surface_scope {
   R3V_SCOPE_GLOBAL,
   R3V_SCOPE_INSTANCE,
   R3V_SCOPE_PHYSICAL_DEVICE,
   R3V_SCOPE_DEVICE,
};

struct r3v_surface_command {
   const char *name;
   enum r3v_surface_scope scope;
};

/* The Vulkan 1.0 command set: a 1.0 instance resolves every one of these. */
extern const struct r3v_surface_command r3v_surface_core10[];
extern const uint32_t r3v_surface_core10_count;

/* Commands a core version above 1.0 introduces, promoted aliases included.
 * A 1.0 instance resolves none of them.
 */
extern const struct r3v_surface_command r3v_surface_higher_core[];
extern const uint32_t r3v_surface_higher_core_count;

/* Promoted KHR/EXT spellings of commands a later core version carries.  Each
 * belongs to the extension that introduced it, so a device whose extension
 * table is empty resolves none of them.
 */
extern const struct r3v_surface_command r3v_surface_alias[];
extern const uint32_t r3v_surface_alias_count;

/* Commands owned by the surface and swapchain extensions the native ICD
 * leaves unenabled.  A device with an empty extension table resolves none.
 */
extern const struct r3v_surface_command r3v_surface_closed_extension[];
extern const uint32_t r3v_surface_closed_extension_count;

#endif /* R3V_NATIVE_SURFACE_H */
