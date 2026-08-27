/*
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>

#include "pipe/p_context.h"
#include "pipe/p_state.h"

#include "util/u_atomic.h"
#include "util/u_inlines.h"

#include "softpipe/sp_tex_tile_cache.h"

int
main(void)
{
   struct pipe_context context = {0};
   struct pipe_resource first_texture = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_R8_UNORM,
      .width0 = 1,
      .height0 = 1,
      .depth0 = 1,
      .array_size = 1,
   };
   struct pipe_resource second_texture = first_texture;
   struct pipe_sampler_view first_view = {
      .texture = &first_texture,
      .format = first_texture.format,
      .swizzle_r = PIPE_SWIZZLE_X,
      .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z,
      .swizzle_a = PIPE_SWIZZLE_W,
   };
   struct pipe_sampler_view second_view = first_view;
   second_view.texture = &second_texture;
   pipe_reference_init(&first_texture.reference, 1);
   pipe_reference_init(&second_texture.reference, 1);

   struct softpipe_tex_tile_cache *cache =
      sp_create_tex_tile_cache(&context);
   if (!cache) {
      fputs("FAIL: the texture tile cache is created\n", stderr);
      return 1;
   }

   int first_initial_count = p_atomic_read(&first_texture.reference.count);
   int second_initial_count = p_atomic_read(&second_texture.reference.count);
   sp_tex_tile_cache_set_sampler_view(cache, &first_view);
   int first_bound_count = p_atomic_read(&first_texture.reference.count);
   if (first_bound_count != first_initial_count + 1) {
      fprintf(stderr,
              "FAIL: the tile cache retains the first texture, got %d "
              "from %d\n",
              first_bound_count, first_initial_count);
      sp_destroy_tex_tile_cache(cache);
      return 1;
   }

   sp_tex_tile_cache_set_sampler_view(cache, &second_view);
   int first_replaced_count = p_atomic_read(&first_texture.reference.count);
   int second_bound_count = p_atomic_read(&second_texture.reference.count);
   if (first_replaced_count != first_initial_count ||
       second_bound_count != second_initial_count + 1) {
      fprintf(stderr,
              "FAIL: replacement releases the first texture and retains the "
              "second, got %d and %d from %d and %d\n",
              first_replaced_count, second_bound_count, first_initial_count,
              second_initial_count);
      sp_destroy_tex_tile_cache(cache);
      return 1;
   }

   sp_tex_tile_cache_set_sampler_view(cache, NULL);
   int second_unbound_count = p_atomic_read(&second_texture.reference.count);
   if (second_unbound_count != second_initial_count) {
      fprintf(stderr,
              "FAIL: explicit unbind releases the second texture, got %d "
              "from %d\n",
              second_unbound_count, second_initial_count);
      sp_destroy_tex_tile_cache(cache);
      return 1;
   }

   sp_tex_tile_cache_set_sampler_view(cache, &first_view);
   sp_destroy_tex_tile_cache(cache);
   int first_destroyed_count = p_atomic_read(&first_texture.reference.count);
   int second_destroyed_count = p_atomic_read(&second_texture.reference.count);
   if (first_destroyed_count != first_initial_count ||
       second_destroyed_count != second_initial_count) {
      fprintf(stderr,
              "FAIL: destruction restores both texture reference counts, got "
              "%d and %d from %d and %d\n",
              first_destroyed_count, second_destroyed_count,
              first_initial_count, second_initial_count);
      return 1;
   }

   puts("softpipe texture tile-cache reference: PASS");
   return 0;
}
