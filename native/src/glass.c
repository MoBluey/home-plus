/* Home+ native: Liquid Glass panels. See glass.h. */
#include "glass.h"

#include <math.h>

#include "gfx.h"

#define LEVELS 5     /* 1/2, 1/4, 1/8, 1/16, 1/32 of the screen */
#define TMP_W 1856
#define TMP_H 1000

static struct {
    int ok, w, h;
    SDL_Texture *shelf, *scene, *lv[LEVELS], *tmp, *sheen, *mask;
    int mask_corner;
    SDL_BlendMode mul_alpha;
} G;

static SDL_Texture *target(SDL_Renderer *r, int w, int h)
{
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, w, h);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

int glass_init(SDL_Renderer *r, int w, int h, SDL_Texture *mask, int mask_corner)
{
    G.w = w;
    G.h = h;
    G.mask = mask;
    G.mask_corner = mask_corner;
    if (!SDL_RenderTargetSupported(r)) return 0;

    /* dst.alpha *= src.alpha, colour untouched: cuts a rounded shape out of a target */
    G.mul_alpha = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
                                             SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
    if (SDL_SetTextureBlendMode(mask, G.mul_alpha) != 0) return 0;
    SDL_SetTextureBlendMode(mask, SDL_BLENDMODE_BLEND);

    G.shelf = target(r, w, h);
    G.scene = target(r, w, h);
    G.tmp = target(r, TMP_W, TMP_H);
    for (int i = 0; i < LEVELS; i++) G.lv[i] = target(r, w >> (i + 1), h >> (i + 1));
    /* sheen: white, strongest at the top edge */
    unsigned char px[256 * 4];
    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = 255;
        px[i * 4 + 3] = (unsigned char)(255 * 0.20f * (1 - t) * (1 - t));
    }
    G.sheen = gfx_texture_rgba(r, px, 1, 256);

    G.ok = G.shelf && G.scene && G.tmp && G.sheen;
    for (int i = 0; i < LEVELS; i++) G.ok = G.ok && G.lv[i];
    return G.ok;
}

int glass_ok(void) { return G.ok; }
SDL_Texture *glass_shelf_target(void) { return G.shelf; }
SDL_Texture *glass_scene_target(void) { return G.scene; }

static void copy_plain(SDL_Renderer *r, SDL_Texture *src, const SDL_Rect *sr, const SDL_FRect *dr)
{
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_RenderCopyF(r, src, sr, dr);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
}

void glass_blit(SDL_Renderer *r, SDL_Texture *t)
{
    copy_plain(r, t, NULL, NULL);
}

SDL_Texture *glass_blur(SDL_Renderer *r, SDL_Texture *src)
{
    if (!G.ok) return NULL;
    SDL_Texture *prev = SDL_GetRenderTarget(r);
    /* halve repeatedly (each bilinear step averages 2x2), then step back up two
     * levels; stretching the result over the screen finishes the blur */
    SDL_Texture *from = src;
    for (int i = 0; i < LEVELS; i++) {
        SDL_SetRenderTarget(r, G.lv[i]);
        copy_plain(r, from, NULL, NULL);
        from = G.lv[i];
    }
    for (int i = LEVELS - 2; i >= LEVELS - 3; i--) {
        SDL_SetRenderTarget(r, G.lv[i]);
        copy_plain(r, from, NULL, NULL);
        from = G.lv[i];
    }
    SDL_SetRenderTarget(r, prev);
    return from;
}

void glass_panel(SDL_Renderer *r, SDL_Texture *blur, float x, float y, float w, float h, float radius,
                 Uint8 tr, Uint8 tg, Uint8 tb, Uint8 tint_a, Uint8 alpha)
{
    if (!alpha) return;
    if (!G.ok || !blur || w > TMP_W || h > TMP_H) {
        gfx_draw_nine(r, G.mask, G.mask_corner, radius, x, y, w, h, tr, tg, tb, (Uint8)(tint_a * alpha / 255));
        return;
    }
    SDL_Texture *prev = SDL_GetRenderTarget(r);
    SDL_SetRenderTarget(r, G.tmp);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);

    /* refraction: sample a slightly larger area than the panel covers, so
     * what's behind looks magnified and bends toward the edges */
    int bw, bh;
    SDL_QueryTexture(blur, NULL, NULL, &bw, &bh);
    float grow = 0.05f, gx = w * grow / 2, gy = h * grow / 2;
    float sx = (x + gx) / G.w * bw, sy = (y + gy) / G.h * bh;
    SDL_Rect src = { (int)floorf(sx), (int)floorf(sy), (int)ceilf((w - 2 * gx) / G.w * bw) + 1, (int)ceilf((h - 2 * gy) / G.h * bh) + 1 };
    SDL_FRect dst = { 0, 0, w, h };
    copy_plain(r, blur, &src, &dst);

    /* lift and tint: glass reads lighter than what's behind it */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, tr, tg, tb, tint_a);
    SDL_RenderFillRectF(r, &dst);
    gfx_draw(r, G.sheen, 0, 0, w, h * 0.6f, 255, 255, 255, 255);

    /* cut out the rounded shape */
    SDL_SetTextureBlendMode(G.mask, G.mul_alpha);
    gfx_draw_nine(r, G.mask, G.mask_corner, radius, 0, 0, w, h, 255, 255, 255, 255);
    SDL_SetTextureBlendMode(G.mask, SDL_BLENDMODE_BLEND);

    SDL_SetRenderTarget(r, prev);
    SDL_Rect part = { 0, 0, (int)ceilf(w), (int)ceilf(h) };
    SDL_FRect out = { x, y, (float)part.w, (float)part.h };
    SDL_SetTextureColorMod(G.tmp, 255, 255, 255);
    SDL_SetTextureAlphaMod(G.tmp, alpha);
    SDL_RenderCopyF(r, G.tmp, &part, &out);
}
