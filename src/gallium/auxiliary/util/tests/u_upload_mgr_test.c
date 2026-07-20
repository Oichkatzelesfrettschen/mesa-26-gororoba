/* SPDX-License-Identifier: MIT */

/* Failure-path ownership contract for u_upload_mgr: a malloc-backed
 * pipe_screen/pipe_context pair drives the real suballocation, rotation,
 * and owned-reference machinery, and the injectable allocator and map
 * knobs prove the two failure edges hold their ownership rules -- a map
 * failure leaves the uploader without a dangling current buffer, and an
 * allocation failure keeps the rotated-out previous buffer in the
 * caller's releasebuf. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "util/u_upload_mgr.h"

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
};

static bool g_fail_resource_create;
static bool g_fail_buffer_map;
static unsigned g_live_buffers;

static struct pipe_resource *
fake_resource_create(struct pipe_screen *screen,
                     const struct pipe_resource *templ)
{
   if (g_fail_resource_create)
      return NULL;
   struct fake_buffer *fb = calloc(1, sizeof(*fb));
   fb->b = *templ;
   pipe_reference_init(&fb->b.reference, 1);
   fb->b.screen = screen;
   fb->data = calloc(1, templ->width0);
   g_live_buffers++;
   return &fb->b;
}

static void
fake_resource_destroy(struct pipe_screen *screen, struct pipe_resource *res)
{
   struct fake_buffer *fb = (struct fake_buffer *)res;
   free(fb->data);
   free(fb);
   g_live_buffers--;
}

static struct pipe_transfer g_fake_transfer;

static void *
fake_buffer_map(struct pipe_context *ctx, struct pipe_resource *res,
                unsigned level, unsigned usage, const struct pipe_box *box,
                struct pipe_transfer **transfer)
{
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
}

static void
fake_transfer_flush_region(struct pipe_context *ctx,
                           struct pipe_transfer *transfer,
                           const struct pipe_box *box)
{
}

static struct pipe_screen g_screen;
static struct pipe_context g_pipe;

static bool
upload_bytes(struct u_upload_mgr *mgr, unsigned size, uint8_t fill)
{
   unsigned offset = ~0u;
   struct pipe_resource *buf = NULL;
   struct pipe_resource *releasebuf = NULL;
   uint8_t *map = NULL;
   /* *outbuf is a borrowed pointer into the uploader's current buffer;
    * only *releasebuf carries ownership out, handed back for the caller
    * to release after any deferred use. */
   u_upload_alloc(mgr, 0, size, 4, &offset, &buf, &releasebuf, (void **)&map);
   pipe_resource_reference(&releasebuf, NULL);
   if (!map || !buf)
      return false;
   memset(map, fill, size);
   return offset + size <= buf->width0;
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
   g_pipe.resource_release = u_default_resource_release;

   struct u_upload_mgr *mgr = u_upload_create(
      &g_pipe, 4096, PIPE_BIND_CUSTOM, PIPE_USAGE_STREAM, 0);
   CHECK(mgr != NULL, "uploader creates against the fake stack");

   printf("map failure recovery:\n");
   CHECK(upload_bytes(mgr, 64, 0xa5), "baseline upload succeeds");
   g_fail_buffer_map = true;
   /* Force a fresh buffer so the failing map runs against a new
    * allocation: request more than the remaining default-size window. */
   CHECK(!upload_bytes(mgr, 4096, 0x5a), "injected map failure surfaces");
   g_fail_buffer_map = false;
   /* The failed map released the new buffer; the uploader must not
    * retain a dangling pointer to it (the retry maps a live buffer,
    * and ASan proves the absence of a use-after-free). */
   CHECK(upload_bytes(mgr, 64, 0x3c), "upload succeeds after map failure");

   printf("allocation failure ownership:\n");
   {
      /* Fill most of the current buffer, then hold a reference to it and
       * force a rotation whose allocation fails: the rotated-out previous
       * buffer must stay owned (held reference keeps it live) and the
       * uploader must recover on the next successful allocation. */
      unsigned offset = ~0u;
      struct pipe_resource *borrowed = NULL;
      struct pipe_resource *held = NULL;
      struct pipe_resource *pre_release = NULL;
      uint8_t *map = NULL;
      u_upload_alloc(mgr, 0, 64, 4, &offset, &borrowed, &pre_release,
                     (void **)&map);
      pipe_resource_reference(&pre_release, NULL);
      /* Take an owned reference; the borrowed pointer alone would die
       * with the rotation. */
      pipe_resource_reference(&held, borrowed);
      CHECK(held && map, "pre-rotation allocation holds its buffer");
      /* Rotation with a failing allocator: the previous buffer travels
       * out through releasebuf and stays owned there; discarding it
       * inside the failure path would leak the caller's handoff. */
      g_fail_resource_create = true;
      unsigned fail_offset = ~0u;
      struct pipe_resource *fail_buf = NULL;
      struct pipe_resource *rotated_out = NULL;
      void *fail_map = NULL;
      u_upload_alloc(mgr, 0, 8192, 4, &fail_offset, &fail_buf, &rotated_out,
                     &fail_map);
      g_fail_resource_create = false;
      CHECK(!fail_map && !fail_buf, "injected allocation failure surfaces");
      CHECK(rotated_out != NULL,
            "rotated-out buffer stays in releasebuf through the failure");
      pipe_resource_reference(&rotated_out, NULL);
      CHECK(held->reference.count >= 1,
            "independently held reference survives the failed rotation");
      pipe_resource_reference(&held, NULL);
      CHECK(upload_bytes(mgr, 64, 0x22),
            "upload succeeds after allocation failure");
   }

   printf("repeated failure/recovery lifetime:\n");
   for (unsigned i = 0; i < 1000; i++) {
      g_fail_buffer_map = (i % 3) == 1;
      g_fail_resource_create = (i % 5) == 2;
      bool ok = upload_bytes(mgr, 128 + (i % 512) * 8, (uint8_t)i);
      g_fail_buffer_map = false;
      g_fail_resource_create = false;
      if ((i % 3) != 1 && (i % 5) != 2 && !ok) {
         CHECK(false, "clean iteration failed inside the lifetime loop");
         break;
      }
   }
   CHECK(true, "1000-cycle mixed failure/recovery loop completes");

   u_upload_destroy(mgr);
   CHECK(g_live_buffers == 0,
         "destroy returns the allocator to zero live buffers");

   printf("%s\n", g_fails ? "FAILED" : "OK");
   return g_fails ? 1 : 0;
}
