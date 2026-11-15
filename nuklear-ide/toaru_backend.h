/*
 * Nuklear backend for ToaruOS: Yutani window + software triangle rasterizer.
 *
 * This file is included by main.c only when IDE_TOARU is defined, after
 * nuklear.h has provided the implementation.  Nuklear still owns layout and
 * widget state; this backend turns its draw commands into pixels in the
 * Yutani window's shared-memory framebuffer.
 */
#ifndef NK_TOARU_BACKEND_H
#define NK_TOARU_BACKEND_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <toaru/graphics.h>
#include <toaru/yutani.h>

struct nk_toaru_vertex {
    float position[2];
    float uv[2];
    nk_byte col[4];
};

struct nk_toaru {
    yutani_t *yctx;
    yutani_window_t *win;
    gfx_context_t *gfx;

    struct nk_font_atlas atlas;
    struct nk_font *font;
    unsigned char *font_tex;
    int font_tex_w;
    int font_tex_h;
    struct nk_draw_null_texture tex_null;

    struct nk_buffer cmds;
    struct nk_buffer vertices;
    struct nk_buffer elements;

    int mouse_x;
    int mouse_y;
    unsigned int mouse_buttons;
    unsigned int scroll_buttons;

    int width;
    int height;
};

static uint32_t nk_toaru_pack(int r, int g, int b, int a)
{
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    if (a < 0) a = 0; else if (a > 255) a = 255;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | (uint32_t)b;
}

static void nk_toaru_texture(const struct nk_toaru *t, float u, float v,
                             int *r, int *g, int *b, int *a)
{
    int x = (int)(u * (float)t->font_tex_w);
    int y = (int)(v * (float)t->font_tex_h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= t->font_tex_w) x = t->font_tex_w - 1;
    if (y >= t->font_tex_h) y = t->font_tex_h - 1;
    const unsigned char *p = t->font_tex +
        ((size_t)y * (size_t)t->font_tex_w + (size_t)x) * 4;
    *r = p[0];
    *g = p[1];
    *b = p[2];
    *a = p[3];
}

static void nk_toaru_triangle(const struct nk_toaru *t, uint32_t *fb,
                              int fb_width, int fb_height,
                              struct nk_rect clip,
                              const struct nk_toaru_vertex *v0,
                              const struct nk_toaru_vertex *v1,
                              const struct nk_toaru_vertex *v2)
{
    float area = (v1->position[0] - v0->position[0]) *
                 (v2->position[1] - v0->position[1]) -
                 (v1->position[1] - v0->position[1]) *
                 (v2->position[0] - v0->position[0]);
    if (area > -0.0001f && area < 0.0001f)
        return;
    float inv_area = 1.0f / area;

    float min_x = v0->position[0];
    float max_x = min_x;
    float min_y = v0->position[1];
    float max_y = min_y;
    if (v1->position[0] < min_x) min_x = v1->position[0];
    if (v2->position[0] < min_x) min_x = v2->position[0];
    if (v1->position[0] > max_x) max_x = v1->position[0];
    if (v2->position[0] > max_x) max_x = v2->position[0];
    if (v1->position[1] < min_y) min_y = v1->position[1];
    if (v2->position[1] < min_y) min_y = v2->position[1];
    if (v1->position[1] > max_y) max_y = v1->position[1];
    if (v2->position[1] > max_y) max_y = v2->position[1];

    int x0 = (int)min_x;
    int x1 = (int)max_x + 1;
    int y0 = (int)min_y;
    int y1 = (int)max_y + 1;
    if (x0 < (int)clip.x) x0 = (int)clip.x;
    if (y0 < (int)clip.y) y0 = (int)clip.y;
    if (x1 > (int)(clip.x + clip.w)) x1 = (int)(clip.x + clip.w);
    if (y1 > (int)(clip.y + clip.h)) y1 = (int)(clip.y + clip.h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb_width) x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int y = y0; y < y1; ++y) {
        float py = (float)y + 0.5f;
        for (int x = x0; x < x1; ++x) {
            float px = (float)x + 0.5f;
            float w0 = ((v2->position[0] - v1->position[0]) *
                        (py - v1->position[1]) -
                        (v2->position[1] - v1->position[1]) *
                        (px - v1->position[0])) * inv_area;
            float w1 = ((v0->position[0] - v2->position[0]) *
                        (py - v2->position[1]) -
                        (v0->position[1] - v2->position[1]) *
                        (px - v2->position[0])) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.0001f || w1 < -0.0001f || w2 < -0.0001f)
                continue;

            float u = w0 * v0->uv[0] + w1 * v1->uv[0] + w2 * v2->uv[0];
            float v = w0 * v0->uv[1] + w1 * v1->uv[1] + w2 * v2->uv[1];
            int tr, tg, tb, ta;
            nk_toaru_texture(t, u, v, &tr, &tg, &tb, &ta);
            if (!ta)
                continue;

            int cr = (int)(w0 * v0->col[0] + w1 * v1->col[0] +
                           w2 * v2->col[0]);
            int cg = (int)(w0 * v0->col[1] + w1 * v1->col[1] +
                           w2 * v2->col[1]);
            int cb = (int)(w0 * v0->col[2] + w1 * v1->col[2] +
                           w2 * v2->col[2]);
            int ca = (int)(w0 * v0->col[3] + w1 * v1->col[3] +
                           w2 * v2->col[3]);
            int sr = cr * tr / 255;
            int sg = cg * tg / 255;
            int sb = cb * tb / 255;
            int sa = ca * ta / 255;
            if (!sa)
                continue;

            uint32_t *dst = &fb[(size_t)y * (size_t)fb_width + (size_t)x];
            uint32_t bg = *dst;
            int da = (int)((bg >> 24) & 0xff);
            int dr = (int)((bg >> 16) & 0xff);
            int dg = (int)((bg >> 8) & 0xff);
            int db = (int)(bg & 0xff);
            int ia = 255 - sa;
            int r = (sr * sa + dr * ia) / 255;
            int g = (sg * sa + dg * ia) / 255;
            int b = (sb * sa + db * ia) / 255;
            int a = sa + da * ia / 255;
            *dst = nk_toaru_pack(r, g, b, a);
        }
    }
}

static bool nk_toaru_init(struct nk_toaru *t, struct nk_context *ctx,
                          int requested_width, int requested_height)
{
    memset(t, 0, sizeof(*t));
    t->yctx = yutani_init();
    if (!t->yctx)
        return false;

    int width = requested_width;
    int height = requested_height;
    int max_width = (int)t->yctx->display_width - 80;
    int max_height = (int)t->yctx->display_height - 100;
    if (width > max_width) width = max_width;
    if (height > max_height) height = max_height;
    if (width < 640) width = 640;
    if (height < 480) height = 480;

    t->win = yutani_window_create(t->yctx, width, height);
    if (!t->win)
        return false;
    t->win->user_data = t;
    yutani_window_advertise(t->yctx, t->win, "Nuklear IDE");
    yutani_window_move(t->yctx, t->win,
                       ((int)t->yctx->display_width - width) / 2,
                       ((int)t->yctx->display_height - height) / 2);
    t->gfx = init_graphics_yutani_double_buffer(t->win);
    if (!t->gfx || !t->gfx->backbuffer)
        return false;
    t->width = t->win->width;
    t->height = t->win->height;

    nk_buffer_init_default(&t->cmds);
    nk_buffer_init_default(&t->vertices);
    nk_buffer_init_default(&t->elements);

    nk_font_atlas_init_default(&t->atlas);
    nk_font_atlas_begin(&t->atlas);
    struct nk_font_config config = nk_font_config(15.0f);
    config.oversample_h = 1;
    config.oversample_v = 1;
    config.pixel_snap = 1;
    t->font = nk_font_atlas_add_default(&t->atlas, 15.0f, &config);
    if (!t->font)
        return false;

    int tex_w = 0, tex_h = 0;
    const void *image = nk_font_atlas_bake(&t->atlas, &tex_w, &tex_h,
                                            NK_FONT_ATLAS_RGBA32);
    if (!image || tex_w <= 0 || tex_h <= 0)
        return false;
    t->font_tex = malloc((size_t)tex_w * (size_t)tex_h * 4);
    if (!t->font_tex)
        return false;
    memcpy(t->font_tex, image, (size_t)tex_w * (size_t)tex_h * 4);
    t->font_tex_w = tex_w;
    t->font_tex_h = tex_h;
    nk_font_atlas_end(&t->atlas, nk_handle_id(0), &t->tex_null);
    nk_style_set_font(ctx, &t->font->handle);
    return true;
}

static void nk_toaru_resize(struct nk_toaru *t, int width, int height)
{
    (void)width;
    (void)height;
    reinit_graphics_yutani(t->gfx, t->win);
    t->width = t->win->width;
    t->height = t->win->height;
}

static void nk_toaru_render(struct nk_toaru *t, struct nk_context *ctx)
{
    if (!t->gfx || !t->win)
        return;
    int width = t->win->width;
    int height = t->win->height;
    uint32_t *fb = (uint32_t *)t->gfx->backbuffer;
    if (!fb)
        return;
    memset(fb, 0, (size_t)width * (size_t)height * sizeof(uint32_t));

    static const struct nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT,
         NK_OFFSETOF(struct nk_toaru_vertex, position)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT,
         NK_OFFSETOF(struct nk_toaru_vertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8,
         NK_OFFSETOF(struct nk_toaru_vertex, col)},
        {NK_VERTEX_LAYOUT_END}
    };

    struct nk_convert_config config;
    memset(&config, 0, sizeof(config));
    config.vertex_layout = layout;
    config.vertex_size = sizeof(struct nk_toaru_vertex);
    config.vertex_alignment = NK_ALIGNOF(struct nk_toaru_vertex);
    config.tex_null = t->tex_null;
    config.circle_segment_count = 18;
    config.curve_segment_count = 18;
    config.arc_segment_count = 18;
    config.global_alpha = 1.0f;
    config.shape_AA = NK_ANTI_ALIASING_ON;
    config.line_AA = NK_ANTI_ALIASING_ON;

    nk_convert(ctx, &t->cmds, &t->vertices, &t->elements, &config);

    const struct nk_toaru_vertex *vertices =
        (const struct nk_toaru_vertex *)nk_buffer_memory_const(&t->vertices);
    const nk_draw_index *indices =
        (const nk_draw_index *)nk_buffer_memory_const(&t->elements);
    const nk_draw_index *offset = indices;

    const struct nk_draw_command *cmd;
    nk_draw_foreach(cmd, ctx, &t->cmds) {
        if (!cmd->elem_count)
            continue;
        struct nk_rect clip = cmd->clip_rect;
        for (nk_size i = 0; i + 2 < cmd->elem_count; i += 3) {
            const struct nk_toaru_vertex *v0 = &vertices[offset[i + 0]];
            const struct nk_toaru_vertex *v1 = &vertices[offset[i + 1]];
            const struct nk_toaru_vertex *v2 = &vertices[offset[i + 2]];
            nk_toaru_triangle(t, fb, width, height, clip, v0, v1, v2);
        }
        offset += cmd->elem_count;
    }

    nk_clear(ctx);
    nk_buffer_clear(&t->cmds);
    nk_buffer_clear(&t->vertices);
    nk_buffer_clear(&t->elements);

    /* Render off-screen, then copy the finished frame into the shared window
     * buffer.  Drawing directly into the shared buffer makes the compositor
     * show half-cleared frames while the mouse is moving. */
    memcpy(t->win->buffer, t->gfx->backbuffer,
           (size_t)t->win->width * (size_t)t->win->height * 4);
    yutani_flip(t->yctx, t->win);
}

static void nk_toaru_shutdown(struct nk_toaru *t)
{
    if (t->font_tex)
        free(t->font_tex);
    nk_font_atlas_clear(&t->atlas);
    nk_buffer_free(&t->cmds);
    nk_buffer_free(&t->vertices);
    nk_buffer_free(&t->elements);
    if (t->gfx)
        release_graphics_yutani(t->gfx);
    if (t->yctx && t->win)
        yutani_close(t->yctx, t->win);
}

#endif /* NK_TOARU_BACKEND_H */
