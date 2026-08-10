/* SPDX-License-Identifier: MIT */

/* The uploader test drives the real suballocation and rotation state through
 * a malloc-backed pipe_screen and pipe_context pair. Injectable resource and
 * map failures exercise the transactional buffer-ownership contract, and
 * live-buffer accounting detects leaks and premature releases. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "cso_cache/cso_cache.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"
#include "util/u_vbuf.h"

static int g_fails;

#define CHECK(cond, name)                                                    \
   do {                                                                      \
      if (cond) {                                                            \
         printf("  ok   - %s\n", name);                                      \
      } else {                                                               \
         printf("  FAIL - %s\n", name);                                      \
         g_fails++;                                                          \
      }                                                                      \
   } while (0)

struct fake_buffer {
   struct pipe_resource b;
   uint8_t *data;
   unsigned release_count;
   bool destroyed;
   struct fake_buffer *preserved_next;
};

static bool g_fail_resource_create;
static bool g_fail_buffer_map;
static unsigned g_live_buffers;
static struct fake_buffer *g_preserved_buffers;
static bool g_double_release;

static struct pipe_resource *g_vbuf_input_resource;
static unsigned g_vbuf_input_map_calls;
static unsigned g_vbuf_resource_create_calls;
static unsigned g_vbuf_stream_map_calls;
static unsigned g_vbuf_draw_calls;
static bool g_vbuf_test_active;

static struct pipe_resource *
fake_resource_create(struct pipe_screen *screen,
                     const struct pipe_resource *templ)
{
   struct fake_buffer *fb;

   if (g_fail_resource_create)
      return NULL;

   fb = calloc(1, sizeof(*fb));
   if (!fb)
      return NULL;

   if (g_vbuf_test_active)
      g_vbuf_resource_create_calls++;

   fb->b = *templ;
   pipe_reference_init(&fb->b.reference, 1);
   fb->b.screen = screen;
   fb->data = calloc(1, templ->width0);
   if (!fb->data) {
      free(fb);
      return NULL;
   }

   g_live_buffers++;
   return &fb->b;
}

static void
fake_resource_destroy(struct pipe_screen *screen, struct pipe_resource *res)
{
   struct fake_buffer *fb = (struct fake_buffer *)res;

   (void)screen;
   fb->destroyed = true;
   free(fb->data);
   fb->data = NULL;
   g_live_buffers--;

   /* The release hook needs destroyed records to remain addressable so a
    * second release becomes an explicit test verdict. */
   fb->preserved_next = g_preserved_buffers;
   g_preserved_buffers = fb;
}

static void
fake_resource_release(struct pipe_context *ctx, struct pipe_resource *res)
{
   struct fake_buffer *fb;

   (void)ctx;
   if (!res)
      return;

   fb = (struct fake_buffer *)res;
   fb->release_count++;
   if (fb->destroyed) {
      g_double_release = true;
      return;
   }

   pipe_resource_reference(&res, NULL);
}

static void
fake_free_preserved_buffers(void)
{
   while (g_preserved_buffers) {
      struct fake_buffer *fb = g_preserved_buffers;
      g_preserved_buffers = fb->preserved_next;
      free(fb);
   }
}

static struct pipe_transfer g_fake_transfer;

static void *
fake_buffer_map(struct pipe_context *ctx, struct pipe_resource *res,
               unsigned level, unsigned usage, const struct pipe_box *box,
               struct pipe_transfer **transfer)
{
   (void)ctx;
   (void)level;
   (void)usage;

   if (g_vbuf_test_active) {
      if (res == g_vbuf_input_resource) {
         g_vbuf_input_map_calls++;
         if (g_vbuf_input_map_calls > 1)
            return NULL;
      } else {
         g_vbuf_stream_map_calls++;
      }
   }

   if (g_fail_buffer_map)
      return NULL;

   g_fake_transfer.resource = res;
   g_fake_transfer.box = *box;
   *transfer = &g_fake_transfer;
   return ((struct fake_buffer *)res)->data + box->x;
}

static void
fake_buffer_unmap(struct pipe_context *ctx, struct pipe_transfer *transfer)
{
   (void)ctx;
   (void)transfer;
}

static void
fake_transfer_flush_region(struct pipe_context *ctx,
                           struct pipe_transfer *transfer,
                           const struct pipe_box *box)
{
   (void)ctx;
   (void)transfer;
   (void)box;
}

static struct pipe_screen g_screen;
static struct pipe_context g_pipe;

static void *
fake_create_vertex_elements_state(struct pipe_context *ctx,
                                  unsigned num_elements,
                                  const struct pipe_vertex_element *elements)
{
   (void)ctx;
   (void)num_elements;
   (void)elements;
   return malloc(1);
}

static void
fake_bind_vertex_elements_state(struct pipe_context *ctx, void *state)
{
   (void)ctx;
   (void)state;
}

static void
fake_delete_vertex_elements_state(struct pipe_context *ctx, void *state)
{
   (void)ctx;
   free(state);
}

static void
fake_set_vertex_buffers(struct pipe_context *ctx, unsigned count,
                        const struct pipe_vertex_buffer *buffers)
{
   (void)ctx;
   (void)count;
   (void)buffers;
}

static void
fake_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info,
              unsigned drawid_offset,
              const struct pipe_draw_indirect_info *indirect,
              const struct pipe_draw_start_count_bias *draws,
              unsigned num_draws)
{
   (void)ctx;
   (void)info;
   (void)drawid_offset;
   (void)indirect;
   (void)draws;
   g_vbuf_draw_calls += num_draws;
}

static bool
upload_bytes(struct u_upload_mgr *mgr, unsigned size, uint8_t fill,
             struct pipe_resource *expected_buffer)
{
   unsigned offset = ~0u;
   struct pipe_resource *buf = NULL;
   struct pipe_resource *releasebuf = NULL;
   uint8_t *map = NULL;

   u_upload_alloc(mgr, 0, size, 4, &offset, &buf, &releasebuf,
                  (void **)&map);
   if (releasebuf)
      pipe_resource_release(&g_pipe, releasebuf);

   if (!map || !buf || (expected_buffer && buf != expected_buffer))
      return false;

   memset(map, fill, size);
   return offset + size <= buf->width0;
}

static void
check_persistent_upload_failures(void)
{
   struct pipe_screen screen = {
      .caps = {
         .buffer_map_persistent_coherent = true,
      },
   };
   struct pipe_context pipe = g_pipe;
   struct u_upload_mgr *mgr = NULL;
   struct pipe_resource *buffer = NULL;
   struct pipe_resource *releasebuf = NULL;
   unsigned offset = ~0u;
   void *map = NULL;

   g_fail_resource_create = false;
   g_fail_buffer_map = false;
   g_double_release = false;
   screen.resource_create = fake_resource_create;
   screen.resource_destroy = fake_resource_destroy;
   pipe.screen = &screen;
   pipe.resource_release = fake_resource_release;
   mgr = u_upload_create(&pipe, 4096, PIPE_BIND_VERTEX_BUFFER,
                         PIPE_USAGE_STREAM, 0);
   CHECK(mgr != NULL, "persistent uploader creates against the fake stack");
   if (!mgr)
      return;

   u_upload_alloc(mgr, 0, 64, 4, &offset, &buffer, &releasebuf, &map);
   if (releasebuf)
      fake_resource_release(&pipe, releasebuf);
   CHECK(buffer && map && offset == 0,
         "persistent uploader maps its initial buffer");
   CHECK(g_live_buffers == 1,
         "persistent uploader owns one initial buffer");

   g_fail_resource_create = true;
   offset = 17;
   buffer = (struct pipe_resource *)(void *)&screen;
   releasebuf = (struct pipe_resource *)(void *)&pipe;
   map = (void *)&screen;
   u_upload_alloc(mgr, 0, 8192, 4, &offset, &buffer, &releasebuf, &map);
   g_fail_resource_create = false;
   CHECK(!buffer && !releasebuf && offset == ~0u && !map,
         "persistent resource-create failure clears all outputs");
   CHECK(g_live_buffers == 1,
         "persistent resource-create failure retains the old buffer");

   u_upload_alloc(mgr, 0, 64, 4, &offset, &buffer, &releasebuf, &map);
   CHECK(buffer && map && g_live_buffers == 1,
         "persistent resource-create failure retries on the old mapping");

   g_fail_buffer_map = true;
   offset = 29;
   buffer = (struct pipe_resource *)(void *)&screen;
   releasebuf = (struct pipe_resource *)(void *)&pipe;
   map = (void *)&screen;
   u_upload_alloc(mgr, 0, 8192, 4, &offset, &buffer, &releasebuf, &map);
   g_fail_buffer_map = false;
   CHECK(!buffer && !releasebuf && offset == ~0u && !map,
         "persistent replacement-map failure clears all outputs");
   CHECK(g_live_buffers == 1,
         "persistent replacement-map failure releases only the replacement");

   u_upload_alloc(mgr, 0, 64, 4, &offset, &buffer, &releasebuf, &map);
   CHECK(buffer && map && g_live_buffers == 1,
         "persistent replacement-map failure retries on the old mapping");

   u_upload_destroy(mgr);
   CHECK(g_live_buffers == 0 && !g_double_release,
         "persistent uploader destruction releases its retained buffer");
}

static void
check_wrapper_reference_contract(void)
{
   struct u_upload_mgr *mgr;
   struct pipe_resource *outbuf = NULL;
   struct fake_buffer *first;
   unsigned offset = ~0u;
   void *ptr = NULL;

   g_fail_resource_create = false;
   g_fail_buffer_map = false;
   g_double_release = false;

   mgr = u_upload_create(&g_pipe, 4096, PIPE_BIND_VERTEX_BUFFER,
                         PIPE_USAGE_STREAM, 0);
   CHECK(mgr != NULL, "reference wrapper creates an uploader");
   if (!mgr)
      goto cleanup;

   u_upload_alloc_ref(mgr, 0, 64, 4, &offset, &outbuf, &ptr);
   CHECK(outbuf && ptr && g_live_buffers == 1,
         "reference wrapper returns an owned current buffer");
   if (!outbuf) {
      u_upload_destroy(mgr);
      goto cleanup;
   }

   first = (struct fake_buffer *)outbuf;
   u_upload_unmap(mgr);
   offset = ~0u;
   ptr = NULL;
   u_upload_alloc_ref(mgr, 0, 8192, 4, &offset, &outbuf, &ptr);
   CHECK(outbuf && ptr && first->destroyed && first->release_count == 1,
         "reference wrapper releases a rotated handoff once");

   u_upload_destroy(mgr);
   fake_resource_release(&g_pipe, outbuf);
   outbuf = NULL;
   CHECK(g_live_buffers == 0 && !g_double_release,
         "reference wrapper destruction leaves no live buffers");

cleanup:
   if (outbuf)
      fake_resource_release(&g_pipe, outbuf);
}

static void
check_u_vbuf_failure_ownership(void)
{
   struct u_upload_mgr *old_stream_uploader = g_pipe.stream_uploader;
   struct u_vbuf *old_vbuf = g_pipe.vbuf;
   struct u_upload_mgr *stream_uploader = NULL;
   struct pipe_resource *input = NULL;
   struct u_vbuf *vbuf = NULL;
   struct pipe_resource *base_buffer = NULL;
   struct pipe_resource *base_releasebuf = NULL;
   struct pipe_resource input_template;
   struct pipe_vertex_buffer vertex_buffer;
   struct cso_velems_state velems;
   struct pipe_draw_info info;
   struct pipe_draw_start_count_bias draws[2];
   struct u_vbuf_caps caps;
   unsigned base_offset = ~0u;
   uint8_t *base_map = NULL;
   struct fake_buffer *base;

   g_fail_resource_create = false;
   g_fail_buffer_map = false;
   g_double_release = false;
   g_vbuf_input_resource = NULL;
   g_vbuf_input_map_calls = 0;
   g_vbuf_resource_create_calls = 0;
   g_vbuf_stream_map_calls = 0;
   g_vbuf_draw_calls = 0;

   stream_uploader = u_upload_create(&g_pipe, 4096,
                                     PIPE_BIND_VERTEX_BUFFER,
                                     PIPE_USAGE_STREAM, 0);
   CHECK(stream_uploader != NULL,
         "u_vbuf fixture creates a stream uploader");
   if (!stream_uploader)
      goto cleanup;

   u_upload_alloc(stream_uploader, 0, 64, 4, &base_offset, &base_buffer,
                  &base_releasebuf, (void **)&base_map);
   if (base_releasebuf)
      fake_resource_release(&g_pipe, base_releasebuf);
   CHECK(base_buffer && base_map && base_offset == 0,
         "u_vbuf fixture pre-fills the current stream buffer");
   if (!base_buffer || !base_map)
      goto cleanup;

   memset(&input_template, 0, sizeof(input_template));
   input_template.target = PIPE_BUFFER;
   input_template.format = PIPE_FORMAT_R8_UNORM;
   input_template.bind = PIPE_BIND_VERTEX_BUFFER;
   input_template.usage = PIPE_USAGE_DEFAULT;
   input_template.width0 = 4097;
   input_template.height0 = 1;
   input_template.depth0 = 1;
   input_template.array_size = 1;
   input = fake_resource_create(&g_screen, &input_template);
   CHECK(input != NULL, "u_vbuf fixture creates an input resource");
   if (!input)
      goto cleanup;

   memset(&caps, 0, sizeof(caps));
   for (unsigned i = 0; i < PIPE_FORMAT_COUNT; i++)
      caps.format_translation[i] = i;
   caps.attrib_element_unaligned = true;
   caps.user_vertex_buffers = true;
   caps.max_vertex_buffers = 16;
   caps.supported_restart_modes = BITFIELD_MASK(MESA_PRIM_COUNT);
   caps.supported_prim_modes = BITFIELD_MASK(MESA_PRIM_COUNT);

   vbuf = u_vbuf_create(&g_pipe, &caps);
   CHECK(vbuf != NULL, "u_vbuf fixture creates the vertex translator");
   if (!vbuf)
      goto cleanup;
   g_pipe.vbuf = vbuf;

   memset(&velems, 0, sizeof(velems));
   velems.count = 1;
   velems.velems[0].src_format = PIPE_FORMAT_R32_FLOAT;
   velems.velems[0].src_stride = 4;
   velems.velems[0].vertex_buffer_index = 0;
   u_vbuf_set_vertex_elements(vbuf, &velems);

   memset(&vertex_buffer, 0, sizeof(vertex_buffer));
   vertex_buffer.buffer.resource = input;
   vertex_buffer.buffer_offset = 1;
   u_vbuf_set_vertex_buffers(vbuf, 1, &vertex_buffer);

   memset(&info, 0, sizeof(info));
   info.mode = MESA_PRIM_TRIANGLES;
   info.instance_count = 1;
   draws[0].start = 0;
   draws[0].count = 1024;
   draws[0].index_bias = 0;
   draws[1] = draws[0];

   g_pipe.stream_uploader = stream_uploader;
   g_vbuf_input_resource = input;
   g_vbuf_test_active = true;
   u_vbuf_draw_vbo(&g_pipe, &info, 0, NULL, draws, ARRAY_SIZE(draws));
   g_vbuf_test_active = false;

   base = (struct fake_buffer *)base_buffer;
   CHECK(g_vbuf_input_map_calls == 2,
         "u_vbuf maps the input for both direct draws");
   CHECK(g_vbuf_resource_create_calls == 1,
         "first translated draw rotates before the second input map");
   CHECK(g_vbuf_stream_map_calls == 1,
         "second input failure happens before another output map");
   CHECK(g_vbuf_draw_calls == 1,
         "u_vbuf submits only the draw with a successful input map");
   CHECK(base->release_count == 1 && base->destroyed,
         "first translated draw releases its handoff exactly once");
   CHECK(!g_double_release,
         "failed second draw does not release a stale handoff slot");

   g_vbuf_input_resource = NULL;
   g_vbuf_input_map_calls = 0;
   g_vbuf_resource_create_calls = 0;
   g_vbuf_stream_map_calls = 0;
   g_vbuf_draw_calls = 0;
   g_double_release = false;
   g_vbuf_test_active = true;
   u_vbuf_draw_vbo(&g_pipe, &info, 0, NULL, draws, ARRAY_SIZE(draws));
   g_vbuf_test_active = false;

   CHECK(g_vbuf_draw_calls == 2,
         "u_vbuf submits both draws when every input map succeeds");
   CHECK(!g_double_release,
         "successful translated draws release every handoff exactly once");

cleanup:
   g_vbuf_test_active = false;
   g_vbuf_input_resource = NULL;
   g_pipe.stream_uploader = old_stream_uploader;
   if (vbuf)
      u_vbuf_destroy(vbuf);
   g_pipe.vbuf = old_vbuf;
   if (stream_uploader)
      u_upload_destroy(stream_uploader);
   if (input)
      fake_resource_release(&g_pipe, input);
   CHECK(g_live_buffers == 0 && !g_double_release,
         "u_vbuf failure cleanup releases all fixture resources");
}

int
main(void)
{
   g_pipe.screen = &g_screen;
   g_screen.resource_create = fake_resource_create;
   g_screen.resource_destroy = fake_resource_destroy;
   g_pipe.buffer_map = fake_buffer_map;
   g_pipe.buffer_unmap = fake_buffer_unmap;
   g_pipe.transfer_flush_region = fake_transfer_flush_region;
   g_pipe.resource_release = fake_resource_release;

   struct u_upload_mgr *mgr = u_upload_create(
      &g_pipe, 4096, PIPE_BIND_CUSTOM, PIPE_USAGE_STREAM, 0);
   CHECK(mgr != NULL, "uploader creates against the fake stack");

   unsigned base_offset = ~0u;
   struct pipe_resource *base_buffer = NULL;
   struct pipe_resource *base_releasebuf = NULL;
   uint8_t *base_map = NULL;
   u_upload_alloc(mgr, 0, 64, 4, &base_offset, &base_buffer,
                  &base_releasebuf, (void **)&base_map);
   if (base_releasebuf)
      pipe_resource_release(&g_pipe, base_releasebuf);
   CHECK(base_buffer && base_map && base_offset == 0,
         "baseline upload returns a mapped current buffer");
   CHECK(g_live_buffers == 1, "baseline upload owns one live buffer");

   printf("resource-create failure:\n");
   {
      struct fake_buffer *base = (struct fake_buffer *)base_buffer;
      unsigned fail_offset = 17;
      struct pipe_resource *fail_buffer = base_buffer;
      struct pipe_resource *fail_releasebuf = base_buffer;
      void *fail_map = base->data;

      g_fail_resource_create = true;
      u_upload_alloc(mgr, 0, 8192, 4, &fail_offset, &fail_buffer,
                     &fail_releasebuf, &fail_map);
      g_fail_resource_create = false;

      CHECK(fail_buffer == NULL, "allocation failure clears outbuf");
      CHECK(fail_releasebuf == NULL, "allocation failure clears releasebuf");
      CHECK(fail_offset == ~0u, "allocation failure clears out_offset");
      CHECK(fail_map == NULL, "allocation failure clears ptr");
      CHECK(g_live_buffers == 1,
            "allocation failure retains the prior live buffer");
      CHECK(upload_bytes(mgr, 64, 0x3c, base_buffer),
            "allocation failure retry remaps the retained buffer");
      CHECK(g_live_buffers == 1,
            "allocation failure retry keeps one live buffer");
   }

   printf("new-buffer map failure:\n");
   {
      struct fake_buffer *base = (struct fake_buffer *)base_buffer;
      unsigned fail_offset = 29;
      struct pipe_resource *fail_buffer = base_buffer;
      struct pipe_resource *fail_releasebuf = base_buffer;
      void *fail_map = base->data;

      g_fail_buffer_map = true;
      u_upload_alloc(mgr, 0, 8192, 4, &fail_offset, &fail_buffer,
                     &fail_releasebuf, &fail_map);
      g_fail_buffer_map = false;

      CHECK(fail_buffer == NULL, "new-buffer map failure clears outbuf");
      CHECK(fail_releasebuf == NULL,
            "new-buffer map failure clears releasebuf");
      CHECK(fail_offset == ~0u,
            "new-buffer map failure clears out_offset");
      CHECK(fail_map == NULL, "new-buffer map failure clears ptr");
      CHECK(g_live_buffers == 1,
            "new-buffer map failure releases only the replacement");
      CHECK(upload_bytes(mgr, 64, 0x5a, base_buffer),
            "new-buffer map failure retry remaps the retained buffer");
      CHECK(g_live_buffers == 1,
            "new-buffer map failure retry keeps one live buffer");
   }

   printf("existing-buffer remap failure:\n");
   {
      struct fake_buffer *base = (struct fake_buffer *)base_buffer;
      unsigned fail_offset = 41;
      struct pipe_resource *fail_buffer = base_buffer;
      struct pipe_resource *fail_releasebuf = base_buffer;
      void *fail_map = base->data;

      u_upload_unmap(mgr);
      g_fail_buffer_map = true;
      u_upload_alloc(mgr, 0, 64, 4, &fail_offset, &fail_buffer,
                     &fail_releasebuf, &fail_map);
      g_fail_buffer_map = false;

      CHECK(fail_buffer == NULL, "existing-buffer remap failure clears outbuf");
      CHECK(fail_releasebuf == NULL,
            "existing-buffer remap failure clears releasebuf");
      CHECK(fail_offset == ~0u,
            "existing-buffer remap failure clears out_offset");
      CHECK(fail_map == NULL, "existing-buffer remap failure clears ptr");
      CHECK(g_live_buffers == 1,
            "existing-buffer remap failure retains one live buffer");
      CHECK(upload_bytes(mgr, 64, 0x7e, base_buffer),
            "existing-buffer remap failure retry remaps the retained buffer");
   }

   printf("successful rotation:\n");
   {
      unsigned rotation_offset = 73;
      struct pipe_resource *rotation_buffer = NULL;
      struct pipe_resource *rotation_releasebuf = NULL;
      void *rotation_map = NULL;

      u_upload_alloc(mgr, 0, 8192, 4, &rotation_offset,
                     &rotation_buffer, &rotation_releasebuf, &rotation_map);
      CHECK(rotation_buffer != NULL && rotation_map != NULL,
            "successful rotation returns a mapped replacement");
      CHECK(rotation_buffer != base_buffer,
            "successful rotation returns a distinct replacement");
      CHECK(rotation_releasebuf == base_buffer,
            "successful rotation transfers the previous buffer");
      CHECK(g_live_buffers == 2,
            "successful rotation has two live buffers before release");
      pipe_resource_release(&g_pipe, rotation_releasebuf);
      CHECK(g_live_buffers == 1,
            "successful rotation release returns to one live buffer");
   }

   printf("multiple successful handoffs followed by failure:\n");
   {
      struct pipe_resource *handoff0 = NULL;
      struct pipe_resource *handoff1 = NULL;
      struct pipe_resource *failed_buffer =
         (struct pipe_resource *)(void *)&g_screen;
      struct pipe_resource *failed_releasebuf =
         (struct pipe_resource *)(void *)&g_pipe;
      unsigned failed_offset = 17;
      void *failed_map = (void *)&g_screen;
      uint8_t *map = NULL;
      unsigned offset = ~0u;
      struct pipe_resource *buffer = NULL;

      u_upload_alloc(mgr, 0, 8192, 4, &offset, &buffer, &handoff0,
                     (void **)&map);
      CHECK(buffer && map && handoff0,
            "first composed rotation returns its handoff");
      u_upload_unmap(mgr);

      map = NULL;
      offset = ~0u;
      buffer = NULL;
      u_upload_alloc(mgr, 0, 8192, 4, &offset, &buffer, &handoff1,
                     (void **)&map);
      CHECK(buffer && map && handoff1 && handoff1 != handoff0,
            "second composed rotation returns a distinct handoff");
      CHECK(g_live_buffers == 3,
            "two successful rotations retain both deferred buffers");

      g_fail_resource_create = true;
      u_upload_alloc(mgr, 0, 8192, 4, &failed_offset, &failed_buffer,
                     &failed_releasebuf, &failed_map);
      g_fail_resource_create = false;
      CHECK(failed_buffer == NULL && failed_releasebuf == NULL &&
               failed_offset == ~0u && failed_map == NULL,
            "later allocation failure clears all outputs after two handoffs");
      CHECK(g_live_buffers == 3,
            "later allocation failure retains both handoffs and current buffer");

      pipe_resource_release(&g_pipe, handoff0);
      pipe_resource_release(&g_pipe, handoff1);
      CHECK(upload_bytes(mgr, 64, 0xa5, NULL),
            "composed handoff failure retries through the retained buffer");
      CHECK(g_live_buffers == 1,
            "composed handoff cleanup returns to one live buffer");
   }

   printf("repeated failure/recovery lifetime:\n");
   {
      bool lifetime_ok = true;
      unsigned failure_iteration = ~0u;
      const char *failure_condition = NULL;

#define LIFETIME_REQUIRE(condition, description)                            \
      do {                                                                  \
         if (!(condition)) {                                                \
            lifetime_ok = false;                                            \
            if (!failure_condition) {                                       \
               failure_iteration = i;                                       \
               failure_condition = description;                             \
            }                                                               \
         }                                                                  \
      } while (0)

      /* The lifetime control drives a rotation or existing-buffer remap on
       * every iteration and alternates injected failures with clean retries. */
      for (unsigned i = 0; i < 1000; i++) {
         const unsigned edge = i % 3;
         const bool inject_failure = ((i / 3) % 2) == 0;
         const unsigned rotation_size = 32768;
         unsigned iteration_offset = inject_failure ? i : ~0u;
         struct pipe_resource *iteration_buffer =
            inject_failure ? (struct pipe_resource *)(void *)&g_screen : NULL;
         struct pipe_resource *iteration_releasebuf =
            inject_failure ? (struct pipe_resource *)(void *)&g_pipe : NULL;
         void *iteration_map = inject_failure ? (void *)&g_screen : NULL;

         g_fail_resource_create = false;
         g_fail_buffer_map = false;

         if (edge == 2) {
            LIFETIME_REQUIRE(upload_bytes(mgr, 64, (uint8_t)i, NULL),
                             "pre-remap upload succeeds");
            u_upload_unmap(mgr);
         } else {
            u_upload_unmap(mgr);
         }

         g_fail_resource_create = edge == 0 && inject_failure;
         g_fail_buffer_map = edge != 0 && inject_failure;
         u_upload_alloc(mgr, 0, edge == 2 ? 64 : rotation_size, 4,
                        &iteration_offset, &iteration_buffer,
                        &iteration_releasebuf, &iteration_map);
         g_fail_resource_create = false;
         g_fail_buffer_map = false;

         if (inject_failure) {
            LIFETIME_REQUIRE(iteration_offset == ~0u &&
                                iteration_buffer == NULL &&
                                iteration_releasebuf == NULL &&
                                iteration_map == NULL,
                             "failure clears every output");
            LIFETIME_REQUIRE(g_live_buffers == 1,
                             "failure retains one live buffer");
            LIFETIME_REQUIRE(
               upload_bytes(mgr, 64, (uint8_t)(i + 1), NULL),
               "clean retry succeeds");
         } else {
            LIFETIME_REQUIRE(iteration_offset != ~0u &&
                                iteration_buffer != NULL &&
                                iteration_map != NULL,
                             "successful allocation publishes outputs");
            if (edge != 2) {
               LIFETIME_REQUIRE(iteration_releasebuf != NULL,
                                "successful rotation transfers a buffer");
               LIFETIME_REQUIRE(g_live_buffers == 2,
                                "successful rotation holds two buffers");
            } else {
               LIFETIME_REQUIRE(iteration_releasebuf == NULL,
                                "existing-buffer allocation has no handoff");
            }
            if (iteration_releasebuf)
               pipe_resource_release(&g_pipe, iteration_releasebuf);
         }

         LIFETIME_REQUIRE(g_live_buffers == 1,
                          "iteration cleanup retains one buffer");
      }

      g_fail_resource_create = false;
      g_fail_buffer_map = false;
      if (!lifetime_ok)
         printf("  first failure at iteration %u, edge %u: %s\n",
                failure_iteration, failure_iteration % 3,
                failure_condition);
      CHECK(lifetime_ok,
            "1000-cycle mixed failure/recovery loop keeps one live buffer");
#undef LIFETIME_REQUIRE
   }

   u_upload_destroy(mgr);
   CHECK(g_live_buffers == 0 && !g_double_release,
         "destroy returns the allocator to zero live buffers");

   check_persistent_upload_failures();
   check_wrapper_reference_contract();

   g_pipe.create_vertex_elements_state = fake_create_vertex_elements_state;
   g_pipe.bind_vertex_elements_state = fake_bind_vertex_elements_state;
   g_pipe.delete_vertex_elements_state = fake_delete_vertex_elements_state;
   g_pipe.set_vertex_buffers = fake_set_vertex_buffers;
   g_pipe.draw_vbo = fake_draw_vbo;
   check_u_vbuf_failure_ownership();

   fake_free_preserved_buffers();
   printf("%s\n", g_fails ? "FAILED" : "OK");
   return g_fails ? 1 : 0;
}
