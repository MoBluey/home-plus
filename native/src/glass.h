/* Home+ native: "Liquid Glass" panels — real blur of what's behind, a little
 * refraction at the edges, sheen and rim. Built from render-to-texture passes
 * on SDL_Renderer (no custom shaders), so it costs a handful of small blits.
 * If render targets or custom blend modes aren't available, glass_ok() is 0
 * and callers draw flat translucent panels instead. */
#ifndef HOMEPLUS_GLASS_H
#define HOMEPLUS_GLASS_H

#include <SDL.h>

/* mask: white rounded-rect texture with mask_corner px corners (for nine-slice). */
int glass_init(SDL_Renderer *r, int w, int h, SDL_Texture *mask, int mask_corner);
int glass_ok(void);

/* Full-screen render targets the frame is composed into. */
SDL_Texture *glass_shelf_target(void);
SDL_Texture *glass_scene_target(void);

/* Blurs src (full screen) into a small texture; valid until the next call. */
SDL_Texture *glass_blur(SDL_Renderer *r, SDL_Texture *src);

/* Draws a glass panel at (x, y, w, h) showing blur (which covers the whole
 * screen). tint is mixed over the blur at tint_a; alpha fades the whole panel. */
void glass_panel(SDL_Renderer *r, SDL_Texture *blur, float x, float y, float w, float h, float radius,
                 Uint8 tr, Uint8 tg, Uint8 tb, Uint8 tint_a, Uint8 alpha);

/* Copies a full-screen texture to the current target with blending off. */
void glass_blit(SDL_Renderer *r, SDL_Texture *t);

#endif
