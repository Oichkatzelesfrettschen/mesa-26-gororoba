/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 * Copyright 2010 Marek Olšák <maraeo@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"
#include "util/format/u_format.h"
#include "util/format/u_format_s3tc.h"
#include "util/u_screen.h"
#include "util/u_memory.h"
#include "util/u_endian.h"
#include "util/hex.h"
#include "util/os_time.h"
#include "util/xmlconfig.h"
#include "vl/vl_video_buffer.h"

#include "r300_context.h"
#include "amd/r300/compiler/r300_nir.h"
#include "r300_texture.h"
#include "r300_screen_buffer.h"
#include "r300_r2vb_plan.h"
#include "r300_state_inlines.h"
#include "r300_public.h"
#include "r300_video.h"
#include "r300_hb_r400_us.h"

#include "draw/draw_context.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return the identifier behind whom the brave coders responsible for this
 * amalgamation of code, sweat, and duct tape, routinely obscure their names.
 *
 * ...I should have just put "Corbin Simpson", but I'm not that cool.
 *
 * (Or egotistical. Yet.) */
static const char* r300_get_vendor(struct pipe_screen* pscreen)
{
    return "Mesa";
}

static const char* r300_get_device_vendor(struct pipe_screen* pscreen)
{
    return "ATI";
}

static bool r300_experimental_ati2n_enabled(void)
{
    const char *ati2n = getenv("R300_EXPERIMENTAL_ATI2N");

    /* Literal 1 is required because this gate exposes unvalidated R300-class
     * RGTC2 sampler behavior; unset, empty, 0, and boolean aliases stay off.
     */
    return ati2n && strcmp(ati2n, "1") == 0;
}

static const char* chip_families[] = {
    "unknown",
    "ATI R300",
    "ATI R350",
    "ATI RV350",
    "ATI RV370",
    "ATI RV380",
    "ATI RS400",
    "ATI RC410",
    "ATI RS480",
    "ATI R420",
    "ATI R423",
    "ATI R430",
    "ATI R480",
    "ATI R481",
    "ATI RV410",
    "ATI RS600",
    "ATI RS690",
    "ATI RS740",
    "ATI RV515",
    "ATI R520",
    "ATI RV530",
    "ATI R580",
    "ATI RV560",
    "ATI RV570"
};

static const char* r300_get_family_name(struct r300_screen* r300screen)
{
    return chip_families[r300screen->caps.family];
}

static const char* r300_get_name(struct pipe_screen* pscreen)
{
    struct r300_screen* r300screen = r300_screen(pscreen);

    return r300_get_family_name(r300screen);
}

static void r300_disk_cache_create(struct r300_screen* r300screen)
{
    blake3_hasher ctx;
    unsigned char blake3[BLAKE3_KEY_LEN];
    char cache_id[BLAKE3_HEX_LEN];

    _mesa_blake3_init(&ctx);
    if (!disk_cache_get_function_identifier(r300_disk_cache_create,
                                            &ctx))
        return;

    _mesa_blake3_final(&ctx, blake3);
    mesa_bytes_to_hex(cache_id, blake3, BLAKE3_KEY_LEN);

    r300screen->disk_shader_cache =
                    disk_cache_create(r300_get_family_name(r300screen),
                                      cache_id,
                                      r300screen->debug);
}

static struct disk_cache* r300_get_disk_shader_cache(struct pipe_screen* pscreen)
{
	struct r300_screen* r300screen = r300_screen(pscreen);
	return r300screen->disk_shader_cache;
}

#define COMMON_NIR_OPTIONS_BASE               \
   .lower_bitops = true,                      \
   .lower_extract_byte = true,                \
   .lower_extract_word = true,                \
   .lower_fceil = true,                       \
   .lower_fdiv = true,                        \
   .lower_fdph = true,                        \
   .lower_ffloor = true,                      \
   .lower_flrp32 = true,                      \
   .lower_flrp64 = true,                      \
   .lower_fmod = true,                        \
   .lower_fsign = true,                       \
   .lower_fsqrt = true,                       \
   .lower_ftrunc = true,                      \
   .lower_insert_byte = true,                 \
   .lower_insert_word = true,                 \
   .lower_uniforms_to_ubo = true,             \
   .no_integers = true

#define COMMON_NIR_OPTIONS                    \
   .float_mul_add32 =                         \
      nir_float_muladd_support_has_fmad |     \
      nir_float_muladd_support_fuse,          \
   .fdot_replicates = true,                   \
   COMMON_NIR_OPTIONS_BASE

static const nir_shader_compiler_options r500_vs_compiler_options = {
   COMMON_NIR_OPTIONS,
   .has_fused_comp_and_csel = true,
   /* Have HW loops support and 1024 max instr count, but don't unroll *too*
    * hard.
    */
   .max_unroll_iterations = 29,
};

static const nir_shader_compiler_options r500_fs_compiler_options = {
   COMMON_NIR_OPTIONS,
   .lower_fpow = true, /* POW is only in the VS */
   .has_fused_comp_and_csel = true,

   /* Have HW loops support and 512 max instr count, but don't unroll *too*
    * hard.
    */
   .max_unroll_iterations = 32,
};

static const nir_shader_compiler_options r300_vs_compiler_options = {
   COMMON_NIR_OPTIONS,
   .lower_fsat = true, /* No fsat in pre-r500 VS */
   .lower_sincos = true,

   /* Note: has HW loops support, but only 256 ALU instructions. */
   .max_unroll_iterations = 32,
};

static const nir_shader_compiler_options r400_vs_compiler_options = {
   COMMON_NIR_OPTIONS,
   .lower_fsat = true, /* No fsat in pre-r500 VS */

   /* Note: has HW loops support, but only 256 ALU instructions. */
   .max_unroll_iterations = 32,
};

static const nir_shader_compiler_options r300_fs_compiler_options = {
   COMMON_NIR_OPTIONS,
   .lower_fpow = true, /* POW is only in the VS */
   .lower_sincos = true,
   .has_fused_comp_and_csel = true,

    /* No HW loops support, so set it equal to ALU instr max */
   .max_unroll_iterations = 64,
};

static const nir_shader_compiler_options gallivm_compiler_options = {
   COMMON_NIR_OPTIONS_BASE,
   .float_mul_add32 = nir_float_muladd_support_keep_weak_ffma,
   .has_fused_comp_and_csel = true,
   .max_unroll_iterations = 32,

   .support_indirect_inputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES),
   .support_indirect_outputs = (uint8_t)BITFIELD_MASK(MESA_SHADER_STAGES),
};

/* Select the per-stage NIR options from the chip caps.  Exported so a host
 * harness that fakes a screen (caps set by hand) carries the same options a
 * created screen would -- the SW-TCL vertex stage in particular selects the
 * gallivm table, whose keep_weak_ffma is a representation fact the R2VB
 * producer route consumes. */
void
r300_screen_init_nir_options(struct r300_screen *r300screen)
{
    r300screen->screen.nir_options[MESA_SHADER_VERTEX] =
        !r300screen->caps.has_tcl ? &gallivm_compiler_options :
        r300screen->caps.is_r500 ? &r500_vs_compiler_options :
        r300screen->caps.is_r400 ? &r400_vs_compiler_options :
                                   &r300_vs_compiler_options;
    r300screen->screen.nir_options[MESA_SHADER_FRAGMENT] =
        r300screen->caps.is_r500 ? &r500_fs_compiler_options
                                 : &r300_fs_compiler_options;
}


/**
 * Whether the format matches:
 *   PIPE_FORMAT_?10?10?10?2_UNORM
 */
static inline bool
util_format_is_rgba1010102_variant(const struct util_format_description *desc)
{
   static const unsigned size[4] = {10, 10, 10, 2};
   unsigned chan;

   if (desc->block.width != 1 ||
       desc->block.height != 1 ||
       desc->block.bits != 32)
      return false;

   for (chan = 0; chan < 4; ++chan) {
      if(desc->channel[chan].type != UTIL_FORMAT_TYPE_UNSIGNED &&
         desc->channel[chan].type != UTIL_FORMAT_TYPE_VOID)
         return false;
      if (desc->channel[chan].size != size[chan])
         return false;
   }

   return true;
}

static bool r300_is_blending_supported(struct r300_screen *rscreen,
                                       enum pipe_format format)
{
    int c;
    const struct util_format_description *desc =
        util_format_description(format);

    if (desc->layout != UTIL_FORMAT_LAYOUT_PLAIN)
        return false;

    c = util_format_get_first_non_void_channel(format);

    /* RGBA16F */
    if (rscreen->caps.is_r500 &&
        desc->nr_channels == 4 &&
        desc->channel[c].size == 16 &&
        desc->channel[c].type == UTIL_FORMAT_TYPE_FLOAT)
        return true;

    if (desc->channel[c].normalized &&
        desc->channel[c].type == UTIL_FORMAT_TYPE_UNSIGNED &&
        desc->channel[c].size >= 4 &&
        desc->channel[c].size <= 10) {
        /* RGB10_A2, RGBA8, RGB5_A1, RGBA4, RGB565 */
        if (desc->nr_channels >= 3)
            return true;

        if (format == PIPE_FORMAT_R8G8_UNORM)
            return true;

        /* R8, I8, L8, A8 */
        if (desc->nr_channels == 1)
            return true;
    }

    return false;
}

static bool r300_is_format_supported(struct pipe_screen* screen,
                                     enum pipe_format format,
                                     enum pipe_texture_target target,
                                     unsigned sample_count,
                                     unsigned storage_sample_count,
                                     unsigned usage)
{
    uint32_t retval = 0;
    bool is_r500 = r300_screen(screen)->caps.is_r500;
    bool is_r400 = r300_screen(screen)->caps.is_r400;
    bool is_color2101010 = format == PIPE_FORMAT_R10G10B10A2_UNORM ||
                           format == PIPE_FORMAT_R10G10B10X2_SNORM ||
                           format == PIPE_FORMAT_B10G10R10A2_UNORM ||
                           format == PIPE_FORMAT_B10G10R10X2_UNORM ||
                           format == PIPE_FORMAT_R10SG10SB10SA2U_NORM;
    bool is_ati1n = format == PIPE_FORMAT_RGTC1_UNORM ||
                    format == PIPE_FORMAT_RGTC1_SNORM ||
                    format == PIPE_FORMAT_LATC1_UNORM ||
                    format == PIPE_FORMAT_LATC1_SNORM;
    bool is_ati2n = format == PIPE_FORMAT_RGTC2_UNORM ||
                    format == PIPE_FORMAT_RGTC2_SNORM ||
                    format == PIPE_FORMAT_LATC2_UNORM ||
                    format == PIPE_FORMAT_LATC2_SNORM;
    bool is_half_float = format == PIPE_FORMAT_R16_FLOAT ||
                         format == PIPE_FORMAT_R16G16_FLOAT ||
                         format == PIPE_FORMAT_R16G16B16_FLOAT ||
                         format == PIPE_FORMAT_R16G16B16A16_FLOAT ||
                         format == PIPE_FORMAT_R16G16B16X16_FLOAT;
    const struct util_format_description *desc;

    if (MAX2(1, sample_count) != MAX2(1, storage_sample_count))
        return false;

    /* Check multisampling support. */
    switch (sample_count) {
        case 0:
        case 1:
            break;
        case 2:
        case 4:
        case 6:
            /* No texturing and scanout. */
            if (usage & (PIPE_BIND_SAMPLER_VIEW |
                         PIPE_BIND_DISPLAY_TARGET |
                         PIPE_BIND_SCANOUT)) {
                return false;
            }

            desc = util_format_description(format);

            if (is_r500) {
                /* Only allow depth/stencil, RGBA8, RGBA1010102, RGBA16F. */
                if (!util_format_is_depth_or_stencil(format) &&
                    !util_format_is_rgba8_variant(desc) &&
                    !util_format_is_rgba1010102_variant(desc) &&
                    format != PIPE_FORMAT_R16G16B16A16_FLOAT &&
                    format != PIPE_FORMAT_R16G16B16X16_FLOAT) {
                    return false;
                }
            } else {
                /* Only allow depth/stencil, RGBA8. */
                if (!util_format_is_depth_or_stencil(format) &&
                    !util_format_is_rgba8_variant(desc)) {
                    return false;
                }
            }
            break;
        default:
            return false;
    }

    /* Check sampler format support. */
    if ((usage & PIPE_BIND_SAMPLER_VIEW) &&
        /* these two are broken for an unknown reason */
        format != PIPE_FORMAT_R8G8B8X8_SNORM &&
        format != PIPE_FORMAT_R16G16B16X16_SNORM &&
        /* ATI1N is r5xx-only. */
        (is_r500 || !is_ati1n) &&
        /* ATI2N is supported on r4xx-r5xx. However state tracker can't handle
	 * fallbacks for ATI1N only, so if we enable ATI2N, we will crash for ATI1N.
	 * Therefore disable both on r400 for now. Additionally, some online source
	 * claim r300 can also do ATI2N.
	 *
	 * R300_EXPERIMENTAL_ATI2N=1 advertises ATI2N (RGTC2) sampler support on
	 * R300-class parts so the silicon claim above can be tested:
	 * r300_translate_texformat already emits R400_TX_FORMAT_ATI2N
	 * unconditionally, so only this exact-value gate stands between an RGTC2
	 * texture and the R300-class TMU. ATI1N stays r5xx-only to avoid the
	 * ATI1N-fallback crash the comment describes. Unset (the default), empty,
	 * and 0 keep R300-class ATI2N disabled, so default behavior is unchanged.
	 */
        (is_r500 ||
         (!is_r400 && is_ati2n && r300_screen(screen)->experimental_ati2n) ||
         !is_ati2n) &&
        r300_is_sampler_format_supported(format)) {
        retval |= PIPE_BIND_SAMPLER_VIEW;
    }

    /* Check colorbuffer format support. */
    if ((usage & (PIPE_BIND_RENDER_TARGET |
                  PIPE_BIND_DISPLAY_TARGET |
                  PIPE_BIND_SCANOUT |
                  PIPE_BIND_SHARED |
                  PIPE_BIND_BLENDABLE)) &&
        /* 2101010 cannot be rendered to on non-r5xx. */
        (!is_color2101010 || is_r500) &&
        r300_is_colorbuffer_format_supported(format)) {
        /* ARGB byte-order surface memory never matches the CRTC ARGB8888
         * scanout layout: big-endian stores them through the render
         * backend's DWORD_SWAP, which the display controller does not
         * apply, and on little-endian the byte order itself differs from
         * the scanout convention.  Expose only RENDER_TARGET and SHARED. */
        bool scanout_byte_order_mismatch;
        switch (format) {
        case PIPE_FORMAT_A8R8G8B8_UNORM:
        case PIPE_FORMAT_A8R8G8B8_SRGB:
        case PIPE_FORMAT_X8R8G8B8_UNORM:
        case PIPE_FORMAT_X8R8G8B8_SRGB:
            scanout_byte_order_mismatch = true;
            break;
        default:
            scanout_byte_order_mismatch = false;
            break;
        }
        unsigned scanout_mask = scanout_byte_order_mismatch
            ? (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED)
            : (PIPE_BIND_RENDER_TARGET |
               PIPE_BIND_DISPLAY_TARGET |
               PIPE_BIND_SCANOUT |
               PIPE_BIND_SHARED);
        retval |= usage & scanout_mask;

        if (r300_is_blending_supported(r300_screen(screen), format)) {
            retval |= usage & PIPE_BIND_BLENDABLE;
        }
    }

    /* Check depth-stencil format support. */
    if (usage & PIPE_BIND_DEPTH_STENCIL &&
        r300_is_zs_format_supported(format)) {
        retval |= PIPE_BIND_DEPTH_STENCIL;
    }

    /* Check vertex buffer format support. */
    if (usage & PIPE_BIND_VERTEX_BUFFER) {
        if (r300_screen(screen)->caps.has_tcl) {
            /* Half float is supported on >= R400. */
            if ((is_r400 || is_r500 || !is_half_float) &&
                r300_translate_vertex_data_type(format) != R300_INVALID_FORMAT) {
                retval |= PIPE_BIND_VERTEX_BUFFER;
            }
        } else {
            /* SW TCL */
            if (!util_format_is_pure_integer(format)) {
                retval |= PIPE_BIND_VERTEX_BUFFER;
            }
        }
    }

    if (usage & PIPE_BIND_INDEX_BUFFER) {
       if (format == PIPE_FORMAT_R8_UINT ||
           format == PIPE_FORMAT_R16_UINT ||
           format == PIPE_FORMAT_R32_UINT)
          retval |= PIPE_BIND_INDEX_BUFFER;
    }

    return retval == usage;
}

static void r300_init_shader_caps(struct r300_screen* r300screen)
{
   bool is_r400 = r300screen->caps.is_r400;
   bool is_r500 = r300screen->caps.is_r500;

   struct pipe_shader_caps *caps =
      (struct pipe_shader_caps *)&r300screen->screen.shader_caps[MESA_SHADER_VERTEX];

   if (r300screen->caps.has_tcl) {
      caps->max_instructions =
      caps->max_alu_instructions = is_r500 ? 1024 : 256;
      /* For loops; not sure about conditionals. */
      caps->max_control_flow_depth = is_r500 ? 4 : 0;
      caps->max_inputs = 16;
      caps->max_outputs = 10;
      caps->max_const_buffer0_size = 256 * sizeof(float[4]);
      caps->max_const_buffers = 1;
      caps->max_temps = 32;
      caps->indirect_const_addr = true;
      caps->tgsi_any_inout_decl_range = true;
   } else {
      draw_init_shader_caps(caps);

      caps->max_texture_samplers = 0;
      caps->max_sampler_views = 0;
      caps->subroutines = false;
      caps->max_shader_buffers = 0;
      caps->max_shader_images = 0;
      /* SW-TCL runs the vertex shader on Draw's direct NIR CPU executor, which
       * executes native integer ops and relative constant
       * addressing.  Expose both for the vertex stage so a shader that indexes a
       * UBO by a runtime value (e.g. ubuf.arr[gl_VertexIndex]) compiles instead
       * of tripping nir_lower_int_to_float on the integer address math.  The
       * fragment stage keeps integers off -- the PFS is float-only. */
      caps->integers = true;
      caps->indirect_const_addr = true;
      /* The r300 SW-TCL lowering and direct executor do not carry a validated
       * 16-bit contract. */
      caps->int16 = false;
      caps->fp16 = false;
      caps->fp16_derivatives = false;
      caps->fp16_const_buffers = false;
      caps->glsl_16bit_load_dst = false;
      /* While draw could normally handle this for the VS, the NIR lowering
       * to regs can't handle our non-native-integers, so we have to lower to
       * if ladders.
       */
      caps->indirect_temp_addr = false;
   }
   caps->supported_irs = 1 << PIPE_SHADER_IR_NIR;

   caps = (struct pipe_shader_caps *)&r300screen->screen.shader_caps[MESA_SHADER_FRAGMENT];

   caps->max_instructions = is_r500 || is_r400 ? 512 : 96;
   caps->max_alu_instructions = is_r500 || is_r400 ? 512 : 64;
   caps->max_tex_instructions = is_r500 || is_r400 ? 512 : 32;
   caps->max_tex_indirections = is_r500 ? 511 : 4;
   caps->max_control_flow_depth = is_r500 ? 64 : 0; /* Actually unlimited on r500. */
   /* 2 colors + 8 texcoords are always supported
    * (minus fog and wpos).
    *
    * R500 has the ability to turn 3rd and 4th color into
    * additional texcoords but there is no two-sided color
    * selection then. However the facing bit can be used instead. */
   caps->max_inputs = 10;
   caps->max_outputs = 4;
   caps->max_const_buffer0_size = (is_r500 ? 256 : 32) * sizeof(float[4]);
   caps->max_const_buffers = 1;
   caps->tgsi_any_inout_decl_range = true;
   caps->max_temps = is_r500 ? 128 : is_r400 ? 64 : 32;
   caps->max_texture_samplers =
   caps->max_sampler_views = r300screen->caps.num_tex_units;
   caps->supported_irs = 1 << PIPE_SHADER_IR_NIR;
}

static void r300_init_screen_caps(struct r300_screen* r300screen)
{
   struct pipe_caps *caps = (struct pipe_caps *)&r300screen->screen.caps;

   u_init_pipe_screen_caps(&r300screen->screen, 1);

   bool is_r500 = r300screen->caps.is_r500;

   /* Supported features (boolean caps). */
   /* Run finalize_nir (and nir_find_inlinable_uniforms) in the linker so the
    * inlinable-uniform flags reach gl_program->info before the state tracker
    * pushes their values through set_inlinable_constants. */
   caps->call_finalize_nir_in_linker = true;
   caps->npot_textures = true;
   caps->mixed_framebuffer_sizes = true;
   caps->mixed_color_depth_bits = true;
   caps->anisotropic_filter = true;
   caps->occlusion_query = true;
   caps->texture_mirror_clamp = true;
   caps->texture_mirror_clamp_to_edge = true;
   caps->blend_equation_separate = true;
   caps->vertex_element_instance_divisor = true;
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->conditional_render = true;
   caps->texture_barrier = true;
   caps->tgsi_can_compact_constants = true;
   caps->clip_halfz = true;
   caps->allow_mapped_buffers_during_execution = true;
   caps->legacy_math_rules = true;
   caps->query_memory_info = true;

   caps->texture_transfer_modes = PIPE_TEXTURE_TRANSFER_BLIT;

   caps->min_map_buffer_alignment = R300_BUFFER_ALIGNMENT;

   caps->constant_buffer_offset_alignment = 16;

   caps->glsl_feature_level =
   caps->glsl_feature_level_compatibility = 120;

   /* r300 cannot do swizzling of compressed textures. Supported otherwise. */
   caps->texture_swizzle = r300screen->caps.dxtc_swizzle;

   /* We don't support color clamping on r500, so that we can use color
    * interpolators for generic varyings. */
   caps->vertex_color_clamped = !is_r500;

   /* Supported on r500 only. */
   caps->vertex_color_unclamped =
   caps->mixed_colorbuffer_formats =
   caps->fragment_shader_texture_lod =
   caps->fragment_shader_derivatives = is_r500;

   caps->shareable_shaders = false;

   caps->max_gs_invocations = 32;
   /* Shader-buffer ceiling: the advertised single-buffer size tracks the
    * kernel-reported GART.  The 128 MB default matches the radeon gartsize
    * module default; a kernel provisioned with a 1 GB GART raises the
    * ceiling to 512 MB so a GL caller can allocate inside the extra
    * aperture.  The GART size itself stays kernel territory. */
   if (r300screen->info.gart_size_kb >= 1024u * 1024u)
      caps->max_shader_buffer_size = 1 << 29; /* 512 MB */
   else
      caps->max_shader_buffer_size = 1 << 27; /* 128 MB */

   /* SWTCL-only features. */
   caps->primitive_restart =
   caps->primitive_restart_fixed_index =
   caps->user_vertex_buffers =
   caps->vs_instanceid =
   caps->vs_window_space_position = !r300screen->caps.has_tcl;

   /* HWTCL-only features / limitations. */
   caps->vertex_input_alignment =
      r300screen->caps.has_tcl ? PIPE_VERTEX_INPUT_ALIGNMENT_4BYTE : PIPE_VERTEX_INPUT_ALIGNMENT_NONE;

   /* Texturing. */
   caps->max_texture_2d_size = is_r500 ? 4096 : 2048;
   caps->max_texture_3d_levels =
   caps->max_texture_cube_levels = is_r500 ? 13 : 12; /* 13 == 4096, 12 == 2048 */

   /* Render targets. */
   caps->max_render_targets = 4;
   caps->endianness = PIPE_ENDIAN_LITTLE;

   caps->max_viewports = 1;

   caps->max_vertex_attrib_stride = 2048;

   caps->max_varyings = 10;

   caps->prefer_imm_arrays_as_constbuf = false;

   caps->vendor_id = 0x1002;
   caps->device_id = r300screen->info.pci_id;
   caps->video_memory = r300screen->info.vram_size_kb >> 10;
   caps->uma = false;
   caps->pci_group = r300screen->info.pci.domain;
   caps->pci_bus = r300screen->info.pci.bus;
   caps->pci_device = r300screen->info.pci.dev;
   caps->pci_function = r300screen->info.pci.func;

   caps->min_line_width =
   caps->min_line_width_aa =
   caps->min_point_size =
   caps->min_point_size_aa = 1;
   caps->point_size_granularity =
   caps->line_width_granularity = 0.1;
   /* Point and line SIZE are a rasterization-coverage property, not the
    * colorbuffer dimension.  The old rationale -- "the colorbuffer dimensions
    * are our practical rendering limits" -- conflated two distinct quantities:
    * the render-target dimension (the colorbuffer size, 2560 native on r3xx)
    * and the largest point/line the rasterizer covers like the reference.  The
    * r3v Vulkan driver proves the split on the same silicon: it advertises
    * VkPhysicalDeviceLimits::maxImageDimension2D = 4096 (composed from a 2560
    * hardware span plus tiled residual blits) for the render target, but
    * pointSizeRange = [1, 64] and lineWidthRange = [1, 8] for the rasterizer
    * (VkPhysicalDeviceLimits::pointSizeRange / lineWidthRange).  Advertising the
    * colorbuffer dimension as the point/line max handed back sizes the GA
    * quad-expansion cannot cover conformantly (dEQP rasterization.limits.points
    * renders the advertised max-size point and the coverage misses), so mirror
    * the r3v conformant point limit.  Lines stay at the coverage-strict 4 the
    * GA end-cap quad matches -- a width-5 line already misses interpolated cap
    * pixels -- below the r3v HW-line 8, because the gallium SWTCL path
    * expands lines through the GA into quads. */
   caps->max_point_size =
   caps->max_point_size_aa = 64.0f;
   caps->max_line_width =
   caps->max_line_width_aa = 4.0f;
   caps->max_texture_anisotropy = 16.0f;
   caps->max_texture_lod_bias = 16.0f;
}

static void r300_destroy_screen(struct pipe_screen* pscreen)
{
    struct r300_screen* r300screen = r300_screen(pscreen);
    struct radeon_winsys *rws = radeon_winsys(pscreen);

    if (rws && !rws->unref(rws))
      return;

    mtx_destroy(&r300screen->cmask_mutex);
    slab_destroy_parent(&r300screen->pool_transfers);

    disk_cache_destroy(r300screen->disk_shader_cache);

    if (rws)
      rws->destroy(rws);

    /* Balance the ref taken in r300_screen_create. */
    glsl_type_singleton_decref();

    FREE(r300screen);
}

static void r300_fence_reference(struct pipe_screen *screen,
                                 struct pipe_fence_handle **ptr,
                                 struct pipe_fence_handle *fence)
{
    struct radeon_winsys *rws = r300_screen(screen)->rws;

    rws->fence_reference(rws, ptr, fence);
}

static bool r300_fence_finish(struct pipe_screen *screen,
                              struct pipe_context *ctx,
                              struct pipe_fence_handle *fence,
                              uint64_t timeout)
{
    struct radeon_winsys *rws = r300_screen(screen)->rws;

    return rws->fence_wait(rws, fence, timeout);
}

static int r300_screen_get_fd(struct pipe_screen *screen)
{
    struct radeon_winsys *rws = r300_screen(screen)->rws;

    return rws->get_fd(rws);
}

static void r300_query_memory_info(struct pipe_screen *pscreen, 
                                   struct pipe_memory_info *info)
{
   struct r300_screen *rscreen = (struct r300_screen*) pscreen;
   struct radeon_winsys *ws = rscreen->rws;

   info->total_device_memory = rscreen->info.vram_size_kb;
   info->total_staging_memory = rscreen->info.gart_size_kb;

   /* The real TTM memory usage is somewhat random, because:
    *
    * 1) TTM delays freeing memory, because it can only free it after
    *    fences expire.
    *
    * 2) The memory usage can be really low if big VRAM evictions are
    *    taking place, but the real usage is well above the size of VRAM.
    *
    * Instead, return statistics of this process.
    */
   unsigned vram_used = ws->query_value(ws, RADEON_VRAM_USAGE) / 1024;
   unsigned gtt_used = ws->query_value(ws, RADEON_GTT_USAGE) / 1024;

   info->avail_device_memory = (vram_used > info->total_device_memory) ? 0 : info->total_device_memory - vram_used;
   info->avail_staging_memory = (gtt_used > info->total_staging_memory) ? 0 : info->total_staging_memory - gtt_used;
   info->device_memory_evicted = ws->query_value(ws, RADEON_NUM_BYTES_MOVED) / 1024;
   info->nr_device_memory_evictions = ws->query_value(ws, RADEON_NUM_EVICTIONS);
}

/* The pipe_screen finalize_nir hook.  Symbol discovery uses
 * (rg --fixed-strings r300_finalize_nir src/gallium/drivers/r300/),
 * (rg --fixed-strings 'prog->info = prog->nir->info'
 * src/mesa/state_tracker/), and (rg --fixed-strings num_inlinable_uniforms
 * src/mesa/state_tracker/st_atom_constbuf.c).  The state tracker copies
 * nir->info into gl_program->info after the hook, and st_atom_constbuf reads
 * num_inlinable_uniforms from gl_program->info to push the values through
 * set_inlinable_constants.  Flag default-block uniforms used as a loop bound
 * or branch condition before the metadata copy so a uniform-bounded loop can
 * be specialized to a constant and statically unrolled per draw (R300/R400
 * fragment hardware has no dynamic control flow). */
static void
r300_finalize_nir(UNUSED struct pipe_screen *pscreen, struct nir_shader *nir,
                  UNUSED bool optimize)
{
   if (nir->info.stage != MESA_SHADER_FRAGMENT)
      return;

   /* nir_find_inlinable_uniforms only sees a uniform once the surrounding math
    * is folded to a constant offset, so fold before marking. */
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_algebraic);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   nir_find_inlinable_uniforms(nir);
}

struct pipe_screen* r300_screen_create(struct radeon_winsys *rws,
                                       const struct pipe_screen_config *config)
{
    struct r300_screen *r300screen = CALLOC_STRUCT(r300_screen);

    if (!r300screen) {
        FREE(r300screen);
        return NULL;
    }

    /* The GL state tracker refs the GLSL type singleton, but the video path
     * creates this screen through pipe_create_multimedia_context without a
     * state tracker.  NIR helper construction still allocates GLSL types, so
     * every r300 screen owns a reference.  r300_destroy_screen drops it. */
    glsl_type_singleton_init_or_ref();

    rws->query_info(rws, &r300screen->info);

    r300_init_debug(r300screen);
    r300_parse_chipset(r300screen->info.pci_id, &r300screen->caps);

    /* The standing route supplies RS485M-only defaults without changing the
     * process environment.  Each explicit option is parsed before the next
     * environment lookup, so zero, empty, and malformed values stay closed. */
    r300_r2vb_runtime_config_init_from_process(
        &r300screen->r2vb, r300screen->caps.family == CHIP_RS480);
    if (r300_screen_r2vb_config(r300screen)->standing_defaults_enabled) {
        fprintf(stderr,
                "r300: R2VB standing defaults captured (RS485M-family measured "
                "domain)\n");
    }

#if UTIL_ARCH_BIG_ENDIAN
    /* All known big-endian r300 systems should have hardware TCL. */
    assert(r300screen->caps.has_tcl);
#endif

    if (SCREEN_DBG_ON(r300screen, DBG_INFO) &&
        r300screen->caps.family == CHIP_RS480) {
        if (r300screen->info.rs480_gart_mc.valid) {
            SCREEN_DBG(r300screen, DBG_INFO,
                       "r300: RS485M GART/MC (%s): AGP_BASE_2=0x%08x GART_FEATURE_ID=0x%08x GART_BASE=0x%08x\n",
                       r300screen->info.rs480_gart_mc.from_debugfs ? "debugfs" : "ioctl",
                       r300screen->info.rs480_gart_mc.agp_base_2,
                       r300screen->info.rs480_gart_mc.gart_feature_id,
                       r300screen->info.rs480_gart_mc.gart_base);
        } else {
            SCREEN_DBG(r300screen, DBG_INFO,
                       "r300: RS485M GART/MC unavailable via ioctl or debugfs fallback.\n");
        }
    }

    /* driconf is optional for callers that do not use DRI2 (e.g. Vulkan ICDs).
     * When config->options is NULL the CALLOC'd r300screen->options fields are
     * already false, matching every OPT_BOOL default in r300_debug_options.h. */
    if (config && config->options) {
        driParseConfigFiles(config->options, config->options_info,
                            &(driConfigFileParseParams) { .driverName = "r300" });

#define OPT_BOOL(name, dflt, description)                                                          \
        r300screen->options.name = driQueryOptionb(config->options, "r300_" #name);
#include "r300_debug_options.h"
    }

    if (SCREEN_DBG_ON(r300screen, DBG_NO_ZMASK) ||
        r300screen->options.nozmask)
        r300screen->caps.zmask_ram = 0;
    if (SCREEN_DBG_ON(r300screen, DBG_NO_HIZ) ||
        r300screen->options.nohiz)
        r300screen->caps.hiz_ram = 0;
    if (SCREEN_DBG_ON(r300screen, DBG_NO_TCL)) {
#if UTIL_ARCH_BIG_ENDIAN
        fprintf(stderr, "r300: RADEON_DEBUG=notcl is unsupported on big-endian, ignoring.\n");
#else
        r300screen->caps.has_tcl = false;
#endif
    }

    if (SCREEN_DBG_ON(r300screen, DBG_IEEEMATH))
        r300screen->options.ieeemath = true;
    if (SCREEN_DBG_ON(r300screen, DBG_FFMATH))
        r300screen->options.ffmath = true;

    /* Read the experimental ATI2N opt-in once here rather than on every
     * r300_is_format_supported query for an RGTC2/LATC2 format. */
    r300screen->experimental_ati2n = r300_experimental_ati2n_enabled();

    r300_hb_tcl_init(r300screen);
    r300_hb_r400_us_init(r300screen);

    r300screen->rws = rws;
    r300screen->screen.destroy = r300_destroy_screen;
    r300screen->screen.get_name = r300_get_name;
    r300screen->screen.get_vendor = r300_get_vendor;
    r300screen->screen.get_device_vendor = r300_get_device_vendor;
    r300screen->screen.get_disk_shader_cache = r300_get_disk_shader_cache;
    r300screen->screen.get_screen_fd = r300_screen_get_fd;
    r300screen->screen.is_format_supported = r300_is_format_supported;
    r300screen->screen.context_create = r300_create_context;
    r300screen->screen.finalize_nir = r300_finalize_nir;
    r300screen->screen.fence_reference = r300_fence_reference;
    r300screen->screen.fence_finish = r300_fence_finish;
    r300screen->screen.query_memory_info = r300_query_memory_info;
    r300screen->screen.get_video_param = r300_get_video_param;
    r300screen->screen.is_video_format_supported =
        vl_video_buffer_is_format_supported;

    r300_screen_init_nir_options(r300screen);

    r300_init_screen_resource_functions(r300screen);

    r300_init_shader_caps(r300screen);
    r300_init_screen_caps(r300screen);

    r300_disk_cache_create(r300screen);

    slab_create_parent(&r300screen->pool_transfers, sizeof(struct pipe_transfer), 64);

    (void) mtx_init(&r300screen->cmask_mutex, mtx_plain);

    return &r300screen->screen;
}
