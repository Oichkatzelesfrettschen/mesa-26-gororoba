/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "draw/draw_context.h"

#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_debug.h"
#include "util/os_misc.h"
#include <string.h>
#include "util/u_pack_color.h"

#include "r300_context.h"
#include "r300_fs.h"
#include "r300_screen.h"
#include "r300_shader_semantics.h"
#include "r300_state_inlines.h"
#include "r300_texture.h"
#include "r300_vs.h"

/* r300_state_derived: Various bits of state which are dependent upon
 * currently bound CSO data. */

/* Position-only SWTCL safe-dummy-texcoord: exact R300_SWTCL_DUMMY_TEXCOORD=1. */

enum r300_rs_swizzle {
    SWIZ_XYZW = 0,
    SWIZ_X001,
    SWIZ_XY01,
    SWIZ_0001,
};

enum r300_rs_col_write_type {
    WRITE_COLOR = 0,
    WRITE_FACE
};

static void r300_draw_emit_attrib(struct r300_context* r300,
                                  enum attrib_emit emit,
                                  enum tgsi_semantic sname,
                                  unsigned sindex)
{
    int output = draw_find_shader_output(r300->draw, sname, sindex);
    draw_emit_vertex_attr(&r300->vertex_info, emit, output);
}

static void r300_draw_emit_all_attribs(struct r300_context* r300)
{
    struct r300_vertex_shader_code* vs = r300_vs(r300)->shader;
    struct r300_shader_semantics* vs_outputs = &vs->outputs;
    int i, gen_count;
    /* gl_PointCoord is delivered as a draw-generated PCOORD vertex output for
     * SW-expanded point sprites. The wide-point stage allocates that output
     * only during the pipeline run, so a -1 slot (draw prepare) means skip it;
     * the run-time r300_render_get_vertex_info rebuild emits it. */
    const bool pcoord_via_draw = r300->point_sprite_via_draw &&
        draw_find_shader_output(r300->draw, TGSI_SEMANTIC_PCOORD, 0) >= 0;
    /* SWTCL analytic derivatives: the draw module supplies the two per-triangle
     * gradient vectors as generic vertex outputs at deriv_ddx/ddy_generic. Valid
     * only once those outputs exist (draw run time), matching FACE/PCOORD. */
    const struct r300_fragment_shader_code *fscode = r300_fs(r300)->shader;
    const bool deriv_via_draw = r300->derivative_via_draw && fscode &&
        fscode->deriv_ddx_generic >= 0 &&
        draw_find_shader_output(r300->draw, TGSI_SEMANTIC_GENERIC,
                                fscode->deriv_ddx_generic) >= 0;
    const int deriv_ddx_g = deriv_via_draw ? fscode->deriv_ddx_generic : -1;
    const int deriv_ddy_g = deriv_via_draw ? fscode->deriv_ddy_generic : -1;

    /* Position. */
    if (vs_outputs->pos != ATTR_UNUSED) {
        r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_POSITION, 0);
    } else {
        assert(0);
    }

    /* Point size. */
    if (vs_outputs->psize != ATTR_UNUSED) {
        r300_draw_emit_attrib(r300, EMIT_1F_PSIZE, TGSI_SEMANTIC_PSIZE, 0);
    }

    /* Colors. */
    for (i = 0; i < ATTR_COLOR_COUNT; i++) {
        if (vs_outputs->color[i] != ATTR_UNUSED) {
            r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_COLOR, i);
        }
    }

    /* Back-face colors. */
    for (i = 0; i < ATTR_COLOR_COUNT; i++) {
        if (vs_outputs->bcolor[i] != ATTR_UNUSED) {
            r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_BCOLOR, i);
        }
    }

    /* gl_FrontFacing. The unfilled stage emits a per-triangle FACE output;
     * allocate_hardware_inputs places the FS face register right after the
     * colors and r300_update_rs_block routes the FACE texcoord in the same slot,
     * so emit it here -- after the (b)colors, before the generics -- to keep
     * vertex_info index-aligned with the RS stream.  Require the current FS to
     * read the face input: after a prior FrontFacing draw the draw module may
     * still own a FACE output while this draw's FS does not, and emitting it
     * would consume a texcoord unit and desync GA streams that do not route it. */
    bool face_via_draw =
        r300->frontface_via_draw &&
        fscode && fscode->inputs.face != ATTR_UNUSED &&
        draw_find_shader_output(r300->draw, TGSI_SEMANTIC_FACE, 0) >= 0;
    if (face_via_draw)
        r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_FACE, 0);

    /* Texture coordinates. */
    /* Only 8 generic vertex attributes can be used. If there are more,
     * they won't be rasterized.  On R300-class, draw-injected FACE routes
     * through one texcoord unit in r300_update_rs_block, so the generic
     * ceiling shrinks by one when FACE is present. */
    gen_count = 0;
    const int gen_limit = face_via_draw ? 7 : 8;
    for (i = 0; i < ATTR_GENERIC_COUNT && gen_count < gen_limit; i++) {
        if (vs_outputs->generic[i] != ATTR_UNUSED &&
            (!(r300->sprite_coord_enable & (1U << i)) || !r300->is_point)) {
            r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_GENERIC, i);
            gen_count++;
        } else if (pcoord_via_draw && (r300->point_sprite_sce & (1U << i))) {
            /* gl_PointCoord sprite for a SW-expanded point. The live
             * sprite_coord_enable / is_point are zeroed mid-draw, so detect the
             * sprite from the draw-entry snapshot. Emit the PCOORD vertex output
             * at the same generic position r300_update_rs_block routes it, so
             * vertex_info and the RS stream stay index-aligned for
             * r300_swtcl_vertex_psc. */
            r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_PCOORD, 0);
            gen_count++;
        } else if (i == deriv_ddx_g || i == deriv_ddy_g) {
            /* Draw-injected per-triangle screen-space gradient. Emit it as a
             * generic vertex output at the same index r300_update_rs_block routes
             * it, after the real generics, so vertex_info and the RS stream stay
             * index-aligned for r300_swtcl_vertex_psc. */
            r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_GENERIC, i);
            gen_count++;
        }
    }

    /* Fog coordinates. */
    if (gen_count < gen_limit && vs_outputs->fog != ATTR_UNUSED) {
        r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_FOG, 0);
        gen_count++;
    }

    /* WPOS. */
    if (r300_fs(r300)->shader->inputs.wpos != ATTR_UNUSED &&
        gen_count < gen_limit) {
        unsigned wpos_slot = 0;

        for (i = 0; i < ATTR_GENERIC_COUNT; i++) {
            if (vs_outputs->generic[i] != ATTR_UNUSED)
                wpos_slot = i + 1;
        }

        assert(wpos_slot < ATTR_GENERIC_COUNT);

        DBG(r300, DBG_SWTCL, "draw_emit_attrib: WPOS, slot: %u\n",
            wpos_slot);
        r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_GENERIC, wpos_slot);
    }

    /* Dummy-texcoord payload vector: r300_update_rs_block declared a TEX0
     * output for the position-only fallback, so the vertex payload must carry
     * a real 4-float vector at that stream slot.  Position is re-emitted as
     * the source -- the FS never reads the vector, only its existence in the
     * VAP stream matters, and reusing an output that always exists avoids a
     * dangling draw-output lookup.  Emitted last so vertex_info stays
     * index-aligned with stream_loc_notcl, whose dummy entry is also last. */
    if (r300->swtcl_dummy_texcoord) {
        r300_draw_emit_attrib(r300, EMIT_4F, TGSI_SEMANTIC_POSITION, 0);
    }
}

/* Update the PSC tables for SW TCL, using Draw. */
static void r300_swtcl_vertex_psc(struct r300_context *r300)
{
    struct r300_vertex_stream_state *vstream = r300->vertex_stream_state.state;
    struct vertex_info *vinfo = &r300->vertex_info;
    uint16_t type, swizzle;
    enum pipe_format format;
    unsigned i, attrib_count;
    int* vs_output_tab = r300->stream_loc_notcl;

    memset(vstream, 0, sizeof(struct r300_vertex_stream_state));

    /* For each Draw attribute, route it to the fragment shader according
     * to the vs_output_tab. */
    attrib_count = vinfo->num_attribs;
    DBG(r300, DBG_SWTCL, "r300: attrib count: %d\n", attrib_count);
    for (i = 0; i < attrib_count; i++) {
        if (vs_output_tab[i] == -1) {
            assert(0);
            abort();
        }

        format = draw_translate_vinfo_format(vinfo->attrib[i].emit);

        DBG(r300, DBG_SWTCL,
            "r300: swtcl_vertex_psc [%i] <- %s\n",
            vs_output_tab[i], util_format_short_name(format));

        /* Obtain the type of data in this attribute. */
        type = r300_translate_vertex_data_type(format);
        if (type == R300_INVALID_FORMAT) {
            fprintf(stderr, "r300: Bad vertex format %s.\n",
                    util_format_short_name(format));
            assert(0);
            abort();
        }

        type |= vs_output_tab[i] << R300_DST_VEC_LOC_SHIFT;

        /* Obtain the swizzle for this attribute. Note that the default
         * swizzle in the hardware is not XYZW! */
        swizzle = r300_translate_vertex_data_swizzle(format);

        /* Add the attribute to the PSC table. */
        if (i & 1) {
            vstream->vap_prog_stream_cntl[i >> 1] |= type << 16;
            vstream->vap_prog_stream_cntl_ext[i >> 1] |= (uint32_t)swizzle << 16;
        } else {
            vstream->vap_prog_stream_cntl[i >> 1] |= type;
            vstream->vap_prog_stream_cntl_ext[i >> 1] |= swizzle;
        }
    }

    /* Set the last vector in the PSC. */
    if (i) {
        i -= 1;
    }
    vstream->vap_prog_stream_cntl[i >> 1] |=
        (R300_LAST_VEC << (i & 1 ? 16 : 0));

    vstream->count = (i >> 1) + 1;
    r300_mark_atom_dirty(r300, &r300->vertex_stream_state);
    r300->vertex_stream_state.size = (1 + vstream->count) * 2;
}

static void r300_rs_col(struct r300_rs_block* rs, int id, int ptr,
                        enum r300_rs_swizzle swiz)
{
    rs->ip[id] |= R300_RS_COL_PTR(ptr);
    if (swiz == SWIZ_0001) {
        rs->ip[id] |= R300_RS_COL_FMT(R300_RS_COL_FMT_0001);
    } else {
        rs->ip[id] |= R300_RS_COL_FMT(R300_RS_COL_FMT_RGBA);
    }
    rs->inst[id] |= R300_RS_INST_COL_ID(id);
}

static void r300_rs_col_write(struct r300_rs_block* rs, int id, int fp_offset,
                              enum r300_rs_col_write_type type)
{
    assert(type == WRITE_COLOR);
    rs->inst[id] |= R300_RS_INST_COL_CN_WRITE |
                    R300_RS_INST_COL_ADDR(fp_offset);
}

static void r300_rs_tex(struct r300_rs_block* rs, int id, int ptr,
                        enum r300_rs_swizzle swiz)
{
    if (swiz == SWIZ_X001) {
        rs->ip[id] |= R300_RS_TEX_PTR(ptr) |
                      R300_RS_SEL_S(R300_RS_SEL_C0) |
                      R300_RS_SEL_T(R300_RS_SEL_K0) |
                      R300_RS_SEL_R(R300_RS_SEL_K0) |
                      R300_RS_SEL_Q(R300_RS_SEL_K1);
    } else if (swiz == SWIZ_XY01) {
        rs->ip[id] |= R300_RS_TEX_PTR(ptr) |
                      R300_RS_SEL_S(R300_RS_SEL_C0) |
                      R300_RS_SEL_T(R300_RS_SEL_C1) |
                      R300_RS_SEL_R(R300_RS_SEL_K0) |
                      R300_RS_SEL_Q(R300_RS_SEL_K1);
    } else {
        rs->ip[id] |= R300_RS_TEX_PTR(ptr) |
                      R300_RS_SEL_S(R300_RS_SEL_C0) |
                      R300_RS_SEL_T(R300_RS_SEL_C1) |
                      R300_RS_SEL_R(R300_RS_SEL_C2) |
                      R300_RS_SEL_Q(R300_RS_SEL_C3);
    }
    rs->inst[id] |= R300_RS_INST_TEX_ID(id);
}

static void r300_rs_tex_write(struct r300_rs_block* rs, int id, int fp_offset)
{
    rs->inst[id] |= R300_RS_INST_TEX_CN_WRITE |
                    R300_RS_INST_TEX_ADDR(fp_offset);
}

static void r500_rs_col(struct r300_rs_block* rs, int id, int ptr,
                        enum r300_rs_swizzle swiz)
{
    rs->ip[id] |= R500_RS_COL_PTR(ptr);
    if (swiz == SWIZ_0001) {
        rs->ip[id] |= R500_RS_COL_FMT(R300_RS_COL_FMT_0001);
    } else {
        rs->ip[id] |= R500_RS_COL_FMT(R300_RS_COL_FMT_RGBA);
    }
    rs->inst[id] |= R500_RS_INST_COL_ID(id);
}

static void r500_rs_col_write(struct r300_rs_block* rs, int id, int fp_offset,
                              enum r300_rs_col_write_type type)
{
    if (type == WRITE_FACE)
        rs->inst[id] |= R500_RS_INST_COL_CN_WRITE_BACKFACE |
                        R500_RS_INST_COL_ADDR(fp_offset);
    else
        rs->inst[id] |= R500_RS_INST_COL_CN_WRITE |
                        R500_RS_INST_COL_ADDR(fp_offset);

}

static void r500_rs_tex(struct r300_rs_block* rs, int id, int ptr,
			enum r300_rs_swizzle swiz)
{
    if (swiz == SWIZ_X001) {
        rs->ip[id] |= R500_RS_SEL_S(ptr) |
                      R500_RS_SEL_T(R500_RS_IP_PTR_K0) |
                      R500_RS_SEL_R(R500_RS_IP_PTR_K0) |
                      R500_RS_SEL_Q(R500_RS_IP_PTR_K1);
    } else if (swiz == SWIZ_XY01) {
        rs->ip[id] |= R500_RS_SEL_S(ptr) |
                      R500_RS_SEL_T(ptr + 1) |
                      R500_RS_SEL_R(R500_RS_IP_PTR_K0) |
                      R500_RS_SEL_Q(R500_RS_IP_PTR_K1);
    } else {
        rs->ip[id] |= R500_RS_SEL_S(ptr) |
                      R500_RS_SEL_T(ptr + 1) |
                      R500_RS_SEL_R(ptr + 2) |
                      R500_RS_SEL_Q(ptr + 3);
    }
    rs->inst[id] |= R500_RS_INST_TEX_ID(id);
}

static void r500_rs_tex_write(struct r300_rs_block* rs, int id, int fp_offset)
{
    rs->inst[id] |= R500_RS_INST_TEX_CN_WRITE |
                    R500_RS_INST_TEX_ADDR(fp_offset);
}

/* Set up the RS block.
 *
 * This is the part of the chipset that is responsible for linking vertex
 * and fragment shaders and stuffed texture coordinates.
 *
 * The rasterizer reads data from VAP, which produces vertex shader outputs,
 * and GA, which produces stuffed texture coordinates. VAP outputs have
 * precedence over GA. All outputs must be rasterized otherwise it locks up.
 * If there are more outputs rasterized than is set in VAP/GA, it locks up
 * too. The funky part is that this info has been pretty much obtained by trial
 * and error. */
static void r300_update_rs_block(struct r300_context *r300)
{
    struct r300_vertex_shader_code *vs = r300_vs(r300)->shader;
    struct r300_shader_semantics *vs_outputs = &vs->outputs;
    struct r300_shader_semantics *fs_inputs = &r300_fs(r300)->shader->inputs;
    struct r300_rs_block rs = {0};
    int i, col_count = 0, tex_count = 0, fp_offset = 0, count, loc = 0, tex_ptr = 0;
    int gen_offset = 0;
    void (*rX00_rs_col)(struct r300_rs_block*, int, int, enum r300_rs_swizzle);
    void (*rX00_rs_col_write)(struct r300_rs_block*, int, int, enum r300_rs_col_write_type);
    void (*rX00_rs_tex)(struct r300_rs_block*, int, int, enum r300_rs_swizzle);
    void (*rX00_rs_tex_write)(struct r300_rs_block*, int, int);
    bool any_bcolor_used = vs_outputs->bcolor[0] != ATTR_UNUSED ||
                           vs_outputs->bcolor[1] != ATTR_UNUSED;
    int *stream_loc_notcl = r300->stream_loc_notcl;
    uint32_t stuffing_enable = 0;
    /* gl_PointCoord for SW-expanded point sprites is routed as a
     * draw-generated vertex texcoord rather than HW GA point-stuffing, which no
     * longer fires once the point is a triangle pair. Gated on the PCOORD draw
     * output existing (valid only at draw run time, see
     * r300_render_get_vertex_info) so the draw-prepare pass stays consistent
     * with r300_draw_emit_all_attribs. */
    const bool pcoord_via_draw = r300->point_sprite_via_draw &&
        draw_find_shader_output(r300->draw, TGSI_SEMANTIC_PCOORD, 0) >= 0;
    /* gl_FrontFacing on an R300-class part: the draw module computed the face
     * and emitted it as a FACE vertex output (the RS WRITE_BACKFACE encoding is
     * an R500 addition). Gated on the FACE draw output existing, like PCOORD. */
    const bool frontface_via_draw = r300->frontface_via_draw &&
        draw_find_shader_output(r300->draw, TGSI_SEMANTIC_FACE, 0) >= 0;
    /* SWTCL analytic derivatives: route the draw-injected gradient generics into
     * the rewritten FS inputs. Gated on the gradient draw output existing, like
     * FACE/PCOORD; the indices come from the bound FS's NIR derivative pass. */
    const struct r300_fragment_shader_code *deriv_fs = r300_fs(r300)->shader;
    const bool derivative_via_draw = r300->derivative_via_draw && deriv_fs &&
        deriv_fs->deriv_ddx_generic >= 0 &&
        draw_find_shader_output(r300->draw, TGSI_SEMANTIC_GENERIC,
                                deriv_fs->deriv_ddx_generic) >= 0;
    const int deriv_ddx_g = derivative_via_draw ? deriv_fs->deriv_ddx_generic : -1;
    const int deriv_ddy_g = derivative_via_draw ? deriv_fs->deriv_ddy_generic : -1;

    if (r300->screen->caps.is_r500) {
        rX00_rs_col       = r500_rs_col;
        rX00_rs_col_write = r500_rs_col_write;
        rX00_rs_tex       = r500_rs_tex;
        rX00_rs_tex_write = r500_rs_tex_write;
    } else {
        rX00_rs_col       = r300_rs_col;
        rX00_rs_col_write = r300_rs_col_write;
        rX00_rs_tex       = r300_rs_tex;
        rX00_rs_tex_write = r300_rs_tex_write;
    }

    /* 0x5555 copied from classic, which means:
     * Select user color 0 for COLOR0 up to COLOR7.
     * What the hell does that mean? */
    rs.vap_vtx_state_cntl = 0x5555;

    /* The position is always present in VAP. */
    rs.vap_vsm_vtx_assm |= R300_INPUT_CNTL_POS;
    rs.vap_out_vtx_fmt[0] |= R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT;
    stream_loc_notcl[loc++] = 0;

    /* Set up the point size in VAP. */
    if (vs_outputs->psize != ATTR_UNUSED) {
        rs.vap_out_vtx_fmt[0] |= R300_VAP_OUTPUT_VTX_FMT_0__PT_SIZE_PRESENT;
        stream_loc_notcl[loc++] = 1;
    }

    /* Set up and rasterize colors. */
    for (i = 0; i < ATTR_COLOR_COUNT; i++) {
        if (vs_outputs->color[i] != ATTR_UNUSED || any_bcolor_used ||
            vs_outputs->color[1] != ATTR_UNUSED) {
            /* Set up the color in VAP. */
            rs.vap_vsm_vtx_assm |= R300_INPUT_CNTL_COLOR;
            rs.vap_out_vtx_fmt[0] |=
                    R300_VAP_OUTPUT_VTX_FMT_0__COLOR_0_PRESENT << i;
            stream_loc_notcl[loc++] = 2 + i;

            /* Rasterize it. */
            rX00_rs_col(&rs, col_count, col_count, SWIZ_XYZW);

            /* Write it to the FS input register if it's needed by the FS. */
            if (fs_inputs->color[i] != ATTR_UNUSED) {
                rX00_rs_col_write(&rs, col_count, fp_offset, WRITE_COLOR);
                fp_offset++;

                DBG(r300, DBG_RS,
                    "r300: Rasterized color %i written to FS.\n", i);
            } else {
                DBG(r300, DBG_RS, "r300: Rasterized color %i unused.\n", i);
            }
            col_count++;
        } else {
            /* Skip the FS input register, leave it uninitialized. */
            /* If we try to set it to (0,0,0,1), it will lock up. */
            if (fs_inputs->color[i] != ATTR_UNUSED) {
                fp_offset++;

                DBG(r300, DBG_RS, "r300: FS input color %i unassigned.\n",
                    i);
            }
        }
    }

    /* Set up back-face colors. The rasterizer will do the color selection
     * automatically. */
    if (any_bcolor_used) {
        if (r300->two_sided_color) {
            /* Rasterize as back-face colors. */
            for (i = 0; i < ATTR_COLOR_COUNT; i++) {
                rs.vap_vsm_vtx_assm |= R300_INPUT_CNTL_COLOR;
                rs.vap_out_vtx_fmt[0] |= R300_VAP_OUTPUT_VTX_FMT_0__COLOR_0_PRESENT << (2+i);
                stream_loc_notcl[loc++] = 4 + i;
            }
        } else {
            /* Rasterize two fake texcoords to prevent from the two-sided color
             * selection. */
            /* XXX Consider recompiling the vertex shader to save 2 RS units. */
            for (i = 0; i < 2; i++) {
                rs.vap_vsm_vtx_assm |= (R300_INPUT_CNTL_TC0 << tex_count);
                rs.vap_out_vtx_fmt[1] |= (4 << (3 * tex_count));
                stream_loc_notcl[loc++] = 6 + tex_count;

                /* Rasterize it. */
                rX00_rs_tex(&rs, tex_count, tex_ptr, SWIZ_XYZW);
                tex_count++;
                tex_ptr += 4;
            }
        }
    }

    /* gl_FrontFacing.
     * Note that we can use either the two-sided color selection based on
     * the front and back vertex shader colors, or gl_FrontFacing,
     * but not both! It locks up otherwise.
     *
     * In Direct3D 9, the two-sided color selection can be used
     * with shaders 2.0 only, while gl_FrontFacing can be used
     * with shaders 3.0 only. The hardware apparently hasn't been designed
     * to support both at the same time. */
    if (r300->screen->caps.is_r500 && fs_inputs->face != ATTR_UNUSED &&
        !(any_bcolor_used && r300->two_sided_color)) {
        rX00_rs_col(&rs, col_count, col_count, SWIZ_XYZW);
        rX00_rs_col_write(&rs, col_count, fp_offset, WRITE_FACE);
        fp_offset++;
        col_count++;
        DBG(r300, DBG_RS, "r300: Rasterized FACE written to FS.\n");
    } else if (frontface_via_draw && fs_inputs->face != ATTR_UNUSED &&
               tex_count < 8) {
        /* R300-class: the draw module computed the face per filled triangle and
         * emitted it as a FACE vertex output. Route it as a texcoord into the FS
         * face input. allocate_hardware_inputs places the face register
         * immediately after the colors, so it must take this fp_offset here --
         * before the generic loop -- to stay register-aligned, and a matching
         * FACE attribute is emitted right after the (b)colors in
         * r300_draw_emit_all_attribs to keep the GA stream aligned. */
        rs.vap_vsm_vtx_assm |= (R300_INPUT_CNTL_TC0 << tex_count);
        rs.vap_out_vtx_fmt[1] |= (4 << (3 * tex_count));
        stream_loc_notcl[loc++] = 6 + tex_count;
        rX00_rs_tex(&rs, tex_count, tex_ptr, SWIZ_XYZW);
        rX00_rs_tex_write(&rs, tex_count, fp_offset);
        fp_offset++;
        tex_count++;
        tex_ptr += 4;
        DBG(r300, DBG_RS, "r300: Draw-injected FACE written to FS in texcoord.\n");
    } else if (frontface_via_draw && fs_inputs->face != ATTR_UNUSED) {
        /* frontface_via_draw is on and FACE is used, but the prior branch needed a
         * free texcoord slot (tex_count < 8); reaching here means all eight are
         * taken, so the draw-injected FACE has nowhere to land and the FS face
         * input stays unassigned -- report it rather than fail silently. */
        fprintf(stderr, "r300: ERROR: FS input FACE unassigned, "
                "no free texcoord slot.\n");
    } else if (fs_inputs->face != ATTR_UNUSED && !r300->frontface_via_draw) {
        /* When frontface_via_draw is set, the draw-generated FACE output does
         * not exist yet at this draw-prepare build; r300_render_get_vertex_info
         * rebuilds the layout once the unfilled stage has allocated it, so this
         * is not an error in that case. */
        fprintf(stderr, "r300: ERROR: FS input FACE unassigned.\n");
    }

    /* Reuse color varyings for generics if possible.
     * The colors are interpolated as 20-bit floats (reduced precision),
     * Use this hack only if there are too many generic varyings.
     * (number of generic varyings + fog + wpos > 8) */
    if (r300->screen->caps.is_r500 && !any_bcolor_used && !r300->flatshade &&
	fs_inputs->face == ATTR_UNUSED &&
        vs_outputs->num_generic + (vs_outputs->fog != ATTR_UNUSED) +
        (fs_inputs->wpos != ATTR_UNUSED) > 8) {
	for (i = 0; i < ATTR_GENERIC_COUNT && col_count < 2; i++) {
	    /* Cannot use color varyings for sprite coords. */
	    if (fs_inputs->generic[i] != ATTR_UNUSED &&
		(r300->sprite_coord_enable & (1U << i)) && r300->is_point) {
		break;
	    }

	    if (vs_outputs->generic[i] != ATTR_UNUSED) {
		/* Set up the color in VAP. */
		rs.vap_vsm_vtx_assm |= R300_INPUT_CNTL_COLOR;
		rs.vap_out_vtx_fmt[0] |=
			R300_VAP_OUTPUT_VTX_FMT_0__COLOR_0_PRESENT << col_count;
		stream_loc_notcl[loc++] = 2 + col_count;

		/* Rasterize it. */
		rX00_rs_col(&rs, col_count, col_count, SWIZ_XYZW);

		/* Write it to the FS input register if it's needed by the FS. */
		if (fs_inputs->generic[i] != ATTR_UNUSED) {
		    rX00_rs_col_write(&rs, col_count, fp_offset, WRITE_COLOR);
		    fp_offset++;

		    DBG(r300, DBG_RS,
			"r300: Rasterized generic %i redirected to color %i and written to FS.\n",
		        i, col_count);
		} else {
		    DBG(r300, DBG_RS, "r300: Rasterized generic %i redirected to color %i unused.\n",
		        i, col_count);
		}
		col_count++;
	    } else {
		/* Skip the FS input register, leave it uninitialized. */
		/* If we try to set it to (0,0,0,1), it will lock up. */
		if (fs_inputs->generic[i] != ATTR_UNUSED) {
		    fp_offset++;

		    DBG(r300, DBG_RS, "r300: FS input generic %i unassigned.\n", i);
		}
	    }
	}
	gen_offset = i;
    }

    /* Rasterize texture coordinates. */
    for (i = gen_offset; i < ATTR_GENERIC_COUNT && tex_count < 8; i++) {
	bool sprite_coord = false;
	bool sw_pcoord = false;

	if (fs_inputs->generic[i] != ATTR_UNUSED) {
	    sprite_coord = !!(r300->sprite_coord_enable & (1 << i)) && r300->is_point;
	    /* SW-expanded point sprite: the live sprite_coord_enable / is_point are
	     * zeroed by the draw module's mid-draw no-cull rasterizer rebind, so
	     * detect the sprite from the draw-entry snapshot and route the
	     * draw-generated PCOORD output as a real vertex texcoord. */
	    sw_pcoord = pcoord_via_draw && !!(r300->point_sprite_sce & (1 << i));
	}

	/* A draw-injected gradient generic has no VS output, so route it like a
	 * real per-vertex texcoord here (it falls past the actual generics, so
	 * fp_offset stays aligned with allocate_hardware_inputs). */
	const bool deriv_gen = (i == deriv_ddx_g || i == deriv_ddy_g);

        if (vs_outputs->generic[i] != ATTR_UNUSED || sprite_coord || sw_pcoord ||
            deriv_gen) {
            if (!sprite_coord || sw_pcoord) {
                /* Set up the texture coordinates in VAP. A SW-expanded
                 * gl_PointCoord (sw_pcoord) is a real per-vertex texcoord, not
                 * HW point-stuffing. */
                rs.vap_vsm_vtx_assm |= (R300_INPUT_CNTL_TC0 << tex_count);
                rs.vap_out_vtx_fmt[1] |= (4 << (3 * tex_count));
                stream_loc_notcl[loc++] = 6 + tex_count;
            } else
                stuffing_enable |=
                    R300_GB_TEX_ST << (R300_GB_TEX0_SOURCE_SHIFT + (tex_count*2));

            /* Rasterize it. The draw-generated PCOORD output carries (s,t,0,1)
             * per vertex, so it uses the full XYZW texcoord swizzle; HW
             * point-stuffing supplies only (s,t). */
            rX00_rs_tex(&rs, tex_count, tex_ptr,
			(sprite_coord && !sw_pcoord) ? SWIZ_XY01 : SWIZ_XYZW);

            /* Write it to the FS input register if it's needed by the FS. */
            if (fs_inputs->generic[i] != ATTR_UNUSED) {
                rX00_rs_tex_write(&rs, tex_count, fp_offset);
                fp_offset++;

                if (deriv_gen && getenv("R300_DERIV_DEBUG"))
                    fprintf(stderr, "r300 deriv: RS routed gradient generic %d "
                            "to texcoord %d (fp_offset %d)\n", i, tex_count,
                            fp_offset - 1);
                DBG(r300, DBG_RS,
                    "r300: Rasterized generic %i written to FS%s in texcoord %d.\n",
                    i, sprite_coord ? " (sprite coord)" : "", tex_count);
            } else {
                DBG(r300, DBG_RS,
                    "r300: Rasterized generic %i unused%s.\n",
                    i, sprite_coord ? " (sprite coord)" : "");
            }
            tex_count++;
            tex_ptr += (sprite_coord && !sw_pcoord) ? 2 : 4;
        } else {
            /* Skip the FS input register, leave it uninitialized. */
            /* If we try to set it to (0,0,0,1), it will lock up. */
            if (fs_inputs->generic[i] != ATTR_UNUSED) {
                fp_offset++;

                DBG(r300, DBG_RS, "r300: FS input generic %i unassigned%s.\n",
                    i, sprite_coord ? " (sprite coord)" : "");
            }
        }
    }

    for (; i < ATTR_GENERIC_COUNT; i++) {
        if (fs_inputs->generic[i] != ATTR_UNUSED) {
            fprintf(stderr, "r300: ERROR: FS input generic %i unassigned, "
                    "not enough hardware slots (it's not a bug, do not "
                    "report it).\n", i);
        }
    }

    /* Rasterize fog coordinates. */
    if (vs_outputs->fog != ATTR_UNUSED && tex_count < 8) {
        /* Set up the fog coordinates in VAP. */
        rs.vap_vsm_vtx_assm |= (R300_INPUT_CNTL_TC0 << tex_count);
        rs.vap_out_vtx_fmt[1] |= (4 << (3 * tex_count));
        stream_loc_notcl[loc++] = 6 + tex_count;

        /* Rasterize it. */
        rX00_rs_tex(&rs, tex_count, tex_ptr, SWIZ_X001);

        /* Write it to the FS input register if it's needed by the FS. */
        if (fs_inputs->fog != ATTR_UNUSED) {
            rX00_rs_tex_write(&rs, tex_count, fp_offset);
            fp_offset++;

            DBG(r300, DBG_RS, "r300: Rasterized fog written to FS.\n");
        } else {
            DBG(r300, DBG_RS, "r300: Rasterized fog unused.\n");
        }
        tex_count++;
        tex_ptr += 4;
    } else {
        /* Skip the FS input register, leave it uninitialized. */
        /* If we try to set it to (0,0,0,1), it will lock up. */
        if (fs_inputs->fog != ATTR_UNUSED) {
            fp_offset++;

            if (tex_count < 8) {
                DBG(r300, DBG_RS, "r300: FS input fog unassigned.\n");
            } else {
                fprintf(stderr, "r300: ERROR: FS input fog unassigned, "
                        "not enough hardware slots. (it's not a bug, "
                        "do not report it)\n");
            }
        }
    }

    /* Rasterize WPOS. */
    /* Don't set it in VAP if the FS doesn't need it. */
    if (fs_inputs->wpos != ATTR_UNUSED && tex_count < 8) {
        /* Set up the WPOS coordinates in VAP. */
        rs.vap_vsm_vtx_assm |= (R300_INPUT_CNTL_TC0 << tex_count);
        rs.vap_out_vtx_fmt[1] |= (4 << (3 * tex_count));
        stream_loc_notcl[loc++] = 6 + tex_count;

        /* Rasterize it. */
        rX00_rs_tex(&rs, tex_count, tex_ptr, SWIZ_XYZW);

        /* Write it to the FS input register. */
        rX00_rs_tex_write(&rs, tex_count, fp_offset);

        DBG(r300, DBG_RS, "r300: Rasterized WPOS written to FS.\n");

        fp_offset++;
        tex_count++;
        tex_ptr += 4;
    } else {
        if (fs_inputs->wpos != ATTR_UNUSED && tex_count >= 8) {
            fprintf(stderr, "r300: ERROR: FS input WPOS unassigned, "
                    "not enough hardware slots. (it's not a bug, do not "
                    "report it)\n");
        }
    }

    /* Position-only dummy-texcoord experiment (exact opt-in): declare and
     * route one real TEX0 vector instead of the dummy color below.  The dummy
     * color rasterizes a color vector the VAP never produces (RS_COUNT counts
     * one color, VAP_OUTPUT_VTX_FMT_0 declares none), the over-rasterization
     * shape the comment at the head of this function names as a lockup.
     * Silicon-tested on RS482: this declared POS+TEX0 shape still wedges the
     * vertex frontend (RBBM latches CP+VAP+GA busy, backend idle) on the
     * first position-only SWTCL draw, so the VAP/RS declaration mismatch is
     * not the wedge cause by itself; the lever is retained for register-
     * contract experiments (the remaining delta against the completing r3v
     * shape is the RS_INST CN_WRITE consumption and state outside VAP/GA).
     * The texcoord must be a real VAP-declared stream output, so
     * r300_draw_emit_all_attribs emits a matching payload vector when
     * swtcl_dummy_texcoord is set; SWTCL-only, since a HWTCL vertex shader
     * would not write the declared vector. */
    r300->swtcl_dummy_texcoord = false;
    {
    static int dummy_texcoord_gate = -1;
    if (dummy_texcoord_gate < 0) {
        const char *e = os_get_option("R300_SWTCL_DUMMY_TEXCOORD");
        dummy_texcoord_gate = (e && strcmp(e, "1") == 0) ? 1 : 0;
    }
    /* Dummy TEX0 is for the ordinary SW-TCL draw path only.  R2VB passthrough
     * re-ingest builds velems from app inputs / producer BOs and does not
     * emit the extra draw-module payload, so enabling both would desync
     * RS/PSC from LOAD_VBPNTR.  Cache the R2VB gates once: this runs on the
     * derived-state hot path. */
    {
        static int r2vb_gate = -1;
        if (r2vb_gate < 0) {
            const char *r2vb = getenv("R300_R2VB_ROUTE");
            const char *r2vb_exec = getenv("R300_R2VB_EXEC");
            r2vb_gate =
                ((r2vb && strcmp(r2vb, "1") == 0) ||
                 (r2vb_exec && strcmp(r2vb_exec, "1") == 0))
                    ? 1
                    : 0;
        }
        const bool r2vb_on = r2vb_gate == 1;
    if (col_count == 0 && tex_count == 0 &&
        !r300->screen->caps.has_tcl && r300->draw &&
        dummy_texcoord_gate == 1 && !r2vb_on) {
        rs.vap_vsm_vtx_assm |= (R300_INPUT_CNTL_TC0 << tex_count);
        rs.vap_out_vtx_fmt[1] |= (4 << (3 * tex_count));
        stream_loc_notcl[loc++] = 6 + tex_count;

        /* Rasterize it, unused by the FS (no CN_WRITE), like any rasterized-
         * but-unused output. */
        rX00_rs_tex(&rs, tex_count, tex_ptr, SWIZ_XYZW);
        tex_count++;
        tex_ptr += 4;
        r300->swtcl_dummy_texcoord = true;

        DBG(r300, DBG_RS, "r300: Rasterized dummy texcoord to prevent lockups.\n");
    }
    }
    }

    /* Invalidate the rest of the no-TCL (GA) stream locations. */
    for (; loc < 16;) {
        stream_loc_notcl[loc++] = -1;
    }

    /* Rasterize at least one color, or bad things happen. */
    if (col_count == 0 && tex_count == 0) {
        rX00_rs_col(&rs, 0, 0, SWIZ_0001);
        col_count++;

        DBG(r300, DBG_RS, "r300: Rasterized color 0 to prevent lockups.\n");
    }

    DBG(r300, DBG_RS, "r300: --- Rasterizer status ---: colors: %i, "
        "generics: %i.\n", col_count, tex_count);

    rs.count = MIN2(tex_ptr, 32) | (col_count << R300_IC_COUNT_SHIFT) |
        R300_HIRES_EN;

    count = MAX3(col_count, tex_count, 1);
    rs.inst_count = count - 1;

    /* set the GB enable flags */
    if (r300->sprite_coord_enable && r300->is_point && !pcoord_via_draw)
	stuffing_enable |= R300_GB_POINT_STUFF_ENABLE;

    rs.gb_enable = stuffing_enable;

    /* Now, after all that, see if we actually need to update the state. */
    if (memcmp(r300->rs_block_state.state, &rs, sizeof(struct r300_rs_block))) {
        memcpy(r300->rs_block_state.state, &rs, sizeof(struct r300_rs_block));
        r300->rs_block_state.size = 13 + count*2;
    }
}

/* Decoded VAP/RS producer-consumer tuple: the semantic register fields that
 * decide whether the RS482 vertex frontend drains or wedges.  Buffer addresses,
 * relocations, draw-packet counts, and viewport constants are excluded -- they
 * are not part of the producer-consumer contract r300_update_rs_block documents,
 * so two draws with identical tuples share the same frontend fate regardless of
 * geometry. */
struct r300_vap_rs_tuple {
    uint32_t vap_out_vtx_fmt0, vap_out_vtx_fmt1, vap_vsm_vtx_assm;
    uint32_t rs_count, rs_inst_count, gb_enable, vap_vtx_size;
    unsigned num_vap_colors;    /* COLOR_n_PRESENT bits set in VAP_OUTPUT_VTX_FMT_0 */
    unsigned num_vap_texcoords; /* nonzero comp-count fields in VAP_OUTPUT_VTX_FMT_1 */
    unsigned sum_vap_tex_comp;  /* summed comp counts across those fields */
    unsigned rs_colors;         /* RS_COUNT IC_COUNT: color vectors the RS rasterizes */
    unsigned rs_tex_comp;       /* RS_COUNT IT_COUNT: texcoord components the RS routes */
    bool pos_present, ptsize_present, hires, point_stuff, last_vec_ok, r500_face;
};

/* Contract violations, most-load-bearing first.  The dummy-color signature is
 * the RS482 GL SW-TCL wedge discriminator: r300_update_rs_block rasterizes one
 * dummy color (SWIZ_0001) only when a draw declares no color and no texcoord, so
 * the RS rasterizes a color vector VAP never produces -- the "more outputs
 * rasterized than set in VAP/GA -> locks up" rule at the head of this file. */
enum r300_vap_rs_violation {
    R300_VAPRS_DUMMY_COLOR   = 1 << 0, /* dummy color with no VAP color and no VAP texcoord */
    R300_VAPRS_TEXCOMP_COUNT = 1 << 1, /* RS tex components != VAP-declared (no stuffing) */
    R300_VAPRS_LAST_VEC      = 1 << 2, /* last PSC stream element missing LAST_VEC */
};

/* Pure function of the decoded tuple so the verdict is testable without the GPU:
 * feed it the finding's GL column (rs_colors=1, no VAP color, no VAP texcoord)
 * and it returns R300_VAPRS_DUMMY_COLOR; feed it the r3v column (rs_colors=0, tex
 * components 4==4) and it returns 0.
 *
 * The verdict keys on the exact dummy-color signature, not on a plain color-count
 * inequality, because both directions of rs_colors != num_vap_colors have
 * documented safe cases: two-sided color sets the back-color VAP_OUTPUT_VTX_FMT_0
 * bits without a matching RS color unit (r300_update_rs_block, the two_sided_color
 * branch), and the R500 FACE path rasterizes a color with no VAP_OUTPUT_VTX_FMT_0
 * bit.  The inequality is still printed as an informational over/under-rasterization
 * signal; only the dummy-color signature is a hard FAIL.
 *
 * GB point-stuffing is deliberately NOT a verdict input: r300_blitter_draw_rectangle
 * (r300_render.c) and the R2VB path write R300_GB_ENABLE directly at emit time, so
 * the RS-block gb_enable this reads is not always the draw-time-latched value.
 * VAP_VTX_SIZE is reported but not checked: draw_compute_vertex_size derives
 * vertex_info.size from the same attrib list, so it is self-consistent here. */
static uint32_t
r300_vap_rs_contract_check(const struct r300_vap_rs_tuple *t)
{
    uint32_t v = 0;
    if (t->rs_colors >= 1 && t->num_vap_colors == 0 && t->num_vap_texcoords == 0 &&
        !t->r500_face)
        v |= R300_VAPRS_DUMMY_COLOR;
    if (!t->point_stuff && t->rs_tex_comp != t->sum_vap_tex_comp)
        v |= R300_VAPRS_TEXCOMP_COUNT;
    if (!t->last_vec_ok)
        v |= R300_VAPRS_LAST_VEC;
    return v;
}

/* No-submit dump of the VAP/RS producer-consumer tuple for the ordinary GL
 * SW-TCL path and the R2VB/r3v path in one field vocabulary.  Runs after the
 * three atomic layers derive (r300_update_rs_block fills the RS block,
 * r300_draw_emit_all_attribs fills vertex_info, r300_swtcl_vertex_psc fills the
 * PSC stream) so every field is fresh.  Reads state and writes stderr only --
 * no CS emit, no GPU work, safe on any boot. */
void r300_dump_vap_rs_tuple(struct r300_context *r300, const char *origin)
{
    const struct r300_rs_block *rs = r300->rs_block_state.state;
    const struct r300_vertex_stream_state *vs = r300->vertex_stream_state.state;
    const struct vertex_info *vinfo = &r300->vertex_info;
    const bool is_r500 = r300->screen->caps.is_r500;

    /* Stop condition: without the RS block or the PSC stream two of the three
     * atomic layers are invisible, so report the gap rather than invent a tuple. */
    if (!rs || !vs) {
        fprintf(stderr, "VAP_RS_TUPLE origin=%s UNAVAILABLE rs=%p vs=%p\n",
                origin, (void *)rs, (void *)vs);
        fflush(stderr);
        return;
    }

    struct r300_vap_rs_tuple t = {0};
    t.vap_out_vtx_fmt0 = rs->vap_out_vtx_fmt[0];
    t.vap_out_vtx_fmt1 = rs->vap_out_vtx_fmt[1];
    t.vap_vsm_vtx_assm = rs->vap_vsm_vtx_assm;
    t.rs_count = rs->count;
    t.rs_inst_count = rs->inst_count;
    t.gb_enable = rs->gb_enable;
    t.vap_vtx_size = vinfo->size; /* the value r300_emit_vertex_format_state emits */

    t.pos_present = t.vap_out_vtx_fmt0 & R300_VAP_OUTPUT_VTX_FMT_0__POS_PRESENT;
    t.ptsize_present = t.vap_out_vtx_fmt0 & R300_VAP_OUTPUT_VTX_FMT_0__PT_SIZE_PRESENT;
    /* Front-face COLOR_0/1 only: COLOR_2/3 are two-sided back colors and do not
     * need matching RS interpolators. */
    for (unsigned i = 0; i < 2; i++)
        if (t.vap_out_vtx_fmt0 & (R300_VAP_OUTPUT_VTX_FMT_0__COLOR_0_PRESENT << i))
            t.num_vap_colors++;
    for (unsigned s = 0; s < 8; s++) {
        unsigned comp = (t.vap_out_vtx_fmt1 >> (3 * s)) & 0x7;
        if (comp) {
            t.num_vap_texcoords++;
            t.sum_vap_tex_comp += comp;
        }
    }
    t.rs_colors = (t.rs_count & R300_IC_COUNT_MASK) >> R300_IC_COUNT_SHIFT;
    t.rs_tex_comp = t.rs_count & R300_IT_COUNT_MASK; /* IT_COUNT */
    t.hires = (t.rs_count & R300_HIRES_EN) != 0;
    t.point_stuff = (t.gb_enable & R300_GB_POINT_STUFF_ENABLE) != 0;
    t.r500_face = false;
    if (is_r500) {
        for (unsigned i = 0; i < 8; i++) {
            uint32_t cn = rs->inst[i] & (3u << 16); /* R500 COL_CN field */
            if (cn == R500_RS_INST_COL_CN_WRITE_BACKFACE)
                t.r500_face = true;
        }
    }

    /* r300_swtcl_vertex_psc marks the highest-index stream element LAST_VEC; the
     * authoritative element count is vertex_info.num_attribs. */
    unsigned n = vinfo->num_attribs;
    t.last_vec_ok = true;
    if (n > 0) {
        unsigned li = n - 1;
        /* vs->vap_prog_stream_cntl has vs->count dwords (2 elements each). */
        if ((li >> 1) >= vs->count) {
            t.last_vec_ok = false;
        } else {
            uint32_t e = vs->vap_prog_stream_cntl[li >> 1] >> ((li & 1) ? 16 : 0);
            t.last_vec_ok = (e & R300_LAST_VEC) != 0;
        }
    }

    uint32_t viol = r300_vap_rs_contract_check(&t);

    fprintf(stderr,
            "VAP_RS_TUPLE origin=%s is_r500=%u is_point=%u num_attribs=%u\n",
            origin, is_r500, r300->is_point, n);
    fprintf(stderr,
            "VAP_RS   VAP_OUTPUT_VTX_FMT_0=0x%08x pos=%u ptsize=%u colors=%u\n",
            t.vap_out_vtx_fmt0, t.pos_present, t.ptsize_present, t.num_vap_colors);
    fprintf(stderr,
            "VAP_RS   VAP_OUTPUT_VTX_FMT_1=0x%08x texcoords=%u tex_comp_sum=%u\n",
            t.vap_out_vtx_fmt1, t.num_vap_texcoords, t.sum_vap_tex_comp);
    fprintf(stderr, "VAP_RS   VAP_VSM_VTX_ASSM=0x%08x VAP_VTX_SIZE=%u\n",
            t.vap_vsm_vtx_assm, t.vap_vtx_size);
    fprintf(stderr,
            "VAP_RS   RS_COUNT=0x%08x ic_count=%u it_count=%u hires=%u "
            "RS_INST_COUNT=0x%08x\n",
            t.rs_count, t.rs_colors, t.rs_tex_comp, t.hires, t.rs_inst_count);
    for (unsigned i = 0; i < vs->count && i < 8; i++) {
        uint32_t c = vs->vap_prog_stream_cntl[i];
        uint32_t x = vs->vap_prog_stream_cntl_ext[i];
        for (unsigned e = 0; e < 2; e++) {
            uint32_t f = c >> (e * 16);
            fprintf(stderr,
                    "VAP_RS   VAP_PROG_STREAM_CNTL[%u].e%u data_type=0x%02x "
                    "dst_vec_loc=%u last=%u  EXT swizzle=0x%04x\n",
                    i, e, f & 0xff, (f >> R300_DST_VEC_LOC_SHIFT) & 0x1f,
                    (f & R300_LAST_VEC) != 0, (x >> (e * 16)) & 0xffff);
        }
    }
    for (unsigned i = 0; i < 8; i++) {
        if (!rs->ip[i] && !rs->inst[i])
            continue;
        fprintf(stderr,
                "VAP_RS   RS_IP[%u]=0x%08x RS_INST[%u]=0x%08x tex_write=%u col_write=%u\n",
                i, rs->ip[i], i, rs->inst[i],
                (rs->inst[i] & R300_RS_INST_TEX_CN_WRITE) != 0,
                (rs->inst[i] & R300_RS_INST_COL_CN_WRITE) != 0);
    }
    /* RS-block atom value; the blitter draw_rectangle and R2VB paths write
     * R300_GB_ENABLE directly, so the draw-time-latched value can differ. */
    fprintf(stderr, "VAP_RS   GB_ENABLE(rs_atom)=0x%08x point_stuff=%u\n",
            t.gb_enable, t.point_stuff);
    /* Informational: the raw color-count inequality is the over/under-
     * rasterization signal described at the head of r300_update_rs_block
     * ("more outputs rasterized than is set in VAP/GA"), but two-sided color
     * and R500 FACE make it safe on their own, so it is reported, not folded
     * into the verdict. */
    fprintf(stderr,
            "VAP_RS_CONTRACT origin=%s verdict=%s colors(rs=%u vap=%u mismatch=%d) "
            "texcomp(rs=%u vap=%u stuff=%u) last_vec=%u violations=0x%x\n",
            origin, viol ? "FAIL" : "PASS", t.rs_colors, t.num_vap_colors,
            t.rs_colors != t.num_vap_colors, t.rs_tex_comp, t.sum_vap_tex_comp,
            t.point_stuff, t.last_vec_ok, viol);
    fflush(stderr);
}

static void rgba_to_bgra(float color[4])
{
    float x = color[0];
    color[0] = color[2];
    color[2] = x;
}

static uint32_t r300_get_border_color(enum pipe_format format,
                                      const float border[4],
                                      bool is_r500)
{
    const struct util_format_description *desc = util_format_description(format);
    float border_swizzled[4] = {0};
    union util_color uc = {0};

    assume(desc);

    /* Do depth formats first. */
    if (util_format_is_depth_or_stencil(format)) {
        switch (format) {
        case PIPE_FORMAT_Z16_UNORM:
            return util_pack_z(PIPE_FORMAT_Z16_UNORM, border[0]);
        case PIPE_FORMAT_X8Z24_UNORM:
        case PIPE_FORMAT_S8_UINT_Z24_UNORM:
            if (is_r500) {
                return util_pack_z(PIPE_FORMAT_X8Z24_UNORM, border[0]);
            } else {
                return util_pack_z(PIPE_FORMAT_Z16_UNORM, border[0]) << 16;
            }
        default:
            assert(0);
            return 0;
        }
    }

    /* Apply inverse swizzle of the format. */
    util_format_unswizzle_4f(border_swizzled, border, desc->swizzle);

    /* Compressed formats. */
    if (util_format_is_compressed(format)) {
        switch (format) {
        case PIPE_FORMAT_RGTC1_SNORM:
        case PIPE_FORMAT_LATC1_SNORM:
            border_swizzled[0] = border_swizzled[0] < 0 ?
                                 border_swizzled[0]*0.5+1 :
                                 border_swizzled[0]*0.5;
            FALLTHROUGH;

        case PIPE_FORMAT_RGTC1_UNORM:
        case PIPE_FORMAT_LATC1_UNORM:
            /* Add 1/32 to round the border color instead of truncating. */
            /* The Y component is used for the border color. */
            border_swizzled[1] = border_swizzled[0] + 1.0f/32;
            util_pack_color(border_swizzled, PIPE_FORMAT_B4G4R4A4_UNORM, &uc);
            return uc.ui[0];
        case PIPE_FORMAT_RGTC2_SNORM:
        case PIPE_FORMAT_LATC2_SNORM:
            util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_SNORM, &uc);
            return uc.ui[0];
        case PIPE_FORMAT_RGTC2_UNORM:
        case PIPE_FORMAT_LATC2_UNORM:
            util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_UNORM, &uc);
            return uc.ui[0];
        case PIPE_FORMAT_DXT1_SRGB:
        case PIPE_FORMAT_DXT1_SRGBA:
        case PIPE_FORMAT_DXT3_SRGBA:
        case PIPE_FORMAT_DXT5_SRGBA:
            util_pack_color(border_swizzled, PIPE_FORMAT_B8G8R8A8_SRGB, &uc);
            return uc.ui[0];
        default:
            util_pack_color(border_swizzled, PIPE_FORMAT_B8G8R8A8_UNORM, &uc);
            return uc.ui[0];
        }
    }

    switch (desc->channel[0].size) {
        case 2:
            rgba_to_bgra(border_swizzled);
            util_pack_color(border_swizzled, PIPE_FORMAT_B2G3R3_UNORM, &uc);
            break;

        case 4:
            rgba_to_bgra(border_swizzled);
            util_pack_color(border_swizzled, PIPE_FORMAT_B4G4R4A4_UNORM, &uc);
            break;

        case 5:
            rgba_to_bgra(border_swizzled);
            if (desc->channel[1].size == 5) {
                util_pack_color(border_swizzled, PIPE_FORMAT_B5G5R5A1_UNORM, &uc);
            } else if (desc->channel[1].size == 6) {
                util_pack_color(border_swizzled, PIPE_FORMAT_B5G6R5_UNORM, &uc);
            } else {
                assert(0);
            }
            break;

        default:
        case 8:
            if (desc->channel[0].type == UTIL_FORMAT_TYPE_SIGNED) {
                util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_SNORM, &uc);
            } else if (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB) {
                if (desc->nr_channels == 2) {
                    border_swizzled[3] = border_swizzled[1];
                    util_pack_color(border_swizzled, PIPE_FORMAT_L8A8_SRGB, &uc);
                } else {
                    util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_SRGB, &uc);
                }
            } else {
                util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_UNORM, &uc);
            }
            break;

        case 10:
            util_pack_color(border_swizzled, PIPE_FORMAT_R10G10B10A2_UNORM, &uc);
            break;

        case 16:
            if (desc->nr_channels <= 2) {
                if (desc->channel[0].type == UTIL_FORMAT_TYPE_FLOAT) {
                    util_pack_color(border_swizzled, PIPE_FORMAT_R16G16_FLOAT, &uc);
                } else if (desc->channel[0].type == UTIL_FORMAT_TYPE_SIGNED) {
                    util_pack_color(border_swizzled, PIPE_FORMAT_R16G16_SNORM, &uc);
                } else {
                    util_pack_color(border_swizzled, PIPE_FORMAT_R16G16_UNORM, &uc);
                }
            } else {
                if (desc->channel[0].type == UTIL_FORMAT_TYPE_SIGNED) {
                    util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_SNORM, &uc);
                } else {
                    util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_UNORM, &uc);
                }
            }
            break;

        case 32:
            if (desc->nr_channels == 1) {
                util_pack_color(border_swizzled, PIPE_FORMAT_R32_FLOAT, &uc);
            } else {
                util_pack_color(border_swizzled, PIPE_FORMAT_R8G8B8A8_UNORM, &uc);
            }
            break;
    }

    return uc.ui[0];
}

static void r300_merge_textures_and_samplers(struct r300_context* r300)
{
    struct r300_textures_state *state =
        (struct r300_textures_state*)r300->textures_state.state;
    struct r300_texture_sampler_state *texstate;
    struct r300_sampler_state *sampler;
    struct r300_sampler_view *view;
    struct r300_resource *tex;
    unsigned base_level, min_level, level_count, i, j, size;
    unsigned count = MIN2(state->sampler_view_count,
                          state->sampler_state_count);
    bool has_us_format = r300->screen->caps.has_us_format;

    /* The KIL opcode fix, see below. */
    if (!count && !r300->screen->caps.is_r500)
        count = 1;

    /* Polygon-stipple draws sample the driver-owned stipple texture from the
     * unit right past the program's own bindings.  The view is spliced into
     * the slot with a reference and stays there while the merged state can
     * name it: r300_emit_buffer_validate re-reads sampler_views[] by
     * tx_enable to validate relocations, the same contract the texkill
     * sampler below relies on.  The sampler CSO is merge-local. */
    int pstip_want = -1;
    if (r300->pstipple_draw && r300->fs.state && r300->pstipple_sampler_view &&
        r300->pstipple_sampler) {
        struct r300_fragment_shader *fs = r300_fs(r300);
        unsigned unit = fs->pstipple_sampler_unit;
        /* Prefer the unit the compiled stipple variant actually lowered to. */
        if (fs->shader && fs->shader->compare_state.pstipple &&
            fs->shader->pstipple_lowered_unit != ~0u)
            unit = fs->shader->pstipple_lowered_unit;
        if (unit < r300->screen->caps.num_tex_units)
            pstip_want = (int)unit;
    }

    if (r300->pstipple_bound_unit >= 0) {
        unsigned bound = r300->pstipple_bound_unit;
        if (state->sampler_views[bound] !=
            (struct r300_sampler_view*)r300->pstipple_sampler_view) {
            /* The app bound its own view over the slot; drop the splice
             * tracking and any saved displaced view. */
            pipe_sampler_view_reference(&r300->pstipple_displaced_view, NULL);
            r300->pstipple_bound_unit = -1;
        } else if (pstip_want != (int)bound) {
            /* Restore the application view that the splice displaced, or
             * clear the slot when nothing was there. */
            pipe_sampler_view_reference(
                (struct pipe_sampler_view**)&state->sampler_views[bound],
                r300->pstipple_displaced_view);
            pipe_sampler_view_reference(&r300->pstipple_displaced_view, NULL);
            r300->pstipple_bound_unit = -1;
        }
    }

    struct r300_sampler_state *pstip_saved_state = NULL;
    bool pstip_injected = false;
    unsigned pstip_unit = 0;
    if (pstip_want >= 0) {
        pstip_unit = pstip_want;
        if (r300->pstipple_bound_unit < 0) {
            struct pipe_sampler_view *prior =
                (struct pipe_sampler_view *)state->sampler_views[pstip_unit];
            if (prior && prior != r300->pstipple_sampler_view)
                pipe_sampler_view_reference(&r300->pstipple_displaced_view,
                                            prior);
            pipe_sampler_view_reference(
                (struct pipe_sampler_view**)&state->sampler_views[pstip_unit],
                r300->pstipple_sampler_view);
            r300->pstipple_bound_unit = pstip_unit;
        }
        pstip_saved_state = state->sampler_states[pstip_unit];
        state->sampler_states[pstip_unit] =
            (struct r300_sampler_state*)r300->pstipple_sampler;
        count = MAX2(count, pstip_unit + 1);
        pstip_injected = true;
    }

    state->tx_enable = 0;
    state->count = 0;
    size = 2;

    for (i = 0; i < count; i++) {
        if (state->sampler_views[i] && state->sampler_states[i]) {
            state->tx_enable |= 1U << i;

            view = state->sampler_views[i];
            tex = r300_resource(view->base.texture);
            sampler = state->sampler_states[i];

            texstate = &state->regs[i];
            texstate->format = view->format;
            texstate->filter0 = sampler->filter0;
            texstate->filter1 = sampler->filter1;

            /* Set the border color. */
            texstate->border_color =
                r300_get_border_color(view->base.format,
                                      sampler->state.border_color.f,
                                      r300->screen->caps.is_r500);

            /* determine min/max levels */
            base_level = view->base.u.tex.first_level;
            min_level = sampler->min_lod;
            level_count = MIN3(sampler->max_lod,
                               tex->b.last_level - base_level,
                               view->base.u.tex.last_level - base_level);

            if (base_level + min_level) {
                unsigned offset;

                if (tex->tex.is_npot) {
                    /* Even though we do not implement mipmapping for NPOT
                     * textures, we should at least honor the minimum level
                     * which is allowed to be displayed. We do this by setting up
                     * an i-th mipmap level as the zero level. */
                    base_level += min_level;
                }
                offset = tex->tex.offset_in_bytes[base_level];

                r300_texture_setup_format_state(r300->screen, tex,
                                                view->base.format,
                                                base_level,
                                                view->width0_override,
		                                view->height0_override,
                                                &texstate->format);
                texstate->format.tile_config |= offset & 0xffffffe0;
                assert((offset & 0x1f) == 0);
            }

            /* Assign a texture cache region. */
            texstate->format.format1 |= view->texcache_region;

            /* Depth textures are kinda special. */
            if (util_format_is_depth_or_stencil(view->base.format)) {
                unsigned char depth_swizzle[4];

                if (!r300->screen->caps.is_r500 &&
                    util_format_get_blocksizebits(view->base.format) == 32) {
                    /* X24x8 is sampled as Y16X16 on r3xx-r4xx.
                     * The depth here is at the Y component. */
                    for (j = 0; j < 4; j++)
                        depth_swizzle[j] = PIPE_SWIZZLE_Y;
                } else {
                    for (j = 0; j < 4; j++)
                        depth_swizzle[j] = PIPE_SWIZZLE_X;
                }

                /* If compare mode is disabled, sampler view swizzles
                 * are stored in the format.
                 * Otherwise, the swizzles must be applied after the compare
                 * mode in the fragment shader. */
                if (sampler->state.compare_mode == PIPE_TEX_COMPARE_NONE) {
                    texstate->format.format1 |=
                        r300_get_swizzle_combined(depth_swizzle,
                                                  view->swizzle, false);
                } else {
                    texstate->format.format1 |=
                        r300_get_swizzle_combined(depth_swizzle, NULL, false);
                }
            }

            if (r300->screen->caps.dxtc_swizzle &&
                util_format_is_compressed(view->base.format)) {
                texstate->filter1 |= R400_DXTC_SWIZZLE_ENABLE;
            }

            /* to emulate 1D textures through 2D ones correctly */
            if (tex->b.target == PIPE_TEXTURE_1D) {
                texstate->filter0 &= ~R300_TX_WRAP_T_MASK;
                texstate->filter0 |= R300_TX_WRAP_T(R300_TX_CLAMP_TO_EDGE);
            }

            /* The hardware doesn't like CLAMP and CLAMP_TO_BORDER
             * for the 3rd coordinate if the texture isn't 3D. */
            if (tex->b.target != PIPE_TEXTURE_3D) {
                texstate->filter0 &= ~R300_TX_WRAP_R_MASK;
            }

            if (tex->tex.is_npot) {
                /* NPOT textures don't support mip filter, unfortunately.
                 * This prevents incorrect rendering. */
                texstate->filter0 &= ~R300_TX_MIN_FILTER_MIP_MASK;

                /* Mask out the mirrored flag. */
                if (texstate->filter0 & R300_TX_WRAP_S(R300_TX_MIRRORED)) {
                    texstate->filter0 &= ~R300_TX_WRAP_S(R300_TX_MIRRORED);
                }
                if (texstate->filter0 & R300_TX_WRAP_T(R300_TX_MIRRORED)) {
                    texstate->filter0 &= ~R300_TX_WRAP_T(R300_TX_MIRRORED);
                }

                /* Change repeat to clamp-to-edge.
                 * (the repeat bit has a value of 0, no masking needed). */
                if ((texstate->filter0 & R300_TX_WRAP_S_MASK) ==
                    R300_TX_WRAP_S(R300_TX_REPEAT)) {
                    texstate->filter0 |= R300_TX_WRAP_S(R300_TX_CLAMP_TO_EDGE);
                }
                if ((texstate->filter0 & R300_TX_WRAP_T_MASK) ==
                    R300_TX_WRAP_T(R300_TX_REPEAT)) {
                    texstate->filter0 |= R300_TX_WRAP_T(R300_TX_CLAMP_TO_EDGE);
                }
            } else {
                /* the MAX_MIP level is the largest (finest) one */
                texstate->format.format0 |= R300_TX_NUM_LEVELS(level_count);
                texstate->filter0 |= R300_TX_MAX_MIP_LEVEL(min_level);
            }

            /* Float textures only support nearest and mip-nearest filtering. */
            if (util_format_is_float(view->base.format)) {
                /* No MAG linear filtering. */
                if ((texstate->filter0 & R300_TX_MAG_FILTER_MASK) ==
                    R300_TX_MAG_FILTER_LINEAR) {
                    texstate->filter0 &= ~R300_TX_MAG_FILTER_MASK;
                    texstate->filter0 |= R300_TX_MAG_FILTER_NEAREST;
                }
                /* No MIN linear filtering. */
                if ((texstate->filter0 & R300_TX_MIN_FILTER_MASK) ==
                    R300_TX_MIN_FILTER_LINEAR) {
                    texstate->filter0 &= ~R300_TX_MIN_FILTER_MASK;
                    texstate->filter0 |= R300_TX_MIN_FILTER_NEAREST;
                }
                /* No mipmap linear filtering. */
                if ((texstate->filter0 & R300_TX_MIN_FILTER_MIP_MASK) ==
                    R300_TX_MIN_FILTER_MIP_LINEAR) {
                    texstate->filter0 &= ~R300_TX_MIN_FILTER_MIP_MASK;
                    texstate->filter0 |= R300_TX_MIN_FILTER_MIP_NEAREST;
                }
                /* No anisotropic filtering. */
                texstate->filter0 &= ~R300_TX_MAX_ANISO_MASK;
                texstate->filter1 &= ~R500_TX_MAX_ANISO_MASK;
                texstate->filter1 &= ~R500_TX_ANISO_HIGH_QUALITY;
            }

            texstate->filter0 |= i << 28;

            size += 16 + (has_us_format ? 2 : 0);
            state->count = i+1;
        } else {
            /* For the KIL opcode to work on r3xx-r4xx, the texture unit
             * assigned to this opcode (it's always the first one) must be
             * enabled. Otherwise the opcode doesn't work.
             *
             * In order to not depend on the fragment shader, we just make
             * the first unit enabled all the time. */
            if (i == 0 && !r300->screen->caps.is_r500) {
                pipe_sampler_view_reference(
                        (struct pipe_sampler_view**)&state->sampler_views[i],
                        &r300->texkill_sampler->base);

                state->tx_enable |= 1U << i;

                texstate = &state->regs[i];

                /* Just set some valid state. */
                texstate->format = r300->texkill_sampler->format;
                texstate->filter0 =
                        r300_translate_tex_filters(PIPE_TEX_FILTER_NEAREST,
                                                   PIPE_TEX_FILTER_NEAREST,
                                                   PIPE_TEX_FILTER_NEAREST,
                                                   false);
                texstate->filter1 = 0;
                texstate->border_color = 0;

                texstate->filter0 |= i << 28;
                size += 16 + (has_us_format ? 2 : 0);
                state->count = i+1;
            }
        }
    }

    if (pstip_injected)
        state->sampler_states[pstip_unit] = pstip_saved_state;

    r300->textures_state.size = size;

    /* Pick a fragment shader based on either the texture compare state
     * or the uses_pitch flag or some other external state. */
    if (count &&
        r300->fs_status == FRAGMENT_SHADER_VALID) {
        r300->fs_status = FRAGMENT_SHADER_MAYBE_DIRTY;
    }
}

static void r300_decompress_depth_textures(struct r300_context *r300)
{
    struct r300_textures_state *state =
        (struct r300_textures_state*)r300->textures_state.state;
    struct pipe_resource *tex;
    unsigned count = MIN2(state->sampler_view_count,
                          state->sampler_state_count);
    unsigned i;

    if (!r300->locked_zbuffer) {
        return;
    }

    for (i = 0; i < count; i++) {
        if (state->sampler_views[i] && state->sampler_states[i]) {
            tex = state->sampler_views[i]->base.texture;

            if (tex == r300->locked_zbuffer->texture) {
                r300_decompress_zmask_locked(r300);
                return;
            }
        }
    }
}

static void r300_validate_fragment_shader(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb = r300->fb_state.state;

    if (r300->fs.state && r300->fs_status != FRAGMENT_SHADER_VALID) {
        struct r300_fragment_program_external_state state;
        memset(&state, 0, sizeof(state));
        r300_fragment_program_get_external_state(r300, &state);

        /* Pick the fragment shader based on external states.
         * Then mark the state dirty if the fragment shader is either dirty
         * or the function r300_pick_fragment_shader changed the shader. */
        if (r300_pick_fragment_shader(r300, r300_fs(r300), &state) ||
            r300->fs_status == FRAGMENT_SHADER_DIRTY) {
            /* Mark the state atom as dirty. */
            r300_mark_fs_code_dirty(r300);

            /* Does Multiwrite need to be changed? */
            if (fb->nr_cbufs > 1) {
                bool new_multiwrite =
                    r300_fragment_shader_writes_all(r300_fs(r300));

                if (r300->fb_multiwrite != new_multiwrite) {
                    r300->fb_multiwrite = new_multiwrite;
                    r300_mark_fb_state_dirty(r300, R300_CHANGED_MULTIWRITE);
                }
            }
        }
        r300->fs_status = FRAGMENT_SHADER_VALID;
    }
}

static void r300_pick_vertex_shader(struct r300_context *r300)
{
    struct r300_vertex_shader_code *ptr;
    struct r300_vertex_shader *vs = r300_vs(r300);

    if (r300->vs_state.state) {
        bool wpos = r300_fs(r300)->shader->inputs.wpos != ATTR_UNUSED;

        if (!vs->first) {
            /* Build the vertex shader for the first time. */
            vs->first = vs->shader = CALLOC_STRUCT(r300_vertex_shader_code);
            vs->first->wpos = wpos;
            r300_translate_vertex_shader(r300, vs);
            if (!vs->first->dummy) {
                r300_mark_vs_code_dirty(r300);
                r300_mark_atom_dirty(r300, &r300->rs_block_state);
            }
            return;
        }
        /* Pick the vertex shader based on whether we need wpos */
        if (vs->first->wpos != wpos) {
            if (vs->first->next && vs->first->next->wpos == wpos) {
                ptr = vs->first->next;
                vs->first->next = NULL;
                ptr->next = vs->first;
                vs->first = vs->shader = ptr;
            } else {
                ptr = CALLOC_STRUCT(r300_vertex_shader_code);
                ptr->next = vs->first;
                vs->first = vs->shader = ptr;
                vs->shader->wpos = wpos;
                r300_translate_vertex_shader(r300, vs);
            }
            if (!vs->first->dummy) {
                r300_mark_vs_code_dirty(r300);
                r300_mark_atom_dirty(r300, &r300->rs_block_state);
            }
        }
    }
}

/* Rebuild the SWTCL hardware vertex layout, its PSC routing, and the RS block
 * as one unit. The three must stay index-consistent: r300_update_rs_block
 * fills stream_loc_notcl[], r300_draw_emit_all_attribs fills vertex_info[], and
 * r300_swtcl_vertex_psc zips them. r300_render_get_vertex_info reruns this at
 * draw run time to fold in the gl_PointCoord (PCOORD) draw output, which the
 * wide-point stage allocates only once the pipeline runs. */
void r300_swtcl_rebuild_vertex_layout(struct r300_context *r300)
{
    r300_update_rs_block(r300);

    if (r300->draw) {
        memset(&r300->vertex_info, 0, sizeof(struct vertex_info));
        r300_draw_emit_all_attribs(r300);
        draw_compute_vertex_size(&r300->vertex_info);
        r300_swtcl_vertex_psc(r300);
    }

    /* No-submit VAP/RS tuple capture: all three atomic layers are now fresh.
     * The GL SW-TCL path and the r3v path (pipe->draw_vbo -> this derivation)
     * both reach here, so the dump reports both in one field vocabulary. */
    if (getenv("R300_VAP_RS_INSPECT"))
        r300_dump_vap_rs_tuple(r300, "swtcl_rebuild");
}

void r300_update_derived_state(struct r300_context* r300)
{
    if (r300->textures_state.dirty) {
        r300_decompress_depth_textures(r300);
        r300_merge_textures_and_samplers(r300);
    }

    r300_validate_fragment_shader(r300);
    if (r300->screen->caps.has_tcl)
        r300_pick_vertex_shader(r300);

    if (r300->rs_block_state.dirty) {
        r300_swtcl_rebuild_vertex_layout(r300);
    }

    r300_update_hyperz_state(r300);
}
