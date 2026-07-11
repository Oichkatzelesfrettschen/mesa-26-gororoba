/*
 * Copyright 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2010 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

/* r300_render: Vertex and index buffer primitive emission. Contains both
 * HW TCL fastpath rendering, and SW TCL Draw-assisted rendering. */

#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"

#include "util/u_inlines.h"
#include "util/u_framebuffer.h"
#include "util/u_sampler.h"

#include "util/u_endian.h"
#include "util/format/u_format.h"
#include "util/u_draw.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_prim.h"

#include "r300_cs.h"
#include "r300_context.h"
#include "r300_r2vb.h"
#include "r300_screen_buffer.h"
#include "r300_emit.h"
#include "r300_fs.h"
#include "r300_reg.h"
#include "r300_vs.h"

#include <inttypes.h>
#include <limits.h>

#define IMMD_DWORDS 32

static uint32_t r300_translate_primitive(unsigned prim)
{
    static const int prim_conv[] = {
        R300_VAP_VF_CNTL__PRIM_POINTS,
        R300_VAP_VF_CNTL__PRIM_LINES,
        R300_VAP_VF_CNTL__PRIM_LINE_LOOP,
        R300_VAP_VF_CNTL__PRIM_LINE_STRIP,
        R300_VAP_VF_CNTL__PRIM_TRIANGLES,
        R300_VAP_VF_CNTL__PRIM_TRIANGLE_STRIP,
        R300_VAP_VF_CNTL__PRIM_TRIANGLE_FAN,
        R300_VAP_VF_CNTL__PRIM_QUADS,
        R300_VAP_VF_CNTL__PRIM_QUAD_STRIP,
        R300_VAP_VF_CNTL__PRIM_POLYGON,
        -1,
        -1,
        -1,
        -1
    };
    unsigned hwprim = prim_conv[prim];

    assert(hwprim != -1);
    return hwprim;
}

static uint32_t r300_provoking_vertex_fixes(struct r300_context *r300,
                                            unsigned mode)
{
    struct r300_rs_state* rs = (struct r300_rs_state*)r300->rs_state.state;
    uint32_t color_control = rs->color_control;

    /* By default (see r300_state.c:r300_create_rs_state) color_control is
     * initialized to provoking the first vertex.
     *
     * Triangle fans must be reduced to the second vertex, not the first, in
     * Gallium flatshade-first mode, as per the GL spec.
     * (http://www.opengl.org/registry/specs/ARB/provoking_vertex.txt)
     *
     * Quads never provoke correctly in flatshade-first mode. The first
     * vertex is never considered as provoking, so only the second, third,
     * and fourth vertices can be selected, and both "third" and "last" modes
     * select the fourth vertex. This is probably due to D3D lacking quads.
     *
     * Similarly, polygons reduce to the first, not the last, vertex, when in
     * "last" mode, and all other modes start from the second vertex.
     *
     * ~ C.
     */

    if (rs->rs.flatshade_first) {
        switch (mode) {
            case MESA_PRIM_TRIANGLE_FAN:
                color_control |= R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_SECOND;
                break;
            case MESA_PRIM_QUADS:
            case MESA_PRIM_QUAD_STRIP:
            case MESA_PRIM_POLYGON:
                color_control |= R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_LAST;
                break;
            default:
                color_control |= R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_FIRST;
                break;
        }
    } else {
        color_control |= R300_GA_COLOR_CONTROL_PROVOKING_VERTEX_LAST;
    }

    return color_control;
}

void r500_emit_index_bias(struct r300_context *r300, int index_bias)
{
    CS_LOCALS(r300);

    BEGIN_CS(2);
    OUT_CS_REG(R500_VAP_INDEX_OFFSET,
               (index_bias & 0xFFFFFF) | (index_bias < 0 ? 1<<24 : 0));
    END_CS;
}

static void r300_emit_draw_init(struct r300_context *r300, unsigned mode,
                                unsigned max_index)
{
    CS_LOCALS(r300);

    assert(max_index < (1 << 24));

    BEGIN_CS(5);
    OUT_CS_REG(R300_GA_COLOR_CONTROL,
            r300_provoking_vertex_fixes(r300, mode));
    OUT_CS_REG_SEQ(R300_VAP_VF_MAX_VTX_INDX, 2);
    OUT_CS(max_index);
    OUT_CS(0);
    END_CS;
}

/* This function splits the index bias value into two parts:
 * - buffer_offset: the value that can be safely added to buffer offsets
 *   in r300_emit_vertex_arrays (it must yield a positive offset when added to
 *   a vertex buffer offset)
 * - index_offset: the value that must be manually subtracted from indices
 *   in an index buffer to achieve negative offsets. */
static void r300_split_index_bias(struct r300_context *r300, int index_bias,
                                  int *buffer_offset, int *index_offset)
{
    struct pipe_vertex_buffer *vb, *vbufs = r300->vertex_buffer;
    struct pipe_vertex_element *velem = r300->velems->velem;
    unsigned i, size;
    int max_neg_bias;

    if (index_bias < 0) {
        /* See how large index bias we may subtract. We must be careful
         * here because negative buffer offsets are not allowed
         * by the DRM API. */
        max_neg_bias = INT_MAX;
        for (i = 0; i < r300->velems->count; i++) {
            vb = &vbufs[velem[i].vertex_buffer_index];
            size = (vb->buffer_offset + velem[i].src_offset) / velem[i].src_stride;
            max_neg_bias = MIN2(max_neg_bias, size);
        }

        /* Now set the minimum allowed value. */
        *buffer_offset = MAX2(-max_neg_bias, index_bias);
    } else {
        /* A positive index bias is OK. */
        *buffer_offset = index_bias;
    }

    *index_offset = index_bias - *buffer_offset;
}

enum r300_prepare_flags {
    PREP_EMIT_STATES    = (1 << 0), /* call emit_dirty_state and friends? */
    PREP_VALIDATE_VBOS  = (1 << 1), /* validate VBOs? */
    PREP_EMIT_VARRAYS       = (1 << 2), /* call emit_vertex_arrays? */
    PREP_EMIT_VARRAYS_SWTCL = (1 << 3), /* call emit_vertex_arrays_swtcl? */
    PREP_INDEXED        = (1 << 4)  /* is this draw_elements? */
};

/**
 * Check if the requested number of dwords is available in the CS and
 * if not, flush.
 * \param r300          The context.
 * \param flags         See r300_prepare_flags.
 * \param cs_dwords     The number of dwords to reserve in CS.
 * \return TRUE if the CS was flushed
 */
static bool r300_reserve_cs_dwords(struct r300_context *r300,
                                   enum r300_prepare_flags flags,
                                   unsigned cs_dwords)
{
    bool flushed        = false;
    bool emit_states    = flags & PREP_EMIT_STATES;
    bool emit_vertex_arrays       = flags & PREP_EMIT_VARRAYS;
    bool emit_vertex_arrays_swtcl = flags & PREP_EMIT_VARRAYS_SWTCL;

    /* Add dirty state, index offset, and AOS. */
    if (emit_states)
        cs_dwords += r300_get_num_dirty_dwords(r300);

    if (r300->screen->caps.is_r500)
        cs_dwords += 2; /* emit_index_offset */

    if (emit_vertex_arrays)
        cs_dwords += 55; /* emit_vertex_arrays */

    if (emit_vertex_arrays_swtcl)
        cs_dwords += 7; /* emit_vertex_arrays_swtcl */

    cs_dwords += r300_get_num_cs_end_dwords(r300);

    /* Reserve requested CS space. */
    if (!r300->rws->cs_check_space(&r300->cs, cs_dwords)) {
        r300_flush(&r300->context, PIPE_FLUSH_ASYNC, NULL);
        flushed = true;
    }

    return flushed;
}

/**
 * Validate buffers and emit dirty state.
 * \param r300          The context.
 * \param flags         See r300_prepare_flags.
 * \param index_buffer  The index buffer to validate. The parameter may be NULL.
 * \param buffer_offset The offset passed to emit_vertex_arrays.
 * \param index_bias    The index bias to emit.
 * \param instance_id   Index of instance to render
 * \return TRUE if rendering should be skipped
 */
static bool r300_emit_states(struct r300_context *r300,
                             enum r300_prepare_flags flags,
                             struct pipe_resource *index_buffer,
                             int buffer_offset,
                             int index_bias, int instance_id)
{
    bool emit_states    = flags & PREP_EMIT_STATES;
    bool emit_vertex_arrays       = flags & PREP_EMIT_VARRAYS;
    bool emit_vertex_arrays_swtcl = flags & PREP_EMIT_VARRAYS_SWTCL;
    bool indexed        = flags & PREP_INDEXED;
    bool validate_vbos  = flags & PREP_VALIDATE_VBOS;

    /* Validate buffers and emit dirty state if needed. */
    if (emit_states || (emit_vertex_arrays && validate_vbos)) {
        if (!r300_emit_buffer_validate(r300, validate_vbos,
                                       index_buffer)) {
           fprintf(stderr, "r300: CS space validation failed. "
                   "(not enough memory?) Skipping rendering.\n");
           return false;
        }
    }

    if (emit_states)
        r300_emit_dirty_state(r300);

    if (r300->screen->caps.is_r500) {
        if (r300->screen->caps.has_tcl)
            r500_emit_index_bias(r300, index_bias);
        else
            r500_emit_index_bias(r300, 0);
    }

    if (emit_vertex_arrays &&
        (r300->vertex_arrays_dirty ||
         r300->vertex_arrays_indexed != indexed ||
         r300->vertex_arrays_offset != buffer_offset ||
         r300->vertex_arrays_instance_id != instance_id)) {
        r300_emit_vertex_arrays(r300, buffer_offset, indexed, instance_id);

        r300->vertex_arrays_dirty = false;
        r300->vertex_arrays_indexed = indexed;
        r300->vertex_arrays_offset = buffer_offset;
        r300->vertex_arrays_instance_id = instance_id;
    }

    if (emit_vertex_arrays_swtcl)
        r300_emit_vertex_arrays_swtcl(r300, indexed);

    return true;
}

/**
 * Check if the requested number of dwords is available in the CS and
 * if not, flush. Then validate buffers and emit dirty state.
 * \param r300          The context.
 * \param flags         See r300_prepare_flags.
 * \param index_buffer  The index buffer to validate. The parameter may be NULL.
 * \param cs_dwords     The number of dwords to reserve in CS.
 * \param buffer_offset The offset passed to emit_vertex_arrays.
 * \param index_bias    The index bias to emit.
 * \param instance_id The instance to render.
 * \return TRUE if rendering should be skipped
 */
static bool r300_prepare_for_rendering(struct r300_context *r300,
                                       enum r300_prepare_flags flags,
                                       struct pipe_resource *index_buffer,
                                       unsigned cs_dwords,
                                       int buffer_offset,
                                       int index_bias,
                                       int instance_id)
{
    /* Make sure there is enough space in the command stream and emit states. */
    if (r300_reserve_cs_dwords(r300, flags, cs_dwords))
        flags |= PREP_EMIT_STATES;

    return r300_emit_states(r300, flags, index_buffer, buffer_offset,
                            index_bias, instance_id);
}

static bool immd_is_good_idea(struct r300_context *r300,
                              unsigned count)
{
    if (DBG_ON(r300, DBG_NO_IMMD)) {
        return false;
    }

    if (count * r300->velems->vertex_size_dwords > IMMD_DWORDS) {
        return false;
    }

    /* Buffers can only be used for read by r300 (except query buffers, but
     * those can't be bound by an gallium frontend as vertex buffers). */
    return true;
}

/* Reserve CS space and emit dirty state for the R2VB MVP producer.  That code
 * lives in r300_r2vb.c and cannot see the static prepare_for_rendering or its
 * flags enum; this thin wrapper runs the real reserve + emit_dirty_state path
 * (proper space accounting and validation) so a freshly bound transform-FS and
 * its const file reach the IB -- the raw r300_emit_dirty_state route left an
 * empty IB (RS4xx zero_ib).  cs_dwords reserves room for the producer the caller
 * emits next. */
bool r300_r2vb_prepare_states(struct r300_context *r300, unsigned cs_dwords)
{
    return r300_prepare_for_rendering(r300, PREP_EMIT_STATES, NULL, cs_dwords, 0, 0, -1);
}

/*****************************************************************************
 * The HWTCL draw functions.                                                 *
 ****************************************************************************/

static void r300_draw_arrays_immediate(struct r300_context *r300,
                                       const struct pipe_draw_info *info,
                                       const struct pipe_draw_start_count_bias *draw)
{
    struct pipe_vertex_element* velem;
    struct pipe_vertex_buffer* vbuf;
    unsigned vertex_element_count = r300->velems->count;
    unsigned i, v, vbi;

    /* Size of the vertex, in dwords. */
    unsigned vertex_size = r300->velems->vertex_size_dwords;

    /* The number of dwords for this draw operation. */
    unsigned dwords = 4 + draw->count * vertex_size;

    /* Size of the vertex element, in dwords. */
    unsigned size[PIPE_MAX_ATTRIBS];

    /* Stride to the same attrib in the next vertex in the vertex buffer,
     * in dwords. */
    unsigned stride[PIPE_MAX_ATTRIBS];

    /* Mapped vertex buffers. */
    uint32_t* map[PIPE_MAX_ATTRIBS] = {0};
    uint32_t* mapelem[PIPE_MAX_ATTRIBS];

    CS_LOCALS(r300);

    if (!r300_prepare_for_rendering(r300, PREP_EMIT_STATES, NULL, dwords, 0, 0, -1))
        return;

    /* Calculate the vertex size, offsets, strides etc. and map the buffers. */
    for (i = 0; i < vertex_element_count; i++) {
        velem = &r300->velems->velem[i];
        size[i] = r300->velems->format_size[i] / 4;
        vbi = velem->vertex_buffer_index;
        vbuf = &r300->vertex_buffer[vbi];
        stride[i] = velem->src_stride / 4;

        /* Map the buffer. */
        if (!map[vbi]) {
            map[vbi] = (uint32_t*)r300->rws->buffer_map(r300->rws,
                r300_resource(vbuf->buffer.resource)->buf,
                &r300->cs, PIPE_MAP_READ | PIPE_MAP_UNSYNCHRONIZED);
            map[vbi] += (vbuf->buffer_offset / 4) + stride[i] * draw->start;
        }
        mapelem[i] = map[vbi] + (velem->src_offset / 4);
    }

    r300_emit_draw_init(r300, info->mode, draw->count-1);

    BEGIN_CS(dwords);
    OUT_CS_REG(R300_VAP_VTX_SIZE, vertex_size);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_IMMD_2, draw->count * vertex_size);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (draw->count << 16) |
            r300_translate_primitive(info->mode));

    /* Emit vertices. */
    for (v = 0; v < draw->count; v++) {
        for (i = 0; i < vertex_element_count; i++) {
            OUT_CS_TABLE(&mapelem[i][stride[i] * v], size[i]);
        }
    }
    END_CS;
}

static void r300_emit_draw_arrays(struct r300_context *r300,
                                  unsigned mode,
                                  unsigned count)
{
    bool alt_num_verts = count > 65535;
    CS_LOCALS(r300);

    if (count >= (1 << 24)) {
        fprintf(stderr, "r300: Got a huge number of vertices: %i, "
                "refusing to render.\n", count);
        return;
    }

    r300_emit_draw_init(r300, mode, count-1);

    BEGIN_CS(2 + (alt_num_verts ? 2 : 0));
    if (alt_num_verts) {
        OUT_CS_REG(R500_VAP_ALT_NUM_VERTICES, count);
    }
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST | (count << 16) |
           r300_translate_primitive(mode) |
           (alt_num_verts ? R500_VAP_VF_CNTL__USE_ALT_NUM_VERTS : 0));
    END_CS;
}

static void r300_emit_draw_elements(struct r300_context *r300,
                                    struct pipe_resource* indexBuffer,
                                    unsigned indexSize,
                                    unsigned max_index,
                                    unsigned mode,
                                    unsigned start,
                                    unsigned count,
                                    uint16_t *imm_indices3)
{
    uint32_t count_dwords, offset_dwords;
    bool alt_num_verts = count > 65535;
    CS_LOCALS(r300);

    if (count >= (1 << 24)) {
        fprintf(stderr, "r300: Got a huge number of vertices: %i, "
                "refusing to render (max_index: %i).\n", count, max_index);
        return;
    }

    DBG(r300, DBG_DRAW, "r300: Indexbuf of %u indices, max %u\n",
        count, max_index);

    r300_emit_draw_init(r300, mode, max_index);

    /* If start is odd, render the first triangle with indices embedded
     * in the command stream. This will increase start by 3 and make it
     * even. We can then proceed without a fallback. */
    if (indexSize == 2 && (start & 1) &&
        mode == MESA_PRIM_TRIANGLES) {
        BEGIN_CS(4);
        OUT_CS_PKT3(R300_PACKET3_3D_DRAW_INDX_2, 2);
        OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (3 << 16) |
               R300_VAP_VF_CNTL__PRIM_TRIANGLES);
        OUT_CS(imm_indices3[1] << 16 | imm_indices3[0]);
        OUT_CS(imm_indices3[2]);
        END_CS;

        start += 3;
        count -= 3;
        if (!count)
           return;
    }

    offset_dwords = indexSize * start / sizeof(uint32_t);

    BEGIN_CS(8 + (alt_num_verts ? 2 : 0));
    if (alt_num_verts) {
        OUT_CS_REG(R500_VAP_ALT_NUM_VERTICES, count);
    }
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_INDX_2, 0);
    if (indexSize == 4) {
        count_dwords = count;
        OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (count << 16) |
               R300_VAP_VF_CNTL__INDEX_SIZE_32bit |
               r300_translate_primitive(mode) |
               (alt_num_verts ? R500_VAP_VF_CNTL__USE_ALT_NUM_VERTS : 0));
    } else {
        count_dwords = (count + 1) / 2;
        OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (count << 16) |
               r300_translate_primitive(mode) |
               (alt_num_verts ? R500_VAP_VF_CNTL__USE_ALT_NUM_VERTS : 0));
    }

    OUT_CS_PKT3(R300_PACKET3_INDX_BUFFER, 2);
    OUT_CS(R300_INDX_BUFFER_ONE_REG_WR | (R300_VAP_PORT_IDX0 >> 2) |
           (0 << R300_INDX_BUFFER_SKIP_SHIFT));
    OUT_CS(offset_dwords << 2);
    OUT_CS(count_dwords);
    OUT_CS_RELOC(r300_resource(indexBuffer));
    END_CS;
}

static void r300_draw_elements_immediate(struct r300_context *r300,
                                         const struct pipe_draw_info *info,
                                         const struct pipe_draw_start_count_bias *draw)
{
#if UTIL_ARCH_BIG_ENDIAN
    uint32_t indices[8];
#else
    const uint8_t *ptr1;
    const uint16_t *ptr2;
    const uint32_t *ptr4;
    unsigned i;
#endif
    unsigned index_size = info->index_size;
    bool use_32bit_indices = index_size == 4;
    unsigned count_dwords;
#if UTIL_ARCH_BIG_ENDIAN
    /* R500 applies draw->index_bias in hardware via R500_VAP_INDEX_OFFSET. */
    int index_bias = draw->index_bias && !r300->screen->caps.is_r500 ?
                     draw->index_bias : 0;
#endif
    CS_LOCALS(r300);

#if UTIL_ARCH_BIG_ENDIAN
    /* The VAP uses one endian-swap mode for all fetched data. On BE, emit
     * immediate indices as 32-bit words to match the vertex streams.
     */
    use_32bit_indices = true;
    assert(draw->count <= ARRAY_SIZE(indices));
    r300_rebuild_elts_to_uint_userptr(&r300->context, info, 0, index_bias,
                                      draw->start, draw->count, indices);
#endif
    count_dwords = use_32bit_indices ? draw->count : (draw->count + 1) / 2;

    /* 19 dwords for r300_draw_elements_immediate. Give up if the function fails. */
    if (!r300_prepare_for_rendering(r300,
            PREP_EMIT_STATES | PREP_VALIDATE_VBOS | PREP_EMIT_VARRAYS |
            PREP_INDEXED, NULL, 2+count_dwords, 0, draw->index_bias, -1))
        return;

    r300_emit_draw_init(r300, info->mode, info->max_index);

    BEGIN_CS(2 + count_dwords);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_INDX_2, count_dwords);

#if UTIL_ARCH_BIG_ENDIAN
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (draw->count << 16) |
           R300_VAP_VF_CNTL__INDEX_SIZE_32bit |
           r300_translate_primitive(info->mode));
    OUT_CS_TABLE(indices, count_dwords);
#else
    switch (index_size) {
    case 1:
        ptr1 = (uint8_t*)info->index.user;
        ptr1 += draw->start;

        OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (draw->count << 16) |
               r300_translate_primitive(info->mode));

        if (draw->index_bias && !r300->screen->caps.is_r500) {
            for (i = 0; i < draw->count-1; i += 2)
                OUT_CS(((ptr1[i+1] + draw->index_bias) << 16) |
                        (ptr1[i]   + draw->index_bias));

            if (draw->count & 1)
                OUT_CS(ptr1[i] + draw->index_bias);
        } else {
            for (i = 0; i < draw->count-1; i += 2)
                OUT_CS(((ptr1[i+1]) << 16) |
                        (ptr1[i]  ));

            if (draw->count & 1)
                OUT_CS(ptr1[i]);
        }
        break;

    case 2:
        ptr2 = (uint16_t*)info->index.user;
        ptr2 += draw->start;

        OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (draw->count << 16) |
               r300_translate_primitive(info->mode));

        if (draw->index_bias && !r300->screen->caps.is_r500) {
            for (i = 0; i < draw->count-1; i += 2)
                OUT_CS(((ptr2[i+1] + draw->index_bias) << 16) |
                        (ptr2[i]   + draw->index_bias));

            if (draw->count & 1)
                OUT_CS(ptr2[i] + draw->index_bias);
        } else {
            /* OUT_CS_TABLE expects full dwords so pack the odd tail manually. */
            if (draw->count & 1) {
                if (count_dwords > 1)
                    OUT_CS_TABLE(ptr2, count_dwords - 1);
                OUT_CS(ptr2[draw->count - 1]);
            } else {
                OUT_CS_TABLE(ptr2, count_dwords);
            }
        }
        break;

    case 4:
        ptr4 = (uint32_t*)info->index.user;
        ptr4 += draw->start;

        OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (draw->count << 16) |
               R300_VAP_VF_CNTL__INDEX_SIZE_32bit |
               r300_translate_primitive(info->mode));

        if (draw->index_bias && !r300->screen->caps.is_r500) {
            for (i = 0; i < draw->count; i++)
                OUT_CS(ptr4[i] + draw->index_bias);
        } else {
            OUT_CS_TABLE(ptr4, count_dwords);
        }
        break;
    }
#endif
    END_CS;
}

static void r300_draw_elements(struct r300_context *r300,
                               const struct pipe_draw_info *info,
                               const struct pipe_draw_start_count_bias *draw,
                               int instance_id)
{
    struct pipe_resource *indexBuffer =
       info->has_user_indices ? NULL : info->index.resource;
    unsigned indexSize = info->index_size;
    struct pipe_resource* orgIndexBuffer = indexBuffer;
    unsigned start = draw->start;
    unsigned count = draw->count;
    bool alt_num_verts = r300->screen->caps.is_r500 &&
                            count > 65536;
    unsigned short_count;
    int buffer_offset = 0, index_offset = 0; /* for index bias emulation */
    uint16_t indices3[3];
    const uint8_t *local_ptr = info->index.user;

    if (draw->index_bias && !r300->screen->caps.is_r500) {
        r300_split_index_bias(r300, draw->index_bias, &buffer_offset,
                              &index_offset);
    }

    r300_translate_index_buffer(r300, info, &indexBuffer,
                                &indexSize, index_offset, &start, count, &local_ptr);

    /* Fallback for misaligned ushort indices. */
    if (indexSize == 2 && (start & 1) && indexBuffer) {
        /* If we got here, then orgIndexBuffer == indexBuffer. */
        uint16_t *ptr = r300->rws->buffer_map(r300->rws, r300_resource(orgIndexBuffer)->buf,
                                              &r300->cs,
                                              PIPE_MAP_READ |
                                              PIPE_MAP_UNSYNCHRONIZED);

        if (info->mode == MESA_PRIM_TRIANGLES) {
           memcpy(indices3, ptr + start, 6);
        } else {
            /* Copy the mapped index buffer directly to the upload buffer.
             * The start index will be aligned simply from the fact that
             * every sub-buffer in the upload buffer is aligned. */
            r300_upload_index_buffer(r300, &indexBuffer, indexSize, &start,
                                     count, (uint8_t*)ptr);
        }
    } else {
        if (info->has_user_indices) {
           struct pipe_resource* indexSaved = indexBuffer;

           if (local_ptr != info->index.user)
              start = 0;

           r300_upload_index_buffer(r300, &indexBuffer, indexSize,
                                     &start, count,
                                     local_ptr);

           pipe_resource_reference(&indexSaved, NULL);
        }
    }

    /* 19 dwords for emit_draw_elements. Give up if the function fails. */
    if (!r300_prepare_for_rendering(r300,
            PREP_EMIT_STATES | PREP_VALIDATE_VBOS | PREP_EMIT_VARRAYS |
            PREP_INDEXED, indexBuffer, 19, buffer_offset, draw->index_bias,
            instance_id))
        goto done;

    if (alt_num_verts || count <= 65535) {
        r300_emit_draw_elements(r300, indexBuffer, indexSize,
                                info->max_index, info->mode, start, count,
                                indices3);
    } else {
        do {
            /* The maximum must be divisible by 4 and 3,
             * so that quad and triangle lists are split correctly.
             *
             * Strips, loops, and fans won't work. */
            short_count = MIN2(count, 65532);

            r300_emit_draw_elements(r300, indexBuffer, indexSize,
                                     info->max_index,
                                     info->mode, start, short_count, indices3);

            start += short_count;
            count -= short_count;

            /* 15 dwords for emit_draw_elements */
            if (count) {
                if (!r300_prepare_for_rendering(r300,
                        PREP_VALIDATE_VBOS | PREP_EMIT_VARRAYS | PREP_INDEXED,
                        indexBuffer, 19, buffer_offset, draw->index_bias,
                        instance_id))
                    goto done;
            }
        } while (count);
    }

done:
    if (indexBuffer != orgIndexBuffer) {
        pipe_resource_reference( &indexBuffer, NULL );
    }
}

static void r300_draw_arrays(struct r300_context *r300,
                             const struct pipe_draw_info *info,
                             const struct pipe_draw_start_count_bias *draw,
                             int instance_id)
{
    bool alt_num_verts = r300->screen->caps.is_r500 &&
                            draw->count > 65536;
    unsigned start = draw->start;
    unsigned count = draw->count;
    unsigned short_count;

    /* 9 spare dwords for emit_draw_arrays. Give up if the function fails. */
    if (!r300_prepare_for_rendering(r300,
                                    PREP_EMIT_STATES | PREP_VALIDATE_VBOS | PREP_EMIT_VARRAYS,
                                    NULL, 9, start, 0, instance_id))
        return;

    if (alt_num_verts || count <= 65535) {
        r300_emit_draw_arrays(r300, info->mode, count);
    } else {
        do {
            /* The maximum must be divisible by 4 and 3,
             * so that quad and triangle lists are split correctly.
             *
             * Strips, loops, and fans won't work. */
            short_count = MIN2(count, 65532);
            r300_emit_draw_arrays(r300, info->mode, short_count);

            start += short_count;
            count -= short_count;

            /* 9 spare dwords for emit_draw_arrays. Give up if the function fails. */
            if (count) {
                if (!r300_prepare_for_rendering(r300,
                                                PREP_VALIDATE_VBOS | PREP_EMIT_VARRAYS, NULL, 9,
                                                start, 0, instance_id))
                    return;
            }
        } while (count);
    }
}

static void r300_draw_arrays_instanced(struct r300_context *r300,
                                       const struct pipe_draw_info *info,
                                       const struct pipe_draw_start_count_bias *draw)
{
    int i;

    for (i = 0; i < info->instance_count; i++)
        r300_draw_arrays(r300, info, draw, i);
}

static void r300_draw_elements_instanced(struct r300_context *r300,
                                         const struct pipe_draw_info *info,
                                         const struct pipe_draw_start_count_bias *draw)
{
    int i;

    for (i = 0; i < info->instance_count; i++)
        r300_draw_elements(r300, info, draw, i);
}

static unsigned r300_max_vertex_count(struct r300_context *r300)
{
   unsigned i, nr = r300->velems->count;
   struct pipe_vertex_element *velems = r300->velems->velem;
   unsigned result = ~0;

   for (i = 0; i < nr; i++) {
      struct pipe_vertex_buffer *vb =
            &r300->vertex_buffer[velems[i].vertex_buffer_index];
      unsigned size, max_count, value;

      /* We're not interested in constant and per-instance attribs. */
      if (!vb->buffer.resource ||
          !velems[i].src_stride ||
          velems[i].instance_divisor) {
         continue;
      }

      size = vb->buffer.resource->width0;

      /* Subtract buffer_offset. */
      value = vb->buffer_offset;
      if (value >= size) {
         return 0;
      }
      size -= value;

      /* Subtract src_offset. */
      value = velems[i].src_offset;
      if (value >= size) {
         return 0;
      }
      size -= value;

      /* Compute the max count. */
      max_count = 1 + size / velems[i].src_stride;
      result = MIN2(result, max_count);
   }
   return result;
}

static void
r300_update_clip_discard_distance(struct r300_context *r300, unsigned prim)
{
    struct r300_rs_state *rs = (struct r300_rs_state*)r300->rs_state.state;
    float target_distance = 0.0f;

    if (rs) {
        if (prim == MESA_PRIM_POINTS)
            target_distance = rs->max_point_size;
        else if (r300_prim_is_lines(prim))
            target_distance = rs->line_width;
    }

    if (r300->current_rast_prim != prim) {
        r300->current_rast_prim = prim;
        r300_set_clip_discard_distance(r300, target_distance);
    } else if (prim == MESA_PRIM_POINTS || r300_prim_is_lines(prim)) {
        r300_set_clip_discard_distance(r300, target_distance);
    }
}

static bool
r300_rasterizer_emits_points(struct r300_context *r300, unsigned prim)
{
    struct r300_rs_state *rs = (struct r300_rs_state*)r300->rs_state.state;

    if (prim == MESA_PRIM_POINTS)
        return true;

    switch (prim) {
    case MESA_PRIM_TRIANGLES:
    case MESA_PRIM_TRIANGLE_STRIP:
    case MESA_PRIM_TRIANGLE_FAN:
    case MESA_PRIM_QUADS:
    case MESA_PRIM_QUAD_STRIP:
    case MESA_PRIM_POLYGON:
        break;
    default:
        return false;
    }

    if (!rs)
        return false;

    bool front_rasterized = !(rs->rs.cull_face & PIPE_FACE_FRONT);
    bool back_rasterized = !(rs->rs.cull_face & PIPE_FACE_BACK);

    if (front_rasterized && rs->rs.fill_front != PIPE_POLYGON_MODE_POINT)
        return false;
    if (back_rasterized && rs->rs.fill_back != PIPE_POLYGON_MODE_POINT)
        return false;

    return front_rasterized || back_rasterized;
}

/* Multipass state-lifetime snapshot (R300_MP_SNAPSHOT=1): the pass A and
 * pass B draws issue back-to-back inside one draw_vbo callback, so a CS
 * boundary between them exercises state paths no ordinary draw sequence
 * does.  The snapshot separates the three loss classes at each stage: the
 * FS override (pass B drawn with the wrong program), the geometry and its
 * dirty re-emission (pass B not drawn at all), and the transient
 * framebuffer/sampler bindings (pass B sampling or writing the wrong
 * surface). */
static void
r300_mp_snapshot(struct r300_context *r300, const char *tag)
{
    struct pipe_framebuffer_state *fb = r300->fb_state.state;
    struct r300_textures_state *ts = r300->textures_state.state;

    fprintf(stderr,
            "MP_SNAP %-10s flush=%" PRIu64 " in_mp=%d override=%p fs_code=%p "
            "fb=%ux%u/%u cb0=%p zs=%p views=%u/%u samp0=%p "
            "dirty[fb=%d tex=%d fs=%d fsc=%d rs=%d va=%d] hw_dirty=%d\n",
            tag, (uint64_t)r300->flush_counter, r300->in_multipass,
            (void *)r300->multipass_override_fs,
            r300->fs.state ? (void *)r300_fs(r300)->shader : NULL,
            fb->width, fb->height, fb->nr_cbufs,
            (void *)fb->cbufs[0].texture, (void *)fb->zsbuf.texture,
            ts->sampler_view_count, ts->sampler_state_count,
            ts->sampler_views[0] ? (void *)ts->sampler_views[0]->base.texture
                                 : NULL,
            r300->fb_state.dirty, r300->textures_state.dirty, r300->fs.dirty,
            r300->fs_constants.dirty, r300->rs_block_state.dirty,
            r300->vertex_arrays_dirty, r300->dirty_hw != 0);
}

/* >64-ALU FS multipass (R300_FS_MULTIPASS): render the split FS in two draws.
 * Pass A (the bound code) renders the byte-packed carry to N scratch RGBA8
 * MRTs (two carried scalar components per target); pass B samples them at
 * units 0..N-1, unpacks the carry, and finishes the program to the real
 * framebuffer.  Gated and entered only when the picked FS code carries a
 * multipass_pass_b partner.  The app re-binds its own samplers before its
 * next draw, so the transient scratch samplers need no explicit restore; a
 * fragment shader that itself samples the low units is rejected at partition
 * time. */
static void r300_fs_multipass_draw(struct pipe_context *pipe,
                                   const struct pipe_draw_info *dinfo,
                                   unsigned drawid_offset,
                                   const struct pipe_draw_indirect_info *indirect,
                                   const struct pipe_draw_start_count_bias *draws,
                                   unsigned num_draws,
                                   struct r300_fragment_shader_code *pass_a)
{
    struct r300_context *r300 = r300_context(pipe);
    struct pipe_screen *screen = pipe->screen;
    struct pipe_framebuffer_state *fb = r300->fb_state.state;
    const unsigned nrt = CLAMP(pass_a->multipass_num_scratch, 1, 4);

    /* PIPE_TEXTURE_RECT so pass B's RECT samplers read at the fragment window
     * coordinate; the compiler's RC_STATE_R300_TEXRECT_FACTOR normalizes it. */
    struct pipe_resource tmpl = {
        .target = PIPE_TEXTURE_RECT,
        .format = PIPE_FORMAT_R8G8B8A8_UNORM,
        .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW,
        .width0 = fb->width,
        .height0 = fb->height,
        .depth0 = 1,
        .array_size = 1,
    };
    struct pipe_resource *scratch[4] = { NULL };
    bool scratch_ok = true;
    for (unsigned k = 0; k < nrt; k++) {
        scratch[k] = screen->resource_create(screen, &tmpl);
        if (!scratch[k])
            scratch_ok = false;
    }

    r300->in_multipass = true;

    static int mp_snap = -1;
    if (mp_snap < 0) {
        const char *e = getenv("R300_MP_SNAPSHOT");
        mp_snap = (e && e[0] == '1') ? 1 : 0;
    }
    if (mp_snap)
        r300_mp_snapshot(r300, "entry");

    if (!scratch_ok) {
        /* No carry targets: render pass A alone (a gated, experimental path). */
        for (unsigned k = 0; k < nrt; k++)
            pipe_resource_reference(&scratch[k], NULL);
        pipe->draw_vbo(pipe, dinfo, drawid_offset, indirect, draws, num_draws);
        r300->in_multipass = false;
        return;
    }

    struct pipe_framebuffer_state saved_fb;
    memset(&saved_fb, 0, sizeof(saved_fb));
    util_copy_framebuffer_state(&saved_fb, fb);

    /* Pass A -> the scratch MRTs. */
    struct pipe_framebuffer_state fb1;
    memset(&fb1, 0, sizeof(fb1));
    fb1.width = fb->width;
    fb1.height = fb->height;
    fb1.nr_cbufs = nrt;
    for (unsigned k = 0; k < nrt; k++) {
        fb1.cbufs[k].texture = scratch[k];
        fb1.cbufs[k].format = PIPE_FORMAT_R8G8B8A8_UNORM;
    }
    pipe->set_framebuffer_state(pipe, &fb1);
    pipe->draw_vbo(pipe, dinfo, drawid_offset, indirect, draws, num_draws);

    if (mp_snap)
        r300_mp_snapshot(r300, "post-A");

    /* Pass A's colour writes sit in the CB cache; pass B's texture fetches
     * do not snoop it.  A read map of each scratch texture forces
     * r300_texture_transfer_map's detile+blit+flush+BO-wait sequence, which
     * ensures pass B reads the correct scratch data.  Map one texel and
     * discard it to force this sequence. */
    pipe->texture_barrier(pipe, PIPE_TEXTURE_BARRIER_SAMPLER);

    for (unsigned k = 0; k < nrt; k++) {
        struct pipe_transfer *xfer = NULL;
        void *map = pipe_texture_map(pipe, scratch[k], 0, 0, PIPE_MAP_READ,
                                     0, 0, 1, 1, &xfer);
        if (map)
            pipe_texture_unmap(pipe, xfer);
    }
    if (mp_snap)
        r300_mp_snapshot(r300, "post-flush");

    /* Pass B (samples the scratch set) -> the real framebuffer. */
    pipe->set_framebuffer_state(pipe, &saved_fb);

    struct pipe_sampler_view *sv[4] = { NULL };
    void *scso[4] = { NULL };
    struct pipe_sampler_state sstate;
    memset(&sstate, 0, sizeof(sstate));
    sstate.wrap_s = sstate.wrap_t = sstate.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    sstate.min_img_filter = sstate.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
    for (unsigned k = 0; k < nrt; k++) {
        struct pipe_sampler_view sv_tmpl;
        u_sampler_view_default_template(&sv_tmpl, scratch[k],
                                        scratch[k]->format);
        sv[k] = pipe->create_sampler_view(pipe, scratch[k], &sv_tmpl);
        scso[k] = pipe->create_sampler_state(pipe, &sstate);
    }

    pipe->set_sampler_views(pipe, MESA_SHADER_FRAGMENT, 0, nrt, 0, sv);
    pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, nrt, scso);

    r300->multipass_override_fs = pass_a->multipass_pass_b;
    if (mp_snap)
        r300_mp_snapshot(r300, "pre-B");
    pipe->draw_vbo(pipe, dinfo, drawid_offset, indirect, draws, num_draws);
    if (mp_snap)
        r300_mp_snapshot(r300, "post-B");
    r300->multipass_override_fs = NULL;

    void *null_cso[4] = { NULL };
    pipe->bind_sampler_states(pipe, MESA_SHADER_FRAGMENT, 0, nrt, null_cso);
    for (unsigned k = 0; k < nrt; k++) {
        pipe->delete_sampler_state(pipe, scso[k]);
        pipe_sampler_view_reference(&sv[k], NULL);
    }
    pipe->set_framebuffer_state(pipe, &saved_fb);
    util_unreference_framebuffer_state(&saved_fb);
    for (unsigned k = 0; k < nrt; k++)
        pipe_resource_reference(&scratch[k], NULL);
    r300->in_multipass = false;
}

/* An all-zero 32x32 GL polygon stipple masks every polygon fragment, so a
 * filled-triangle draw under that pattern produces no visible output. R3xx
 * has no stipple-pattern register to encode the pattern in hardware; the
 * all-zero case short-circuits the draw here and every other pattern rides
 * the fragment-shader stipple variant, so this only applies to unstippled
 * fill of MESA_PRIM_TRIANGLES. */
static bool r300_poly_stipple_masks_draw(struct r300_context *r300,
                                          enum mesa_prim reduced_prim)
{
    struct r300_rs_state *rs = (struct r300_rs_state*)r300->rs_state.state;

    return rs && rs->rs.poly_stipple_enable &&
           rs->rs.fill_front == PIPE_POLYGON_MODE_FILL &&
           rs->rs.fill_back == PIPE_POLYGON_MODE_FILL &&
           reduced_prim == MESA_PRIM_TRIANGLES &&
           r300->poly_stipple_set && r300->poly_stipple_all_zero;
}

/* Polygon stipple applies to filled triangle draws only; derive the per-draw
 * flag the fragment-shader variant key and the merged texture state read.
 * A transition re-merges the texture state (the stipple texture splices in
 * at the unit past the program's own bindings) and revalidates the FS. */
static void r300_update_pstipple_draw(struct r300_context *r300,
                                      enum mesa_prim mode)
{
    struct r300_rs_state *rs = (struct r300_rs_state*)r300->rs_state.state;
    bool pstipple = rs && rs->rs.poly_stipple_enable &&
                    rs->rs.fill_front == PIPE_POLYGON_MODE_FILL &&
                    rs->rs.fill_back == PIPE_POLYGON_MODE_FILL &&
                    r300->pstipple_sampler_view &&
                    u_reduced_prim(mode) == MESA_PRIM_TRIANGLES;

    if (pstipple != r300->pstipple_draw) {
        r300->pstipple_draw = pstipple;
        r300_mark_atom_dirty(r300, &r300->textures_state);
        if (r300->fs_status == FRAGMENT_SHADER_VALID)
            r300->fs_status = FRAGMENT_SHADER_MAYBE_DIRTY;
    }
}

static void r300_draw_vbo(struct pipe_context* pipe,
                          const struct pipe_draw_info *dinfo,
                          unsigned drawid_offset,
                          const struct pipe_draw_indirect_info *indirect,
                          const struct pipe_draw_start_count_bias *draws,
                          unsigned num_draws)
{
   if (num_draws > 1) {
      util_draw_multi(pipe, dinfo, drawid_offset, indirect, draws, num_draws);
      return;
   }

    struct r300_context* r300 = r300_context(pipe);
    struct pipe_draw_info info = *dinfo;
    struct pipe_draw_start_count_bias draw = draws[0];

    if (r300->skip_rendering ||
        !u_trim_pipe_prim(info.mode, &draw.count)) {
        return;
    }

    if (r300_poly_stipple_masks_draw(r300, u_reduced_prim(info.mode))) {
        return;
    }

    r300_update_clip_discard_distance(r300, info.mode);

    if (r300->sprite_coord_enable != 0) {
        bool is_point = r300_rasterizer_emits_points(r300, info.mode);
        if (is_point != r300->is_point) {
            r300->is_point = is_point;
            r300_mark_atom_dirty(r300, &r300->rs_block_state);
        }
    }

    r300_update_pstipple_draw(r300, info.mode);

    r300_update_derived_state(r300);

    /* Skip draw if we failed to compile the vertex shader. */
    if (r300_vs(r300)->shader->dummy)
        return;

    /* Draw. */
    if (info.index_size) {
        unsigned max_count = r300_max_vertex_count(r300);

        if (!max_count) {
           fprintf(stderr, "r300: Skipping a draw command. There is a buffer "
                   " which is too small to be used for rendering.\n");
           return;
        }

        if (max_count == ~0) {
           /* There are no per-vertex vertex elements. Use the hardware maximum. */
           max_count = 0xffffff;
        }

        info.max_index = max_count - 1;

        if (info.instance_count <= 1) {
            if (draw.count <= 8 && info.has_user_indices) {
                r300_draw_elements_immediate(r300, &info, &draw);
            } else {
                r300_draw_elements(r300, &info, &draw, -1);
            }
        } else {
            r300_draw_elements_instanced(r300, &info, &draw);
        }
    } else {
        if (info.instance_count <= 1) {
            if (immd_is_good_idea(r300, draw.count)) {
                r300_draw_arrays_immediate(r300, &info, &draw);
            } else {
                r300_draw_arrays(r300, &info, &draw, -1);
            }
        } else {
            r300_draw_arrays_instanced(r300, &info, &draw);
        }
    }
}

/****************************************************************************
 * The rest of this file is for SW TCL rendering only. Please be polite and *
 * keep these functions separated so that they are easier to locate. ~C.    *
 ***************************************************************************/

/* SW TCL elements, using Draw. */
/* WARNING -- R300_R2VB_EXEC is experimental and SUSPECTED to hang the GPU.  On
 * the first silicon run where this actually executed (vertex data uploaded from
 * the SWTCL malloced_buffer shadow), the reset-less RS482 went unresponsive
 * during the route-on draw.  The likely cause: r300_update_derived_state set the
 * RS interpolators and VAP_OUTPUT_VTX_FMT for the gallivm draw-module OUTPUT
 * vertex layout, which is not guaranteed to match the application vertex-element
 * layout this path feeds straight to the VAP; a mismatch can stall the VAP/GA.
 * Correctly executing the route needs the RS / VAP-output-format state rebuilt
 * for the app velems (or a proof the passthrough layouts coincide) before it is
 * safe to enable.  Gated off by default (R300_R2VB_EXEC), so ordinary drawing is
 * unaffected.
 *
 * Execute a PASSTHROUGH-classified draw via the direct-VB route: re-ingest the
 * application vertex arrays at TCL_BYPASS, skipping the gallivm draw module.
 * r3v feeds the SWTCL path USER vertex buffers (CPU pointers, no winsys BO);
 * a LOAD_VBPNTR relocation needs a BO, so upload each used user buffer's range to
 * one via r300->uploader, swap r300->vertex_buffer to the uploads for the emit,
 * then restore and unref.  The upload is a memcpy of already-mapped data, far
 * cheaper than the gallivm per-vertex interpreter.  Returns true if it handled
 * the draw (executed, or intentionally skipped on a flush failure as the HW-TCL
 * path does); false to fall back to gallivm. */
/* No-submit first-principles capture of the VAP-stream + RS-routing the direct-VB
 * path would feed the VAP under TCL_BYPASS, decoded to the fields that matter for
 * the suspected hang.  Pure CPU inspection of the bound state atoms -- no CS emit,
 * no GPU work -- so it is safe on any boot.  Axioms it surfaces:
 *  A1 VAP_PROG_STREAM_CNTL: per fetched element, DATA_TYPE + DST_VEC_LOC (which
 *     VAP output vector the element lands in) -- this is the velems CSO stream.
 *  A3 VAP_OUTPUT_VTX_FMT_0/1: which output vectors the VAP declares present.
 *  A4 RS_COUNT / RS_IP: which VAP output vectors the rasteriser routes to FS
 *     inputs.  The chain is consistent iff every RS source vector is produced by
 *     a stream element's DST_VEC_LOC and position lands where SU/GA expects. */
static void r300_r2vb_inspect_passthrough(struct r300_context *r300)
{
    struct r300_vertex_stream_state *vs =
        (struct r300_vertex_stream_state *)r300->vertex_stream_state.state;
    struct r300_rs_block *rs = (struct r300_rs_block *)r300->rs_block_state.state;

    /* velems->format_size / vertex_size_dwords are populated only for has_tcl
     * (r300_create_vertex_elements_state), so on SWTCL they are 0; compute the
     * per-vertex dword count from the element formats directly, as that HWTCL
     * path does (align(blocksize,4)/4 per element). */
    unsigned vap_vtx_size = 0;
    for (unsigned i = 0; r300->velems && i < r300->velems->count; i++)
        vap_vtx_size += align(util_format_get_blocksize(r300->velems->velem[i].src_format), 4) / 4;
    fprintf(stderr, "r2vb_inspect velems_count=%u nvb=%u would_emit_vap_vtx_size=%u\n",
            r300->velems ? r300->velems->count : 0, r300->nr_vertex_buffers, vap_vtx_size);
    /* The LOAD_VBPNTR SIZE field r300_emit_vertex_arrays emits per array comes
     * from velems->format_size[i], which r300_create_vertex_elements_state fills
     * only under has_tcl -- so on SWTCL it is zero and every array fetches zero
     * dwords.  Report the live value next to the format-derived size so a
     * SIZE=0 (the malformed fetch) is visible in the no-submit capture. */
    for (unsigned i = 0; r300->velems && i < r300->velems->count; i++)
        fprintf(stderr,
                "  velem[%u] vbi=%u src_stride=%u src_offset=%u "
                "format_size_live=%u format_size_expect=%u\n",
                i, r300->velems->velem[i].vertex_buffer_index,
                r300->velems->velem[i].src_stride, r300->velems->velem[i].src_offset,
                r300->velems->format_size[i],
                align(util_format_get_blocksize(r300->velems->velem[i].src_format), 4));
    if (vs) {
        fprintf(stderr, "r2vb_inspect vap_stream count=%u\n", vs->count);
        for (unsigned i = 0; i < vs->count && i < 8; i++) {
            uint32_t c = vs->vap_prog_stream_cntl[i];
            for (unsigned e = 0; e < 2; e++) {
                uint32_t f = c >> (e * 16);
                fprintf(stderr,
                        "  stream[%u].e%u raw=0x%08x data_type=%u dst_vec_loc=%u last=%u\n",
                        i, e, c, f & 0xf, (f >> 8) & 0x1f, (f >> 13) & 1);
            }
        }
    }
    /* Viewport/VTE: the SWTCL path leaves vte_control = VTX_XY_FMT (no HW
     * transform) for gallivm's window-space output; the route needs the HW
     * viewport transform for clip-space app verts.  Report both the bound
     * vte_control and r300->viewport scale/offset the route would program. */
    {
        struct r300_viewport_state *vps =
            (struct r300_viewport_state *)r300->viewport_state.state;
        const struct pipe_viewport_state *vp = &r300->viewport;
        fprintf(stderr,
                "r2vb_inspect bound_vte_control=0x%08x vp_scale=%.3f,%.3f,%.3f "
                "vp_translate=%.3f,%.3f,%.3f\n",
                vps ? vps->vte_control : 0u, vp->scale[0], vp->scale[1], vp->scale[2],
                vp->translate[0], vp->translate[1], vp->translate[2]);
    }
    if (rs) {
        fprintf(stderr,
                "r2vb_inspect vap_out_vtx_fmt0=0x%08x pos=%u ptsize=%u fmt1=0x%08x "
                "rs_count=0x%08x rs_inst_count=0x%08x\n",
                rs->vap_out_vtx_fmt[0], rs->vap_out_vtx_fmt[0] & 1,
                (rs->vap_out_vtx_fmt[0] >> 16) & 1, rs->vap_out_vtx_fmt[1], rs->count,
                rs->inst_count);
        for (unsigned i = 0; i < 8; i++)
            if (rs->ip[i] || rs->inst[i])
                fprintf(stderr, "  rs[%u] ip=0x%08x inst=0x%08x\n", i, rs->ip[i], rs->inst[i]);
    }

    /* Emit the normalized VAP/RS tuple + contract verdict in the same field
     * vocabulary the SW-TCL rebuild path uses, so the direct-VB route and the
     * ordinary GL SW-TCL route are diffable field-by-field. */
    r300_dump_vap_rs_tuple(r300, "r2vb_inspect");
}

/* Position-only delivery invariant for the producer-fed capture: the delivery
 * fetches the producer outputs and nothing extra.  Enforced clauses (a failure
 * refuses the delivery and the capture): the velem count does not exceed the
 * VAP output count r300->vertex_info declares (the proven multi-stream delivery
 * binds FEWER arrays than outputs when one application buffer sources several
 * passthrough varyings -- the RS duplicates the vector, so equality is not the
 * invariant); VAP_VTX_SIZE equals sum(format_size/4) over the bound elements;
 * and the position element's bound resource is the clip BO the producer
 * published (r2vb_capture_clip, set alongside r2vb_producer_kind -- not
 * r2vb_slot_pos_bo, which is the producer's slot-pixel input stream).
 * Recorded-but-not-enforced clauses (logged for the offline decode, too fragile
 * to hard-gate on a partial PSC parse): the VAP declares POS_PRESENT, and every
 * RS source vector has a producing PSC stream element.  Returns true when the
 * enforced clauses hold. */
static bool r300_r2vb_capture_preflight(struct r300_context *r300,
                                        unsigned vap_vtx_size,
                                        enum r300_r2vb_producer_kind kind,
                                        struct pipe_resource *capture_clip)
{
    struct r300_vertex_element_state *ve = r300->velems;
    struct r300_rs_block *rs = (struct r300_rs_block *)r300->rs_block_state.state;
    unsigned num_attribs = r300->vertex_info.num_attribs;

    unsigned sz_sum = 0;
    for (unsigned i = 0; ve && i < ve->count; i++)
        sz_sum += align(util_format_get_blocksize(ve->velem[i].src_format), 4) / 4;

    bool count_bounded = ve && ve->count > 0 && ve->count <= num_attribs;
    bool vtxsize_match = sz_sum == vap_vtx_size;

    /* The VAP packs VARYING_SLOT_POS first, so velem[0] is the position element;
     * its bound vertex buffer must be the clip BO the producer published.  A
     * producer-fed delivery always has one (both producers set it with the
     * kind), so a NULL here is itself a refusal. */
    struct pipe_resource *pos_res = NULL;
    if (ve && ve->count > 0) {
        unsigned vbi = ve->velem[0].vertex_buffer_index;
        pos_res = r300->vertex_buffer[vbi].buffer.resource;
    }
    bool pos_to_clip = capture_clip != NULL && pos_res == capture_clip;

    bool pos_present = rs && (rs->vap_out_vtx_fmt[0] &
                              R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT);

    bool ok = count_bounded && vtxsize_match && pos_to_clip;

    fprintf(stderr,
            "r2vb_delivery_capture invariant=%s producer=%s velems_count=%u "
            "num_attribs=%u vap_vtx_size=%u format_size_sum=%u pos_res=%p "
            "clip_bo=%p enforce_count_bounded=%d enforce_vtxsize=%d "
            "enforce_pos_to_clip=%d record_pos_present=%d\n",
            ok ? "HOLD" : "FAIL",
            kind == R300_R2VB_PRODUCER_SPLIT ? "split" : "single",
            ve ? ve->count : 0, num_attribs, vap_vtx_size, sz_sum,
            (void *)pos_res, (void *)capture_clip,
            count_bounded, vtxsize_match, pos_to_clip, pos_present);

    if (!ok)
        fprintf(stderr,
                "r2vb_delivery_capture refuse clause=%s (falling back to gallivm)\n",
                !count_bounded ? "0<count<=num_attribs" :
                !vtxsize_match ? "vap_vtx_size==sum(format_size/4)" :
                "position->clip_bo");
    return ok;
}

/* Sidecar record for the producer-fed delivery capture, emitted after the full
 * delivery IB is built and before the RADEON_FLUSH_NOOP discard: the reloc
 * indices and draw-packet dword offset are only valid while the CS still holds
 * the packets.  One machine-greppable key=value block prefixed
 * r2vb_delivery_capture.  Setting R300_TRACE additionally captures the raw
 * unpatched IB + relocation table through the winsys no-submit trace branch the
 * NOOP flush reaches (radeon_drm_cs_emit_ioctl_oneshot is skipped; the else
 * branch writes the trace); when R300_R2VB_DELIVERY_CAPTURE_DIR is unset the raw
 * dwords are hexdumped to stderr here instead. */
static void r300_r2vb_capture_record(struct r300_context *r300,
                                     const struct pipe_draw_info *info,
                                     const struct pipe_draw_start_count_bias *draw,
                                     enum r300_r2vb_producer_kind kind,
                                     struct pipe_resource *capture_clip,
                                     unsigned vap_vtx_size,
                                     unsigned draw_pkt_off)
{
    struct r300_vertex_element_state *ve = r300->velems;
    struct r300_vertex_stream_state *vs =
        (struct r300_vertex_stream_state *)r300->vertex_stream_state.state;
    struct r300_rs_block *rs = (struct r300_rs_block *)r300->rs_block_state.state;

    fprintf(stderr,
            "r2vb_delivery_capture header producer=%s cs_cdw=%u draw_pkt_dword_off=%u "
            "num_attribs=%u velems_count=%u nr_vertex_buffers=%u vap_vtx_size=%u "
            "count=%u prim=%u\n",
            kind == R300_R2VB_PRODUCER_SPLIT ? "split" : "single",
            r300->cs.current.cdw, draw_pkt_off, r300->vertex_info.num_attribs,
            ve ? ve->count : 0, r300->nr_vertex_buffers, vap_vtx_size,
            draw->count, info->mode);

    /* Per-velem format size and the VAP_VTX_SIZE sum. */
    for (unsigned i = 0; ve && i < ve->count; i++)
        fprintf(stderr,
                "r2vb_delivery_capture velem[%u] vbi=%u format_size=%u "
                "expect_role=LOAD_VBPNTR_array%u\n",
                i, ve->velem[i].vertex_buffer_index,
                align(util_format_get_blocksize(ve->velem[i].src_format), 4), i);

    /* PSC (VAP_PROG_STREAM_CNTL / _EXT) words + count. */
    if (vs) {
        fprintf(stderr, "r2vb_delivery_capture psc_count=%u\n", vs->count);
        for (unsigned i = 0; i < vs->count && i < 8; i++)
            fprintf(stderr,
                    "r2vb_delivery_capture psc[%u] cntl=0x%08x cntl_ext=0x%08x\n",
                    i, vs->vap_prog_stream_cntl[i], vs->vap_prog_stream_cntl_ext[i]);
    }

    /* VAP output-format words and the RS routing count / instructions. */
    if (rs) {
        fprintf(stderr,
                "r2vb_delivery_capture vap_out_vtx_fmt0=0x%08x vap_out_vtx_fmt1=0x%08x "
                "pos_present=%u rs_count=0x%08x rs_inst_count=0x%08x\n",
                rs->vap_out_vtx_fmt[0], rs->vap_out_vtx_fmt[1],
                rs->vap_out_vtx_fmt[0] & R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT ? 1u : 0u,
                rs->count, rs->inst_count);
        for (unsigned i = 0; i < 8; i++)
            if (rs->ip[i] || rs->inst[i])
                fprintf(stderr,
                        "r2vb_delivery_capture rs[%u] ip=0x%08x inst=0x%08x\n",
                        i, rs->ip[i], rs->inst[i]);
    }

    /* Detailed BO identity (slab, parent offset, va, domain, size) for the clip BO
     * the position element fetches and every bound vertex-buffer resource, as far
     * as the winsys exposes.  These lines carry the fields the RELOC grammar omits;
     * the RELOC / roles lines below carry the analyzer's join keys. */
    r300_r2vb_report_bo_identity(r300, "r2vb_delivery_capture clip_bo_identity",
                                 capture_clip);
    for (unsigned i = 0; ve && i < ve->count; i++) {
        unsigned vbi = ve->velem[i].vertex_buffer_index;
        char tag[64];
        snprintf(tag, sizeof(tag), "r2vb_delivery_capture vbuf_identity[%u]", i);
        r300_r2vb_report_bo_identity(r300, tag,
                                     r300->vertex_buffer[vbi].buffer.resource);
    }

    /* Semantic role sidecar (roles.log grammar): one BO -> role pair per token.
     * The position element's bound resource is the clip BO the producer wrote;
     * every other bound element is an application passthrough upload; the
     * framebuffer colour surface is the delivery render target.  RELOC lines
     * (below) share the same bo=<handle> token so the analyzer joins slot -> role;
     * the handle is the winsys BO pointer, a stable per-run key with no side
     * effects (buffer_get_handle would export a GEM name). */
    struct role_bo { struct pipe_resource *pr; const char *role; };
    struct role_bo roles[PIPE_MAX_ATTRIBS + 2];
    unsigned n_roles = 0;
    for (unsigned i = 0; ve && i < ve->count && n_roles < PIPE_MAX_ATTRIBS; i++) {
        struct pipe_resource *pr =
            r300->vertex_buffer[ve->velem[i].vertex_buffer_index].buffer.resource;
        if (!pr)
            continue;
        roles[n_roles].pr = pr;
        roles[n_roles].role =
            pr == capture_clip ? "clip_bo" : "app_upload";
        n_roles++;
    }
    {
        struct pipe_framebuffer_state *fb =
            (struct pipe_framebuffer_state *)r300->fb_state.state;
        struct pipe_resource *fbres =
            (fb && fb->nr_cbufs && fb->cbufs[0].texture) ? fb->cbufs[0].texture : NULL;
        if (fbres && n_roles < PIPE_MAX_ATTRIBS + 2) {
            roles[n_roles].pr = fbres;
            roles[n_roles].role = "app_framebuffer";
            n_roles++;
        }
    }

    fprintf(stderr, "r2vb_delivery_capture");
    for (unsigned i = 0; i < n_roles; i++) {
        struct r300_resource *rr = r300_resource(roles[i].pr);
        fprintf(stderr, " bo_%u=0x%" PRIxPTR " role_%u=%s",
                i, (uintptr_t)(rr ? rr->buf : NULL), i, roles[i].role);
    }
    fprintf(stderr, "\n");

    /* RELOC grammar: slot -> BO for every role BO currently in the command stream.
     * The slot is the winsys reloc-table index (cs_lookup_buffer), the same value
     * the DW reloc annotation carries; a resource not yet in the CS reports slot
     * -1. */
    for (unsigned i = 0; i < n_roles; i++) {
        struct r300_resource *rr = r300_resource(roles[i].pr);
        if (!rr || !rr->buf)
            continue;
        int slot = r300->rws->cs_lookup_buffer(&r300->cs, rr->buf);
        enum radeon_bo_domain dom = r300->rws->buffer_get_initial_domain(rr->buf);
        fprintf(stderr,
                "RELOC %d bo=0x%" PRIxPTR " domain=%s offset=0x%x role=%s\n",
                slot, (uintptr_t)rr->buf,
                dom == RADEON_DOMAIN_VRAM ? "vram" :
                dom == RADEON_DOMAIN_GTT ? "gtt" :
                dom == RADEON_DOMAIN_VRAM_GTT ? "vram_gtt" : "other",
                r300->rws->buffer_get_reloc_offset(rr->buf), roles[i].role);
    }

    /* The delivery path rebinds the application FS and marks the framebuffer /
     * rasteriser atoms dirty before it runs, so the producer FS microcode and the
     * producer's hand-rolled framebuffer registers are re-emitted with application
     * values and do not appear in this delivery IB.  The RAW_IB decode below is the
     * byte-level check. */
    fprintf(stderr,
            "r2vb_delivery_capture producer_fs_in_final_ib=0 producer_fb_in_final_ib=0 "
            "basis=app_state_rebound_before_delivery\n");

    /* RAW_IB grammar: the unpatched command stream, one DW line per dword.  A
     * dword immediately following a 0xc0001000 PKT3_NOP is an r300 relocation slot
     * (OUT_CS_RELOC writes the marker then cs_lookup_buffer(buf)*4), so its slot is
     * value/4 -- the winsys reloc-table index, not a physical address, which does
     * not exist until DRM_RADEON_CS patches the IB.  The NOOP flush never submits,
     * so this IB is the raw artifact.  R300_TRACE additionally writes the winsys
     * binary trace during the same flush. */
    {
        unsigned cdw = r300->cs.current.cdw;
        const uint32_t *buf = r300->cs.current.buf;
        unsigned nrelocs = 0;
        for (unsigned j = 1; j < cdw; j++)
            if (buf[j - 1] == 0xc0001000)
                nrelocs++;
        fprintf(stderr, "RAW_IB cdw=%u relocs=%u\n", cdw, nrelocs);
        for (unsigned j = 0; j < cdw; j++) {
            bool is_reloc = j > 0 && buf[j - 1] == 0xc0001000;
            if (is_reloc)
                fprintf(stderr, "DW %u 0x%08x reloc=%u\n", j, buf[j], buf[j] / 4);
            else
                fprintf(stderr, "DW %u 0x%08x\n", j, buf[j]);
        }
    }
}

bool r300_r2vb_exec_passthrough_draw(struct r300_context *r300,
                                            const struct pipe_draw_info *info,
                                            const struct pipe_draw_start_count_bias *draw)
{
    /* Capture+decode mode: dump the routing state and fall back to gallivm (which
     * renders correctly), so the suspected-hang emit never reaches the GPU. */
    if (getenv("R300_R2VB_INSPECT")) {
        r300_r2vb_inspect_passthrough(r300);
        return false;
    }

    /* Producer-fed delivery capture (R300_R2VB_DELIVERY_CAPTURE=1): build the full
     * final delivery command stream, record its structure and BO identities, then
     * discard it with a RADEON_FLUSH_NOOP flush so no IB reaches DRM_RADEON_CS.
     * Confined to a producer-fed delivery (r2vb_producer_kind != NONE): the
     * position-only invariant's clip-BO clause has no meaning for a pure-passthrough
     * app-buffer draw, which has no producer pass and no clip BO.  Gate unset ->
     * the whole block is inert and the route stays byte-identical.  The producer
     * kind is consumed here (reset to NONE) so a stale SPLIT cannot label a later
     * pure-passthrough delivery. */
    static int delivery_capture_env = -1;
    if (delivery_capture_env < 0) {
        const char *e = getenv("R300_R2VB_DELIVERY_CAPTURE");
        delivery_capture_env = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    enum r300_r2vb_producer_kind producer_kind = r300->r2vb_producer_kind;
    struct pipe_resource *capture_clip = r300->r2vb_capture_clip;
    r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
    r300->r2vb_capture_clip = NULL;
    bool capture = delivery_capture_env &&
                   producer_kind != R300_R2VB_PRODUCER_NONE;

#define R2VB_BAIL(reason) do { if (getenv("R300_R2VB_ROUTE_DEBUG")) \
    fprintf(stderr, "r2vb_passthrough_fallback reason=%s nvb=%u\n", (reason), \
            r300->nr_vertex_buffers); return false; } while (0)

    if (r300_vs(r300)->shader->dummy)
        R2VB_BAIL("dummy_vs");

    unsigned nvb = r300->nr_vertex_buffers;
    if (nvb > PIPE_MAX_ATTRIBS || !r300->uploader)
        R2VB_BAIL("nvb_or_uploader");

    struct pipe_vertex_buffer saved[PIPE_MAX_ATTRIBS];
    struct pipe_resource *uploaded[PIPE_MAX_ATTRIBS] = {0};
    bool swapped[PIPE_MAX_ATTRIBS] = {0};
    bool any_swap = false, ok = true;

    if (!r300->velems || r300->velems->count == 0)
        R2VB_BAIL("no_velems");

    /* r300_emit_buffer_validate (reached via PREP_VALIDATE_VBOS) iterates ALL of
     * vertex_buffer[0..nr_vertex_buffers] and adds buffer.resource.  In the SWTCL
     * context that array carries user buffers (the union aliases a CPU pointer)
     * and stale slots from earlier binds (a dangling resource whose ->buf is
     * NULL), either of which faults cs_add_buffer.  So normalise every slot the
     * loop will touch: a velem-referenced user buffer is uploaded to a BO; every
     * UNREFERENCED slot is NULLed so the validate loop skips it.  Velem-referenced
     * real-BO slots are left as-is (they carry a valid ->buf).  Restored after. */
    bool referenced[PIPE_MAX_ATTRIBS] = {0};
    unsigned ref_stride[PIPE_MAX_ATTRIBS] = {0};
    for (unsigned i = 0; i < r300->velems->count; i++) {
        unsigned vbi = r300->velems->velem[i].vertex_buffer_index;
        if (vbi >= nvb)
            R2VB_BAIL("velem_vbi_oob"); /* element points outside the bound buffers */
        referenced[vbi] = true;
        ref_stride[vbi] = MAX2(ref_stride[vbi], r300->velems->velem[i].src_stride);
    }

    for (unsigned vbi = 0; vbi < nvb && ok; vbi++) {
        struct pipe_vertex_buffer *vb = &r300->vertex_buffer[vbi];

        if (!referenced[vbi]) {
            if (vb->is_user_buffer || vb->buffer.resource) {
                saved[vbi] = *vb;
                swapped[vbi] = true;
                any_swap = true;
                vb->is_user_buffer = false;
                vb->buffer_offset = 0;
                vb->buffer.resource = NULL;
            }
            continue;
        }

        /* Determine the vertex data source for this referenced slot:
         *  - a user buffer: the CPU pointer directly;
         *  - a real-BO resource (->buf set): used as-is, no upload;
         *  - a CPU-only resource (malloced_buffer, ->buf NULL): r300's SWTCL path
         *    keeps real-resource vertex buffers as a CPU shadow with no winsys BO
         *    (r300_set_vertex_buffers_swtcl), which is what r3v binds; upload
         *    that shadow.
         * Anything else cannot be re-ingested -> fall back. */
        const void *cpu_src = NULL;
        bool real_bo = false;
        if (vb->is_user_buffer) {
            cpu_src = vb->buffer.user;
        } else if (vb->buffer.resource) {
            struct r300_resource *rr = r300_resource(vb->buffer.resource);
            if (rr->buf)
                real_bo = true;
            else
                cpu_src = rr->malloced_buffer;
        }

        if (real_bo)
            continue; /* already a BO; the validate loop will add it */

        if (!cpu_src || !ref_stride[vbi]) {
            if (getenv("R300_R2VB_ROUTE_DEBUG"))
                fprintf(stderr, "r2vb_passthrough_fallback reason=no_cpu_src vbi=%u res=%p user=%d\n",
                        vbi, (void *)vb->buffer.resource, vb->is_user_buffer);
            ok = false;
            break;
        }

        unsigned size = vb->buffer_offset + (draw->start + draw->count) * ref_stride[vbi];
        unsigned out_off = 0;
        struct pipe_resource *out_res = NULL;
        /* _ref so out_res gains a reference we own; the CS keeps its own via
         * cs_add_buffer during the emit, so dropping ours afterward is safe. */
        u_upload_data_ref(r300->uploader, 0, size, 4, cpu_src, &out_off, &out_res);
        if (!out_res) {
            ok = false;
            break;
        }
        saved[vbi] = *vb;
        swapped[vbi] = true;
        any_swap = true;
        uploaded[vbi] = out_res;
        vb->is_user_buffer = false;
        vb->buffer_offset = out_off + saved[vbi].buffer_offset;
        vb->buffer.resource = out_res;
    }

    if (ok) {
        if (any_swap)
            u_upload_unmap(r300->uploader);
        /* Force the vertex-array validate + emit to pick up the swapped buffers
         * (r300_emit_buffer_validate adds the BOs only when this is set). */
        r300->vertex_arrays_dirty = true;
        /* SWTCL leaves velems->format_size[] and ->vertex_size_dwords zero:
         * r300_create_vertex_elements_state fills them only under has_tcl, but
         * r300_emit_vertex_arrays (reached here via PREP_EMIT_VARRAYS) emits each
         * array's LOAD_VBPNTR SIZE field from format_size[i].  A zero SIZE makes
         * the VAP fetch zero dwords per array -- the malformed fetch behind the
         * suspected stall.  Populate them from the bound element formats using
         * the same align(blocksize, 4) the has_tcl path computes; this is derived
         * per-CSO data, so filling it is idempotent and the gallivm SWTCL path
         * does not read it (it fetches through r300->vertex_info instead).
         *
         * VAP_VTX_SIZE is the matching per-vertex dword count the VAP fetches
         * under TCL_BYPASS.  Neither r300_emit_states (no vs_state when !has_tcl)
         * nor r300_emit_draw_arrays emits it, and the inherited value is the
         * gallivm draw-module output size, so set it explicitly alongside. */
        unsigned vap_vtx_size = 0;
        r300->velems->vertex_size_dwords = 0;
        for (unsigned i = 0; i < r300->velems->count; i++) {
            unsigned fs =
                align(util_format_get_blocksize(r300->velems->velem[i].src_format), 4);
            r300->velems->format_size[i] = fs;
            r300->velems->vertex_size_dwords += fs / 4;
            vap_vtx_size += fs / 4;
        }
        /* Producer-fed delivery capture preflight: the position-only invariant
         * runs before any emission, so a violation refuses the delivery and the
         * capture and falls back to gallivm exactly as the guards above do. */
        if (capture && !r300_r2vb_capture_preflight(r300, vap_vtx_size, producer_kind,
                                                     capture_clip))
            ok = false;

        if (ok) {
        /* Viewport transform.  gallivm's draw module applies the viewport on the
         * CPU and emits window-space vertices, so the SWTCL path sets VAP_VTE_CNTL
         * to VTX_XY_FMT|VTX_Z_FMT (pre-divided, no HW transform) and returns
         * early without populating the viewport scale/offset
         * (r300_set_viewport_states, the if(r300->draw) branch).  The direct-VB
         * route instead feeds the application's clip-space vertices, so the VAP
         * must run the hardware viewport transform: VTX_W0_FMT does the perspective
         * divide, and the VPORT scale/offset map NDC to window.  Emit the transform
         * for this draw from r300->viewport (the same values the has_tcl branch
         * would program), then restore VTX_XY_FMT after the draw so the next gallivm
         * draw -- whose vertices are already window-space -- is not transformed
         * twice.  Reserve covers VAP_VTX_SIZE(2) + VPORT seq(7) + VTE(2) + the
         * restore(2) + emit_draw_arrays. */
        const struct pipe_viewport_state *vp = &r300->viewport;
        float vport6[6] = {vp->scale[0], vp->translate[0], vp->scale[1],
                           vp->translate[1], vp->scale[2], vp->translate[2]};
        if (r300_prepare_for_rendering(r300,
                                       PREP_EMIT_STATES | PREP_VALIDATE_VBOS | PREP_EMIT_VARRAYS,
                                       NULL, 24 + (r300->r2vb_reingest_barrier ? 8 : 0),
                                       draw->start, 0, -1)) {
            CS_LOCALS(r300);
            BEGIN_CS(2 + 7 + 2);
            OUT_CS_REG(R300_VAP_VTX_SIZE, vap_vtx_size);
            OUT_CS_REG_SEQ(R300_SE_VPORT_XSCALE, 6);
            OUT_CS_TABLE(vport6, 6);
            /* Coordinate-space contract: a window-space source (the producer
             * already applied divide + viewport) fetches verbatim -- XY/Z format
             * bits bypass the viewport transform and W0 bypasses the divide.
             * A clip-space source runs the full hardware transform. */
            OUT_CS_REG(R300_VAP_VTE_CNTL,
                       r300->r2vb_source_window
                           ? (R300_VTX_XY_FMT | R300_VTX_Z_FMT | R300_VTX_W0_FMT)
                           : (R300_VTX_W0_FMT | R300_VPORT_X_SCALE_ENA |
                              R300_VPORT_X_OFFSET_ENA | R300_VPORT_Y_SCALE_ENA |
                              R300_VPORT_Y_OFFSET_ENA | R300_VPORT_Z_SCALE_ENA |
                              R300_VPORT_Z_OFFSET_ENA));
            END_CS;
            /* Single-CS R2VB MVP re-ingest: the producer wrote the transformed
             * positions into this vertex buffer through the RB3D colour cache
             * earlier in the same command stream, and prepare_for_rendering's
             * dirty-state re-emit (above) sits between emit_producer's barrier
             * and this draw.  Re-assert that barrier here, adjacent to the fetch:
             * flush the colour cache to memory, drain the 3D pipe, and sync the
             * VAP vertex-fetch engine so it reads the transform instead of stale
             * vertex-cache content.  Mirrors the sequence in r300_r2vb_emit_producer. */
            if (r300->r2vb_reingest_barrier) {
                BEGIN_CS(8);
                OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT,
                           R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
                               R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
                OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT,
                           R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D |
                               R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS);
                OUT_CS_REG(RADEON_WAIT_UNTIL, RADEON_WAIT_3D_IDLECLEAN);
                OUT_CS_REG(R300_VAP_PVS_STATE_FLUSH_REG, 0x0);
                END_CS;
            }
            /* Dword offset of the 3D_DRAW_VBUF_2 packet for the capture record,
             * read before the draw emit appends it. */
            unsigned draw_pkt_off = r300->cs.current.cdw;
            r300_emit_draw_arrays(r300, info->mode, draw->count);
            BEGIN_CS(2);
            OUT_CS_REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
            END_CS;
            /* Producer-fed delivery capture: the full delivery IB is now built.
             * Record its structure and BO identities while the CS still holds the
             * packets (the reloc indices go stale on flush), then discard the IB
             * with RADEON_FLUSH_NOOP -- a no-submit flush that never reaches
             * DRM_RADEON_CS (radeon_drm_cs_flush no-submit branch), so no GPU work
             * results.  The route returns handled so gallivm issues no second draw. */
            if (capture) {
                r300_r2vb_capture_record(r300, info, draw, producer_kind,
                                         capture_clip, vap_vtx_size, draw_pkt_off);
                r300->rws->cs_flush(&r300->cs, RADEON_FLUSH_NOOP, NULL);
            }
        }
        if (getenv("R300_R2VB_ROUTE_DEBUG")) {
            static bool once = false;
            if (!once) {
                once = true;
                fprintf(stderr, "r2vb_passthrough_executed count=%u uploaded_vbs=%d (direct-VB, "
                                "gallivm skipped)\n", draw->count, any_swap);
            }
        }
        } /* if (ok): producer-fed capture preflight passed or gate unset */
    }

    /* Restore the original (user) buffers and release the uploads.  Mark the
     * arrays dirty so the next draw re-emits against the restored state. */
    for (unsigned vbi = 0; vbi < nvb; vbi++) {
        if (swapped[vbi]) {
            r300->vertex_buffer[vbi] = saved[vbi];
            pipe_resource_reference(&uploaded[vbi], NULL);
        }
    }
    if (any_swap)
        r300->vertex_arrays_dirty = true;
    return ok;
}

static void r300_swtcl_draw_vbo(struct pipe_context* pipe,
                                const struct pipe_draw_info *info,
                                unsigned drawid_offset,
                                const struct pipe_draw_indirect_info *indirect,
                                const struct pipe_draw_start_count_bias *draws,
                                unsigned num_draws)
{
   if (num_draws > 1) {
      util_draw_multi(pipe, info, drawid_offset, indirect, draws, num_draws);
      return;
   }

    struct r300_context* r300 = r300_context(pipe);
    struct pipe_draw_start_count_bias draw = draws[0];

    if (r300->skip_rendering) {
        return;
    }

    if (!u_trim_pipe_prim(info->mode, &draw.count))
       return;

    if (r300_poly_stipple_masks_draw(r300, u_reduced_prim(info->mode))) {
        return;
    }

    if (info->index_size) {
        draw_set_indexes(r300->draw,
                         info->has_user_indices ?
                             info->index.user :
                             r300_resource(info->index.resource)->malloced_buffer,
                         info->index_size, ~0);
    }

    r300->point_sprite_via_draw = false;
    r300->point_sprite_sce = 0;
    if (r300->sprite_coord_enable != 0) {
        bool is_point = r300_rasterizer_emits_points(r300, info->mode);
        if (is_point != r300->is_point) {
            r300->is_point = is_point;
            r300_mark_atom_dirty(r300, &r300->rs_block_state);
        }
        /* gl_PointCoord on a SW-expanded point reaches the FS as a
         * draw-generated sprite texcoord, not HW GA point-stuffing (inert once
         * the point is a triangle pair). Snapshot the sprite-coord state now,
         * while it is correct -- the draw module rebinds a no-cull rasterizer
         * mid-draw and zeroes the live sprite_coord_enable / is_point -- so the
         * run-time rebuild in r300_render_get_vertex_info can still route the
         * PCOORD varying. The PCOORD draw output exists only at draw run time,
         * so the layout is finalized there. */
        if (is_point) {
            r300->point_sprite_via_draw = true;
            r300->point_sprite_sce = r300->sprite_coord_enable;
            r300_mark_atom_dirty(r300, &r300->rs_block_state);
        }
    }

    /* gl_FrontFacing on an R300-class SWTCL part. The rasterizer cannot route a
     * face bit to the FS (the RS WRITE_BACKFACE encoding is an R500 addition),
     * so ask the draw module to compute the face per filled triangle; the RS
     * block then routes it as a vertex texcoord into the FS face input.
     * Restricted to triangle draws (the only primitives with a meaningful
     * winding) and to non-r500 parts (r500 uses the native HW WRITE_FACE path in
     * r300_update_rs_block). On by default; R300_FRONTFACE_VIA_DRAW=0 is the
     * escape hatch. The fragment shader is not picked until
     * r300_update_derived_state below, so the actual gl_FrontFacing use is gated
     * downstream: the draw module checks its own FS copy (uses_frontface) before
     * forcing the stage, and r300_update_rs_block checks the validated FS face
     * input before routing. */
    bool frontface_via_draw = false;
    if (!r300->screen->caps.is_r500 &&
        u_reduced_prim(info->mode) == MESA_PRIM_TRIANGLES) {
        static int gate = -1;
        if (gate < 0) {
            const char *e = getenv("R300_FRONTFACE_VIA_DRAW");
            gate = (e && strcmp(e, "0") == 0) ? 0 : 1;
        }
        frontface_via_draw = gate != 0;
    }
    /* draw_enable_frontface_injection flushes the draw module, so only call it on
     * a transition -- otherwise every triangle draw pays an unconditional flush in
     * the SWTCL hot path. */
    if (frontface_via_draw != r300->frontface_via_draw)
        draw_enable_frontface_injection(r300->draw, frontface_via_draw);
    r300->frontface_via_draw = frontface_via_draw;

    /* SWTCL analytic derivatives on an R300-class part. dFdx/dFdy have no
     * hardware here, so a shader that builds a normal as
     * normalize(cross(dFdx(pos), dFdy(pos))) renders unlit unless the draw
     * module supplies the per-triangle gradient. Same gating shape as
     * frontface: triangle draws, non-r500, default on with R300_DERIV_VIA_DRAW=0
     * as the escape hatch. The RS block reads this flag; the actual draw
     * injection is enabled after the FS is picked (its recorded generic indices
     * name the differentiated varying). */
    bool derivative_via_draw = false;
    if (!r300->screen->caps.is_r500 &&
        u_reduced_prim(info->mode) == MESA_PRIM_TRIANGLES) {
        static int gate = -1;
        if (gate < 0) {
            const char *e = getenv("R300_DERIV_VIA_DRAW");
            gate = (e && strcmp(e, "0") == 0) ? 0 : 1;
        }
        derivative_via_draw = gate != 0;
    }
    r300->derivative_via_draw = derivative_via_draw;

    r300_update_pstipple_draw(r300, info->mode);

    r300_update_derived_state(r300);

    /* >64-ALU FS multipass: if the picked fragment shader split into two passes,
     * run the 2-pass scratch-RT draw instead of a single draw (gated, default off;
     * the in_multipass guard stops the wrapper's inner draws re-entering here). */
    if (!r300->in_multipass) {
        struct r300_fragment_shader_code *mp = r300_fs(r300)->shader;
        if (mp && mp->multipass_pass_b) {
            r300_fs_multipass_draw(pipe, info, drawid_offset, indirect,
                                   draws, num_draws, mp);
            return;
        }
    }

    /* With the FS now picked, enable (or update) the draw-module derivative
     * injection for shaders that actually read a derivative. deriv_src_generic
     * is -1 for shaders without one; the gradient generics (ddx/ddy) are fixed,
     * so a change of the differentiated varying is the only re-enable trigger.
     * draw_enable flushes the draw module, so only call it on a transition. */
    {
        struct r300_fragment_shader_code *fscode = r300_fs(r300)->shader;
        bool want = derivative_via_draw && fscode &&
                    fscode->deriv_src_generic >= 0;
        /* -1 is the injection-off sentinel: a TEXn source maps to generic
         * index 0, so 0 is a real index and cannot mark the off state. */
        int want_src = want ? fscode->deriv_src_generic : -1;
        if (want_src != r300->draw_deriv_src) {
            draw_enable_derivative_injection(
                r300->draw, want,
                want ? fscode->deriv_src_generic : -1,
                want ? fscode->deriv_ddx_generic : -1,
                want ? fscode->deriv_ddy_generic : -1);
            r300->draw_deriv_src = want_src;
        }
        if (getenv("R300_DERIV_DEBUG"))
            fprintf(stderr, "r300 deriv: draw_vbo enable: via_draw=%d fs=%p "
                    "deriv_src_generic=%d want=%d\n",
                    derivative_via_draw, (void *)fscode,
                    fscode ? fscode->deriv_src_generic : -2, want);
    }

    /* RS482 fragment-ALU R2VB vertex route (experiment-gated by R300_R2VB_ROUTE).
     * Classifies the draw against the simple-draw class and, once the producer is
     * built, would transform + re-ingest it instead of running the gallivm CPU
     * draw module.  Returns false today (classifier only), so this falls through
     * to gallivm with zero behaviour change.  This is the single choke point for
     * both GL and r3v-Vulkan draws -- r3v replays through this same gallium
     * draw_vbo, so no separate Vulkan-side wiring is needed. */
    /* Clear the producer-fed discriminator for this draw before routing: only a
     * producer pass that runs during this draw's route sets SINGLE or SPLIT, so a
     * producer that ran for an earlier draw but whose re-ingest refused delivery
     * cannot mislabel this one's delivery capture. */
    r300->r2vb_producer_kind = R300_R2VB_PRODUCER_NONE;
    r300->r2vb_capture_clip = NULL;

    /* Passthrough direct-VB route: re-ingest the app vertex arrays at TCL_BYPASS,
     * skipping the gallivm draw module.  Falls back to gallivm if the route
     * declines or cannot execute the draw. */
    if (r300_r2vb_route_draw(r300, info, &draw) &&
        r300_r2vb_exec_passthrough_draw(r300, info, &draw))
        return;

    /* MVP route (gl_Position = M * in_pos on the fragment ALU), separately gated
     * by R300_R2VB_MVP_EXEC.  Returns false until the re-ingest half is built, so
     * this falls through to gallivm with no behaviour change. */
    if (r300_r2vb_route_mvp(r300, info, &draw) &&
        r300_r2vb_exec_mvp_draw(r300, info, &draw))
        return;

    draw_vbo(r300->draw, info, drawid_offset, NULL, &draw, 1, 0);
    draw_flush(r300->draw);
}

/* Object for rendering using Draw. */
struct r300_render {
    /* Parent class */
    struct vbuf_render base;

    /* Pipe context */
    struct r300_context* r300;

    /* Vertex information */
    size_t vertex_size;
    unsigned prim;
    unsigned hwprim;

    /* VBO */
    size_t vbo_max_used;
    uint8_t *vbo_ptr;

    /* SW-TCL GART backpressure.  gtt_drain_mark is the RADEON_GTT_USAGE level after
     * the last drain; a drain fires when usage grows a budget past it, which measures
     * a draw's own GTT churn and ignores the app's resident GTT.  gtt_drain_streak
     * counts consecutive drains that did not bound the working set, tripping a hard
     * cap so an unbounded SW-TCL draw fails cleanly instead of OOM-ing the host. */
    uint64_t gtt_drain_mark;
    unsigned gtt_drain_streak;
    /* Drops the draw's CS submission in r300_render_draw_{arrays,elements} once
     * gtt_drain_streak reaches the cap.  The vbuf stage maps the vertex buffer after
     * every allocate_vertices, so the draw is dropped at submission.  Reset per draw. */
    bool gtt_budget_exceeded;
};

static inline struct r300_render*
r300_render(struct vbuf_render* render)
{
    return (struct r300_render*)render;
}

static const struct vertex_info*
r300_render_get_vertex_info(struct vbuf_render* render)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;

    /* The draw module calls this once per draw before the vbuf stage allocates: reset
     * the per-draw GART backpressure budget here.  The drain streak and drop flag are
     * per-draw; gtt_drain_mark persists as the running GTT level across draws. */
    r300render->gtt_drain_streak = 0;
    r300render->gtt_budget_exceeded = false;

    /* The wide-point stage allocates the gl_PointCoord (PCOORD) sprite vertex
     * output during the pipeline run, after r300_update_derived_state already
     * built the layout at draw prepare with no such output. The unfilled stage
     * allocates the gl_FrontFacing (FACE) output the same way. The vbuf stage
     * (draw_pipe_vbuf.c vbuf_start_prim) calls this once the extra exists, so
     * rebuild the layout to fold the draw-generated output into the HW vertex
     * and RS routing, then re-dirty the RS block so the run-time version is
     * emitted. in_swtcl_layout_rebuild guards re-entry from the atom dirtying. */
    struct r300_fragment_shader_code *deriv_fs = r300_fs(r300)->shader;
    const int deriv_ddx_g =
        deriv_fs ? deriv_fs->deriv_ddx_generic : -1;
    if (getenv("R300_DERIV_DEBUG") && r300->derivative_via_draw && deriv_ddx_g >= 0)
        fprintf(stderr, "r300 deriv: get_vertex_info: ddx_generic=%d "
                "draw_output=%d\n", deriv_ddx_g,
                draw_find_shader_output(r300->draw, TGSI_SEMANTIC_GENERIC,
                                        deriv_ddx_g));
    if (!r300->in_swtcl_layout_rebuild &&
        ((r300->point_sprite_via_draw &&
          draw_find_shader_output(r300->draw, TGSI_SEMANTIC_PCOORD, 0) >= 0) ||
         (r300->frontface_via_draw &&
          draw_find_shader_output(r300->draw, TGSI_SEMANTIC_FACE, 0) >= 0) ||
         (r300->derivative_via_draw && deriv_ddx_g >= 0 &&
          draw_find_shader_output(r300->draw, TGSI_SEMANTIC_GENERIC,
                                  deriv_ddx_g) >= 0))) {
        r300->in_swtcl_layout_rebuild = true;
        r300_swtcl_rebuild_vertex_layout(r300);
        r300_mark_atom_dirty(r300, &r300->rs_block_state);
        r300->in_swtcl_layout_rebuild = false;
    }

    return &r300->vertex_info;
}

/* SW-TCL GART backpressure budget: drain once a draw has grown this many bytes of GTT
 * past the post-last-drain mark, and hard-cap the draw after this many consecutive
 * drains fail to bound the working set.  32 MiB x 4 keeps worst-case pinned-GART growth
 * near 128 MiB -- safe on the smallest UMA part -- while a draw the GPU keeps pace with
 * never drains. */
#define R300_SWTCL_GTT_DRAIN_BUDGET     (32ull * 1024 * 1024)
#define R300_SWTCL_GTT_DRAIN_STREAK_CAP 4

static bool r300_render_allocate_vertices(struct vbuf_render* render,
                                          uint16_t vertex_size,
                                          uint16_t count)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;
    struct radeon_winsys *rws = r300->rws;
    size_t size = (size_t)vertex_size * (size_t)count;

    DBG(r300, DBG_DRAW, "r300: render_allocate_vertices (size: %d)\n", size);

    if (!r300->vbo || size + r300->draw_vbo_offset > r300->vbo->size) {
        /* A fresh GTT vertex buffer pins another R300_MAX_DRAW_VBO_SIZE of GART, which
         * on UMA RS480/RS485 is system DRAM.  A single very large SW-TCL primitive (a
         * 2^19-point wide-point draw splits into hundreds of vsplit segments) cycles
         * this path and its companion u_upload index buffers hundreds of times, and the
         * GPU keeps reading each buffer long enough that the pb_cache allocates a fresh
         * one each time; the working set climbs until the kernel OOM-kills the session.
         * Measuring growth from the level after the last drain leaves an app's resident
         * GTT (textures) out of the budget.  Once this draw grows a budget past that
         * mark, a flush to GPU retirement idles every in-flight GTT buffer -- the vertex
         * buffer and the index uploads alike -- and the winsys recycles them.  The query
         * runs on the rare path where a >= 1 MB vertex buffer has filled, so a draw the
         * GPU keeps pace with pays nothing.  Hardware-TCL parts run a different draw
         * path and reach this code only through software fallback. */
        uint64_t gtt_now = rws->query_value(rws, RADEON_GTT_USAGE);
        if (r300render->gtt_drain_mark == 0) {
            /* First realloc of this render object: record the app's resident GTT as
             * the baseline, so the budget measures this draw's own growth. */
            r300render->gtt_drain_mark = gtt_now;
        } else if (gtt_now > r300render->gtt_drain_mark + R300_SWTCL_GTT_DRAIN_BUDGET) {
            struct pipe_fence_handle *fence = NULL;
            r300_flush(&r300->context, PIPE_FLUSH_ASYNC, &fence);
            if (fence) {
                rws->fence_wait(rws, fence, OS_TIMEOUT_INFINITE);
                rws->fence_reference(rws, &fence, NULL);
            }
            r300render->gtt_drain_mark = rws->query_value(rws, RADEON_GTT_USAGE);
            r300render->gtt_drain_streak++;
        } else {
            if (gtt_now < r300render->gtt_drain_mark)
                r300render->gtt_drain_mark = gtt_now;
            r300render->gtt_drain_streak = 0;
        }

        /* Hard cap for a draw whose GART working set keeps growing across drains: the
         * GPU holds the retired buffers past the point a fence wait reclaims them.
         * gtt_budget_exceeded drops the remaining submissions in
         * r300_render_draw_{arrays,elements}; those vertex buffers stay off the GPU, so
         * the winsys recycles them and GART growth stops.  The vbuf stage maps the
         * vertex buffer after every allocate_vertices, so the drop lands at submission.
         * A 2^19-point SW-TCL draw on a UMA part exceeds device memory; the dropped
         * draw keeps the session alive (correct-or-reject). */
        if (r300render->gtt_drain_streak >= R300_SWTCL_GTT_DRAIN_STREAK_CAP)
            r300render->gtt_budget_exceeded = true;

	radeon_bo_reference(r300->rws, &r300->vbo, NULL);
        r300->vbo = NULL;
        r300render->vbo_ptr = NULL;

        r300->vbo = rws->buffer_create(rws,
                                       MAX2(R300_MAX_DRAW_VBO_SIZE, size),
                                       R300_BUFFER_ALIGNMENT,
                                       RADEON_DOMAIN_GTT,
                                       RADEON_FLAG_NO_INTERPROCESS_SHARING);
        if (!r300->vbo) {
            return false;
        }
        r300->draw_vbo_offset = 0;
        r300render->vbo_ptr = rws->buffer_map(rws, r300->vbo, &r300->cs,
                                              PIPE_MAP_WRITE);
    }

    r300render->vertex_size = vertex_size;
    return true;
}

static void* r300_render_map_vertices(struct vbuf_render* render)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;

    DBG(r300, DBG_DRAW, "r300: render_map_vertices\n");

    assert(r300render->vbo_ptr);
    return r300render->vbo_ptr + r300->draw_vbo_offset;
}

static void r300_render_unmap_vertices(struct vbuf_render* render,
                                       uint16_t min,
                                       uint16_t max)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;

    DBG(r300, DBG_DRAW, "r300: render_unmap_vertices\n");

    r300render->vbo_max_used = MAX2(r300render->vbo_max_used,
                                    r300render->vertex_size * (max + 1));
}

static void r300_render_release_vertices(struct vbuf_render* render)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;

    DBG(r300, DBG_DRAW, "r300: render_release_vertices\n");

    r300->draw_vbo_offset += r300render->vbo_max_used;
    r300render->vbo_max_used = 0;
}

static void r300_render_set_primitive(struct vbuf_render* render,
                                      enum mesa_prim prim)
{
    struct r300_render* r300render = r300_render(render);

    r300render->prim = prim;
    r300render->hwprim = r300_translate_primitive(prim);
}

static void r300_render_draw_elements(struct vbuf_render* render,
                                      const uint16_t* indices,
                                      uint count);

/* Stale-VAP-fetch containment experiment gate (R300_SWTCL_INDEXED_CONTROL):
 * re-emit the SWTCL vertex-list draw as an indexed draw over the trivial
 * identity index list.  1 = PRIM_WALK_INDICES with vertex reuse enabled;
 * 2 = PRIM_WALK_INDICES with VTX_REUSE_DIS (the bit is legal only under
 * indexed walking).  Discriminates whether the VAP vertex reuse store
 * carries a predecessor client's geometry into a byte-identical vertex-list
 * draw.  0 (default) leaves the vertex-list path untouched. */
static int r300_swtcl_indexed_control(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("R300_SWTCL_INDEXED_CONTROL");
        cached = e ? atoi(e) : 0;
        if (cached < 0 || cached > 2)
            cached = 0;
    }
    return cached;
}

static void r300_render_draw_arrays(struct vbuf_render* render,
                                    unsigned start,
                                    unsigned count)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;
    uint8_t* ptr;
    unsigned i;

    /* The GART backpressure hard cap tripped this draw: drop the submission.  The
     * vertices occupy a recycled vertex buffer that stays off the GPU, so pinned GART
     * stays bounded. */
    if (r300render->gtt_budget_exceeded)
        return;

    /* Containment-experiment rewrite: the identity index list makes the draw
     * geometrically identical to the vertex-list form while switching the VAP
     * to indexed primitive walking (and, at gate 2, disabling vertex reuse). */
    if (r300_swtcl_indexed_control() && start == 0 && count <= 65535) {
        uint16_t *idx = malloc(count * sizeof(uint16_t));
        if (idx) {
            for (unsigned k = 0; k < count; k++)
                idx[k] = (uint16_t)k;
            r300_render_draw_elements(render, idx, count);
            free(idx);
            return;
        }
    }
    /* VAP_VF_CNTL packs the vertex count in a 16-bit field; only r5xx widens it
     * via R500_VAP_ALT_NUM_VERTICES.  The HWTCL emitter (r300_emit_draw_arrays)
     * already takes the alt path for count > 65535; mirror it so the SWTCL
     * backend is not the asymmetric one that silently emits a truncated count.
     * r3xx/r4xx is bounded to <= 65535 per batch by max_vertex_buffer_bytes. */
    bool alt_num_verts = r300->screen->caps.is_r500 && count > 65535;
    unsigned dwords = 6 + (alt_num_verts ? 2 : 0);

    CS_LOCALS(r300);
    (void) i; (void) ptr;

    assert(start == 0);
    /* A 65536-vertex draw with no alt support shifts to 0 in the count field
     * and underflows the kernel CS validator; refuse it rather than emit a
     * malformed IB that corrupts driver state. */
    if (!alt_num_verts && count > 65535) {
        fprintf(stderr, "r300: SWTCL draw of %u vertices exceeds the 16-bit "
                "VAP count limit on a non-r5xx part; skipping.\n", count);
        return;
    }

    DBG(r300, DBG_DRAW, "r300: render_draw_arrays (count: %d)\n", count);

    if (!r300_prepare_for_rendering(r300,
                                    PREP_EMIT_STATES | PREP_EMIT_VARRAYS_SWTCL,
                                    NULL, dwords, 0, 0, -1)) {
        return;
    }

    BEGIN_CS(dwords);
    OUT_CS_REG(R300_GA_COLOR_CONTROL,
            r300_provoking_vertex_fixes(r300, r300render->prim));
    if (alt_num_verts) {
        OUT_CS_REG(R500_VAP_ALT_NUM_VERTICES, count);
    }
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, count - 1);
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_VBUF_2, 0);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_LIST | (count << 16) |
           r300render->hwprim |
           (alt_num_verts ? R500_VAP_VF_CNTL__USE_ALT_NUM_VERTS : 0));
    END_CS;
}

static void r300_render_draw_elements(struct vbuf_render* render,
                                      const uint16_t* indices,
                                      uint count)
{
    struct r300_render* r300render = r300_render(render);
    struct r300_context* r300 = r300render->r300;

    /* The GART backpressure hard cap tripped this draw: drop the submission.  The
     * vertices occupy a recycled vertex buffer that stays off the GPU, so pinned GART
     * stays bounded. */
    if (r300render->gtt_budget_exceeded)
        return;

    unsigned max_index = (r300->vbo->size - r300->draw_vbo_offset) /
                         (r300render->r300->vertex_info.size * 4) - 1;
    struct pipe_resource *index_buffer = NULL;
    unsigned index_buffer_offset;

    CS_LOCALS(r300);
    DBG(r300, DBG_DRAW, "r300: render_draw_elements (count: %d)\n", count);

    u_upload_data_ref(r300->uploader, 0, count * 2, 4, indices,
                  &index_buffer_offset, &index_buffer);
    if (!index_buffer) {
        return;
    }

    if (!r300_prepare_for_rendering(r300,
                                    PREP_EMIT_STATES |
                                    PREP_EMIT_VARRAYS_SWTCL | PREP_INDEXED,
                                    index_buffer, 12, 0, 0, -1)) {
        pipe_resource_reference(&index_buffer, NULL);
        return;
    }

    BEGIN_CS(12);
    OUT_CS_REG(R300_GA_COLOR_CONTROL,
               r300_provoking_vertex_fixes(r300, r300render->prim));
    OUT_CS_REG(R300_VAP_VF_MAX_VTX_INDX, max_index);

    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_INDX_2, 0);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_INDICES | (count << 16) |
           r300render->hwprim |
           (r300_swtcl_indexed_control() == 2 ? R300_VAP_VF_CNTL__VTX_REUSE_DIS
                                              : 0));

    OUT_CS_PKT3(R300_PACKET3_INDX_BUFFER, 2);
    OUT_CS(R300_INDX_BUFFER_ONE_REG_WR | (R300_VAP_PORT_IDX0 >> 2));
    OUT_CS(index_buffer_offset);
    OUT_CS((count + 1) / 2);
    OUT_CS_RELOC(r300_resource(index_buffer));
    END_CS;

    pipe_resource_reference(&index_buffer, NULL);
}

static void r300_render_destroy(struct vbuf_render* render)
{
    FREE(render);
}

static struct vbuf_render* r300_render_create(struct r300_context* r300)
{
    struct r300_render* r300render = CALLOC_STRUCT(r300_render);

    r300render->r300 = r300;

    /* The draw module derives the per-batch vertex count from this byte budget
     * (max_vertex_buffer_bytes / vertex_size).  The smallest SWTCL vertex is the
     * mandatory 4-float clip position (16 bytes), so a 1 MiB budget yields up to
     * 65536 vertices -- one past the 16-bit VAP_VF_CNTL count field.  r5xx widens
     * that field with R500_VAP_ALT_NUM_VERTICES (handled in
     * r300_render_draw_arrays); r3xx/r4xx cannot, so a 65536-vertex batch would
     * shift to 0 in the field and make the kernel CS validator's (nverts-1) size
     * check underflow.  Cap the r3xx/r4xx budget so the worst-case 16-byte vertex
     * stays at or below 65535 per batch. */
    r300render->base.max_vertex_buffer_bytes =
        r300->screen->caps.is_r500 ? R300_MAX_DRAW_VBO_SIZE : 65535 * 16;
    r300render->base.max_indices = 16 * 1024;

    r300render->base.get_vertex_info = r300_render_get_vertex_info;
    r300render->base.allocate_vertices = r300_render_allocate_vertices;
    r300render->base.map_vertices = r300_render_map_vertices;
    r300render->base.unmap_vertices = r300_render_unmap_vertices;
    r300render->base.set_primitive = r300_render_set_primitive;
    r300render->base.draw_elements = r300_render_draw_elements;
    r300render->base.draw_arrays = r300_render_draw_arrays;
    r300render->base.release_vertices = r300_render_release_vertices;
    r300render->base.destroy = r300_render_destroy;

    return &r300render->base;
}

struct draw_stage* r300_draw_stage(struct r300_context* r300)
{
    struct vbuf_render* render;
    struct draw_stage* stage;

    render = r300_render_create(r300);

    if (!render) {
        return NULL;
    }

    stage = draw_vbuf_stage(r300->draw, render);

    if (!stage) {
        render->destroy(render);
        return NULL;
    }

    draw_set_render(r300->draw, render);

    return stage;
}

/****************************************************************************
 *                         End of SW TCL functions                          *
 ***************************************************************************/

/* This functions is used to draw a rectangle for the blitter module.
 *
 * If we rendered a quad, the pixels on the main diagonal
 * would be computed and stored twice, which makes the clear/copy codepaths
 * somewhat inefficient. Instead we use a rectangular point sprite. */
void r300_blitter_draw_rectangle(struct blitter_context *blitter,
                                 void *vertex_elements_cso,
                                 blitter_get_vs_func get_vs,
                                 int x1, int y1, int x2, int y2,
                                 float depth, unsigned num_instances,
                                 enum blitter_attrib_type type,
                                 const struct blitter_attrib *attrib)
{
    struct r300_context *r300 = r300_context(util_blitter_get_pipe(blitter));
    unsigned last_sprite_coord_enable = r300->sprite_coord_enable;
    unsigned last_is_point = r300->is_point;
    /* We othewise always scissor to the viewport, but blits ignore it. */
    struct pipe_scissor_state last_vp_scissor = r300->viewport_scissor;
    r300->viewport_scissor = (struct pipe_scissor_state){0, 0, 16384, 16384};
    unsigned width = x2 - x1;
    unsigned height = y2 - y1;
    unsigned vertex_size = !r300->draw ? 8 : 4;
    unsigned dwords = 15 + vertex_size +
                      (type == UTIL_BLITTER_ATTRIB_TEXCOORD_XY ? 7 : 0);
    CS_LOCALS(r300);

    /* This function renders the rectangle as a single hardware point that the
     * GA expands to GA_POINT_SIZE (the "rectangular point sprite" above), and
     * for TEXCOORD_XY it additionally turns on GB_POINT_STUFF_ENABLE so the GA
     * stuffs texcoords across the expanded point.  That point-sprite vertex
     * frontend path locks up SWTCL parts: the ATTRIB_NONE case has fallen back
     * to the plain-quad util_blitter path since 2013 (7969b567bd43, "fix a
     * lockup in MSAA resolve"), but the TEXCOORD_XY case was left on the
     * point-sprite path.  On RS480-class SWTCL (has_tcl=false) a TEXCOORD_XY
     * blit -- e.g. a format-converting texture copy driving a texture upload or
     * readback -- wedges the vertex frontend (RBBM latches CP+VAP+GA busy,
     * backend idle) before any application draw.  Route TEXCOORD_XY through the
     * plain-quad path too on SWTCL, which draws a real two-triangle quad with
     * per-vertex texcoords and completes.  Also fall back for TEXCOORD_XYZW and
     * instanced draws this function does not handle. */

    /* R300_SWTCL_WEDGE_TEXCOORD_BLIT re-emits the pre-#996 hazardous path: it
     * keeps a SWTCL TEXCOORD_XY rectangle on the point-sprite frontend so it
     * recreates the exact non-draining VAP/GA wedge (RBBM CP+VAP+GA busy,
     * backend idle) that #996 fixed.  This is the fault source for the RS480
     * wedged-3D reset rung (WD3B); the plain-quad fallback stays the default and
     * every other caller is unaffected.  It prints the blit tuple before submit
     * so the run proves it exercised the hazardous path. */
    bool swtcl_wedge_texcoord =
        !r300->screen->caps.has_tcl &&
        type == UTIL_BLITTER_ATTRIB_TEXCOORD_XY &&
        debug_get_bool_option("R300_SWTCL_WEDGE_TEXCOORD_BLIT", false);
    if (swtcl_wedge_texcoord)
        fprintf(stderr,
                "[R300_SWTCL_WEDGE] pre-#996 TEXCOORD_XY point-sprite blit: "
                "has_tcl=0 type=TEXCOORD_XY rect=(%d,%d)-(%d,%d) -> native "
                "point-sprite frontend (expected VAP/GA wedge)\n",
                x1, y1, x2, y2);

    if ((!r300->screen->caps.has_tcl &&
         (type == UTIL_BLITTER_ATTRIB_NONE ||
          (type == UTIL_BLITTER_ATTRIB_TEXCOORD_XY && !swtcl_wedge_texcoord))) ||
        type == UTIL_BLITTER_ATTRIB_TEXCOORD_XYZW ||
        num_instances > 1) {
        util_blitter_draw_rectangle(blitter, vertex_elements_cso, get_vs,
                                    x1, y1, x2, y2,
                                    depth, num_instances, type, attrib);
        return;
    }

    if (r300->skip_rendering)
        return;

    r300->context.bind_vertex_elements_state(&r300->context, vertex_elements_cso);
    r300->context.bind_vs_state(&r300->context, get_vs(blitter));

    if (type == UTIL_BLITTER_ATTRIB_TEXCOORD_XY) {
        /* The blitter's passthrough VS outputs GENERIC[0], which u_blitter
         * encodes here as sprite_coord_enable bit 0. After
         * ntr_fixup_varying_slots in nir_to_rc, the corresponding FS input
         * lands at index 9 in fs_inputs->generic[] (VAR0 -> VAR9 from the
         * +9 shift that leaves room for TEX0..TEX7 and PNTC). Match that
         * by setting bit 9 instead of bit 0; the rest of the rasterizer
         * setup (r300_state_derived.c) walks generic[i] and tests
         * sprite_coord_enable & (1 << i) so the indices need to agree.
         */
        r300->sprite_coord_enable = 1 << 9;
        r300->is_point = true;
    }

    /* SW point-sprite gl_PointCoord routing is a property of the gallium draw
     * path; the blitter emits its own HW point with GA point-stuffing, so clear
     * any value left over from a prior point draw before the derived-state
     * rebuild so it does not divert this RS block. */
    r300->point_sprite_via_draw = false;
    /* Same staleness class for polygon stipple: this path bypasses the
     * draw_vbo entry that derives pstipple_draw, so a value left over from a
     * stippled app draw would compile the blitter's own fragment shader with
     * the stipple lowering.  The blitter never stipples. */
    if (r300->pstipple_draw) {
        r300->pstipple_draw = false;
        r300_mark_atom_dirty(r300, &r300->textures_state);
        if (r300->fs_status == FRAGMENT_SHADER_VALID)
            r300->fs_status = FRAGMENT_SHADER_MAYBE_DIRTY;
    }
    r300_update_derived_state(r300);

    /* Mark some states we don't care about as non-dirty. */
    r300->viewport_state.dirty = false;

    if (!r300_prepare_for_rendering(r300, PREP_EMIT_STATES, NULL, dwords, 0, 0, -1))
        goto done;

    DBG(r300, DBG_DRAW, "r300: draw_rectangle\n");

    BEGIN_CS(dwords);
    /* Set up GA. */
    OUT_CS_REG(R300_GA_POINT_SIZE, (height * 6) | ((width * 6) << 16));
    OUT_CS_REG(R300_SC_CLIP_RULE, r300->scissor_enabled ? 0xAAAA : 0xFFFF);

    if (type == UTIL_BLITTER_ATTRIB_TEXCOORD_XY) {
        /* Set up the GA to generate texcoords. */
        OUT_CS_REG(R300_GB_ENABLE, R300_GB_POINT_STUFF_ENABLE |
                   (R300_GB_TEX_STR << R300_GB_TEX0_SOURCE_SHIFT));
        OUT_CS_REG_SEQ(R300_GA_POINT_S0, 4);
        OUT_CS_32F(attrib->texcoord.x1);
        OUT_CS_32F(attrib->texcoord.y2);
        OUT_CS_32F(attrib->texcoord.x2);
        OUT_CS_32F(attrib->texcoord.y1);
    }

    /* Set up VAP controls. */
    OUT_CS_REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
    OUT_CS_REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
    OUT_CS_REG(R300_VAP_VTX_SIZE, vertex_size);
    OUT_CS_REG_SEQ(R300_VAP_VF_MAX_VTX_INDX, 2);
    OUT_CS(1);
    OUT_CS(0);

    /* Draw. */
    OUT_CS_PKT3(R300_PACKET3_3D_DRAW_IMMD_2, vertex_size);
    OUT_CS(R300_VAP_VF_CNTL__PRIM_WALK_VERTEX_EMBEDDED | (1 << 16) |
           R300_VAP_VF_CNTL__PRIM_POINTS);

    OUT_CS_32F(x1 + width * 0.5f);
    OUT_CS_32F(y1 + height * 0.5f);
    OUT_CS_32F(depth);
    OUT_CS_32F(1);

    if (vertex_size == 8) {
        static const float zeros[4];
        OUT_CS_TABLE(zeros, 4);
    }
    END_CS;

done:
    /* Restore the state. */
    r300_mark_atom_dirty(r300, &r300->rs_state);
    r300_mark_atom_dirty(r300, &r300->viewport_state);
    r300_mark_atom_dirty(r300, &r300->scissor_state);

    r300->sprite_coord_enable = last_sprite_coord_enable;
    r300->is_point = last_is_point;
    r300->viewport_scissor = last_vp_scissor;
}

void r300_init_render_functions(struct r300_context *r300)
{
    /* Set draw functions based on presence of HW TCL. */
    if (r300->screen->caps.has_tcl) {
        r300->context.draw_vbo = r300_draw_vbo;
    } else {
        /* RS48x IGPs keep the vertex transform in Gallium Draw, but the
         * post-Draw stream still goes through hardware LOAD_VBPNTR/PSC fetch,
         * TCL_BYPASS VAP setup, and the normal raster/fragment/backend path. */
        r300->context.draw_vbo = r300_swtcl_draw_vbo;
    }

    /* Plug in the two-sided stencil reference value fallback if needed. */
    if (!r300->screen->caps.is_r500)
        r300_plug_in_stencil_ref_fallback(r300);
}
