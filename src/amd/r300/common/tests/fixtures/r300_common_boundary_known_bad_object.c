/*
 * SPDX-License-Identifier: MIT
 */

extern void nir_r300_common_boundary_known_bad(void);

/* The fixture compiles under the tree's -Werror=missing-prototypes; its
 * one purpose is the undefined nir_ reference the boundary audit refuses.
 */
void r300_common_boundary_known_bad_object(void);

void
r300_common_boundary_known_bad_object(void)
{
   nir_r300_common_boundary_known_bad();
}
