/*
 * SPDX-License-Identifier: MIT
 */

#ifndef TERAKAN_NIR_WIDE_PHI_H
#define TERAKAN_NIR_WIDE_PHI_H

#include "nir.h"

bool terakan_nir_wide_phi_auto_segment_enabled(void);

nir_def * terakan_nir_wide_phi_selector(nir_def * root_phi_def, unsigned case_count);

#endif /* TERAKAN_NIR_WIDE_PHI_H */
