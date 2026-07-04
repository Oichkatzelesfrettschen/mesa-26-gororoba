/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * \brief  Drawing stage for handling glPolygonMode(line/point).
 * Convert triangles to points or lines as needed.
 */

/* Authors:  Keith Whitwell <keithw@vmware.com>
 */

#include "util/u_memory.h"
#include "pipe/p_defines.h"
#include "draw_private.h"
#include "draw_pipe.h"
#include "draw_fs.h"


struct unfilled_stage {
   struct draw_stage stage;

   /** [0] = front face, [1] = back face.
    * legal values:  PIPE_POLYGON_MODE_FILL, PIPE_POLYGON_MODE_LINE,
    * and PIPE_POLYGON_MODE_POINT,
    */
   unsigned mode[2];

   int face_slot;

   /* Derivative injection: the two extra output slots the per-triangle dFdx and
    * dFdy gradients are written to, and the VS-output slot of the varying being
    * differentiated. -1 when the backend did not request injection. */
   int ddx_slot;
   int ddy_slot;
   int deriv_src_slot;
};


static inline struct
unfilled_stage *unfilled_stage(struct draw_stage *stage)
{
   return (struct unfilled_stage *) stage;
}


static void
inject_front_face_info(struct draw_stage *stage,
                       struct prim_header *header)
{
   struct unfilled_stage *unfilled = unfilled_stage(stage);
   int slot = unfilled->face_slot;

   /* In case the backend doesn't care about it */
   if (slot < 0) {
      return;
   }

   /* The signed area lives in header->det, but draw_pipe_cull is the only stage
    * that computes it and it runs only when face culling is enabled. Filled-
    * triangle face injection forces this stage with no cull stage ahead of it,
    * so recompute the signed area from the position-output slot exactly as
    * draw_pipe_cull does rather than read a possibly-uninitialized header->det.
    * The formula is identical, so the unfilled line/point callers are
    * unaffected. */
   const unsigned pos = draw_current_shader_position_output(stage->draw);
   const float *p0 = header->v[0]->data[pos];
   const float *p1 = header->v[1]->data[pos];
   const float *p2 = header->v[2]->data[pos];
   const float ex = p0[0] - p2[0];
   const float ey = p0[1] - p2[1];
   const float fx = p1[0] - p2[0];
   const float fy = p1[1] - p2[1];
   const float det = ex * fy - ey * fx;
   const bool is_front_face = (
      (stage->draw->rasterizer->front_ccw && det < 0.0f) ||
      (!stage->draw->rasterizer->front_ccw && det > 0.0f));

   /* The generic TGSI FACE semantic (and llvmpipe) reads 1.0 as front-facing.
    * A driver that requested filled-triangle injection (R300) feeds the value
    * into an FS input whose compiler applies the hardware backface convention
    * (rc_transform_fragment_face: temp = 1.0 - face, so 1.0 means back-facing),
    * so hand that consumer the complement. */
   const float face_value =
      stage->draw->pipeline.frontface_inject ? !is_front_face : is_front_face;

   for (unsigned i = 0; i < 3; ++i) {
      struct vertex_header *v = header->v[i];
      v->data[slot][0] = face_value;
      v->data[slot][1] = face_value;
      v->data[slot][2] = face_value;
      v->data[slot][3] = face_value;
      v->vertex_id = UNDEFINED_VERTEX_ID;
   }
}


static void
inject_screen_gradient_info(struct draw_stage *stage,
                            struct prim_header *header)
{
   struct unfilled_stage *unfilled = unfilled_stage(stage);
   const int ddx_slot = unfilled->ddx_slot;
   const int ddy_slot = unfilled->ddy_slot;
   const int src_slot = unfilled->deriv_src_slot;

   /* In case the backend didn't ask for it, or the differentiated varying is
    * not actually a VS output. */
   if (ddx_slot < 0 || ddy_slot < 0 || src_slot < 0) {
      return;
   }

   /* A varying's screen-space derivatives are constant across a triangle and
    * equal its analytic per-primitive gradient. Solve the 2x2 system mapping
    * the two edge vectors in screen xy to the varying's differences along
    * them; that yields d(varying)/dx and d(varying)/dy. For a position varying,
    * cross(dFdx, dFdy) is along the geometric face normal, so a shader building
    * its normal as normalize(cross(dFdx(pos), dFdy(pos))) recovers the exact
    * normal that R500 quad-difference hardware computes. Clip-space xy (the
    * same coordinates draw_pipe_cull reads for the signed area) is an affine
    * image of window space, so it keeps the gradient direction correct for the
    * normalize(cross()) consumer; only the absolute magnitude differs, and only
    * for perspective-interpolated varyings. */
   const unsigned pos = draw_current_shader_position_output(stage->draw);

   if (getenv("R300_DERIV_DEBUG")) {
      static int once = 0;
      if (!once) {
         once = 1;
         fprintf(stderr, "r300 deriv inject: pos_slot=%u src_slot=%d "
                 "ddx_slot=%d ddy_slot=%d num_outputs=%u\n",
                 pos, src_slot, ddx_slot, ddy_slot,
                 draw_current_shader_outputs(stage->draw));
      }
   }

   const float *p0 = header->v[0]->data[pos];
   const float *p1 = header->v[1]->data[pos];
   const float *p2 = header->v[2]->data[pos];
   const float ex = p1[0] - p0[0];
   const float ey = p1[1] - p0[1];
   const float fx = p2[0] - p0[0];
   const float fy = p2[1] - p0[1];
   const float det = ex * fy - ey * fx;
   const float inv = det != 0.0f ? 1.0f / det : 0.0f;

   /* The 2x2 solve above works in clip xy, so its gradients are per clip
    * unit.  Screen-space consumers -- fwidth, and the analytic LOD a
    * fractional clamp lowering computes -- need per-PIXEL magnitude, and
    * d(clip)/d(pixel) is 1 / viewport_scale per axis (2 / viewport extent,
    * signed, so a flipped y keeps its orientation).  The
    * normalize(cross()) face-normal consumer is invariant under this
    * per-axis scaling: each gradient vector scales as a whole, so the
    * cross-product direction is unchanged. */
   const float *vps = stage->draw->viewports[0].scale;
   const float px_scale = vps[0] != 0.0f ? 1.0f / vps[0] : 0.0f;
   const float py_scale = vps[1] != 0.0f ? 1.0f / vps[1] : 0.0f;

   const float *v0 = header->v[0]->data[src_slot];
   const float *v1 = header->v[1]->data[src_slot];
   const float *v2 = header->v[2]->data[src_slot];

   float ddx[4], ddy[4];
   for (unsigned c = 0; c < 4; ++c) {
      const float d1 = v1[c] - v0[c];
      const float d2 = v2[c] - v0[c];
      ddx[c] = (d1 * fy - d2 * ey) * inv * px_scale;
      ddy[c] = (d2 * ex - d1 * fx) * inv * py_scale;
   }

   for (unsigned i = 0; i < 3; ++i) {
      struct vertex_header *v = header->v[i];
      for (unsigned c = 0; c < 4; ++c) {
         v->data[ddx_slot][c] = ddx[c];
         v->data[ddy_slot][c] = ddy[c];
      }
      v->vertex_id = UNDEFINED_VERTEX_ID;
   }
}


static void
point(struct draw_stage *stage,
      struct prim_header *header,
      struct vertex_header *v0)
{
   struct prim_header tmp;
   tmp.det = header->det;
   tmp.flags = 0;
   tmp.v[0] = v0;
   stage->next->point(stage->next, &tmp);
}


static void
line(struct draw_stage *stage,
     struct prim_header *header,
     struct vertex_header *v0,
     struct vertex_header *v1)
{
   struct prim_header tmp;
   tmp.det = header->det;
   tmp.flags = 0;
   tmp.v[0] = v0;
   tmp.v[1] = v1;
   stage->next->line(stage->next, &tmp);
}


static void
points(struct draw_stage *stage,
       struct prim_header *header)
{
   struct vertex_header *v0 = header->v[0];
   struct vertex_header *v1 = header->v[1];
   struct vertex_header *v2 = header->v[2];

   inject_front_face_info(stage, header);
   inject_screen_gradient_info(stage, header);

   if ((header->flags & DRAW_PIPE_EDGE_FLAG_0) && v0->edgeflag)
      point(stage, header, v0);
   if ((header->flags & DRAW_PIPE_EDGE_FLAG_1) && v1->edgeflag)
      point(stage, header, v1);
   if ((header->flags & DRAW_PIPE_EDGE_FLAG_2) && v2->edgeflag)
      point(stage, header, v2);
}


static void
lines(struct draw_stage *stage,
      struct prim_header *header)
{
   struct vertex_header *v0 = header->v[0];
   struct vertex_header *v1 = header->v[1];
   struct vertex_header *v2 = header->v[2];

   if (header->flags & DRAW_PIPE_RESET_STIPPLE)
      /*
       * XXX could revisit this. The only stage which cares is the line
       * stipple stage. Could just emit correct reset flags here and not
       * bother about all the calling through reset_stipple_counter
       * stages. Though technically it is necessary if line stipple is
       * handled by the driver, but this is not actually hooked up when
       * using vbuf (vbuf stage reset_stipple_counter does nothing).
       */
      stage->next->reset_stipple_counter(stage->next);

   inject_front_face_info(stage, header);
   inject_screen_gradient_info(stage, header);

   if ((header->flags & DRAW_PIPE_EDGE_FLAG_2) && v2->edgeflag)
      line(stage, header, v2, v0);
   if ((header->flags & DRAW_PIPE_EDGE_FLAG_0) && v0->edgeflag)
      line(stage, header, v0, v1);
   if ((header->flags & DRAW_PIPE_EDGE_FLAG_1) && v1->edgeflag)
      line(stage, header, v1, v2);
}


/** For debugging */
static void
print_header_flags(unsigned flags)
{
   debug_printf("header->flags = ");
   if (flags & DRAW_PIPE_RESET_STIPPLE)
      debug_printf("RESET_STIPPLE ");
   if (flags & DRAW_PIPE_EDGE_FLAG_0)
      debug_printf("EDGE_FLAG_0 ");
   if (flags & DRAW_PIPE_EDGE_FLAG_1)
      debug_printf("EDGE_FLAG_1 ");
   if (flags & DRAW_PIPE_EDGE_FLAG_2)
      debug_printf("EDGE_FLAG_2 ");
   debug_printf("\n");
}


/* Unfilled tri:
 *
 * Note edgeflags in the vertex struct is not sufficient as we will
 * need to manipulate them when decomposing primitives.
 *
 * We currently keep the vertex edgeflag and primitive edgeflag mask
 * separate until the last possible moment.
 */
static void
unfilled_tri(struct draw_stage *stage,
             struct prim_header *header)
{
   struct unfilled_stage *unfilled = unfilled_stage(stage);
   unsigned cw = header->det >= 0.0;
   unsigned mode = unfilled->mode[cw];

   if (0)
      print_header_flags(header->flags);

   switch (mode) {
   case PIPE_POLYGON_MODE_FILL:
      /* A filled triangle keeps its winding, so the backend could derive the
       * face itself -- except an R300-class rasterizer cannot route that bit to
       * the FS. When the driver asked for injection (face_slot >= 0), stamp the
       * computed face onto the vertices before passing the triangle through. */
      inject_front_face_info(stage, header);
      inject_screen_gradient_info(stage, header);
      stage->next->tri(stage->next, header);
      break;
   case PIPE_POLYGON_MODE_LINE:
      lines(stage, header);
      break;
   case PIPE_POLYGON_MODE_POINT:
      points(stage, header);
      break;
   default:
      assert(0);
   }
}


static void
unfilled_first_tri(struct draw_stage *stage,
                   struct prim_header *header)
{
   struct unfilled_stage *unfilled = unfilled_stage(stage);
   struct draw_context *draw = stage->draw;
   const struct pipe_rasterizer_state *rast = draw->rasterizer;

   unfilled->mode[0] = rast->front_ccw ? rast->fill_front : rast->fill_back;
   unfilled->mode[1] = rast->front_ccw ? rast->fill_back : rast->fill_front;

   /* draw_unfilled_prepare_outputs allocates the FACE output for backends that
    * call draw_prepare_shader_outputs. A backend that drives the draw module
    * directly without that call (r300) requests injection via frontface_inject;
    * allocate the FACE output here during the pipeline run, the same way the
    * wide-point stage allocates gl_PointCoord. draw_alloc_extra_vertex_attrib is
    * idempotent, so repeated draws reuse the slot. */
   const struct draw_fragment_shader *fs = draw->fs.fragment_shader;
   if (draw->pipeline.frontface_inject && fs && fs->info.uses_frontface) {
      unfilled->face_slot =
         draw_alloc_extra_vertex_attrib(draw, TGSI_SEMANTIC_FACE, 0);
   }

   /* Same idea for the per-triangle screen-space gradients: allocate the two
    * extra outputs during the pipeline run and locate the differentiated VS
    * output. draw_alloc_extra_vertex_attrib is idempotent.  The base
    * fragment shader's info is not consulted: the driver enables injection
    * per draw for the picked VARIANT, whose derivatives (a fractional
    * LOD-clamp lowering) never appear in the base shader. */
   if (draw->pipeline.derivative_inject &&
       draw->pipeline.derivative_src_generic >= 0) {
      unfilled->ddx_slot = draw_alloc_extra_vertex_attrib(
         draw, TGSI_SEMANTIC_GENERIC, draw->pipeline.derivative_ddx_generic);
      unfilled->ddy_slot = draw_alloc_extra_vertex_attrib(
         draw, TGSI_SEMANTIC_GENERIC, draw->pipeline.derivative_ddy_generic);
      unfilled->deriv_src_slot = draw_find_shader_output(
         draw, TGSI_SEMANTIC_GENERIC, draw->pipeline.derivative_src_generic);
   }

   stage->tri = unfilled_tri;
   stage->tri(stage, header);
}


static void
unfilled_flush(struct draw_stage *stage,
               unsigned flags)
{
   stage->next->flush(stage->next, flags);

   stage->tri = unfilled_first_tri;
}


static void
unfilled_reset_stipple_counter(struct draw_stage *stage)
{
   stage->next->reset_stipple_counter(stage->next);
}


static void
unfilled_destroy(struct draw_stage *stage)
{
   draw_free_temp_verts(stage);
   FREE(stage);
}


/*
 * Try to allocate an output slot which we can use
 * to preserve the front face information.
 */
void
draw_unfilled_prepare_outputs(struct draw_context *draw,
                               struct draw_stage *stage)
{
   struct unfilled_stage *unfilled = unfilled_stage(stage);
   const struct pipe_rasterizer_state *rast = draw ? draw->rasterizer : NULL;
   bool is_unfilled = (rast &&
                       (rast->fill_front != PIPE_POLYGON_MODE_FILL ||
                        rast->fill_back != PIPE_POLYGON_MODE_FILL));
   const struct draw_fragment_shader *fs = draw ? draw->fs.fragment_shader : NULL;

   /* A driver whose rasterizer cannot route a hardware face bit to the FS
    * (frontface_inject) needs the face delivered as a vertex attribute for
    * filled triangles too, not just for the line/point decomposition. */
   bool want_face = is_unfilled || (draw && draw->pipeline.frontface_inject);

   if (want_face && fs && fs->info.uses_frontface)  {
      unfilled->face_slot = draw_alloc_extra_vertex_attrib(
         stage->draw, TGSI_SEMANTIC_FACE, 0);
   } else {
      unfilled->face_slot = -1;
   }

   if (draw && draw->pipeline.derivative_inject &&
       draw->pipeline.derivative_src_generic >= 0) {
      unfilled->ddx_slot = draw_alloc_extra_vertex_attrib(
         stage->draw, TGSI_SEMANTIC_GENERIC, draw->pipeline.derivative_ddx_generic);
      unfilled->ddy_slot = draw_alloc_extra_vertex_attrib(
         stage->draw, TGSI_SEMANTIC_GENERIC, draw->pipeline.derivative_ddy_generic);
      unfilled->deriv_src_slot = draw_find_shader_output(
         stage->draw, TGSI_SEMANTIC_GENERIC, draw->pipeline.derivative_src_generic);
   } else {
      unfilled->ddx_slot = -1;
      unfilled->ddy_slot = -1;
      unfilled->deriv_src_slot = -1;
   }
}


/**
 * Create unfilled triangle stage.
 */
struct draw_stage *
draw_unfilled_stage(struct draw_context *draw)
{
   struct unfilled_stage *unfilled = CALLOC_STRUCT(unfilled_stage);
   if (!unfilled)
      goto fail;

   unfilled->stage.draw = draw;
   unfilled->stage.name = "unfilled";
   unfilled->stage.next = NULL;
   unfilled->stage.tmp = NULL;
   unfilled->stage.point = draw_pipe_passthrough_point;
   unfilled->stage.line = draw_pipe_passthrough_line;
   unfilled->stage.tri = unfilled_first_tri;
   unfilled->stage.flush = unfilled_flush;
   unfilled->stage.reset_stipple_counter = unfilled_reset_stipple_counter;
   unfilled->stage.destroy = unfilled_destroy;

   unfilled->face_slot = -1;
   unfilled->ddx_slot = -1;
   unfilled->ddy_slot = -1;
   unfilled->deriv_src_slot = -1;

   if (!draw_alloc_temp_verts(&unfilled->stage, 0))
      goto fail;

   return &unfilled->stage;

 fail:
   if (unfilled)
      unfilled->stage.destroy(&unfilled->stage);

   return NULL;
}
