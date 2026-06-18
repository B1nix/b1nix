/* M53 variant B render test: drive Mesa's gallium `virgl` driver on the host
 * GPU through the b1nix /dev/virtio-gpu winsys. Creates a virgl pipe_screen,
 * a render-target resource, clears it to a known colour with the gallium
 * pipe API (Mesa's virgl driver encodes the CLEAR and submits it via the
 * b1nix winsys -> host GPU), reads the result back and pixel-verifies it.
 *
 * On a host without a virgl device the screen create returns NULL and the
 * test reports an honest skip. Marker: M53-GFX: ok gl-accelerated. */
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "pipe/p_shader_tokens.h"
#include "compiler/shader_enums.h"
#include "util/format/u_formats.h"
#include "util/u_inlines.h"
#include "util/u_draw.h"
#include "util/u_simple_shaders.h"

#include "virgl_b1nix_public.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void emit(const char *s) { write(1, s, strlen(s)); }

#define DIM 64

int main(void)
{
   emit("M53-GLACCEL: start\n");

   struct pipe_screen *screen = virgl_b1nix_screen_create();
   if (!screen) {
      emit("M53-GFX: skip gl-accelerated (no virgl device)\n");
      return 0;
   }
   emit("M53-GLACCEL: ok screen\n");

   struct pipe_context *ctx = screen->context_create(screen, NULL, 0);
   if (!ctx) {
      emit("M53-GFX: fail gl-accelerated (context)\n");
      return 1;
   }

   struct pipe_resource tmpl;
   memset(&tmpl, 0, sizeof(tmpl));
   tmpl.target = PIPE_TEXTURE_2D;
   tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
   tmpl.width0 = DIM;
   tmpl.height0 = DIM;
   tmpl.depth0 = 1;
   tmpl.array_size = 1;
   tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
   struct pipe_resource *rt = screen->resource_create(screen, &tmpl);
   if (!rt) {
      emit("M53-GFX: fail gl-accelerated (resource)\n");
      return 1;
   }

   struct pipe_surface surf_tmpl;
   memset(&surf_tmpl, 0, sizeof(surf_tmpl));
   surf_tmpl.format = tmpl.format;
   struct pipe_surface *surf = ctx->create_surface(ctx, rt, &surf_tmpl);
   if (!surf) {
      emit("M53-GFX: fail gl-accelerated (surface)\n");
      return 1;
   }

   /* Bind the surface as the framebuffer and clear it — the canonical gallium
    * clear path the virgl driver encodes as SET_FRAMEBUFFER_STATE + CLEAR. */
   struct pipe_framebuffer_state fb;
   memset(&fb, 0, sizeof(fb));
   fb.width = DIM;
   fb.height = DIM;
   fb.nr_cbufs = 1;
   fb.cbufs[0] = surf;
   ctx->set_framebuffer_state(ctx, &fb);

   union pipe_color_union color;
   color.f[0] = 0.25f;
   color.f[1] = 0.50f;
   color.f[2] = 0.75f;
   color.f[3] = 1.00f;
   ctx->clear(ctx, PIPE_CLEAR_COLOR, NULL, &color, 0.0, 0);
   ctx->flush(ctx, NULL, 0);

   /* Read the rendered pixels back from the host GPU. */
   struct pipe_box box;
   memset(&box, 0, sizeof(box));
   box.width = DIM;
   box.height = DIM;
   box.depth = 1;
   struct pipe_transfer *xfer = NULL;
   void *map = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &box, &xfer);
   if (!map) {
      emit("M53-GFX: fail gl-accelerated (map)\n");
      return 1;
   }

   uint32_t px = ((uint32_t *)map)[0];
   uint32_t b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF,
            a = (px >> 24) & 0xFF;
   ctx->texture_unmap(ctx, xfer);

   if (a >= 250 && r >= 60 && r <= 68 && g >= 124 && g <= 132 && b >= 187 &&
       b <= 195) {
      emit("M53-GFX: ok gl-accelerated\n");
   } else {
      emit("M53-GFX: fail gl-accelerated (pixel)\n");
      return 1;
   }

   /* --- Draw a real triangle: proves the full draw pipeline (vertex/fragment
    * shaders + vertex buffer + rasterisation on the host GPU), not just a
    * clear. Clear to black, then draw a red triangle covering the centre. The
    * centre pixel must end up red and a corner must stay black. ---------- */
   ctx->clear(ctx, PIPE_CLEAR_COLOR, NULL, &(union pipe_color_union){0}, 0.0, 0);

   /* Render state: no cull, no depth/stencil, plain copy blend. */
   struct pipe_rasterizer_state rs;
   memset(&rs, 0, sizeof(rs));
   rs.half_pixel_center = 1;
   rs.bottom_edge_rule = 1;
   rs.depth_clip_near = 1;
   rs.depth_clip_far = 1;
   void *rcso = ctx->create_rasterizer_state(ctx, &rs);
   ctx->bind_rasterizer_state(ctx, rcso);

   struct pipe_blend_state bl;
   memset(&bl, 0, sizeof(bl));
   bl.rt[0].colormask = PIPE_MASK_RGBA;
   void *bcso = ctx->create_blend_state(ctx, &bl);
   ctx->bind_blend_state(ctx, bcso);

   struct pipe_depth_stencil_alpha_state dsa;
   memset(&dsa, 0, sizeof(dsa));
   void *dcso = ctx->create_depth_stencil_alpha_state(ctx, &dsa);
   ctx->bind_depth_stencil_alpha_state(ctx, dcso);

   ctx->set_sample_mask(ctx, ~0u);

   struct pipe_viewport_state vp;
   memset(&vp, 0, sizeof(vp));
   vp.scale[0] = DIM / 2.0f;
   vp.scale[1] = DIM / 2.0f;
   vp.scale[2] = 0.5f;
   vp.translate[0] = DIM / 2.0f;
   vp.translate[1] = DIM / 2.0f;
   vp.translate[2] = 0.5f;
   ctx->set_viewport_states(ctx, 0, 1, &vp);

   /* Vertex buffer: 3 vertices, each pos(vec4) + colour(vec4), all red. */
   static const float verts[] = {
      0.0f,  0.8f, 0.0f, 1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
      -0.8f, -0.8f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
      0.8f,  -0.8f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
   };
   struct pipe_resource vbt;
   memset(&vbt, 0, sizeof(vbt));
   vbt.target = PIPE_BUFFER;
   vbt.format = PIPE_FORMAT_R8_UNORM;
   vbt.width0 = sizeof(verts);
   vbt.height0 = 1;
   vbt.depth0 = 1;
   vbt.array_size = 1;
   vbt.bind = PIPE_BIND_VERTEX_BUFFER;
   struct pipe_resource *vbuf = screen->resource_create(screen, &vbt);
   if (!vbuf) {
      emit("M53-GFX: fail gl-triangle (vbuf)\n");
      return 1;
   }
   ctx->buffer_subdata(ctx, vbuf, PIPE_MAP_WRITE, 0, sizeof(verts), verts);

   struct pipe_vertex_buffer vb;
   memset(&vb, 0, sizeof(vb));
   vb.buffer.resource = vbuf;
   vb.buffer_offset = 0;
   ctx->set_vertex_buffers(ctx, 1, 0, false, &vb);

   struct pipe_vertex_element ve[2];
   memset(ve, 0, sizeof(ve));
   ve[0].src_offset = 0;
   ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   ve[0].src_stride = 8 * sizeof(float);
   ve[0].vertex_buffer_index = 0;
   ve[1].src_offset = 4 * sizeof(float);
   ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
   ve[1].src_stride = 8 * sizeof(float);
   ve[1].vertex_buffer_index = 0;
   void *vecso = ctx->create_vertex_elements_state(ctx, 2, ve);
   ctx->bind_vertex_elements_state(ctx, vecso);

   /* Passthrough shaders (TGSI): vs forwards POSITION+COLOR, fs writes COLOR. */
   const enum tgsi_semantic vs_sem[2] = {TGSI_SEMANTIC_POSITION,
                                         TGSI_SEMANTIC_COLOR};
   const unsigned vs_idx[2] = {0, 0};
   void *vs = util_make_vertex_passthrough_shader(ctx, 2, vs_sem, vs_idx, false);
   ctx->bind_vs_state(ctx, vs);
   void *fs = util_make_fragment_passthrough_shader(
       ctx, TGSI_SEMANTIC_COLOR, TGSI_INTERPOLATE_PERSPECTIVE, true);
   ctx->bind_fs_state(ctx, fs);

   util_draw_arrays(ctx, MESA_PRIM_TRIANGLES, 0, 3);
   ctx->flush(ctx, NULL, 0);

   struct pipe_transfer *xfer2 = NULL;
   uint8_t *m2 = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &box, &xfer2);
   if (!m2) {
      emit("M53-GFX: fail gl-triangle (map)\n");
      return 1;
   }
   uint32_t cpx = *(uint32_t *)(m2 + (DIM / 2) * xfer2->stride + (DIM / 2) * 4);
   uint32_t corner = *(uint32_t *)m2;
   /* Count red pixels: the triangle must actually cover an area, not leave one
    * stray pixel. It spans ~1/3 of the 64x64 target. */
   uint32_t red_px = 0;
   for (int y = 0; y < DIM; y++)
      for (int x = 0; x < DIM; x++) {
         uint32_t p = *(uint32_t *)(m2 + y * xfer2->stride + x * 4);
         if (((p >> 16) & 0xFF) >= 250 && (p & 0xFF) <= 6 &&
             ((p >> 8) & 0xFF) <= 6)
            red_px++;
      }
   ctx->texture_unmap(ctx, xfer2);

   uint32_t cb = cpx & 0xFF, cg = (cpx >> 8) & 0xFF, cr = (cpx >> 16) & 0xFF;
   uint32_t kr = (corner >> 16) & 0xFF, kg = (corner >> 8) & 0xFF,
            kb = corner & 0xFF;
   if (cr >= 250 && cg <= 6 && cb <= 6 && kr <= 6 && kg <= 6 && kb <= 6 &&
       red_px > 500) {
      emit("M53-GFX: ok gl-triangle\n");
   } else {
      emit("M53-GFX: fail gl-triangle (pixel)\n");
      return 1;
   }

   /* Tear down so the winsys frees the host resource + device slot (RES_UNREF)
    * instead of leaking it. */
   pipe_resource_reference(&vbuf, NULL);
   pipe_surface_reference(&surf, NULL);
   pipe_resource_reference(&rt, NULL);
   ctx->destroy(ctx);
   screen->destroy(screen);
   return 0;
}
