/*
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "r300_context.h"
#include "r300_emit.h"
#include "r300_texture.h"
#include "r300_reg.h"

#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_math.h"
#include "util/u_pack_color.h"
#include "util/u_surface.h"

enum r300_blitter_op /* bitmask */
{
    R300_STOP_QUERY         = 1,
    R300_SAVE_TEXTURES      = 2,
    R300_SAVE_FRAMEBUFFER   = 4,
    R300_IGNORE_RENDER_COND = 8,

    R300_CLEAR         = R300_STOP_QUERY,

    R300_CLEAR_SURFACE = R300_STOP_QUERY | R300_SAVE_FRAMEBUFFER,

    R300_COPY          = R300_STOP_QUERY | R300_SAVE_FRAMEBUFFER |
                         R300_SAVE_TEXTURES | R300_IGNORE_RENDER_COND,

    R300_BLIT          = R300_STOP_QUERY | R300_SAVE_FRAMEBUFFER |
                         R300_SAVE_TEXTURES,

    R300_DECOMPRESS    = R300_STOP_QUERY | R300_IGNORE_RENDER_COND,
};

static void r300_blitter_begin(struct r300_context* r300, enum r300_blitter_op op)
{
    if ((op & R300_STOP_QUERY) && r300->query_current) {
        r300->blitter_saved_query = r300->query_current;
        r300_stop_query(r300);
    }

    /* Yeah we have to save all those states to ensure the blitter operation
     * is really transparent. The states will be restored by the blitter once
     * copying is done. */
    util_blitter_save_blend(r300->blitter, r300->blend_state.state);
    util_blitter_save_depth_stencil_alpha(r300->blitter, r300->dsa_state.state);
    util_blitter_save_stencil_ref(r300->blitter, &(r300->stencil_ref));
    util_blitter_save_rasterizer(r300->blitter, r300->rs_state.state);
    util_blitter_save_fragment_shader(r300->blitter, r300->fs.state);
    util_blitter_save_vertex_shader(r300->blitter, r300->vs_state.state);
    util_blitter_save_viewport(r300->blitter, &r300->viewport);
    util_blitter_save_scissor(r300->blitter, r300->scissor_state.state);
    util_blitter_save_sample_mask(r300->blitter, *(unsigned*)r300->sample_mask.state, 0);
    util_blitter_save_vertex_buffers(r300->blitter, r300->vertex_buffer,
                                     r300->nr_vertex_buffers);
    util_blitter_save_vertex_elements(r300->blitter, r300->velems);

    struct pipe_constant_buffer cb = {
       /* r300 doesn't use the size for FS at all. The shader determines it.
        * Set something for blitter.
        */
       .buffer_size = 4,
       .user_buffer = ((struct r300_constant_buffer*)r300->fs_constants.state)->ptr,
    };
    util_blitter_save_fragment_constant_buffer_slot(r300->blitter, &cb);

    if (op & R300_SAVE_FRAMEBUFFER) {
        util_blitter_save_framebuffer(r300->blitter, r300->fb_state.state);
    }

    if (op & R300_SAVE_TEXTURES) {
        struct r300_textures_state* state =
            (struct r300_textures_state*)r300->textures_state.state;

        util_blitter_save_fragment_sampler_states(
            r300->blitter, state->sampler_state_count,
            (void**)state->sampler_states);

        util_blitter_save_fragment_sampler_views(
            r300->blitter, state->sampler_view_count,
            (struct pipe_sampler_view**)state->sampler_views);
    }

    if (op & R300_IGNORE_RENDER_COND) {
        /* Save the flag. */
        r300->blitter_saved_skip_rendering = r300->skip_rendering+1;
        r300->skip_rendering = false;
    } else {
        r300->blitter_saved_skip_rendering = 0;
    }
}

static void r300_blitter_end(struct r300_context *r300)
{
    if (r300->blitter_saved_query) {
        r300_resume_query(r300, r300->blitter_saved_query);
        r300->blitter_saved_query = NULL;
    }

    if (r300->blitter_saved_skip_rendering) {
        /* Restore the flag. */
        r300->skip_rendering = r300->blitter_saved_skip_rendering-1;
    }
}

static uint32_t r300_depth_clear_cb_value(enum pipe_format format,
                                          const float* rgba)
{
    union util_color uc;

    format = r300_unbyteswap_array_format(format);

    util_pack_color(rgba, format, &uc);

    if (util_format_get_blocksizebits(format) == 32) {
        /* CBZB clears reuse ZB_DEPTHCLEARVALUE, which expects the 32-bit
         * payload in little-endian byte order.
         */
        return util_cpu_to_le32(uc.ui[0]);
    }

    return uc.us | (uc.us << 16);
}

static bool r300_cbzb_clear_allowed(struct r300_context *r300,
                                    unsigned clear_buffers)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    /* Only color clear allowed, and only one colorbuffer. */
    if ((clear_buffers & ~PIPE_CLEAR_COLOR) != 0 || fb->nr_cbufs != 1 || !fb->cbufs[0].texture)
        return false;

    return r300_surface(r300->fb_cbufs[0])->cbzb_allowed;
}

static bool r300_fast_zclear_allowed(struct r300_context *r300,
                                     unsigned clear_buffers)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;
    struct r300_resource *tex = r300_resource(fb->zsbuf.texture);
    unsigned zmask_dwords = tex->tex.zmask_dwords[fb->zsbuf.level];

    if (!zmask_dwords)
        return false;

    /* Empirically, RV530 (R5xx, tested with 1P/2Z config) fails 3D_CLEAR_ZMASK
     * when the ZMASK buffer exceeds 0x1400 dwords.  The is_r500 guard limits
     * fast-clear to that threshold.
     *
     * The analyzed pre-R5xx integrated and RV3xx cases stay below the RV530
     * threshold through their allocation bound: RS480/RC410 and RV350/RV370/RV380
     * use zmask_ram = RV3xx_ZMASK_SIZE = 5120 (= 0x1400) dwords with one GB
     * pipe, so zmask_dwords is bounded by r300_setup_hyperz_properties().
     * This does not claim that every pre-R5xx pipe configuration is bounded
     * below 0x1400.
     * TODO: Confirm this static bound with a fast-clear test on RS480/RC410 hardware. */
    if (r300->screen->caps.is_r500 && zmask_dwords > 0x1400)
        return false;

    return true;
}

static bool r300_hiz_clear_allowed(struct r300_context *r300)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    return r300_resource(fb->zsbuf.texture)->tex.hiz_dwords[fb->zsbuf.level] != 0;
}

static uint32_t r300_depth_clear_value(enum pipe_format format,
                                       double depth, unsigned stencil)
{
    switch (format) {
        case PIPE_FORMAT_Z16_UNORM:
        case PIPE_FORMAT_X8Z24_UNORM:
            return util_pack_z(format, depth);

        case PIPE_FORMAT_S8_UINT_Z24_UNORM:
            return util_pack_z_stencil(format, depth, stencil);

        default:
            assert(0);
            return 0;
    }
}

static uint32_t r300_hiz_clear_value(double depth)
{
    uint32_t r = (uint32_t)(CLAMP(depth, 0, 1) * 255.5);
    assert(r <= 255);
    return r | (r << 8) | (r << 16) | (r << 24);
}

static void r300_set_clear_color(struct r300_context *r300,
                                 const union pipe_color_union *color)
{
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;
    union util_color uc;

    memset(&uc, 0, sizeof(uc));
    util_pack_color(color->f, fb->cbufs[0].format, &uc);

    if (fb->cbufs[0].format == PIPE_FORMAT_R16G16B16A16_FLOAT ||
        fb->cbufs[0].format == PIPE_FORMAT_R16G16B16X16_FLOAT) {
        /* (0,1,2,3) maps to (B,G,R,A) */
        r300->color_clear_value_gb = uc.h[0] | ((uint32_t)uc.h[1] << 16);
        r300->color_clear_value_ar = uc.h[2] | ((uint32_t)uc.h[3] << 16);
    } else {
        r300->color_clear_value = uc.ui[0];
    }
}

DEBUG_GET_ONCE_BOOL_OPTION(hyperz, "RADEON_HYPERZ", false)

/* Clear currently bound buffers. */
static void r300_clear(struct pipe_context* pipe,
                       unsigned buffers,
                       uint32_t color_clear_mask,
                       uint8_t stencil_clear_mask,
                       const struct pipe_scissor_state *scissor_state,
                       const union pipe_color_union *color,
                       double depth,
                       unsigned stencil)
{
    /* My notes about Zbuffer compression:
     *
     * 1) The zbuffer must be micro-tiled and whole microtiles must be
     *    written if compression is enabled. If microtiling is disabled,
     *    it locks up.
     *
     * 2) There is ZMASK RAM which contains a compressed zbuffer.
     *    Each dword of the Z Mask contains compression information
     *    for 16 4x4 pixel tiles, that is 2 bits for each tile.
     *    On chips with 2 Z pipes, every other dword maps to a different
     *    pipe. On newer chipsets, there is a new compression mode
     *    with 8x8 pixel tiles per 2 bits.
     *
     * 3) The FASTFILL bit has nothing to do with filling. It only tells hw
     *    it should look in the ZMASK RAM first before fetching from a real
     *    zbuffer.
     *
     * 4) If a pixel is in a cleared state, ZB_DEPTHCLEARVALUE is returned
     *    during zbuffer reads instead of the value that is actually stored
     *    in the zbuffer memory. A pixel is in a cleared state when its ZMASK
     *    is equal to 0. Therefore, if you clear ZMASK with zeros, you may
     *    leave the zbuffer memory uninitialized, but then you must enable
     *    compression, so that the ZMASK RAM is actually used.
     *
     * 5) Each 4x4 (or 8x8) tile is automatically decompressed and recompressed
     *    during zbuffer updates. A special decompressing operation should be
     *    used to fully decompress a zbuffer, which basically just stores all
     *    compressed tiles in ZMASK to the zbuffer memory.
     *
     * 6) For a 16-bit zbuffer, compression causes a hung with one or
     *    two samples and should not be used.
     *
     * 7) FORCE_COMPRESSED_STENCIL_VALUE should be enabled for stencil clears
     *    to avoid needless decompression.
     *
     * 8) Fastfill must not be used if reading of compressed Z data is disabled
     *    and writing of compressed Z data is enabled (RD/WR_COMP_ENABLE),
     *    i.e. it cannot be used to compress the zbuffer.
     *
     * 9) ZB_CB_CLEAR does not interact with zbuffer compression in any way.
     *
     * - Marek
     */

    struct r300_context* r300 = r300_context(pipe);
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;
    struct r300_hyperz_state *hyperz =
        (struct r300_hyperz_state*)r300->hyperz_state.state;
    uint32_t width = fb->width;
    uint32_t height = fb->height;
    uint32_t hyperz_dcv = hyperz->zb_depthclearvalue;

    /* Use fast Z clear.
     * The zbuffer must be in micro-tiled mode, otherwise it locks up. */
    if (buffers & PIPE_CLEAR_DEPTHSTENCIL) {
        bool zmask_clear, hiz_clear;

        /* If both depth and stencil are present, they must be cleared together. */
        if (fb->zsbuf.texture->format == PIPE_FORMAT_S8_UINT_Z24_UNORM &&
            (buffers & PIPE_CLEAR_DEPTHSTENCIL) != PIPE_CLEAR_DEPTHSTENCIL) {
            zmask_clear = false;
            hiz_clear = false;
        } else {
            zmask_clear = r300_fast_zclear_allowed(r300, buffers);
            hiz_clear = r300_hiz_clear_allowed(r300);
        }

        /* If we need Hyper-Z. */
        if (zmask_clear || hiz_clear) {
            /* Try to obtain the access to Hyper-Z buffers if we don't have one. */
            if (!r300->hyperz_enabled &&
                (r300->screen->caps.is_r500 || debug_get_option_hyperz())) {
                r300->hyperz_enabled =
                    r300->rws->cs_request_feature(&r300->cs,
                                                RADEON_FID_R300_HYPERZ_ACCESS,
                                                true);
                if (r300->hyperz_enabled) {
                   /* Need to emit HyperZ buffer regs for the first time. */
                   r300_mark_fb_state_dirty(r300, R300_CHANGED_HYPERZ_FLAG);
                }
            }

            /* Setup Hyper-Z clears. */
            if (r300->hyperz_enabled) {
                if (zmask_clear) {
                    hyperz_dcv = hyperz->zb_depthclearvalue =
                        r300_depth_clear_value(fb->zsbuf.format, depth, stencil);

                    r300_mark_atom_dirty(r300, &r300->zmask_clear);
                    r300_mark_atom_dirty(r300, &r300->gpu_flush);
                    buffers &= ~PIPE_CLEAR_DEPTHSTENCIL;
                }

                if (hiz_clear) {
                    r300->hiz_clear_value = r300_hiz_clear_value(depth);
                    r300_mark_atom_dirty(r300, &r300->hiz_clear);
                    r300_mark_atom_dirty(r300, &r300->gpu_flush);
                }
                r300->num_z_clears++;
            }
        }
    }

    /* Use fast color clear for an AA colorbuffer.
     * The CMASK is shared between all colorbuffers, so we use it
     * if there is only one colorbuffer bound. */
    if ((buffers & PIPE_CLEAR_COLOR) && fb->nr_cbufs == 1 && fb->cbufs[0].texture &&
        r300_resource(fb->cbufs[0].texture)->tex.cmask_dwords) {
        /* Try to obtain the access to the CMASK if we don't have one. */
        if (!r300->cmask_access) {
            r300->cmask_access =
                r300->rws->cs_request_feature(&r300->cs,
                                              RADEON_FID_R300_CMASK_ACCESS,
                                              true);
        }

        /* Setup the clear. */
        if (r300->cmask_access) {
            /* Pair the resource with the CMASK to avoid other resources
             * accessing it. */
            if (!r300->screen->cmask_resource) {
                mtx_lock(&r300->screen->cmask_mutex);
                /* Double checking (first unlocked, then locked). */
                if (!r300->screen->cmask_resource) {
                    /* Don't reference this, so that the texture can be
                     * destroyed while set in cmask_resource.
                     * Then in texture_destroy, we set cmask_resource to NULL. */
                    r300->screen->cmask_resource = fb->cbufs[0].texture;
                }
                mtx_unlock(&r300->screen->cmask_mutex);
            }

            if (r300->screen->cmask_resource == fb->cbufs[0].texture) {
                r300_set_clear_color(r300, color);
                r300_mark_atom_dirty(r300, &r300->cmask_clear);
                r300_mark_atom_dirty(r300, &r300->gpu_flush);
                buffers &= ~PIPE_CLEAR_COLOR;
            }
        }
    }
    /* Enable CBZB clear. */
    else if (r300_cbzb_clear_allowed(r300, buffers)) {
        struct r300_surface *surf = r300_surface(r300->fb_cbufs[0]);

        hyperz->zb_depthclearvalue =
                r300_depth_clear_cb_value(surf->base.format, color->f);

        width = surf->cbzb_width;
        height = surf->cbzb_height;

        r300->cbzb_clear = true;
        r300_mark_fb_state_dirty(r300, R300_CHANGED_HYPERZ_FLAG);
    }

    /* Clear. */
    if (buffers) {
        /* Clear using the blitter.  The clear quad rides the same GA
         * rasterizer conversion as every ordinary primitive, so a cleared
         * depth agrees bit-exactly with rasterized geometry under GL_EQUAL;
         * only an explicit gl_FragDepth export (floor(trunc_fp24(z) * 2^n)
         * in the US) diverges by one code at integral products, and that
         * divergence is a hardware property of the export unit, not of the
         * clear path. */
        r300_blitter_begin(r300, R300_CLEAR);
        /* The clear quad may reuse the same viewport dimensions as the
         * previous draw, but VAP_VTE_CNTL lives in the viewport atom and can
         * retain an incompatible coordinate-space mode. Force the atom before
         * DRAW_VBUF_2 so the VAP interprets the generated clear vertices in
         * the mode selected by the current blitter viewport. */
        r300_mark_atom_dirty(r300, &r300->viewport_state);
        util_blitter_clear(r300->blitter, width, height, 1,
                           buffers, color, depth, stencil,
                           util_framebuffer_get_num_samples(fb) > 1);
        r300_blitter_end(r300);
    } else if (r300->zmask_clear.dirty ||
               r300->hiz_clear.dirty ||
               r300->cmask_clear.dirty) {
        /* Just clear zmask and hiz now, this does not use the standard draw
         * procedure. */
        /* Calculate zmask_clear and hiz_clear atom sizes. */
        unsigned dwords =
            r300->gpu_flush.size +
            (r300->zmask_clear.dirty ? r300->zmask_clear.size : 0) +
            (r300->hiz_clear.dirty ? r300->hiz_clear.size : 0) +
            (r300->cmask_clear.dirty ? r300->cmask_clear.size : 0) +
            r300_get_num_cs_end_dwords(r300);

        /* Reserve CS space. */
        if (!r300->rws->cs_check_space(&r300->cs, dwords)) {
            r300_flush(&r300->context, PIPE_FLUSH_ASYNC, NULL);
        }

        /* Emit clear packets. */
        r300_emit_gpu_flush(r300, r300->gpu_flush.size, r300->gpu_flush.state);
        r300->gpu_flush.dirty = false;

        if (r300->zmask_clear.dirty) {
            r300_emit_zmask_clear(r300, r300->zmask_clear.size,
                                  r300->zmask_clear.state);
            r300->zmask_clear.dirty = false;
        }
        if (r300->hiz_clear.dirty) {
            r300_emit_hiz_clear(r300, r300->hiz_clear.size,
                                r300->hiz_clear.state);
            r300->hiz_clear.dirty = false;
        }
        if (r300->cmask_clear.dirty) {
            r300_emit_cmask_clear(r300, r300->cmask_clear.size,
                                  r300->cmask_clear.state);
            r300->cmask_clear.dirty = false;
        }
    } else {
        assert(0);
    }

    /* Disable CBZB clear. */
    if (r300->cbzb_clear) {
        r300->cbzb_clear = false;
        hyperz->zb_depthclearvalue = hyperz_dcv;
        r300_mark_fb_state_dirty(r300, R300_CHANGED_HYPERZ_FLAG);
    }

    /* If we are clearing texture currently bound for sampling we need to invalidate the cache. */
    if (buffers & PIPE_CLEAR_COLOR) {
        struct r300_textures_state *texstate =
            (struct r300_textures_state*)r300->textures_state.state;
        for (unsigned i = 0; i < fb->nr_cbufs; i++) {
            struct pipe_resource *cbuf_tex = fb->cbufs[i].texture;
            if (!cbuf_tex)
                continue;
            for (unsigned s = 0; s < texstate->sampler_view_count; s++) {
                struct r300_sampler_view *view = texstate->sampler_views[s];
                if (view && view->base.texture == cbuf_tex) {
                    r300_mark_atom_dirty(r300, &r300->texture_cache_inval);
                    break;
                }
            }
        }
    }

    /* Enable fastfill and/or hiz.
     *
     * If we cleared zmask/hiz, it's in use now. The Hyper-Z state update
     * looks if zmask/hiz is in use and programs hardware accordingly. */
    if (r300->zmask_in_use || r300->hiz_in_use) {
        r300_mark_atom_dirty(r300, &r300->hyperz_state);
    }
}

/* Clear a region of a color surface to a constant value. */
static void r300_clear_render_target(struct pipe_context *pipe,
                                     struct pipe_surface *dst,
                                     const union pipe_color_union *color,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height,
                                     bool render_condition_enabled)
{
    struct r300_context *r300 = r300_context(pipe);

    r300_blitter_begin(r300, R300_CLEAR_SURFACE |
                       (render_condition_enabled ? 0 : R300_IGNORE_RENDER_COND));
    util_blitter_clear_render_target(r300->blitter, dst, color,
                                     dstx, dsty, width, height);
    r300_blitter_end(r300);
}

/* Clear a region of a depth stencil surface. */
static void r300_clear_depth_stencil(struct pipe_context *pipe,
                                     struct pipe_surface *dst,
                                     unsigned clear_flags,
                                     double depth,
                                     unsigned stencil,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height,
                                     bool render_condition_enabled)
{
    struct r300_context *r300 = r300_context(pipe);
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;

    if (r300->zmask_in_use && !r300->locked_zbuffer) {
        if (fb->zsbuf.texture == dst->texture) {
            r300_decompress_zmask(r300);
        }
    }

    /* XXX Do not decompress ZMask of the currently-set zbuffer. */
    r300_blitter_begin(r300, R300_CLEAR_SURFACE |
                       (render_condition_enabled ? 0 : R300_IGNORE_RENDER_COND));
    util_blitter_clear_depth_stencil(r300->blitter, dst, clear_flags, depth, stencil,
                                     dstx, dsty, width, height);
    r300_blitter_end(r300);
}

void r300_decompress_zmask(struct r300_context *r300)
{
    struct pipe_surface *zsurf = r300->fb_zsbuf;

    if (zsurf) {
        r300_decompress_zmask_locked_unsafe(r300);
    }
}

void r300_decompress_zmask_locked_unsafe(struct r300_context *r300)
{
    struct pipe_context *pipe = &r300->context;
    struct pipe_framebuffer_state *fb =
        (struct pipe_framebuffer_state*)r300->fb_state.state;
    struct pipe_surface *zsurf = r300->locked_zbuffer;
    struct pipe_surface surf_tmpl;

    if (!zsurf)
        zsurf = fb->zsbuf.texture ? r300_create_surface(pipe, fb->zsbuf.texture, &fb->zsbuf) : NULL;

    if (!zsurf)
        return;

    if (r300->hiz_in_use) {
        struct pipe_box box = {0, 0, 0, fb->width, fb->height, 1};

        r300_blitter_begin(r300, R300_DECOMPRESS);
        util_blitter_custom_depth_stencil(r300->blitter, zsurf, NULL,
                                          r300->dsa_decompress_zmask, 1.0f,
                                          0, zsurf->width, zsurf->height);
        r300_blitter_end(r300);

        pipe->resource_copy_region(pipe, zsurf->texture, zsurf->level,
                                   0, 0, 0, zsurf->texture, zsurf->level,
                                   &box);
        r300->hiz_in_use = false;
    }

    memset(&surf_tmpl, 0, sizeof(surf_tmpl));
    surf_tmpl.format = zsurf->format;
    surf_tmpl.u.tex.level = zsurf->level;

    r300_blitter_begin(r300, R300_DECOMPRESS);
    util_blitter_custom_depth_stencil(r300->blitter, zsurf, NULL,
                                      r300->dsa_decompress_zmask, 1.0f,
                                      0, zsurf->width, zsurf->height);
    r300_blitter_end(r300);

    if (!r300->locked_zbuffer)
        pipe_surface_unref(pipe, &zsurf, r300_surface_destroy);
}

void r300_flush_depth_stencil(struct pipe_context *pipe,
                              struct pipe_resource *dst,
                              unsigned level,
                              unsigned layer)
{
    struct r300_context *r300 = r300_context(pipe);
    struct pipe_surface surf_tmpl, *surf;

    memset(&surf_tmpl, 0, sizeof(surf_tmpl));
    surf_tmpl.format = dst->format;
    surf_tmpl.u.tex.level = level;
    surf_tmpl.u.tex.first_layer = layer;
    surf_tmpl.u.tex.last_layer = layer;

    surf = r300_create_surface(pipe, dst, &surf_tmpl);
    r300_clear_depth_stencil(pipe, surf, PIPE_CLEAR_DEPTH, 1.0, 0, 0, 0,
                             dst->width0, dst->height0, false);
    pipe_surface_unref(pipe, &surf, r300_surface_destroy);
}

void r300_blit_init(struct pipe_context *pipe)
{
    pipe->clear = r300_clear;
    pipe->clear_render_target = r300_clear_render_target;
    pipe->clear_depth_stencil = r300_clear_depth_stencil;
    pipe->flush_depth_stencil = r300_flush_depth_stencil;
    pipe->blit = r300_blit;
    pipe->resource_copy_region = r300_resource_copy_region;
}
