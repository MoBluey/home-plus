/* Home+ native: drawing helpers built on SDL_Renderer.
 *
 * Everything the UI draws is a white (or alpha-only) texture tinted per draw
 * with SDL_SetTextureColorMod / AlphaMod, so a frame is a few dozen textured
 * quads and nothing is rasterised on the CPU after start-up. */
#ifndef HOMEPLUS_GFX_H
#define HOMEPLUS_GFX_H

#include <SDL.h>

/* Anti-aliased rounded rectangle, white. border > 0 draws only a ring of that
 * width whose alpha fades from top_alpha at the top edge to bottom_alpha. */
SDL_Texture *gfx_rounded(SDL_Renderer *r, int w, int h, float radius, float border,
                         float top_alpha, float bottom_alpha);

/* White radial glow: alpha 1 in the centre fading to 0 at the edge. */
SDL_Texture *gfx_glow(SDL_Renderer *r, int w, int h);

/* Black gradient strip for scrims. vertical: alpha goes a0 (top) -> a1 (bottom);
 * otherwise left -> right. Stretch it over the area you want darkened. */
SDL_Texture *gfx_gradient(SDL_Renderer *r, int vertical, float a0, float a1);

/* Upload an RGBA8 buffer as a texture (linear filtered by the renderer hint). */
SDL_Texture *gfx_texture_rgba(SDL_Renderer *r, const unsigned char *rgba, int w, int h);

/* White line icons for the top bar, drawn once at start-up. */
enum { ICON_RECENTS, ICON_SETTINGS };
SDL_Texture *gfx_icon(SDL_Renderer *r, int kind, int size);

/* ---- text ---- */

typedef struct Font Font;

Font *font_load(const char *path);
void font_free(Font *f);

/* Renders UTF-8 text in white at the given pixel height. max_w > 0 truncates
 * with an ellipsis. Returns NULL for empty text. */
SDL_Texture *text_render(SDL_Renderer *r, Font *f, float px, const char *utf8, int max_w,
                         int *out_w, int *out_h);

/* ---- drawing ---- */

/* Draw tex at (x, y, w, h) tinted with rgb and alpha (0..255). */
void gfx_draw(SDL_Renderer *r, SDL_Texture *tex, float x, float y, float w, float h,
              Uint8 cr, Uint8 cg, Uint8 cb, Uint8 alpha);

/* Nine-slice draw for rounded shapes of any size: tex is a square rounded
 * texture whose corners are src_corner px; they're drawn at dst_corner px and
 * the edges/centre stretch. Keeps corners round at any width/height. */
void gfx_draw_nine(SDL_Renderer *r, SDL_Texture *tex, int src_corner, float dst_corner,
                   float x, float y, float w, float h, Uint8 cr, Uint8 cg, Uint8 cb, Uint8 alpha);

/* Draw tex scaled to cover (x, y, w, h), cropping the overflow (like CSS
 * background-size: cover). */
void gfx_draw_cover(SDL_Renderer *r, SDL_Texture *tex, float x, float y, float w, float h, Uint8 alpha);

#endif
