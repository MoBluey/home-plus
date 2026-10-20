/* Home+ native: headless preview helpers for the desktop build (screenshots in
 * tools/native-preview.sh). Linked into the TV build too, where they're unused. */
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

int homeplus_screenshot(SDL_Renderer *r, const char *path)
{
    int w, h;
    SDL_GetRendererOutputSize(r, &w, &h);
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ABGR8888, px, w * 4);
    int ok = stbi_write_png(path, w, h, 4, px, w * 4);
    free(px);
    return ok;
}

/* HOMEPLUS_KEYS: a script of remote presses, one every 150 ms from 1.5 s in.
 * L R U D = arrows, O = OK, H = hold OK (opens the sheet), B = back. */
void homeplus_preview_input(Uint32 elapsed)
{
    static size_t done;
    static Uint32 hold_until;
    const char *keys = getenv("HOMEPLUS_KEYS");
    if (!keys) return;
    if (hold_until && elapsed >= hold_until) {
        SDL_Event e = { 0 };
        e.type = SDL_KEYUP;
        e.key.keysym.sym = SDLK_RETURN;
        SDL_PushEvent(&e);
        hold_until = 0;
    }
    if (hold_until || done >= strlen(keys) || elapsed < 1500 + done * 150) return;
    char c = keys[done++];
    SDL_Keycode k = c == 'L' ? SDLK_LEFT : c == 'R' ? SDLK_RIGHT : c == 'U' ? SDLK_UP : c == 'D' ? SDLK_DOWN
                  : c == 'B' ? SDLK_ESCAPE : SDLK_RETURN;
    SDL_Event e = { 0 };
    e.type = SDL_KEYDOWN;
    e.key.keysym.sym = k;
    SDL_PushEvent(&e);
    if (c == 'H') {
        hold_until = elapsed + 800;
        return;
    }
    e.type = SDL_KEYUP;
    SDL_PushEvent(&e);
}
