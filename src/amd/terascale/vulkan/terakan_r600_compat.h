/* Compatibility defines for r600 register values
 * that exist in Mesa 25.2 eg_sq.h but were removed/moved in Mesa 26.
 * These are hardware register constants that do not change. */

#ifndef TERAKAN_R600_COMPAT_H
#define TERAKAN_R600_COMPAT_H

#include "amd/terascale/common/terascale_eg_sq.h"
#include "amd/terascale/common/terascale_r600_sq.h"
#include "amd/terascale/common/terascale_evergreend.h"

/* Bank swizzle constants */
#ifndef SQ_ALU_VEC_012
#define SQ_ALU_VEC_012 0x00
#endif
#ifndef SQ_ALU_SCL_210
#define SQ_ALU_SCL_210 0x00
#endif

/* ALU source special values */
#ifndef V_SQ_ALU_SRC_PV
#define V_SQ_ALU_SRC_PV 0x000000FE
#endif
#ifndef V_SQ_ALU_SRC_PS
#define V_SQ_ALU_SRC_PS 0x000000FF
#endif

/* VTX fetch type */
#ifndef SQ_VTX_FETCH_NO_INDEX_OFFSET
#define SQ_VTX_FETCH_NO_INDEX_OFFSET 2
#endif

/* Polygon mode */
#ifndef V_028814_X_DRAW_POINTS
#define V_028814_X_DRAW_POINTS 0
#endif
#ifndef V_028814_X_DRAW_LINES
#define V_028814_X_DRAW_LINES 1
#endif
#ifndef V_028814_X_DRAW_TRIANGLES
#define V_028814_X_DRAW_TRIANGLES 2
#endif
#ifndef V_028814_X_DUAL_MODE
#define V_028814_X_DUAL_MODE 0
#endif
#ifndef V_028814_X_DISABLE_POLY_MODE
#define V_028814_X_DISABLE_POLY_MODE 0
#endif

/* RAT instruction */
#ifndef V_RAT_INST_STORE_TYPED
#define V_RAT_INST_STORE_TYPED 1
#endif

#endif /* TERAKAN_R600_COMPAT_H */
