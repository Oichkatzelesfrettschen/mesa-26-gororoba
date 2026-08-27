/*
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"

#include "frontend/sw_winsys.h"
#include "draw/draw_context.h"
#include "draw/draw_private.h"
#include "softpipe/sp_public.h"
#include "sw/null/null_sw_winsys.h"

#include "util/detect_os.h"
#include "util/format/u_format.h"
#include "util/os_file.h"
#include "util/os_misc.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_sampler.h"

#include "vl_mpeg12_decoder.h"

#if DETECT_OS_WINDOWS
#define TEST_PATH_SEPARATOR "\\"
#else
#define TEST_PATH_SEPARATOR "/"
#endif

static struct pipe_sampler_view *
(*saved_create_sampler_view)(struct pipe_context *context,
                             struct pipe_resource *texture,
                             const struct pipe_sampler_view *template);
static unsigned sampler_view_create_calls;
static unsigned sampler_view_fail_call;

static struct pipe_sampler_view *
create_sampler_view_with_failure(struct pipe_context *context,
                                 struct pipe_resource *texture,
                                 const struct pipe_sampler_view *template)
{
   ++sampler_view_create_calls;
   if (sampler_view_create_calls == sampler_view_fail_call)
      return NULL;
   return saved_create_sampler_view(context, texture, template);
}

static bool
pstipple_install_failure_preserves_pipeline(struct pipe_context *context)
{
   struct draw_context *draw = context->draw;
   if (!draw || !draw->pipeline.pstipple) {
      fputs("FAIL: the installed polygon-stipple stage is observable\n",
            stderr);
      return false;
   }

   struct draw_stage *installed_stage = draw->pipeline.pstipple;
   saved_create_sampler_view = context->create_sampler_view;
   context->create_sampler_view = create_sampler_view_with_failure;
   sampler_view_create_calls = 0;
   sampler_view_fail_call = 1;

   bool install_result = draw_install_pstipple_stage(draw, context);
   struct draw_stage *published_stage = draw->pipeline.pstipple;

   context->create_sampler_view = saved_create_sampler_view;
   saved_create_sampler_view = NULL;
   sampler_view_fail_call = 0;
   unsigned create_calls = sampler_view_create_calls;
   sampler_view_create_calls = 0;

   bool pass = !install_result && create_calls == 1 &&
               published_stage == installed_stage;
   if (published_stage != installed_stage)
      draw->pipeline.pstipple = installed_stage;
   if (!pass) {
      fprintf(stderr,
              "FAIL: failed polygon-stipple installation preserves the "
              "published stage: result=%d calls=%u before=%p after=%p\n",
              install_result, create_calls, (void *)installed_stage,
              (void *)published_stage);
   }
   return pass;
}

static bool
sampler_view_cache_recovers(struct pipe_context *context,
                            struct pipe_video_buffer *buffer,
                            bool component_views)
{
   saved_create_sampler_view = context->create_sampler_view;
   context->create_sampler_view = create_sampler_view_with_failure;
   sampler_view_create_calls = 0;
   sampler_view_fail_call = 2;

   struct pipe_sampler_view **views = component_views
      ? buffer->get_sampler_view_components(buffer)
      : buffer->get_sampler_view_planes(buffer);
   struct pipe_sampler_view **failed_attempt_views = views;
   unsigned failed_attempt_calls = sampler_view_create_calls;
   bool failed_attempt_pass = !views && failed_attempt_calls == 2;

   sampler_view_create_calls = 0;
   sampler_view_fail_call = 0;
   views = component_views ? buffer->get_sampler_view_components(buffer)
                           : buffer->get_sampler_view_planes(buffer);
   unsigned retry_calls = sampler_view_create_calls;
   bool retry_pass = views && retry_calls == 3 && views[0] && views[1] &&
                     views[2];

   context->create_sampler_view = saved_create_sampler_view;
   saved_create_sampler_view = NULL;
   sampler_view_create_calls = 0;
   sampler_view_fail_call = 0;
   if (!failed_attempt_pass || !retry_pass) {
      fprintf(stderr,
              "FAIL: %s sampler-view cache recovery: failed views=%p "
              "calls=%u, retry views=%p calls=%u\n",
              component_views ? "component" : "plane",
              (void *)failed_attempt_views,
              failed_attempt_calls, (void *)views, retry_calls);
   }
   return failed_attempt_pass && retry_pass;
}

static bool
make_path(char *path, size_t capacity, const char *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   int length = vsnprintf(path, capacity, format, arguments);
   va_end(arguments);
   return length >= 0 && (size_t)length < capacity;
}

static bool
create_test_root(char *root, size_t capacity)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();

   for (unsigned serial = 0; serial < 1024; ++serial) {
      if (!make_path(root, capacity,
                     "vl-mpeg12-mc-dump-stage-test-%08u", serial))
         return false;
      int result = io->mkdir(root, 0700);
      if (result == 0)
         return true;
      if (result != -EEXIST)
         return false;
   }
   return false;
}

static bool
remove_dump_session_and_root(struct vl_mpeg12_dump *dump, const char *root)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   bool pass = true;
   if (vl_mpeg12_dump_enabled(dump))
      pass = io->remove_directory(dump->session_path) == 0;
   return io->remove_directory(root) == 0 && pass;
}

static bool
remove_published_frame(struct vl_mpeg12_dump *dump,
                       const struct vl_mpeg12_dump_stage *stages,
                       unsigned stage_count,
                       uint64_t frame)
{
   const struct vl_mpeg12_dump_io *io = vl_mpeg12_dump_default_io();
   char path[2048];
   bool pass = true;

   for (unsigned stage_index = 0; stage_index < stage_count; ++stage_index) {
      const struct vl_mpeg12_dump_stage *stage = &stages[stage_index];
      for (unsigned plane = 0; plane < stage->plane_count; ++plane) {
         const struct pipe_sampler_view *view = stage->planes[plane];
         unsigned level = view->u.tex.first_level;
         unsigned width = u_minify(view->texture->width0, level);
         unsigned height = u_minify(view->texture->height0, level);
         unsigned first_layer = view->u.tex.first_layer;
         unsigned last_layer = view->u.tex.last_layer;
         unsigned layer_count = last_layer - first_layer + 1;
         const char *storage_format_name =
            util_format_short_name(view->texture->format);
         const char *view_format_name = util_format_short_name(view->format);

         pass = make_path(
                   path, sizeof(path),
                   "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                   TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR
                   "%s_storage%u_level%u_layers%u-%u_%ux%ux%u_storage-%s_"
                   "view-%s.raw",
                   dump->session_path, frame, stage->name, plane, level,
                   first_layer, last_layer, width, height, layer_count,
                   storage_format_name, view_format_name) &&
                io->remove_file(path) == 0 && pass;
      }
   }

   pass = make_path(path, sizeof(path),
                    "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                    TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR
                    "manifest.tsv",
                    dump->session_path, frame) &&
          io->remove_file(path) == 0 && pass;
   pass = make_path(path, sizeof(path),
                    "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                    TEST_PATH_SEPARATOR "complete",
                    dump->session_path, frame) &&
          io->remove_directory(path) == 0 && pass;
   pass = make_path(path, sizeof(path),
                    "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                    TEST_PATH_SEPARATOR "payload",
                    dump->session_path, frame) &&
          io->remove_directory(path) == 0 && pass;
   pass = make_path(path, sizeof(path),
                    "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64,
                    dump->session_path, frame) &&
          io->remove_directory(path) == 0 && pass;
   return pass;
}

static bool
manifest_has_mc_buffer_identity(const struct vl_mpeg12_dump *dump,
                                uint64_t frame)
{
   char path[2048];
   if (!make_path(path, sizeof(path),
                  "%s" TEST_PATH_SEPARATOR "frame-%020" PRIu64
                  TEST_PATH_SEPARATOR "payload" TEST_PATH_SEPARATOR
                  "manifest.tsv",
                  dump->session_path, frame))
      return false;

   size_t size = 0;
   char *manifest = os_read_file(path, &size);
   char identity[128];
   int identity_length = snprintf(
      identity, sizeof(identity), "stage1\t%s\t",
      util_format_short_name(PIPE_FORMAT_IYUV));
   bool found = manifest && size && identity_length > 0 &&
                (size_t)identity_length < sizeof(identity) &&
                strstr(manifest, identity);
   free(manifest);
   return found;
}

static struct pipe_resource *
bind_context_teardown_sampler_view(struct pipe_context *context,
                                   bool *pass)
{
   struct pipe_resource resource_template = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 8,
      .height0 = 8,
      .depth0 = 1,
      .array_size = 1,
      .usage = PIPE_USAGE_DEFAULT,
      .bind = PIPE_BIND_SAMPLER_VIEW,
   };
   struct pipe_resource *texture =
      context->screen->resource_create(context->screen, &resource_template);
   if (!texture) {
      fputs("FAIL: the teardown sampler texture is created\n", stderr);
      *pass = false;
      return NULL;
   }

   struct pipe_sampler_view view_template;
   u_sampler_view_default_template(
      &view_template, texture, resource_template.format);
   struct pipe_sampler_view *view =
      context->create_sampler_view(context, texture, &view_template);
   if (!view) {
      fputs("FAIL: the teardown sampler view is created\n", stderr);
      *pass = false;
      pipe_resource_reference(&texture, NULL);
      return NULL;
   }

   context->set_sampler_views(
      context, MESA_SHADER_FRAGMENT, 0, 1, 0, &view);
   pipe_sampler_view_reference(&view, NULL);
   int reference_count = p_atomic_read(&texture->reference.count);
   if (reference_count != 3) {
      fprintf(stderr,
              "FAIL: sampler binding owns one view and one tile-cache "
              "reference, got %d\n",
              reference_count);
      *pass = false;
   }
   return texture;
}

int
main(void)
{
   struct sw_winsys *winsys = null_sw_create();
   if (!winsys) {
      fputs("FAIL: the null software winsys is created\n", stderr);
      return 1;
   }

   struct pipe_screen *screen = softpipe_create_screen(winsys);
   if (!screen) {
      fputs("FAIL: the softpipe screen is created\n", stderr);
      winsys->destroy(winsys);
      return 1;
   }

   struct pipe_context *context = screen->context_create(screen, NULL, 0);
   if (!context) {
      fputs("FAIL: the softpipe context is created\n", stderr);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 1;
   }

   bool pass = pstipple_install_failure_preserves_pipeline(context);

   char root[128];
   bool root_created = create_test_root(root, sizeof(root));
   if (!root_created) {
      fputs("FAIL: the MC dump root is created\n", stderr);
      context->destroy(context);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 1;
   }
   os_set_option("VL_MPEG12_DUMP_DIR", root, true);

   struct pipe_video_codec template = {
      .profile = PIPE_VIDEO_PROFILE_MPEG2_MAIN,
      .entrypoint = PIPE_VIDEO_ENTRYPOINT_MC,
      .chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420,
      .width = 32,
      .height = 32,
      .max_references = 2,
   };
   struct pipe_video_codec *codec =
      vl_create_mpeg12_decoder(context, &template);
   os_unset_option("VL_MPEG12_DUMP_DIR");
   if (!codec) {
      fputs("FAIL: the MPEG-2 MC decoder is created\n", stderr);
      vl_mpeg12_dump_default_io()->remove_directory(root);
      context->destroy(context);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 1;
   }

   struct vl_mpeg12_decoder *decoder = (struct vl_mpeg12_decoder *)codec;
   if (!decoder->mc_source) {
      fputs("FAIL: the MPEG-2 MC source is created\n", stderr);
      remove_dump_session_and_root(&decoder->dump, root);
      codec->destroy(codec);
      context->destroy(context);
      screen->destroy(screen);
      winsys->destroy(winsys);
      return 1;
   }

   struct pipe_video_buffer cache_template = {
      .buffer_format = PIPE_FORMAT_IYUV,
      .width = template.width,
      .height = template.height,
   };
   struct pipe_video_buffer *cache_buffer =
      vl_video_buffer_create(context, &cache_template);
   bool cache_pass = cache_buffer &&
                     sampler_view_cache_recovers(context, cache_buffer,
                                                  false) &&
                     sampler_view_cache_recovers(context, cache_buffer, true);
   pass = cache_pass && pass;
   if (cache_buffer)
      cache_buffer->destroy(cache_buffer);

   struct vl_mpeg12_dump_stage stage = {0};
   int stage_result = vl_mpeg12_dump_stage_from_video_buffer(
      &stage, "stage1", decoder->mc_source);
   pass = pass && decoder->mc_source->buffer_format == PIPE_FORMAT_IYUV &&
          vl_mpeg12_dump_enabled(&decoder->dump) && stage_result == 0 &&
          stage.buffer_format == PIPE_FORMAT_IYUV && stage.plane_count == 3 &&
          stage.planes && stage.planes[0] && stage.planes[1] && stage.planes[2];

   struct pipe_video_buffer target_template = {
      .buffer_format = PIPE_FORMAT_IYUV,
      .width = template.width,
      .height = template.height,
   };
   struct pipe_video_buffer *target =
      vl_video_buffer_create(context, &target_template);
   pass = pass && target;

   short blocks[4][6 * 64] = {{0}};
   struct pipe_mpeg12_picture_desc picture = {
      .base = {
         .profile = template.profile,
         .entry_point = template.entrypoint,
      },
      .picture_coding_type = PIPE_MPEG12_PICTURE_CODING_TYPE_I,
      .picture_structure = PIPE_MPEG12_PICTURE_STRUCTURE_FRAME,
      .frame_pred_frame_dct = 1,
   };
   struct pipe_mpeg12_macroblock macroblocks[4] = {0};
   for (unsigned index = 0; index < ARRAY_SIZE(macroblocks); ++index) {
      macroblocks[index].base.codec = PIPE_VIDEO_FORMAT_MPEG12;
      macroblocks[index].x = index % 2;
      macroblocks[index].y = index / 2;
      macroblocks[index].macroblock_type = PIPE_MPEG12_MB_TYPE_INTRA;
      macroblocks[index].coded_block_pattern = 0x3f;
      macroblocks[index].blocks = blocks[index];
   }
   int end_result = -EIO;
   if (target) {
      codec->begin_frame(codec, target, &picture.base);
      codec->decode_macroblock(codec, target, &picture.base,
                               &macroblocks[0].base, ARRAY_SIZE(macroblocks));
      end_result = codec->end_frame(codec, target, &picture.base);
   }
   pass = pass && end_result == 0 && decoder->dump.frame == 1 &&
          manifest_has_mc_buffer_identity(&decoder->dump, 0);

   struct pipe_video_buffer invalid_source = *decoder->mc_source;
   invalid_source.buffer_format = PIPE_FORMAT_NONE;
   pass = pass && vl_mpeg12_dump_stage_from_video_buffer(
                     &stage, "stage1", &invalid_source) == -EINVAL;

   struct vl_mpeg12_dump_stage published_stages[2] = {stage};
   int output_stage_result = target
      ? vl_mpeg12_dump_stage_from_video_buffer(
           &published_stages[1], "out", target)
      : -EIO;
   pass = pass && output_stage_result == 0;
   if (end_result == 0 && output_stage_result == 0)
      pass = remove_published_frame(
                &decoder->dump, published_stages, 2, 0) && pass;
   pass = remove_dump_session_and_root(&decoder->dump, root) && pass;

   if (target)
      target->destroy(target);
   codec->destroy(codec);
   struct pipe_resource *teardown_texture =
      bind_context_teardown_sampler_view(context, &pass);
   context->destroy(context);
   if (teardown_texture) {
      int reference_count =
         p_atomic_read(&teardown_texture->reference.count);
      if (reference_count != 1) {
         fprintf(stderr,
                 "FAIL: context teardown restores the sampler texture "
                 "reference count, got %d\n",
                 reference_count);
         pass = false;
      }
      pipe_resource_reference(&teardown_texture, NULL);
   }
   screen->destroy(screen);
   winsys->destroy(winsys);

   puts(pass ? "vl-mpeg12 MC dump frame: PASS"
             : "vl-mpeg12 MC dump frame: FAIL");
   return pass ? 0 : 1;
}
