/* Home+ native: drawing helpers. See gfx.h. */
#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

SDL_Texture *gfx_texture_rgba(SDL_Renderer *r, const unsigned char *rgba, int w, int h)
{
    /* ABGR8888 is R,G,B,A byte order on little-endian, which is what we hold. */
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, rgba, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

/* Signed distance from p to a rounded box with half-extents (bx, by) and corner radius rad. */
static float sd_round_box(float px, float py, float bx, float by, float rad)
{
    float qx = fabsf(px) - bx + rad, qy = fabsf(py) - by + rad;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - rad;
}

SDL_Texture *gfx_rounded(SDL_Renderer *r, int w, int h, float radius, float border,
                         float top_alpha, float bottom_alpha)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    float bx = w * 0.5f, by = h * 0.5f;
    for (int y = 0; y < h; y++) {
        float edge_alpha = top_alpha + (bottom_alpha - top_alpha) * (float)y / (float)(h - 1);
        for (int x = 0; x < w; x++) {
            float d = sd_round_box(x + 0.5f - bx, y + 0.5f - by, bx, by, radius);
            float a = clampf(0.5f - d, 0, 1);
            if (border > 0) a = (a - clampf(0.5f - (d + border), 0, 1)) * edge_alpha;
            unsigned char *p = px + ((size_t)y * w + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
    }
    SDL_Texture *t = gfx_texture_rgba(r, px, w, h);
    free(px);
    return t;
}

SDL_Texture *gfx_glow(SDL_Renderer *r, int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return NULL;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float dx = (x + 0.5f) / w * 2 - 1, dy = (y + 0.5f) / h * 2 - 1;
            float d = clampf(sqrtf(dx * dx + dy * dy), 0, 1);
            float a = 1 - d;
            a = a * a * (3 - 2 * a); /* smoothstep falloff */
            unsigned char *p = px + ((size_t)y * w + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(a * 255.0f);
        }
    }
    SDL_Texture *t = gfx_texture_rgba(r, px, w, h);
    free(px);
    return t;
}

SDL_Texture *gfx_gradient(SDL_Renderer *r, int vertical, float a0, float a1)
{
    enum { N = 256 };
    unsigned char px[N * 4];
    for (int i = 0; i < N; i++) {
        px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
        px[i * 4 + 3] = (unsigned char)((a0 + (a1 - a0) * i / (float)(N - 1)) * 255.0f);
    }
    return vertical ? gfx_texture_rgba(r, px, 1, N) : gfx_texture_rgba(r, px, N, 1);
}

void gfx_draw(SDL_Renderer *r, SDL_Texture *tex, float x, float y, float w, float h,
              Uint8 cr, Uint8 cg, Uint8 cb, Uint8 alpha)
{
    if (!tex || alpha == 0) return;
    SDL_SetTextureColorMod(tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_FRect d = { x, y, w, h };
    SDL_RenderCopyF(r, tex, NULL, &d);
}

void gfx_draw_nine(SDL_Renderer *r, SDL_Texture *tex, int sc, float dc,
                   float x, float y, float w, float h, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 alpha)
{
    if (!tex || alpha == 0) return;
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    if (dc * 2 > w) dc = w / 2;
    if (dc * 2 > h) dc = h / 2;
    SDL_SetTextureColorMod(tex, cr, cg, cb);
    SDL_SetTextureAlphaMod(tex, alpha);
    int sx[4] = { 0, sc, tw - sc, tw }, sy[4] = { 0, sc, th - sc, th };
    /* snap to whole pixels so neighbouring slices never overlap (a visible seam on translucent fills) */
    float dx[4] = { roundf(x), roundf(x + dc), roundf(x + w - dc), roundf(x + w) };
    float dy[4] = { roundf(y), roundf(y + dc), roundf(y + h - dc), roundf(y + h) };
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            SDL_Rect s = { sx[i], sy[j], sx[i + 1] - sx[i], sy[j + 1] - sy[j] };
            SDL_FRect d = { dx[i], dy[j], dx[i + 1] - dx[i], dy[j + 1] - dy[j] };
            if (s.w > 0 && s.h > 0 && d.w > 0 && d.h > 0) SDL_RenderCopyF(r, tex, &s, &d);
        }
    }
}

void gfx_draw_cover(SDL_Renderer *r, SDL_Texture *tex, float x, float y, float w, float h, Uint8 alpha)
{
    if (!tex || alpha == 0) return;
    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);
    float s = fmaxf(w / tw, h / th);
    float sw = w / s, sh = h / s;
    SDL_Rect src = { (int)((tw - sw) / 2), (int)((th - sh) / 4), (int)sw, (int)sh }; /* bias upward, like center 25% */
    SDL_FRect d = { x, y, w, h };
    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopyF(r, tex, &src, &d);
}

/* ------------------------------------------------------------------ icons */

/* Inside tests in a unit square centred on 0 (x, y in -0.5..0.5, y down). */
static int in_rrect(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    return sd_round_box(x - cx, y - cy, hw, hh, r) <= 0;
}

static int in_ring_rrect(float x, float y, float cx, float cy, float hw, float hh, float r, float t)
{
    float d = sd_round_box(x - cx, y - cy, hw, hh, r);
    return d <= 0 && d > -t;
}

static int icon_inside(int kind, float x, float y)
{
    const float T = 0.075f; /* stroke width */
    switch (kind) {
    case ICON_RECENTS:
        /* two stacked cards, the back one peeking out top-right */
        if (in_ring_rrect(x, y, -0.06f, 0.07f, 0.28f, 0.22f, 0.08f, T)) return 1;
        return in_ring_rrect(x, y, 0.06f, -0.07f, 0.28f, 0.22f, 0.08f, T) &&
               !in_rrect(x, y, -0.06f, 0.07f, 0.28f, 0.22f, 0.08f);
    case ICON_INPUT: {
        /* a screen with an arrow entering it from the left */
        float d = sd_round_box(x - 0.08f, y, 0.32f, 0.26f, 0.08f);
        int frame = d <= 0 && d > -T && !(x < -0.18f && fabsf(y) < 0.12f); /* gap where the arrow enters */
        int shaft = x > -0.44f && x < 0.04f && fabsf(y) < T / 2;
        float ax = x - 0.12f; /* arrow head: a chevron pointing right */
        int head = ax < 0 && ax > -0.2f && fabsf(fabsf(y) - (-ax)) < T * 0.75f;
        return frame || shaft || head;
    }
    case ICON_SETTINGS: {
        /* gear: 8 teeth around a ring with a hole */
        float rr = sqrtf(x * x + y * y), a = atan2f(y, x);
        float tooth = cosf(a * 8) > 0.25f ? 0.44f : 0.34f;
        return rr <= tooth && rr >= 0.14f;
    }
    }
    return 0;
}

SDL_Texture *gfx_icon(SDL_Renderer *r, int kind, int size)
{
    enum { SS = 4 };
    unsigned char *px = malloc((size_t)size * size * 4);
    if (!px) return NULL;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int n = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++)
                    n += icon_inside(kind, (x + (sx + 0.5f) / SS) / size - 0.5f, (y + (sy + 0.5f) / SS) / size - 0.5f);
            unsigned char *p = px + ((size_t)y * size + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (unsigned char)(255 * n / (SS * SS));
        }
    }
    SDL_Texture *t = gfx_texture_rgba(r, px, size, size);
    free(px);
    return t;
}

/* ------------------------------------------------------------------ text */

struct Font {
    unsigned char *data;
    stbtt_fontinfo info;
};

Font *font_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    Font *f = calloc(1, sizeof *f);
    f->data = malloc(n);
    if (fread(f->data, 1, n, fp) != (size_t)n || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        fclose(fp);
        free(f->data);
        free(f);
        return NULL;
    }
    fclose(fp);
    return f;
}

void font_free(Font *f)
{
    if (!f) return;
    free(f->data);
    free(f);
}

/* Decodes one UTF-8 codepoint; advances *s. Invalid bytes map to '?'. */
static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = p[0], n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { (*s)++; return '?'; }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += i; return '?'; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s += n + 1;
    return c;
}

#define MAX_CPS 512

/* Fills pos[0..n] with pen positions (pos[n] = total width). */
static float layout_line(Font *f, float scale, const int *cps, int n, float *pos)
{
    float pen = 0;
    for (int i = 0; i < n; i++) {
        int adv, lsb;
        pos[i] = pen;
        stbtt_GetCodepointHMetrics(&f->info, cps[i], &adv, &lsb);
        pen += adv * scale;
        if (i + 1 < n) pen += scale * stbtt_GetCodepointKernAdvance(&f->info, cps[i], cps[i + 1]);
    }
    pos[n] = pen;
    return pen;
}

SDL_Texture *text_render(SDL_Renderer *r, Font *f, float px, const char *utf8, int max_w,
                         int *out_w, int *out_h)
{
    if (!f || !utf8 || !*utf8) return NULL;
    int cps[MAX_CPS + 1], n = 0;
    for (const char *s = utf8; *s && n < MAX_CPS;) cps[n++] = utf8_next(&s);

    float scale = stbtt_ScaleForMappingEmToPixels(&f->info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);

    /* lay out; if too wide, replace trailing characters with an ellipsis */
    float pos[MAX_CPS + 2];
    float width = layout_line(f, scale, cps, n, pos);
    if (max_w > 0 && width > max_w) {
        while (n > 1) {
            n--;
            cps[n - 1] = 0x2026;
            if (layout_line(f, scale, cps, n, pos) <= max_w) break;
        }
    }

    int w = (int)ceilf(pos[n]) + 4;
    int h = (int)ceilf((asc - desc) * scale) + 4;
    int baseline = (int)roundf(asc * scale) + 2;
    unsigned char *cov = calloc((size_t)w * h, 1);
    for (int i = 0; i < n; i++) {
        float x = pos[i] + 1;
        int ix = (int)floorf(x);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&f->info, cps[i], scale, scale, x - ix, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        unsigned char *g = malloc((size_t)gw * gh);
        stbtt_MakeCodepointBitmapSubpixel(&f->info, g, gw, gh, gw, scale, scale, x - ix, 0, cps[i]);
        for (int yy = 0; yy < gh; yy++) {
            int ty = baseline + y0 + yy;
            if (ty < 0 || ty >= h) continue;
            for (int xx = 0; xx < gw; xx++) {
                int tx = ix + x0 + xx;
                if (tx < 0 || tx >= w) continue;
                int v = cov[ty * w + tx] + g[yy * gw + xx];
                cov[ty * w + tx] = v > 255 ? 255 : (unsigned char)v;
            }
        }
        free(g);
    }

    unsigned char *rgba = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }
    SDL_Texture *t = gfx_texture_rgba(r, rgba, w, h);
    free(cov);
    free(rgba);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return t;
}
