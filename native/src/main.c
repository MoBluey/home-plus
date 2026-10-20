/* Home+ native — a tvOS-style home screen for webOS, drawn with SDL2 + GLES2.
 *
 * Rendered on the GPU by our own GLES2 renderer (render.c): Liquid Glass panels,
 * rounded shapes and text, a few dozen quads a frame. Animations run at the
 * display refresh rate, and when nothing moves the loop sleeps.
 *
 * Data comes from the Home+ service's local API (see service/service.js). */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>
/* LG's libSDL2 is the Wayland build; the vendored generic headers only show
 * SDL_SysWMinfo's Wayland members when asked */
#define SDL_VIDEO_DRIVER_WAYLAND 1
#include <SDL_syswm.h>

#include "cJSON.h"
#include "gfx.h"
#include "net.h"
#include "sound.h"

/* ------------------------------------------------------------------ layout */

/* Spacing system: the dock spans the 80/60 px TV safe area; inside its 40 px
 * padding five columns of 5:3 tiles spread evenly. Containers keep their
 * corners concentric with their content (outer radius = inner radius +
 * padding). The Top Shelf text, the rail and the top bar line up with the
 * dock's edges. */
#define W 1920
#define H 1080
#define SAFE_X 80
#define SAFE_Y 60
#define COLS 5
#define TILE_W 272
#define TILE_H 163
#define TILE_R 22                      /* every card's corner: app tiles, Continue Watching, the big Top Shelf tile */
#define PAD 40
#define DOCK_X SAFE_X                  /* glass platform behind the top row, safe area wide */
#define DOCK_W (W - 2 * DOCK_X)
#define GAP_X ((DOCK_W - 2 * PAD - COLS * TILE_W) / (COLS - 1)) /* 80: columns spread across the dock */
#define LEFT (DOCK_X + PAD)            /* first column */
#define RIGHT (W - DOCK_X)             /* the top bar ends where the dock does */
#define DOCK_H (TILE_H + 2 * PAD)
#define DOCK_Y (974 - DOCK_H)
#define GRID_TOP (DOCK_Y + PAD)
#define ROW_H (TILE_H + 100)           /* rows at least 100 px apart; labels sit in the gap */
#define FOCUS_SCALE 1.10f
#define LONG_PRESS_MS 650
#define RAIL_CARD_W TILE_W             /* Continue Watching cards sit on the same columns */
#define RAIL_CARD_H 153                /* 16:9 */
#define RAIL_GAP GAP_X
#define RAIL_Y (DOCK_Y - PAD - RAIL_CARD_H - 16)
#define RAIL_VISIBLE 5
#define BAR 56                         /* clock capsule and round top-bar buttons */
#define SW_W 680                       /* App Switcher cards: the app's tile, large */
#define SW_H (SW_W * TILE_H / TILE_W)
#define SW_PITCH (SW_W + 80)
#define SW_CY 500                      /* their centre line */
#define SHEET_BH 80                    /* sheet buttons, and the gap between them */
#define SHEET_GAP 16
#define ICON_FIT 256                   /* icons feed logos, covers and the big Top Shelf tile */
#define THUMB_FIT_W 480                /* rail thumbnails: cards, and briefly the background */
#define THUMB_FIT_H 270
#define BAR_GAP 16
#define TEXT_STATIC 218                /* OLED: static text stays at ~85% white */

#define MAX_APPS 128
#define MAX_RAIL 16
#define MAX_SHEET 64
#define ID_LEN 160

enum { TAG_APPS = 1, TAG_LAYOUT, TAG_STREMIO, TAG_ICON, TAG_ART, TAG_RAIL_THUMB, TAG_RAIL_BG, TAG_LAUNCH, TAG_WAKE,
       TAG_SYSTEM, TAG_TRAKT_LINK, TAG_TRAKT_STATUS, TAG_RUNNING, TAG_CLOSED, TAG_HOME, TAG_HOME_SET,
       TAG_STATUS, TAG_ELEVATE, TAG_RESTART, TAG_UNINSTALL,
       TAG_PREFS };

/* zones: what the remote is moving through */
enum { ZONE_GRID, ZONE_RAIL, ZONE_TOP };
/* top bar buttons, left of the clock (no Inputs: the remote's Input key has LG's own menu) */
enum { TOP_RECENTS, TOP_SETTINGS, TOP_COUNT };
#define MAX_RUNNING 32
#define MAX_SYS 16

typedef struct {
    char id[ID_LEN], title[ID_LEN], icon[ID_LEN + 16], art[ID_LEN + 16];
    Uint8 col[3];
    int has_col, fullbleed;
    Tex *icon_tex, *art_tex, *label, *big;
    Tex *logo_tex;          /* the icon's logo, background keyed out and trimmed (tiles) */
    Tex *cover_tex;         /* made from the icon, for the Top Shelf when the app has no artwork */
    float logo_w, logo_h, logo_r; /* its size on a tile, and corner radius for picture icons */
    int icon_state, art_state; /* 0 idle, 1 loading, 2 ready, 3 failed */
    int lw, lh, bw, bh;
    float s, sv; /* focus animation 0..1, and its spring velocity */
} App;

typedef struct {
    char id[64], type[16], name[200], bg[40], thumb[40];
    float progress;
    int season, episode, remaining;
    Tex *thumb_tex, *bg_tex, *name_tex, *title_tex, *meta_tex;
    int thumb_state, bg_state;
    int nw, nh, tw, th, mw, mh;
    float s, sv;
} RailItem;

typedef enum { ACT_NONE, ACT_MOVE, ACT_FRONT, ACT_HIDE, ACT_UNHIDE, ACT_UNHIDE_ALL, ACT_STREMIO_HELP, ACT_CANCEL,
               ACT_LAUNCH_ID, ACT_TRAKT_LINK, ACT_TRAKT_UNLINK,
               ACT_HOME_MODE, ACT_SOUNDS,
               ACT_SETUP_RETRY, ACT_HOME_YES, ACT_HOME_NO, ACT_HOME_TOGGLE, ACT_REMOVE_ASK, ACT_REMOVE,
               ACT_ACCESSIBILITY, ACT_REDUCE_MOTION, ACT_REDUCE_TRANSPARENCY } Action;

/* First run: the service has to be running and elevated (root, via Homebrew
 * Channel) before Home+ can list apps. Each step that can fail has a screen. */
enum { SETUP_CHECK, SETUP_STARTING, SETUP_ELEVATING, SETUP_RESTARTING, SETUP_NO_HBC, SETUP_DENIED,
       SETUP_FAILED, SETUP_REMOVING, SETUP_DONE };
#define HBC_ELEVATE "luna://org.webosbrew.hbchannel.service/elevateService"
#define SHEET_BODY_LINES 3

typedef struct {
    Action act;
    int arg;
    int destructive;
    char id[ID_LEN]; /* ACT_LAUNCH_ID */
    Tex *tex;
    int w, h;
} SheetItem;

static struct {
    SDL_Window *win;
    Font *f_medium, *f_semi, *f_bold;

    /* shared textures, tinted per draw (shapes and glass are shaders) */
    Tex *scrim_v, *scrim_h, *scrim_top;
    /* top bar: a label under the focused button, and the Top Shelf hint while it has focus */
    Tex *top_label[TOP_COUNT], *top_hint[TOP_COUNT];
    int top_lw[TOP_COUNT], top_lh[TOP_COUNT], top_hw[TOP_COUNT], top_hh[TOP_COUNT];
    Tex *rail_hint, *launch_tex; /* "Opens in Stremio"; "Opening Netflix…" under a launching tile */
    int rail_hint_w, rail_hint_h, launch_w, launch_h;
    int launching, launch_item; /* OK on a tile: it breathes until the app is in front */
    Uint32 launch_at;

    App apps[MAX_APPS];
    int napps, generation; /* generation: bumps when the app list is rebuilt */
    char apps_sig[4096];
    int items[MAX_APPS + 1], nitems; /* indices into apps; -1 = "Hidden" tile */

    char order[MAX_APPS][ID_LEN];
    int norder;
    char hidden[MAX_APPS][ID_LEN];
    int nhidden;
    int layout_loaded;

    RailItem rail[MAX_RAIL];
    int nrail, rail_connected, rail_index, rail_generation;
    int rail_first; /* first card on screen: the rail scrolls only to keep focus in view */
    char rail_sig[2048];

    int focus, zone; /* ZONE_* */
    int top_index;
    float top_s[TOP_COUNT], top_v[TOP_COUNT];
    Tex *top_tex[TOP_COUNT];
    int top_w[TOP_COUNT], top_h[TOP_COUNT];

    /* Control Center data from /api/system */
    struct { char id[ID_LEN], label[96]; } settings[MAX_SYS];
    int nsettings;

    int rail_stremio, trakt_configured, trakt_connected;
    char home_mode[16]; /* "redirect" (no-flash), "watch" or "off" */
    int sounds_off; /* focus sounds switched off in Settings (saved by the service) */
    Uint32 idle_dim_ms, idle_off_ms; /* OLED idle stages (0 = default) */
    int noflash_active, noflash_available; /* available: LG's home is a web app we can replace */
    Uint32 trakt_poll; /* while showing a Trakt code: next status check */
    int moving;
    float scroll, scroll_v, scroll_target; /* grid y offset */
    float scrolled;              /* 0 at the top row, 1 below it */
    int shelf_cur, shelf_prev;   /* item indices shown in the Top Shelf */
    float shelf_a;               /* crossfade: 0 = prev, 1 = cur */
    int rail_art_cur, rail_art_prev;
    float rail_art_a, rail_shift, rail_shift_v;

    /* App Switcher (Recents, like tvOS): the running apps as big cards over the
     * blurred home screen, most recent first */
    struct {
        int open, n, index, loading, failed, close_all; /* close_all: focus is on Close All */
        float t, tv;                 /* 0 closed .. 1 open */
        int zooming;                 /* OK: the card grows to fill the screen while the app opens */
        float zoom, zoomv;
        Uint32 zoom_at, empty_at;    /* empty_at: the last card went; close soon after */
        struct {
            char id[ID_LEN];
            Tex *name;
            int nw, nh;
            float x, xv;             /* offset from the centre, in cards */
            float s, sv;             /* focus */
            float fly, flyv;         /* closing: it flies up and away */
            int flying;
        } c[MAX_RUNNING];
    } recents;

    struct {
        int open, index, n, tw, th;
        int locked;           /* setup screens: Back doesn't close them */
        Tex *body[SHEET_BODY_LINES];
        int bw[SHEET_BODY_LINES], bh[SHEET_BODY_LINES], nbody;
        int first;            /* first list button on screen: long lists scroll */
        float scroll, scroll_v; /* where the list is drawn from, springing to first */
        float t, tv;          /* morph: 0 = still the button it grew from, 1 = open */
        float fx, fy, fw, fh; /* that button's rect */
        float px, py, pw, ph, pr; /* where the panel was last drawn */
        Tex *title;
        SheetItem it[MAX_SHEET];
    } sheet;

    Tex *clock_tex, *toast_tex;
    int clock_w, clock_h, clock_min, toast_w, toast_h;
    Uint32 toast_until;

    Uint32 ok_down;
    int ok_long;
    int away;           /* we launched another app; reset to the top when we're back */
    int service_ok, wake_tries;
    int setup, setup_tries, elevate_tries, home_asked;
    int reduce_motion, reduce_transparency; /* Settings -> Accessibility (tvOS's names) */
    int ptr_press, ptr_dismiss;             /* the pointer's OK went down on something / outside a sheet */
    /* Which input is in charge. The remote's keys take over from the pointer: then the
     * pointer doesn't move focus, and its click (what OK sends while the cursor shows)
     * presses what the keys focused. Moving the pointer clearly hands it back. */
    int ptr_mode;
    int ptr_ax, ptr_ay; /* where the pointer was when the keys took over, -1: not seen yet */
    Uint32 setup_retry;
    Uint32 next_retry;
    int logged_keys;
    /* OLED protection: idle stages, fades, pixel orbit */
    Uint32 last_input;
    int idle_stage, swallow_up, offscreen, orbit_k; /* offscreen: another app is in front */
    int wl_fd;              /* Wayland socket the idle wait sleeps on, -1 = SDL's own wait */
    Uint32 keys_until;      /* a key went down lately: keep pumping so SDL makes its repeats */
    float dim, black;
    /* motion: the focused element sways briefly and its highlight follows a moving light */
    float sway_t;           /* seconds since focus last moved */
    int focus_key;          /* changes whenever focus moves */
    int tick_armed; /* an arrow key was pressed: a focus change it causes ticks */
    float ptr_x, ptr_y;     /* light direction from the Magic Remote pointer */
    Uint32 ptr_at;          /* when the pointer last moved */
    float light_x, light_y; /* light for focused glass and rims this frame */
    float toast_t, toast_v; /* toast rising in */
    struct {                /* a closed sheet's glass shrinking back into its button */
        int on;
        float t, tv, fx, fy, fw, fh, px, py, pw, ph, pr;
    } ghost;
    Tex *heading_tex, *sub_tex, *sub_connect_tex, *sheet_big;
    int sheet_big_w, sheet_big_h;
    int hw, hh, sw, sh, scw, sch;
} S;

/* ------------------------------------------------------------------ helpers */

static float approach(float *v, float target, float speed, float dt, int *moving)
{
    float d = target - *v;
    if (fabsf(d) < 0.0005f) {
        *v = target;
        return *v;
    }
    *v += d * (1.0f - expf(-speed * dt));
    *moving = 1;
    return *v;
}

/* Spring toward target, parametrised like SwiftUI's: response is the period in
 * seconds, damping 1 settles without overshoot, below 1 bounces a little. */
static void spring(float *x, float *v, float target, float response, float damping, float dt, int *moving)
{
    if (S.reduce_motion && damping < 1) damping = 1; /* Reduce Motion: settle without overshoot */
    float k = 2 * (float)M_PI / response, c = 4 * (float)M_PI * damping / response;
    k *= k;
    int n = (int)ceilf(dt * 240);
    if (n < 1) n = 1;
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        *v += (-k * (*x - target) - c * *v) * h;
        *x += *v * h;
    }
    if (fabsf(*x - target) < 0.0005f && fabsf(*v) < 0.005f) {
        *x = target;
        *v = 0;
        return;
    }
    *moving = 1;
}

static void tex_free(Tex **t) { r_tex_free(t); }

static void log_remote(const char *fmt, ...)
{
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "homeplus: %s\n", msg);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message", msg);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/log", body, 0);
    free(body);
    cJSON_Delete(o);
}

static void toast(const char *text, int ms)
{
    tex_free(&S.toast_tex);
    S.toast_tex = text_render(S.f_medium, 26, text, 1300, &S.toast_w, &S.toast_h);
    S.toast_until = SDL_GetTicks() + ms;
    S.toast_t = S.toast_v = 0;
}

static int toast_on(void) { return S.toast_tex && SDL_GetTicks() < S.toast_until; }

/* appends src to dst (capacity cap), cut short when full */
static void append(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(dst);
    if (n + 1 < cap) snprintf(dst + n, cap - n, "%s", src);
}

static void copy_id(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= ID_LEN) n = ID_LEN - 1;
    memmove(dst, src, n);
    dst[n] = 0;
}

static int parse_hex(const char *s, Uint8 out[3])
{
    unsigned v;
    if (!s || s[0] != '#' || strlen(s) != 7 || sscanf(s + 1, "%6x", &v) != 1) return 0;
    out[0] = (v >> 16) & 255;
    out[1] = (v >> 8) & 255;
    out[2] = v & 255;
    return 1;
}

static const char *jstr(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static double jnum(cJSON *o, const char *k)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static App *item_app(int item)
{
    if (item < 0 || item >= S.nitems || S.items[item] < 0) return NULL;
    return &S.apps[S.items[item]];
}

/* case-insensitive substring test (SDL_strcasestr needs SDL 2.24; the TV has 2.0.14) */
static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int is_stremio(const App *a)
{
    return a && (contains_ci(a->id, "stremio") || contains_ci(a->title, "stremio"));
}

static int is_hidden(const char *id)
{
    for (int i = 0; i < S.nhidden; i++) if (!strcmp(S.hidden[i], id)) return 1;
    return 0;
}

static void app_color(const App *a, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (a && (a->has_col || a->fullbleed)) {
        *r = a->col[0]; *g = a->col[1]; *b = a->col[2];
    } else {
        *r = 70; *g = 70; *b = 82;
    }
}

/* ------------------------------------------------------------------ items & layout */

static void rebuild_items(void)
{
    int used[MAX_APPS] = { 0 };
    S.nitems = 0;
    for (int o = 0; o < S.norder; o++) {
        for (int i = 0; i < S.napps; i++) {
            if (!used[i] && !strcmp(S.apps[i].id, S.order[o])) {
                used[i] = 1;
                if (!is_hidden(S.apps[i].id)) S.items[S.nitems++] = i;
            }
        }
    }
    for (int i = 0; i < S.napps; i++) if (!used[i] && !is_hidden(S.apps[i].id)) S.items[S.nitems++] = i;
    int any_hidden = 0;
    for (int i = 0; i < S.napps; i++) if (is_hidden(S.apps[i].id)) any_hidden = 1;
    if (any_hidden) S.items[S.nitems++] = -1;
    if (S.focus >= S.nitems) S.focus = S.nitems > 0 ? S.nitems - 1 : 0;
}

static void save_layout(void)
{
    /* order = what's on screen, then hidden apps (their position doesn't matter) */
    S.norder = 0;
    for (int i = 0; i < S.nitems; i++) if (S.items[i] >= 0) copy_id(S.order[S.norder++], S.apps[S.items[i]].id);
    for (int i = 0; i < S.napps; i++) if (is_hidden(S.apps[i].id)) copy_id(S.order[S.norder++], S.apps[i].id);

    cJSON *o = cJSON_CreateObject(), *ord = cJSON_AddArrayToObject(o, "order"), *hid = cJSON_AddArrayToObject(o, "hidden");
    for (int i = 0; i < S.norder; i++) cJSON_AddItemToArray(ord, cJSON_CreateString(S.order[i]));
    for (int i = 0; i < S.nhidden; i++) cJSON_AddItemToArray(hid, cJSON_CreateString(S.hidden[i]));
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/layout", body, 0);
    free(body);
    cJSON_Delete(o);
}

static void parse_layout(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    cJSON *v;
    S.norder = S.nhidden = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "order"))
        if (cJSON_IsString(v) && S.norder < MAX_APPS) snprintf(S.order[S.norder++], ID_LEN, "%s", v->valuestring);
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "hidden"))
        if (cJSON_IsString(v) && S.nhidden < MAX_APPS) snprintf(S.hidden[S.nhidden++], ID_LEN, "%s", v->valuestring);
    cJSON_Delete(o);
    S.layout_loaded = 1;
}

static void free_app(App *a)
{
    tex_free(&a->logo_tex);
    tex_free(&a->cover_tex);
    tex_free(&a->icon_tex);
    tex_free(&a->art_tex);
    tex_free(&a->label);
    tex_free(&a->big);
}

static void parse_apps(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    cJSON *arr = cJSON_GetObjectItem(o, "apps"), *v;

    /* skip the rebuild when nothing changed: keeps textures and focus */
    char sig[4096] = "";
    cJSON_ArrayForEach(v, arr) {
        append(sig, sizeof sig, jstr(v, "id"));
        append(sig, sizeof sig, ",");
    }
    if (S.napps && !strcmp(sig, S.apps_sig)) {
        cJSON_Delete(o);
        return;
    }
    snprintf(S.apps_sig, sizeof S.apps_sig, "%s", sig);

    char keep[ID_LEN] = "";
    App *fa = item_app(S.focus);
    if (fa) snprintf(keep, sizeof keep, "%s", fa->id);

    for (int i = 0; i < S.napps; i++) free_app(&S.apps[i]);
    memset(S.apps, 0, sizeof S.apps);
    S.napps = 0;
    S.generation++;
    cJSON_ArrayForEach(v, arr) {
        if (S.napps >= MAX_APPS) break;
        App *a = &S.apps[S.napps++];
        snprintf(a->id, sizeof a->id, "%s", jstr(v, "id"));
        snprintf(a->title, sizeof a->title, "%s", jstr(v, "title"));
        snprintf(a->icon, sizeof a->icon, "%s", jstr(v, "icon"));
        snprintf(a->art, sizeof a->art, "%s", jstr(v, "art"));
        a->has_col = parse_hex(jstr(v, "color"), a->col);
    }
    cJSON_Delete(o);

    rebuild_items();
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        if (a && keep[0] && !strcmp(a->id, keep)) S.focus = i;
    }
    for (int i = 0; i < S.napps; i++) {
        App *a = &S.apps[i];
        if (a->icon[0]) {
            a->icon_state = 1;
            net_get_image(a->icon, TAG_ICON, i, S.generation, 1, ICON_FIT, ICON_FIT);
        }
    }
    S.shelf_cur = S.shelf_prev = -1;
}

/* the big title and meta line, made only for the card on show (~0.4 MB each) */
static void ensure_rail_texts(RailItem *it)
{
    if (it->title_tex) return;
    /* "Season 2 · Episode 5 · 31 min left" / "Episode 5 · ..." (anime) / "Movie · 48 min left" */
    char meta[200] = "";
    if (!strcmp(it->type, "series") && it->season)
        snprintf(meta, sizeof meta, "Season %d  \xC2\xB7  Episode %d", it->season, it->episode);
    else if (it->episode)
        snprintf(meta, sizeof meta, "Episode %d", it->episode);
    else if (!strcmp(it->type, "movie"))
        snprintf(meta, sizeof meta, "Movie");
    if (it->remaining) {
        char left[48];
        snprintf(left, sizeof left, "%s%d min left", meta[0] ? "  \xC2\xB7  " : "", it->remaining);
        append(meta, sizeof meta, left);
    }
    it->title_tex = text_render(S.f_bold, 66, it->name, 1100, &it->tw, &it->th);
    it->meta_tex = text_render(S.f_medium, 27, meta, 1100, &it->mw, &it->mh);
}

/* The first of n items to show, `visible` at a time, so that item i stays in view
 * with `margin` items around it: the list only scrolls when i would get closer
 * to an edge than that (or leave it). */
static int keep_in_view(int first, int i, int visible, int n, int margin)
{
    if (first > i - margin) first = i - margin;
    if (first < i + margin - (visible - 1)) first = i + margin - (visible - 1);
    if (first > n - visible) first = n - visible;
    return first < 0 ? 0 : first;
}

/* Focuses Continue Watching card i. The cards stay over the same columns as focus moves. */
static void rail_focus(int i)
{
    if (S.nrail <= 0) {
        S.rail_index = S.rail_first = 0;
        return;
    }
    S.rail_index = i < 0 ? 0 : i >= S.nrail ? S.nrail - 1 : i;
    S.rail_first = keep_in_view(S.rail_first, S.rail_index, RAIL_VISIBLE, S.nrail, 0);
}

static void parse_stremio(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return;
    S.rail_connected = cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"));
    S.rail_stremio = cJSON_IsTrue(cJSON_GetObjectItem(o, "stremio"));
    cJSON *tr = cJSON_GetObjectItem(o, "trakt");
    S.trakt_configured = cJSON_IsTrue(cJSON_GetObjectItem(tr, "configured"));
    S.trakt_connected = cJSON_IsTrue(cJSON_GetObjectItem(tr, "connected"));
    {
        char heading[96];
        snprintf(heading, sizeof heading, "Continue Watching%s%s", S.rail_stremio ? " \xC2\xB7 Stremio" : "",
                 S.trakt_connected ? " \xC2\xB7 Trakt" : "");
        tex_free(&S.heading_tex);
        S.heading_tex = text_render(S.f_semi, 24, heading, 0, &S.hw, &S.hh);
    }
    cJSON *arr = cJSON_GetObjectItem(o, "items"), *v;
    char sig[2048] = "";
    cJSON_ArrayForEach(v, arr) {
        char part[96];
        snprintf(part, sizeof part, "%s:%.3f,", jstr(v, "id"), jnum(v, "progress"));
        append(sig, sizeof sig, part);
    }
    if (!strcmp(sig, S.rail_sig) && S.nrail) {
        cJSON_Delete(o);
        return;
    }
    snprintf(S.rail_sig, sizeof S.rail_sig, "%s", sig);

    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        tex_free(&it->thumb_tex); tex_free(&it->bg_tex); tex_free(&it->name_tex); tex_free(&it->title_tex); tex_free(&it->meta_tex);
    }
    memset(S.rail, 0, sizeof S.rail);
    S.nrail = 0;
    S.rail_generation++;
    cJSON_ArrayForEach(v, arr) {
        if (S.nrail >= MAX_RAIL) break;
        RailItem *it = &S.rail[S.nrail];
        snprintf(it->id, sizeof it->id, "%s", jstr(v, "id"));
        snprintf(it->type, sizeof it->type, "%s", jstr(v, "type"));
        snprintf(it->name, sizeof it->name, "%s", jstr(v, "name"));
        snprintf(it->bg, sizeof it->bg, "%s", jstr(v, "background"));
        snprintf(it->thumb, sizeof it->thumb, "%s", jstr(v, "thumbnail"));
        it->progress = (float)jnum(v, "progress");
        it->season = (int)jnum(v, "season");
        it->episode = (int)jnum(v, "episode");
        it->remaining = (int)jnum(v, "remainingMin");
        it->name_tex = text_render(S.f_semi, 24, it->name, RAIL_CARD_W - 32, &it->nw, &it->nh);
        if (it->thumb[0]) {
            it->thumb_state = 1;
            net_get_image(it->thumb, TAG_RAIL_THUMB, S.nrail, S.rail_generation, 0, THUMB_FIT_W, THUMB_FIT_H);
        }
        S.nrail++;
    }
    cJSON_Delete(o);
    rail_focus(S.rail_index < S.nrail ? S.rail_index : 0);
    S.rail_art_cur = S.rail_art_prev = -1;
    if (!S.nrail && S.zone == 1) S.zone = 0;
}

static void parse_system(const char *json)
{
    cJSON *o = cJSON_Parse(json), *v;
    if (!o) return;
    S.nsettings = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(o, "settings")) {
        if (S.nsettings >= MAX_SYS) break;
        copy_id(S.settings[S.nsettings].id, jstr(v, "id"));
        snprintf(S.settings[S.nsettings].label, sizeof S.settings[0].label, "%s", jstr(v, "label"));
        S.nsettings++;
    }
    cJSON_Delete(o);
}

static void request_data(void)
{
    if (!S.layout_loaded) net_get_json("/api/layout", TAG_LAYOUT, 0, 0);
    net_get_json("/api/apps", TAG_APPS, 0, 0);
    net_get_json("/api/continue", TAG_STREMIO, 0, 0);
    net_get_json("/api/system", TAG_SYSTEM, 0, 0);
    net_get_json("/api/home", TAG_HOME, 0, 0);
    net_get_json("/api/prefs", TAG_PREFS, 0, 0);
}

static void sheet_open(const char *title);
static void on_status(Result *r);
static void sheet_body(const char *text);
static void on_elevate(Result *r);
static void open_top(int which);
static void sw_fill(const char *json);
static void sw_reset(void);
static void sheet_close(void);
static void sheet_dismiss(void);
static void sheet_add(const char *label, Action act, int arg, int destructive);
static int sheet_footer(void);
static int sheet_rows(int nfoot);
static float sheet_head(void);

/* ------------------------------------------------------------------ uniform tiles */

/* Like tvOS app icons, every tile is the icon's own background colour from edge
 * to edge with its logo centred at the same visual size. LG icons are small
 * squares with their own background, so: find that colour on the border, key
 * it out, trim to the logo and size it to a common target. Icons without a
 * solid background keep their whole picture, at one size, with round corners;
 * logo-only icons (transparent) get one graphite tile (light if the logo is dark). */
static float smooth01(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* A cover for apps without artwork: the icon stretched over a small 16:9
 * frame (over the app's colour where it's transparent), blurred into soft
 * colour fields, darkened towards the edges. Drawn full screen, the GPU's
 * filtering smooths it further. */
static void make_cover(App *a, const unsigned char *px, int w, int h)
{
    enum { CW = 160, CH = 90, PASSES = 3, R = 7 };
    float *f = malloc(sizeof(float) * CW * CH * 3), *t = malloc(sizeof(float) * CW * CH * 3);
    unsigned char *out = malloc((size_t)CW * CH * 4);
    if (!f || !t || !out) {
        free(f);
        free(t);
        free(out);
        return;
    }
    float sc = fmaxf((float)CW / w, (float)CH / h);
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            int sx = (int)((x + 0.5f - CW / 2.0f) / sc + w / 2.0f), sy = (int)((y + 0.5f - CH / 2.0f) / sc + h / 2.0f);
            sx = sx < 0 ? 0 : sx >= w ? w - 1 : sx;
            sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy;
            const unsigned char *p = px + ((size_t)sy * w + sx) * 4;
            float al = p[3] / 255.0f;
            for (int c = 0; c < 3; c++) f[(y * CW + x) * 3 + c] = p[c] * al + a->col[c] * (1 - al);
        }
    }
    for (int pass = 0; pass < PASSES; pass++) { /* box blur, horizontal then vertical */
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -R; k <= R; k++) {
                        int xx = x + k < 0 ? 0 : x + k >= CW ? CW - 1 : x + k;
                        sum += f[(y * CW + xx) * 3 + c];
                    }
                    t[(y * CW + x) * 3 + c] = sum / (2 * R + 1);
                }
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++)
                for (int c = 0; c < 3; c++) {
                    float sum = 0;
                    for (int k = -R; k <= R; k++) {
                        int yy = y + k < 0 ? 0 : y + k >= CH ? CH - 1 : y + k;
                        sum += t[(yy * CW + x) * 3 + c];
                    }
                    f[(y * CW + x) * 3 + c] = sum / (2 * R + 1);
                }
    }
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            float dx = (x - CW / 2.0f) / (CW / 2.0f), dy = (y - CH / 2.0f) / (CH / 2.0f);
            /* calm: halfway to grey and dark, so a focused app doesn't flood the room
             * (HyperHDR mirrors the screen into the lights); darker towards the edges */
            float shade = 0.45f * (1 - 0.35f * fminf(1, (dx * dx + dy * dy) * 0.6f));
            const float *pc = &f[(y * CW + x) * 3];
            float luma = 0.299f * pc[0] + 0.587f * pc[1] + 0.114f * pc[2];
            unsigned char *o = out + ((size_t)y * CW + x) * 4;
            for (int c = 0; c < 3; c++) {
                float v = (pc[c] + (luma - pc[c]) * 0.55f) * shade;
                o[c] = (unsigned char)(v > 255 ? 255 : v);
            }
            o[3] = 255;
        }
    }
    tex_free(&a->cover_tex);
    a->cover_tex = r_tex(out, CW, CH);
    free(f);
    free(t);
    free(out);
}

static void make_logo(App *a, const unsigned char *px, int w, int h)
{
    /* background: the first opaque pixel (2 px in, past the anti-aliased edge)
     * on 24 lines inward from each side */
    enum { LINES = 24 };
    unsigned char samp[4 * LINES][3];
    int n = 0;
    double m[3] = { 0 };
    for (int side = 0; side < 4; side++) {
        for (int k = 1; k <= LINES; k++) {
            int along = side < 2 ? w : h, depth = side < 2 ? h : w, t = k * along / (LINES + 1);
            for (int d = 0; d < depth / 3; d++) {
                int dd = d;
                int x = side == 0 || side == 1 ? t : side == 2 ? dd : w - 1 - dd;
                int y = side == 0 ? dd : side == 1 ? h - 1 - dd : t;
                if (px[((size_t)y * w + x) * 4 + 3] <= 200) continue;
                dd = d + 2 < depth ? d + 2 : d;
                x = side == 0 || side == 1 ? t : side == 2 ? dd : w - 1 - dd;
                y = side == 0 ? dd : side == 1 ? h - 1 - dd : t;
                const unsigned char *p = px + ((size_t)y * w + x) * 4;
                memcpy(samp[n++], p, 3);
                m[0] += p[0];
                m[1] += p[1];
                m[2] += p[2];
                break;
            }
        }
    }
    enum { SOLID, PICTURE, CLEAR } mode = CLEAR;
    if (n >= 4 * LINES * 3 / 4) {
        double dev = 0;
        for (int c = 0; c < 3; c++) m[c] /= n;
        for (int i = 0; i < n; i++) dev += fabs(samp[i][0] - m[0]) + fabs(samp[i][1] - m[1]) + fabs(samp[i][2] - m[2]);
        mode = dev / n < 24 ? SOLID : PICTURE;
    }

    /* keyed copy: background pixels fade to transparent; then the logo's bounding box */
    unsigned char *k = malloc((size_t)w * h * 4);
    if (!k) return;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    double lum = 0, lum_n = 0;
    for (int i = 0; i < w * h; i++) {
        const unsigned char *p = px + (size_t)i * 4;
        unsigned char *q = k + (size_t)i * 4;
        float alpha = p[3];
        if (mode == SOLID) {
            float dist = fabsf(p[0] - (float)m[0]) + fabsf(p[1] - (float)m[1]) + fabsf(p[2] - (float)m[2]);
            alpha *= smooth01(24, 72, dist);
        }
        memcpy(q, p, 3);
        q[3] = (unsigned char)alpha;
        if (mode == PICTURE ? p[3] > 40 : alpha > 40) {
            int x = i % w, y = i / w;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            lum += (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255;
            lum_n++;
        }
    }
    if (x1 < x0) { /* nothing but background: keep the whole icon */
        x0 = y0 = 0;
        x1 = w - 1;
        y1 = h - 1;
        mode = PICTURE;
    }
    if (mode == PICTURE) memcpy(k, px, (size_t)w * h * 4);
    double ink = 0; /* how much logo there is: sizes logos by what you see, not their box */
    x0 = x0 > 2 ? x0 - 2 : 0;
    y0 = y0 > 2 ? y0 - 2 : 0;
    x1 = x1 < w - 3 ? x1 + 2 : w - 1;
    y1 = y1 < h - 3 ? y1 + 2 : h - 1;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    unsigned char *crop = malloc((size_t)cw * ch * 4);
    if (!crop) {
        free(k);
        return;
    }
    for (int y = 0; y < ch; y++) memcpy(crop + (size_t)y * cw * 4, k + ((size_t)(y0 + y) * w + x0) * 4, (size_t)cw * 4);
    for (int i = 0; i < cw * ch; i++) ink += crop[(size_t)i * 4 + 3] / 255.0;
    free(k);
    tex_free(&a->logo_tex);
    a->logo_tex = r_tex(crop, cw, ch);
    free(crop);

    /* one visual size: about the same area for every logo, within a box */
    if (mode == PICTURE) {
        float side = TILE_H * 0.62f;
        a->logo_w = a->logo_h = side;
        a->logo_r = TILE_R * side / TILE_H; /* same corner-to-size ratio as the tile */
    } else {
        /* the same amount of logo on every tile (a square of about a third of the
         * tile's height, filled), but never wider or taller than the tile allows */
        float sc = TILE_H * 0.33f / sqrtf((float)(ink > 1 ? ink : 1));
        sc = fminf(sc, fminf(TILE_W * 0.62f / cw, TILE_H * 0.56f / ch));
        a->logo_w = cw * sc;
        a->logo_h = ch * sc;
        a->logo_r = 0;
    }
    /* the tile: the icon's background, a darkened average behind pictures, graphite behind bare logos */
    if (mode == SOLID) {
        for (int c = 0; c < 3; c++) a->col[c] = (Uint8)(m[c] + 0.5);
    } else if (mode == PICTURE) {
        for (int c = 0; c < 3; c++) a->col[c] = (Uint8)(n ? m[c] * 0.55 : 40);
    } else {
        int light = lum_n > 0 && lum / lum_n < 0.25;
        a->col[0] = a->col[1] = light ? 236 : 40;
        a->col[2] = light ? 240 : 46;
    }
    a->has_col = 1;
    a->fullbleed = mode == SOLID;
    make_cover(a, px, w, h);
}

/* ------------------------------------------------------------------ results */

static void handle_result(Result *r)
{
    switch (r->tag) {
    case TAG_WAKE:
        break;
    case TAG_LAYOUT:
        if (r->status == 200) parse_layout(r->body);
        break;
    case TAG_STATUS:
        on_status(r);
        break;
    case TAG_ELEVATE:
        on_elevate(r);
        break;
    case TAG_RESTART:
        break; /* the service exits; setup_retry wakes it and checks again */
    case TAG_UNINSTALL:
        if (r->status == 200) {
            sheet_open("Home+ Is Being Removed");
            sheet_body("Press Home to go back to LG\xE2\x80\x99s home screen.");
            S.sheet.locked = 1;
        } else {
            S.setup = SETUP_DONE;
            sheet_close();
            toast("Couldn\xE2\x80\x99t remove Home+", 4000);
        }
        break;
    case TAG_APPS:
        if (r->status == 200) {
            S.service_ok = 1;
            parse_apps(r->body);
        } else {
            S.service_ok = 0;
            /* service not up yet: start it, then retry */
            if (S.wake_tries++ < 10) {
                net_wake_service();
                S.next_retry = SDL_GetTicks() + 2000;
            }
            toast(S.wake_tries < 10 ? "Starting the Home+ service\xE2\x80\xA6" : "Home+ service isn't responding. Is the Home+ Service package installed?", 600000);
        }
        if (S.service_ok && S.toast_until > SDL_GetTicks() + 100000) S.toast_until = 0;
        break;
    case TAG_STREMIO:
        if (r->status == 200) parse_stremio(r->body);
        break;
    case TAG_SYSTEM:
        if (r->status == 200) parse_system(r->body);
        break;
    case TAG_RUNNING:
        if (r->status == 200) sw_fill(r->body);
        else if (S.recents.loading) S.recents.loading = 0, S.recents.failed = 1;
        break;
    case TAG_CLOSED:
        if (r->status != 200) toast("Couldn\xE2\x80\x99t close that app", 2500);
        break;
    case TAG_HOME:
    case TAG_HOME_SET:
        if (r->status == 200) {
            cJSON *o = cJSON_Parse(r->body);
            if (o) {
                snprintf(S.home_mode, sizeof S.home_mode, "%s", jstr(o, "mode"));
                S.noflash_active = cJSON_IsTrue(cJSON_GetObjectItem(o, "noFlashActive"));
                S.noflash_available = cJSON_IsTrue(cJSON_GetObjectItem(o, "noFlashAvailable"));
                if (!strcmp(S.home_mode, "redirect") && !S.noflash_active && r->tag == TAG_HOME_SET)
                    toast("No-flash mode isn\xE2\x80\x99t available on this TV; using standard mode.", 5000);
                cJSON_Delete(o);
            }
        }
        break;
    case TAG_PREFS:
        if (r->status == 200) {
            cJSON *o = cJSON_Parse(r->body);
            S.sounds_off = o && cJSON_IsFalse(cJSON_GetObjectItem(o, "sounds"));
            snd_enable(!S.sounds_off);
            S.reduce_motion = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "reduceMotion"));
            S.reduce_transparency = o && cJSON_IsTrue(cJSON_GetObjectItem(o, "reduceTransparency"));
            double dm = jnum(o, "dimMinutes"), om = jnum(o, "offMinutes");
            S.idle_dim_ms = dm > 0 ? (Uint32)(dm * 60000) : 0;
            S.idle_off_ms = om > 0 ? (Uint32)(om * 60000) : 0;
            cJSON_Delete(o);
        }
        break;
    case TAG_TRAKT_LINK:
    case TAG_TRAKT_STATUS: {
        cJSON *o = r->status == 200 ? cJSON_Parse(r->body) : NULL;
        if (!o) {
            if (r->tag == TAG_TRAKT_LINK) toast("Trakt isn\xE2\x80\x99t set up yet \xE2\x80\x94 run traktSetup from your Mac (see README).", 7000);
            S.trakt_poll = 0;
            break;
        }
        int connected = cJSON_IsTrue(cJSON_GetObjectItem(o, "connected"));
        cJSON *pend = cJSON_GetObjectItem(o, "pending");
        if (connected) {
            S.trakt_poll = 0;
            S.trakt_connected = 1;
            if (S.sheet.open && S.sheet_big) sheet_dismiss();
            toast("Trakt connected", 3000);
            net_get_json("/api/continue", TAG_STREMIO, 0, 0);
        } else if (cJSON_IsObject(pend)) {
            if (r->tag == TAG_TRAKT_LINK) {
                /* show the code big, like tvOS sign-in screens */
                char title[160];
                snprintf(title, sizeof title, "On your phone, go to %s and enter", jstr(pend, "url"));
                sheet_open(title);
                tex_free(&S.sheet_big);
                S.sheet_big = text_render(S.f_bold, 96, jstr(pend, "code"), 0, &S.sheet_big_w, &S.sheet_big_h);
                sheet_add("Cancel", ACT_CANCEL, 0, 0);
            }
            S.trakt_poll = SDL_GetTicks() + 3000;
        } else {
            S.trakt_poll = 0;
            if (r->tag == TAG_TRAKT_LINK) toast("Trakt sign-in didn\xE2\x80\x99t start. Check the Client ID/Secret (see README).", 6000);
        }
        cJSON_Delete(o);
        break;
    }
    case TAG_ICON:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            if (r->rgba) {
                make_logo(a, r->rgba, r->w, r->h); /* already shrunk to ICON_FIT while decoding */
                a->icon_state = 2;
            } else {
                a->icon_state = 3;
            }
        }
        break;
    case TAG_ART:
        if (r->generation == S.generation && r->index < S.napps) {
            App *a = &S.apps[r->index];
            a->art_tex = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            a->art_state = r->rgba ? 2 : 3;
            /* the shelf is showing this app: fade its artwork in over the cover */
            if (a->art_tex && S.shelf_cur >= 0 && S.shelf_cur < S.nitems && S.items[S.shelf_cur] == r->index) {
                S.shelf_prev = S.shelf_cur;
                S.shelf_a = 0;
            }
        }
        break;
    case TAG_RAIL_THUMB:
    case TAG_RAIL_BG:
        if (r->generation == S.rail_generation && r->index < S.nrail) {
            RailItem *it = &S.rail[r->index];
            Tex *t = r->rgba ? r_tex(r->rgba, r->w, r->h) : NULL;
            if (r->tag == TAG_RAIL_THUMB) { it->thumb_tex = t; it->thumb_state = t ? 2 : 3; }
            else { it->bg_tex = t; it->bg_state = t ? 2 : 3; }
        }
        break;
    case TAG_LAUNCH:
        if (r->status != 200 || (r->body && strstr(r->body, "\"returnValue\":false"))) {
            S.away = 0;
            S.launching = 0;
            if (S.recents.zooming) sw_reset();
            toast("Couldn\xE2\x80\x99t open that app.", 3500);
        }
        break;
    }
}

/* ------------------------------------------------------------------ actions */

static int rail_active(void)
{
    return S.focus < COLS && is_stremio(item_app(S.focus)) && S.nrail > 0;
}

/* hint: optional message the system shows over the app once it has opened */
static void launch(const char *id, const char *hint)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    if (hint && hint[0]) cJSON_AddStringToObject(o, "toast", hint);
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/launch", body, TAG_LAUNCH);
    free(body);
    cJSON_Delete(o);
    S.away = 1;
}

static void sheet_close(void)
{
    tex_free(&S.sheet.title);
    tex_free(&S.sheet_big);
    for (int i = 0; i < S.sheet.nbody; i++) tex_free(&S.sheet.body[i]);
    S.sheet.nbody = 0;
    S.sheet.locked = 0;
    S.trakt_poll = 0;
    for (int i = 0; i < S.sheet.n; i++) tex_free(&S.sheet.it[i].tex);
    S.sheet.open = 0;
    S.sheet.n = 0;
}

/* Tiles, Continue Watching cards and the top-bar buttons all line up with the
 * dock's columns, so Up and Down keep the column. */
static int column_at(float x)
{
    int c = (int)floorf((x - LEFT + GAP_X / 2.0f) / (TILE_W + GAP_X));
    return c < 0 ? 0 : c >= COLS ? COLS - 1 : c;
}

/* left edge of top-bar button i; the buttons sit left of the clock */
static float top_button_x(int i)
{
    return RIGHT - (S.clock_w + 48) - (TOP_COUNT - i) * (BAR + BAR_GAP);
}

static int top_column(int i) { return column_at(top_button_x(i) + BAR / 2.0f); }

/* the top-bar button over a column; Settings when both are as near */
static int top_nearest(int col)
{
    int best = TOP_COUNT - 1;
    for (int i = TOP_COUNT - 2; i >= 0; i--)
        if (abs(top_column(i) - col) < abs(top_column(best) - col)) best = i;
    return best;
}

/* the dock tile in a column, or the last tile when the row is shorter */
static int dock_tile(int col)
{
    return col < S.nitems ? col : S.nitems > 0 ? S.nitems - 1 : 0;
}

/* Where Down from the top bar lands: the Continue Watching card under the
 * button, else the dock tile under it (-1 for the other). */
static int top_down_card(void)
{
    int col = top_column(S.top_index);
    return rail_active() && S.rail_first + col < S.nrail ? S.rail_first + col : -1;
}

static int top_down_tile(void)
{
    return top_down_card() < 0 ? dock_tile(top_column(S.top_index)) : -1;
}

/* rect of the focused element: where a sheet grows from */
static void focus_rect(float *x, float *y, float *w, float *h)
{
    if (S.zone == ZONE_TOP) {
        *x = top_button_x(S.top_index);
        *y = SAFE_Y;
        *w = *h = BAR;
    } else if (S.zone == ZONE_GRID && S.focus < S.nitems) {
        *x = LEFT + (S.focus % COLS) * (TILE_W + GAP_X);
        *y = GRID_TOP + (S.focus / COLS) * ROW_H + S.scroll;
        *w = TILE_W;
        *h = TILE_H;
    } else {
        *w = 240;
        *h = 80;
        *x = (W - *w) / 2;
        *y = (H - *h) / 2;
    }
}

/* Closes the sheet the way it opened: its glass shrinks back into the button. */
static void sheet_dismiss(void)
{
    if (S.sheet.open && S.sheet.pw > 0 && !S.reduce_motion) {
        S.ghost.on = 1;
        S.ghost.t = S.sheet.t > 1 ? 1 : S.sheet.t;
        S.ghost.tv = 0;
        S.ghost.fx = S.sheet.fx; S.ghost.fy = S.sheet.fy; S.ghost.fw = S.sheet.fw; S.ghost.fh = S.sheet.fh;
        S.ghost.px = S.sheet.px; S.ghost.py = S.sheet.py; S.ghost.pw = S.sheet.pw; S.ghost.ph = S.sheet.ph;
        S.ghost.pr = S.sheet.pr;
    }
    sheet_close();
}

static void sheet_open(const char *title)
{
    int was_open = S.sheet.open || S.ghost.on; /* replacing a sheet (even one just closing): no morph */
    S.ghost.on = 0;
    sheet_close();
    if (was_open) {
        S.sheet.t = 1; /* swapping one sheet for another: no morph */
        S.sheet.tv = 0;
    } else {
        S.sheet.t = S.sheet.tv = 0;
        focus_rect(&S.sheet.fx, &S.sheet.fy, &S.sheet.fw, &S.sheet.fh);
    }
    if (S.reduce_motion) S.sheet.t = 1, S.sheet.tv = 0; /* appears in place, no growing */
    S.sheet.open = 1;
    S.sheet.index = S.sheet.first = 0;
    S.sheet.scroll = S.sheet.scroll_v = 0;
    S.sheet.title = text_render(S.f_semi, 30, title, 900, &S.sheet.tw, &S.sheet.th);
}

static void sheet_add(const char *label, Action act, int arg, int destructive)
{
    if (S.sheet.n >= MAX_SHEET) return;
    SheetItem *it = &S.sheet.it[S.sheet.n++];
    it->act = act;
    it->arg = arg;
    it->destructive = destructive;
    it->tex = text_render(S.f_medium, 31, label, 600, &it->w, &it->h);
}

/* A few lines of explanation under the sheet's title; lines are split on '\n'. */
static void sheet_body(const char *text)
{
    char line[300];
    const char *p = text;
    while (*p && S.sheet.nbody < SHEET_BODY_LINES) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n);
        line[n] = 0;
        int i = S.sheet.nbody++;
        S.sheet.body[i] = text_render(S.f_medium, 26, line, 900, &S.sheet.bw[i], &S.sheet.bh[i]);
        p = e ? e + 1 : p + n;
    }
}

static void sheet_add_id(const char *label, const char *id)
{
    sheet_add(label, ACT_LAUNCH_ID, 0, 0);
    copy_id(S.sheet.it[S.sheet.n - 1].id, id);
}

static void close_apps(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    if (id) cJSON_AddStringToObject(o, "id", id);
    else cJSON_AddTrueToObject(o, "all");
    char *body = cJSON_PrintUnformatted(o);
    net_post("/api/close", body, TAG_CLOSED);
    free(body);
    cJSON_Delete(o);
}

static App *app_by_id(const char *id)
{
    for (int i = 0; i < S.napps; i++)
        if (!strcmp(S.apps[i].id, id)) return &S.apps[i];
    return NULL;
}

static void sw_clear(void)
{
    for (int i = 0; i < S.recents.n; i++) tex_free(&S.recents.c[i].name);
    S.recents.n = S.recents.index = S.recents.close_all = S.recents.zooming = 0;
    S.recents.zoom = S.recents.zoomv = 0;
    S.recents.empty_at = 0;
}

/* gone at once (Home+ went to the back, or came home) */
static void sw_reset(void)
{
    sw_clear();
    S.recents.open = S.recents.loading = S.recents.failed = 0;
    S.recents.t = S.recents.tv = 0;
}

/* Recents opens at once over the blurred home screen; the running apps arrive a
 * moment later, so keys pressed meanwhile never reach the grid behind. */
static void sw_open(void)
{
    sheet_close();
    sw_clear();
    S.recents.open = S.recents.loading = 1;
    S.recents.failed = 0;
    net_get_json("/api/running", TAG_RUNNING, 0, 0);
}

static void sw_fill(const char *json)
{
    if (!S.recents.open || !S.recents.loading) return; /* closed meanwhile */
    S.recents.loading = 0;
    cJSON *o = cJSON_Parse(json), *v;
    cJSON *list = o ? cJSON_GetObjectItem(o, "apps") : NULL;
    cJSON_ArrayForEach(v, list) {
        if (S.recents.n >= MAX_RUNNING) break;
        int i = S.recents.n++;
        memset(&S.recents.c[i], 0, sizeof S.recents.c[i]);
        copy_id(S.recents.c[i].id, jstr(v, "id"));
        S.recents.c[i].name = text_render(S.f_semi, 30, jstr(v, "title"), SW_W, &S.recents.c[i].nw, &S.recents.c[i].nh);
        S.recents.c[i].x = i + 0.5f; /* they slide in from the right */
    }
    cJSON_Delete(o);
}

static void sw_ok(void)
{
    if (S.recents.zooming || !S.recents.n) return;
    if (S.recents.close_all) {
        close_apps(NULL);
        for (int i = 0; i < S.recents.n; i++) {
            S.recents.c[i].flying = 1;
            S.recents.c[i].fly = -0.12f * abs(i - S.recents.index); /* one after another, outwards */
        }
        S.recents.close_all = 0;
        return;
    }
    if (S.recents.c[S.recents.index].flying) return;
    S.recents.zooming = 1; /* the card grows to fill the screen while the app opens */
    S.recents.zoom_at = SDL_GetTicks();
    launch(S.recents.c[S.recents.index].id, NULL);
}

static void sw_update(float dt, int *anim)
{
    spring(&S.recents.t, &S.recents.tv, S.recents.open ? 1.0f : 0.0f, S.recents.open ? 0.42f : 0.3f, S.recents.open ? 0.86f : 1.0f, dt, anim);
    if (!S.recents.open && S.recents.t < 0.004f) {
        if (S.recents.n) sw_clear();
        return;
    }
    for (int i = 0; i < S.recents.n; i++) {
        spring(&S.recents.c[i].x, &S.recents.c[i].xv, (float)(i - S.recents.index), 0.4f, 0.86f, dt, anim);
        spring(&S.recents.c[i].s, &S.recents.c[i].sv, i == S.recents.index && !S.recents.close_all ? 1.0f : 0.0f, 0.35f, 0.75f, dt, anim);
        if (S.recents.c[i].flying) spring(&S.recents.c[i].fly, &S.recents.c[i].flyv, 1.2f, 0.35f, 1.0f, dt, anim);
    }
    /* a card that flew off is gone; the others close the gap on their springs */
    for (int i = 0; i < S.recents.n; i++) {
        if (S.recents.c[i].fly < 1.0f) continue;
        tex_free(&S.recents.c[i].name);
        memmove(&S.recents.c[i], &S.recents.c[i + 1], (size_t)(S.recents.n - i - 1) * sizeof S.recents.c[0]);
        S.recents.n--;
        if (S.recents.index > i) S.recents.index--;
        if (S.recents.index >= S.recents.n) S.recents.index = S.recents.n > 0 ? S.recents.n - 1 : 0;
        if (!S.recents.n) S.recents.empty_at = SDL_GetTicks();
        i--;
        *anim = 1;
    }
    Uint32 now = SDL_GetTicks();
    if (S.recents.open && S.recents.empty_at && now - S.recents.empty_at > 500) S.recents.open = 0; /* nothing left to show */
    if (S.recents.zooming) {
        spring(&S.recents.zoom, &S.recents.zoomv, 1.0f, 0.45f, 0.95f, dt, anim);
        if (now - S.recents.zoom_at > 10000) { /* the app never came up */
            sw_reset();
            toast("That app is taking a while to open", 3500);
        }
    }
    if (S.recents.loading) *anim = 1; /* "Loading…" fades in after a moment */
}

/* Control Center: TV settings + accounts */
/* ------------------------------------------------------------------ first run */

static void setup_check(void) { net_get_json("/api/status", TAG_STATUS, 0, 0); }

/* The screen for the current setup step: plain words, no SSH or service names. */
static void setup_screen(void)
{
    const char *title, *body;
    switch (S.setup) {
    case SETUP_NO_HBC:
        title = "Homebrew Channel Needed";
        body = "Home+ runs on rooted LG TVs with the Homebrew Channel installed.\nInstall it, then try again.";
        break;
    case SETUP_DENIED:
        title = "Couldn\xE2\x80\x99t Get Permission";
        body = "The Homebrew Channel didn\xE2\x80\x99t give Home+ the access it needs.\nCheck that your TV is rooted and the Homebrew Channel is up to date.";
        break;
    case SETUP_FAILED:
        title = "Home+ Couldn\xE2\x80\x99t Start";
        body = "Part of Home+ that runs in the background didn\xE2\x80\x99t respond.\nRestarting the TV usually fixes this.";
        break;
    case SETUP_REMOVING:
        title = "Removing Home+\xE2\x80\xA6";
        body = "Turning off the Home button takeover and clearing Home+\xE2\x80\x99s settings.";
        break;
    default:
        title = "Setting Up Home+";
        body = "Getting Home+ ready. The first time takes a few seconds.";
    }
    sheet_open(title);
    sheet_body(body);
    S.sheet.locked = 1;
    if (S.setup == SETUP_NO_HBC || S.setup == SETUP_DENIED || S.setup == SETUP_FAILED)
        sheet_add("Try Again", ACT_SETUP_RETRY, 0, 0);
}

static void setup_set(int state)
{
    int changed = state != S.setup;
    S.setup = state;
    if (state == SETUP_DONE) {
        if (S.sheet.open && S.sheet.locked) sheet_dismiss();
        return;
    }
    /* "Setting up" steps share one screen: only redraw it when the words change */
    int working = state == SETUP_STARTING || state == SETUP_ELEVATING || state == SETUP_RESTARTING;
    if (changed && !(working && S.sheet.open && S.sheet.locked && S.sheet.n == 0)) setup_screen();
}

static void ask_home_screen(void)
{
    sheet_open("Use Home+ as Your Home Screen?");
    sheet_body("The Home button will open Home+ instead of LG\xE2\x80\x99s home screen.\nYou can change this any time in Settings.");
    sheet_add("Use Home+", ACT_HOME_YES, 0, 0);
    sheet_add("Not Now", ACT_HOME_NO, 0, 0);
}

static void set_home_mode(const char *mode, int asked)
{
    char body[64];
    snprintf(body, sizeof body, "{\"mode\":\"%s\"%s}", mode, asked ? ",\"asked\":true" : "");
    net_post("/api/home", body, TAG_HOME_SET);
    snprintf(S.home_mode, sizeof S.home_mode, "%s", mode);
}

static void on_status(Result *r)
{
    if (S.setup == SETUP_DONE || S.setup == SETUP_REMOVING) return;
    cJSON *o = r->status == 200 ? cJSON_Parse(r->body) : NULL;
    if (!o) {
        /* nothing answered: start the service (luna starts it on demand) and look again */
        if (S.setup_tries++ < 12) {
            net_wake_service();
            S.setup_retry = SDL_GetTicks() + 2000;
            if (S.setup != SETUP_RESTARTING) setup_set(SETUP_STARTING);
        } else {
            setup_set(SETUP_FAILED);
        }
        return;
    }
    int elevated = cJSON_IsTrue(cJSON_GetObjectItem(o, "elevated"));
    S.home_asked = cJSON_IsTrue(cJSON_GetObjectItem(o, "homeAsked"));
    cJSON_Delete(o);
    if (elevated) {
        setup_set(SETUP_DONE);
        request_data();
        if (!S.home_asked) ask_home_screen();
        return;
    }
    if (S.setup == SETUP_RESTARTING && S.setup_tries++ < 6) {
        S.setup_retry = SDL_GetTicks() + 1500; /* still the old, unelevated process: give it a moment */
        return;
    }
    if (S.elevate_tries++ < 2) {
        setup_set(SETUP_ELEVATING);
        net_luna(HBC_ELEVATE, "{\"id\":\"" SERVICE_ID "\"}", TAG_ELEVATE);
    } else {
        setup_set(SETUP_DENIED);
    }
}

static void on_elevate(Result *r)
{
    if (r->status == 200) {
        /* elevated: the running process is still unprivileged, so restart it */
        S.setup_tries = 0;
        setup_set(SETUP_RESTARTING);
        net_post("/api/restart", "{}", TAG_RESTART);
        S.setup_retry = SDL_GetTicks() + 1500;
        return;
    }
    const char *out = r->body ? r->body : "";
    fprintf(stderr, "homeplus: elevateService: %s\n", out);
    if (contains_ci(out, "does not exist") || contains_ci(out, "unknown service") || contains_ci(out, "not found") ||
        contains_ci(out, "luna-send: not found") || !*out)
        setup_set(SETUP_NO_HBC);
    else
        setup_set(SETUP_DENIED);
}

static void setup_retry_now(void)
{
    S.setup_tries = S.elevate_tries = 0;
    S.setup = SETUP_CHECK;
    setup_screen();
    setup_check();
}

static int stremio_installed(void)
{
    for (int i = 0; i < S.napps; i++)
        if (is_stremio(&S.apps[i])) return 1;
    return 0;
}

static void open_settings_sheet(void)
{
    sheet_open("Settings");
    for (int i = 0; i < S.nsettings; i++) sheet_add_id(S.settings[i].label, S.settings[i].id);
    /* no-flash needs LG's home to be a web app; on TVs where it's native (the C3)
     * Home+ always uses standard mode, so there's nothing to switch */
    int home_on = S.home_mode[0] && strcmp(S.home_mode, "off");
    sheet_add(home_on ? "Home Button: Opens Home+" : "Home Button: Opens LG Home", ACT_HOME_TOGGLE, 0, 0);
    if (home_on && S.noflash_available)
        sheet_add(S.noflash_active ? "Home Button: No Flash" : "Home Button: Standard", ACT_HOME_MODE, 0, 0);
    sheet_add(S.sounds_off ? "Sounds: Off" : "Sounds: On", ACT_SOUNDS, 0, 0);
    sheet_add("Accessibility\xE2\x80\xA6", ACT_ACCESSIBILITY, 0, 0);
    if (S.trakt_connected) sheet_add("Disconnect Trakt", ACT_TRAKT_UNLINK, 0, 1);
    else if (S.trakt_configured) sheet_add("Connect Trakt", ACT_TRAKT_LINK, 0, 0);
    if (!S.rail_stremio && stremio_installed()) sheet_add("Connect Stremio", ACT_STREMIO_HELP, 0, 0);
    sheet_add("Remove Home+\xE2\x80\xA6", ACT_REMOVE_ASK, 0, 1);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

/* Control Center: what a top-bar button (or its colour key) opens */
static void open_top(int which)
{
    if (which == TOP_RECENTS) sw_open();
    else open_settings_sheet();
}

static void open_app_sheet(void)
{
    App *a = item_app(S.focus);
    if (!a || S.zone != 0) return;
    sheet_open(a->title);
    sheet_add("Move", ACT_MOVE, 0, 0);
    sheet_add("Move to Front", ACT_FRONT, 0, 0);
    if (is_stremio(a) && !S.rail_connected) sheet_add("Connect Stremio Account", ACT_STREMIO_HELP, 0, 0);
    sheet_add("Hide App", ACT_HIDE, 0, 1);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

static void open_hidden_sheet(void)
{
    sheet_open("Hidden Apps");
    int n = 0;
    for (int i = 0; i < S.napps; i++) {
        if (!is_hidden(S.apps[i].id)) continue;
        char label[200];
        snprintf(label, sizeof label, "Show \xE2\x80\x9C%s\xE2\x80\x9D", S.apps[i].title);
        sheet_add(label, ACT_UNHIDE, i, 0);
        n++;
    }
    if (n > 1) sheet_add("Show All", ACT_UNHIDE_ALL, 0, 0);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

static void move_item_to(int to)
{
    int last = S.nitems - 1;
    if (last >= 0 && S.items[last] < 0) last--; /* never past the Hidden tile */
    if (to > last) to = last;
    if (to < 0 || to == S.focus) return;
    int v = S.items[S.focus];
    if (to < S.focus) memmove(&S.items[to + 1], &S.items[to], (S.focus - to) * sizeof(int));
    else memmove(&S.items[S.focus], &S.items[S.focus + 1], (to - S.focus) * sizeof(int));
    S.items[to] = v;
    S.focus = to;
}

static void run_action(Action act, int arg)
{
    App *a = item_app(S.focus);
    switch (act) {
    case ACT_MOVE: S.moving = 1; break;
    case ACT_FRONT: move_item_to(0); save_layout(); break;
    case ACT_HIDE:
        if (a && S.nhidden < MAX_APPS) {
            char msg[200];
            snprintf(msg, sizeof msg, "%s is now in Hidden, at the end", a->title);
            copy_id(S.hidden[S.nhidden++], a->id);
            save_layout();
            rebuild_items();
            toast(msg, 3000);
        }
        break;
    case ACT_UNHIDE:
    case ACT_UNHIDE_ALL:
        for (int i = 0; i < S.nhidden; i++) {
            if (act == ACT_UNHIDE_ALL || !strcmp(S.hidden[i], S.apps[arg].id)) {
                memmove(&S.hidden[i], &S.hidden[i + 1], (S.nhidden - i - 1) * ID_LEN);
                S.nhidden--;
                i--;
            }
        }
        rebuild_items();
        save_layout();
        break;
    case ACT_HOME_MODE: {
        const char *next = !strcmp(S.home_mode, "watch") ? "redirect" : "watch";
        char body[48];
        snprintf(body, sizeof body, "{\"mode\":\"%s\"}", next);
        net_post("/api/home", body, TAG_HOME_SET);
        snprintf(S.home_mode, sizeof S.home_mode, "%s", next);
        toast(!strcmp(next, "redirect") ? "Home button: no-flash mode" : "Home button: standard mode", 3000);
        break;
    }
    case ACT_SETUP_RETRY:
        setup_retry_now();
        break;
    case ACT_HOME_YES:
        set_home_mode(S.noflash_available ? "redirect" : "watch", 1);
        toast("The Home button now opens Home+", 3000);
        break;
    case ACT_HOME_NO:
        set_home_mode("off", 1);
        toast("You can turn this on later in Settings", 3000);
        break;
    case ACT_HOME_TOGGLE: {
        int on = S.home_mode[0] && strcmp(S.home_mode, "off");
        set_home_mode(on ? "off" : S.noflash_available ? "redirect" : "watch", 1);
        toast(on ? "The Home button opens LG\xE2\x80\x99s home screen" : "The Home button now opens Home+", 3000);
        break;
    }
    case ACT_REMOVE_ASK:
        sheet_open("Remove Home+?");
        sheet_body("This deletes Home+\xE2\x80\x99s boot script, settings and sign-ins (Stremio, Trakt),\nturns off the Home button takeover and uninstalls Home+.");
        sheet_add("Remove Home+", ACT_REMOVE, 0, 1);
        sheet_add("Cancel", ACT_CANCEL, 0, 0);
        break;
    case ACT_REMOVE:
        setup_set(SETUP_REMOVING);
        net_post("/api/uninstall", "{}", TAG_UNINSTALL);
        break;
    case ACT_TRAKT_LINK:
        toast("Asking Trakt for a code\xE2\x80\xA6", 4000);
        net_post("/api/trakt/link", "{}", TAG_TRAKT_LINK);
        break;
    case ACT_TRAKT_UNLINK:
        net_post("/api/trakt/disconnect", "{}", 0);
        S.trakt_connected = 0;
        S.rail_sig[0] = 0;
        net_get_json("/api/continue", TAG_STREMIO, 0, 0);
        toast("Trakt disconnected", 3000);
        break;
    case ACT_STREMIO_HELP:
        toast("To see Continue Watching, sign in to Stremio from a computer (see the Home+ README)", 8000);
        break;
    case ACT_ACCESSIBILITY:
        sheet_open("Accessibility");
        sheet_add(S.reduce_motion ? "Reduce Motion: On" : "Reduce Motion: Off", ACT_REDUCE_MOTION, 0, 0);
        sheet_add(S.reduce_transparency ? "Reduce Transparency: On" : "Reduce Transparency: Off", ACT_REDUCE_TRANSPARENCY, 0, 0);
        sheet_add("Done", ACT_CANCEL, 0, 0);
        break;
    case ACT_REDUCE_MOTION:
    case ACT_REDUCE_TRANSPARENCY: {
        int *flag = act == ACT_REDUCE_MOTION ? &S.reduce_motion : &S.reduce_transparency;
        *flag = !*flag;
        char body[64];
        snprintf(body, sizeof body, "{\"%s\":%s}", act == ACT_REDUCE_MOTION ? "reduceMotion" : "reduceTransparency",
                 *flag ? "true" : "false");
        net_post("/api/prefs", body, 0);
        run_action(ACT_ACCESSIBILITY, 0); /* back to the list, with the new label */
        S.sheet.index = act == ACT_REDUCE_MOTION ? 0 : 1;
        break;
    }
    case ACT_SOUNDS:
        S.sounds_off = !S.sounds_off;
        snd_enable(!S.sounds_off);
        net_post("/api/prefs", S.sounds_off ? "{\"sounds\":false}" : "{\"sounds\":true}", 0);
        toast(S.sounds_off ? "Sounds off" : "Sounds on", 2000);
        break;
    default: break;
    }
}

static void activate(void)
{
    if (S.zone == ZONE_TOP) {
        open_top(S.top_index);
        return;
    }
    if (S.zone == 1) {
        App *a = item_app(S.focus);
        if (a && S.rail_index < S.nrail) {
            /* Stremio's LG app can't open a title directly: open it and say where the title is */
            char hint[300];
            snprintf(hint, sizeof hint, "\xE2\x80\x9C%s\xE2\x80\x9D is in Stremio\xE2\x80\x99s Continue Watching", S.rail[S.rail_index].name);
            launch(a->id, hint);
            snprintf(hint, sizeof hint, "Opening Stremio. Look for \xE2\x80\x9C%s\xE2\x80\x9D in Continue Watching", S.rail[S.rail_index].name);
            toast(hint, 6000);
        }
        return;
    }
    if (S.moving) {
        S.moving = 0;
        save_layout();
        return;
    }
    if (S.focus < S.nitems && S.items[S.focus] < 0) {
        open_hidden_sheet();
        return;
    }
    App *a = item_app(S.focus);
    if (!a || (S.launching && SDL_GetTicks() - S.launch_at < 3000)) return; /* a second OK while it opens */
    launch(a->id, NULL);
    /* the tile breathes and says so until the app is in front (webOS takes 1-5 s) */
    char msg[200];
    snprintf(msg, sizeof msg, "Opening %s\xE2\x80\xA6", a->title);
    tex_free(&S.launch_tex);
    S.launch_tex = text_render(S.f_medium, 26, msg, TILE_W + 60, &S.launch_w, &S.launch_h);
    S.launching = 1;
    S.launch_item = S.focus;
    S.launch_at = SDL_GetTicks();
}

static void move_focus(int dx, int dy)
{
    if (S.zone == ZONE_TOP) {
        if (dy > 0) { /* down: the Continue Watching card under the button, else the tile under it */
            int card = top_down_card();
            if (card >= 0) {
                S.zone = ZONE_RAIL;
                rail_focus(card);
            } else {
                S.zone = ZONE_GRID;
                S.focus = top_down_tile();
            }
        } else if (dx && S.top_index + dx >= 0 && S.top_index + dx < TOP_COUNT) S.top_index += dx;
        return;
    }
    if (S.zone == ZONE_RAIL) {
        int col = S.rail_index - S.rail_first;
        if (dy > 0) { S.zone = ZONE_GRID; S.focus = dock_tile(col); }
        else if (dy < 0) { S.zone = ZONE_TOP; S.top_index = top_nearest(col); }
        else if (dx && S.rail_index + dx >= 0 && S.rail_index + dx < S.nrail) rail_focus(S.rail_index + dx);
        return;
    }
    int n = S.nitems, i = S.focus, next = i;
    if (dx) {
        next = i + dx;
        if (next < 0 || next >= n || next / COLS != i / COLS) return;
    }
    if (dy > 0) {
        next = i + COLS;
        if (next >= n) {
            if ((n - 1) / COLS > i / COLS) next = n - 1;
            else return;
        }
    }
    if (dy < 0) {
        if (i < COLS) {
            /* up from the top row: the Stremio card above, else the top bar (Recents / Settings) */
            if (!S.moving) {
                if (rail_active()) {
                    S.zone = ZONE_RAIL;
                    rail_focus(S.rail_first + i);
                } else {
                    S.zone = ZONE_TOP;
                    S.top_index = top_nearest(i);
                }
            }
            return;
        }
        next = i - COLS;
    }
    if (S.moving) {
        if (S.items[next] >= 0) move_item_to(next);
        return;
    }
    S.focus = next;
}

static void go_home(void)
{
    sheet_close();
    sw_reset();
    if (S.moving) { S.moving = 0; save_layout(); }
    S.zone = 0;
    S.focus = 0;
}

/* ------------------------------------------------------------------ input */

/* LG's own key numbers: SDL_WEBOS_SCANCODE_* in LG's SDL_webOS.h, which the vendored
 * 2.0.14 headers don't have. Like the arrows they arrive in keysym.sym (Back is sym=482).
 * Settings and Input never showed up in the key log on the C3: webOS opens its own menus. */
enum {
    WEBOS_KEY_BACK = 482,
    WEBOS_KEY_CURSOR_SHOW = 484, /* the Magic Remote pointer woke up / went to sleep: not presses */
    WEBOS_KEY_CURSOR_HIDE = 485,
    WEBOS_KEY_RED = 486,
    WEBOS_KEY_GREEN = 487,
    WEBOS_KEY_YELLOW = 488,
    WEBOS_KEY_BLUE = 489
};

/* LG's SDL 2.0.14 reports remote keys with the *scancode* number in keysym.sym
 * (Right arrow arrives as sym=79 = SDL_SCANCODE_RIGHT). Map those back to keycodes;
 * real keycodes (from a USB keyboard or the desktop build) pass through, and so do
 * the colour keys, which have no SDL keycode. */
static SDL_Keycode remote_key(const SDL_Keysym *ks)
{
    switch (ks->sym) {
    case SDL_SCANCODE_RIGHT: return SDLK_RIGHT;
    case SDL_SCANCODE_LEFT: return SDLK_LEFT;
    case SDL_SCANCODE_DOWN: return SDLK_DOWN;
    case SDL_SCANCODE_UP: return SDLK_UP;
    case SDL_SCANCODE_RETURN: return SDLK_RETURN;
    case SDL_SCANCODE_KP_ENTER: return SDLK_KP_ENTER;
    case SDL_SCANCODE_ESCAPE: return SDLK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return SDLK_BACKSPACE;
    case SDL_SCANCODE_AC_BACK: return SDLK_AC_BACK;
    case WEBOS_KEY_BACK: return SDLK_AC_BACK;
    default: return ks->sym;
    }
}

static int is_back(SDL_Keycode k)
{
    return k == SDLK_ESCAPE || k == SDLK_AC_BACK || k == SDLK_BACKSPACE;
}

/* keys Home+ acts on; anything else gets logged so it can be mapped */
static int is_mapped(SDL_Keycode k)
{
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_LEFT: case SDLK_RIGHT: case SDLK_UP: case SDLK_DOWN:
    case WEBOS_KEY_RED: case WEBOS_KEY_GREEN: case WEBOS_KEY_YELLOW: case WEBOS_KEY_BLUE:
        return 1;
    default:
        return is_back(k);
    }
}

/* Left/Right pick an app, Up closes it (its card flies away), Down reaches Close All */
static void sw_key(SDL_Keycode k)
{
    if (S.recents.zooming) return;
    if (is_back(k) || k == WEBOS_KEY_RED) {
        S.recents.open = 0; /* animates out; sw_update() clears it */
        return;
    }
    if (!S.recents.n) return;
    if (S.recents.close_all) {
        if (k == SDLK_UP) S.recents.close_all = 0;
        return;
    }
    if (k == SDLK_LEFT && S.recents.index > 0) S.recents.index--;
    else if (k == SDLK_RIGHT && S.recents.index < S.recents.n - 1) S.recents.index++;
    else if (k == SDLK_DOWN) S.recents.close_all = 1;
    else if (k == SDLK_UP && !S.recents.c[S.recents.index].flying) {
        S.recents.c[S.recents.index].flying = 1;
        close_apps(S.recents.c[S.recents.index].id);
    }
}

#define PTR_TAKE_PX 40 /* pointer travel (logical px) that hands control back to it */
static int ptr_lx = -1, ptr_ly = -1; /* the pointer's last position */

static void keys_take_over(void)
{
    S.ptr_mode = 0;
    S.ptr_ax = ptr_lx;
    S.ptr_ay = ptr_ly;
}

/* the pointer moved to mx,my: 1 when it's (now) the one in charge */
static int pointer_moved(int mx, int my)
{
    ptr_lx = mx;
    ptr_ly = my;
    if (S.ptr_mode) return 1;
    if (S.ptr_ax < 0) { /* first sighting since the keys took over: just remember it */
        S.ptr_ax = mx;
        S.ptr_ay = my;
        return 0;
    }
    int dx = mx - S.ptr_ax, dy = my - S.ptr_ay;
    if (dx * dx + dy * dy < PTR_TAKE_PX * PTR_TAKE_PX) return 0; /* a hand's tremor */
    S.ptr_mode = 1;
    return 1;
}

static void on_key_down(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if (k == WEBOS_KEY_CURSOR_SHOW || k == WEBOS_KEY_CURSOR_HIDE) return;
    if (k != SDLK_RETURN && k != SDLK_KP_ENTER) keys_take_over();
    if (k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_UP || k == SDLK_DOWN) S.tick_armed = 1;
    if (S.logged_keys < 40 && !is_mapped(k)) {
        S.logged_keys++;
        log_remote("unknown key sym=%d scancode=%d mod=%d", (int)k, (int)e->keysym.scancode, (int)e->keysym.mod);
    }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
        if (!e->repeat && !S.ok_down) {
            S.ok_down = SDL_GetTicks();
            S.ok_long = 0;
        }
        return;
    }
    if (S.recents.open) {
        sw_key(k);
        return;
    }
    if (S.sheet.open) {
        if (k == SDLK_UP && S.sheet.index > 0) S.sheet.index--;
        else if (k == SDLK_DOWN && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
        else if (is_back(k) && !S.sheet.locked) {
            sheet_dismiss();
        }
        return;
    }
    switch (k) {
    case SDLK_LEFT: move_focus(-1, 0); break;
    case SDLK_RIGHT: move_focus(1, 0); break;
    case SDLK_UP: move_focus(0, -1); break;
    case SDLK_DOWN: move_focus(0, 1); break;
    /* colour keys: Recents, Settings, the focused app's options, move it (again: done) */
    case WEBOS_KEY_RED: if (!S.moving) open_top(TOP_RECENTS); break;
    case WEBOS_KEY_GREEN: if (!S.moving) open_top(TOP_SETTINGS); break;
    case WEBOS_KEY_YELLOW: if (!S.moving) open_app_sheet(); break;
    case WEBOS_KEY_BLUE:
        if (S.moving) { S.moving = 0; save_layout(); }
        else if (S.zone == ZONE_GRID && item_app(S.focus)) S.moving = 1;
        break;
    default:
        if (is_back(k)) {
            if (S.zone != ZONE_GRID) S.zone = ZONE_GRID;
            else if (S.moving) { S.moving = 0; save_layout(); }
            else if (S.focus) S.focus = 0;
            else open_top(TOP_RECENTS); /* already home: Back shows what's open */
        }
    }
}

/* OK let go (the remote's key, or the pointer's click): select what has focus. */
static void ok_release(void)
{
    int was_long = S.ok_long;
    S.ok_down = 0;
    if (was_long) return;
    snd_play(SND_SELECT);
    if (S.recents.open) {
        sw_ok();
    } else if (S.sheet.open) {
        if (!S.sheet.n) return; /* "Setting up…": nothing to press */
        SheetItem it = S.sheet.it[S.sheet.index];
        sheet_dismiss();
        if (it.act == ACT_LAUNCH_ID) launch(it.id, NULL);
        else run_action(it.act, it.arg);
    } else {
        activate();
    }
}

static void on_key_up(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if ((k != SDLK_RETURN && k != SDLK_KP_ENTER) || !S.ok_down) return;
    ok_release();
}

static int hit_item(int mx, int my)
{
    for (int i = 0; i < S.nitems; i++) {
        float x = LEFT + (i % COLS) * (TILE_W + GAP_X);
        float y = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
        if (mx >= x && mx < x + TILE_W && my >= y && my < y + TILE_H) return i;
    }
    return -1;
}


/* A sheet's button under the pointer (-1: none); *outside: not on the panel at all.
 * Mirrors draw_sheet()'s layout, from where the panel was last drawn. */
static int sheet_button_at(int mx, int my, int *outside)
{
    *outside = mx < S.sheet.px || mx > S.sheet.px + S.sheet.pw || my < S.sheet.py || my > S.sheet.py + S.sheet.ph;
    if (*outside || !S.sheet.n) return -1;
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    float step = SHEET_BH + SHEET_GAP, y0 = S.sheet.py + sheet_head(), list_h = rows * step;
    if (mx < S.sheet.px + PAD || mx > S.sheet.px + S.sheet.pw - PAD) return -1;
    if (my >= y0 && my < y0 + list_h) {
        float r = (my - y0) / step + S.sheet.scroll;
        int i = (int)floorf(r);
        if (r - i <= SHEET_BH / step && i >= 0 && i < nlist) return i;
        return -1;
    }
    for (int k = 0; k < nfoot; k++) {
        float fy = y0 + list_h + k * step;
        if (my >= fy && my < fy + SHEET_BH) return nlist + k;
    }
    return -1;
}

/* What's under the pointer; with apply, it gets focus (as if the remote moved
 * there). Returns 1 when it's something OK would act on. */
static int pointer_at(int mx, int my, int apply)
{
    if (S.recents.open) {
        if (S.recents.zooming || !S.recents.n) return 0;
        float bx = (W - 360) / 2.0f, by = H - SAFE_Y - SHEET_BH; /* Close All */
        if (mx >= bx && mx < bx + 360 && my >= by && my < by + SHEET_BH) {
            if (apply) S.recents.close_all = 1;
            return 1;
        }
        for (int i = 0; i < S.recents.n; i++) {
            float cx = W / 2.0f + S.recents.c[i].x * SW_PITCH;
            if (fabsf(mx - cx) < SW_W * 0.42f && fabsf((float)(my - SW_CY)) < SW_H * 0.42f) {
                if (apply) S.recents.index = i, S.recents.close_all = 0;
                return 1;
            }
        }
        return 0;
    }
    if (S.sheet.open) {
        int outside, i = sheet_button_at(mx, my, &outside);
        if (i >= 0 && apply) S.sheet.index = i;
        return i >= 0;
    }
    if (S.moving) return 1; /* OK anywhere puts the moved app down */
    if (S.scrolled < 0.5f) { /* the top bar and Continue Watching are only there at the top */
        for (int i = 0; i < TOP_COUNT; i++) {
            float x = top_button_x(i);
            if (mx >= x && mx < x + BAR && my >= SAFE_Y && my < SAFE_Y + BAR) {
                if (apply) S.zone = ZONE_TOP, S.top_index = i;
                return 1;
            }
        }
        if (rail_active()) {
            for (int i = 0; i < S.nrail; i++) {
                float x = LEFT + i * (RAIL_CARD_W + RAIL_GAP) - S.rail_shift;
                if (mx >= x && mx < x + RAIL_CARD_W && my >= RAIL_Y && my < RAIL_Y + RAIL_CARD_H) {
                    if (apply) S.zone = ZONE_RAIL, rail_focus(i);
                    return 1;
                }
            }
        }
    }
    int h = hit_item(mx, my);
    if (h < 0) return 0;
    if (apply) {
        S.focus = h;
        S.zone = ZONE_GRID;
        /* the light comes from where the pointer is, relative to the tile */
        float dx = mx - (LEFT + (h % COLS) * (TILE_W + GAP_X) + TILE_W / 2.0f);
        float dy = my - (GRID_TOP + (h / COLS) * ROW_H + S.scroll + TILE_H / 2.0f);
        float l = sqrtf(dx * dx + dy * dy);
        if (l > 8) {
            S.ptr_x = dx / l;
            S.ptr_y = dy / l;
            S.ptr_at = SDL_GetTicks();
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ animation */

/* OLED protection: after a while without input the screen dims (all but the
 * focused tile, like tvOS), then fades to black and stops drawing. */
#define IDLE_DIM_MS (2 * 60 * 1000)  /* defaults; /api/prefs dimMinutes / offMinutes override */
#define IDLE_OFF_MS (10 * 60 * 1000)

static float special_s, special_v; /* focus animation of the "Hidden" tile */

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int m = t->tm_hour * 60 + t->tm_min;
    if (m == S.clock_min && S.clock_tex) return;
    char buf[16];
    int h12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;
    snprintf(buf, sizeof buf, "%d:%02d %s", h12, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
    tex_free(&S.clock_tex);
    S.clock_tex = text_render(S.f_semi, 27, buf, 0, &S.clock_w, &S.clock_h);
    S.clock_min = m;
}

/* Advances every animated value once per frame. Drawing only reads them: the
 * scene is drawn twice, into the glass backdrop and then to the screen. */
static int update(float dt)
{
    int anim = 0;
    update_clock();
    int row = S.focus / COLS;
    approach(&S.scrolled, row > 0 ? 1.0f : 0.0f, 10, dt, &anim);
    S.scroll_target = row == 0 ? 0 : 380 - (GRID_TOP + row * ROW_H);
    if (row > 0) {
        /* don't scroll past the point where the last row (and its label) sits at the
         * bottom of the safe area: a short last stretch would otherwise stop mid-screen */
        int rows = (S.nitems + COLS - 1) / COLS;
        float lowest = (H - SAFE_Y) - (GRID_TOP + (rows - 1) * ROW_H + TILE_H + 60);
        if (S.scroll_target < lowest) S.scroll_target = lowest;
    }
    spring(&S.scroll, &S.scroll_v, S.scroll_target, 0.5f, 1.0f, dt, &anim);

    /* the Top Shelf belongs to the last top-row app focused, also while it shows
     * Continue Watching: scrolled below the dock, its picture stays behind, blurred */
    int cur = S.focus < COLS ? S.focus : S.shelf_cur;
    if (cur != S.shelf_cur) {
        S.shelf_prev = S.shelf_cur;
        S.shelf_cur = cur;
        S.shelf_a = 0;
    }
    approach(&S.shelf_a, 1, 9, dt, &anim);
    if (rail_active()) {
        RailItem *cur = S.rail_index < S.nrail ? &S.rail[S.rail_index] : NULL;
        /* background art: lazy-load the focused one, crossfade between items */
        if (cur && cur->bg[0] && cur->bg_state == 0) {
            cur->bg_state = 1;
            net_get_image(cur->bg, TAG_RAIL_BG, S.rail_index, S.rail_generation, 0, W, H);
        }
        if (S.rail_art_cur != S.rail_index) {
            S.rail_art_prev = S.rail_art_cur;
            S.rail_art_cur = S.rail_index;
            S.rail_art_a = 0;
        }
        approach(&S.rail_art_a, 1, 9, dt, &anim);
        /* keep only nearby full-screen backgrounds in GPU memory (~8 MB each) */
        for (int i = 0; i < S.nrail; i++) {
            RailItem *it = &S.rail[i];
            if (it->bg_tex && abs(i - S.rail_index) > 2 && i != S.rail_art_prev) {
                tex_free(&it->bg_tex);
                it->bg_state = 0;
            }
        }
        float shift = (float)S.rail_first * (RAIL_CARD_W + RAIL_GAP);
        spring(&S.rail_shift, &S.rail_shift_v, shift, 0.45f, 1.0f, dt, &anim);
        for (int i = 0; i < S.nrail; i++)
            spring(&S.rail[i].s, &S.rail[i].sv, S.zone == ZONE_RAIL && i == S.rail_index ? (S.ok_down && !S.ok_long ? 0.6f : 1.0f) : 0.0f,
                   0.35f, 0.7f, dt, &anim);
    } else {
        App *ca = item_app(S.shelf_cur);
        if (ca && ca->art[0] && ca->art_state == 0) {
            ca->art_state = 1;
            net_get_image(ca->art, TAG_ART, S.items[S.shelf_cur], S.generation, 0, W, H);
        }
    }
    /* while OK is down the focused thing sinks a little (1.06), then pops back */
    int pressed = S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open;
    float on = pressed ? 0.6f : 1.0f, resp = pressed ? 0.12f : 0.35f, damp = pressed ? 1.0f : 0.7f;
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        int f = i == S.focus && S.zone == ZONE_GRID;
        spring(a ? &a->s : &special_s, a ? &a->sv : &special_v, f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
    }
    for (int i = 0; i < TOP_COUNT; i++) {
        int f = S.zone == ZONE_TOP && S.top_index == i;
        spring(&S.top_s[i], &S.top_v[i], f ? on : 0.0f, f ? resp : 0.35f, f ? damp : 0.7f, dt, &anim);
    }
    if (pressed) anim = 1; /* the hold bar fills */
    if (S.launching) {
        Uint32 held = SDL_GetTicks() - S.launch_at;
        if (held < 2500) anim = 1;
        if (held > 10000) {
            char msg[200];
            App *la = item_app(S.launch_item);
            snprintf(msg, sizeof msg, "%s is taking a while to open", la ? la->title : "The app");
            toast(msg, 3500);
            S.launching = 0;
        }
    }
    if (S.moving && !S.reduce_motion) anim = 1; /* wobble */
    if (S.sheet.open) {
        spring(&S.sheet.t, &S.sheet.tv, 1, 0.42f, 0.82f, dt, &anim);
        int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
        if (S.sheet.index < nlist) S.sheet.first = keep_in_view(S.sheet.first, S.sheet.index, rows, nlist, rows > 4);
        if (S.sheet.t < 0.05f) S.sheet.scroll = (float)S.sheet.first; /* opens already in place */
        spring(&S.sheet.scroll, &S.sheet.scroll_v, (float)S.sheet.first, 0.5f, 1.0f, dt, &anim);
    }
    sw_update(dt, &anim);
    if (S.ghost.on) {
        spring(&S.ghost.t, &S.ghost.tv, 0, 0.32f, 1.0f, dt, &anim);
        if (S.ghost.t <= 0.002f) S.ghost.on = 0;
    }
    spring(&S.toast_t, &S.toast_v, toast_on() ? 1.0f : 0.0f, 0.4f, 0.85f, dt, &anim);

    /* when focus lands the element sways for a moment and its highlight follows
     * a moving light (from the pointer if it moved lately), then it settles */
    int layer = S.recents.open ? 2 : S.sheet.open ? 1 : 0; /* home, a sheet, the App Switcher */
    int key = layer == 2 ? 2000000 + S.recents.index * 2 + S.recents.close_all
            : layer == 1 ? 1000000 + S.sheet.index
                         : S.zone * 100000 + S.focus * 1000 + S.top_index * 100 + S.rail_index;
    if (key != S.focus_key) {
        /* a tick when a key moved focus: not when a layer opens or closes, nor
         * when the pointer or a reset moves it */
        if (S.tick_armed && layer == S.focus_key / 1000000) snd_play(SND_TICK);
        S.focus_key = key;
        S.sway_t = 0;
    }
    S.tick_armed = 0;
    float bx = -0.6f, by = -0.8f;
    if (S.ptr_at && SDL_GetTicks() - S.ptr_at < 3000) {
        bx = S.ptr_x;
        by = S.ptr_y;
    }
    float amp = 0;
    if (S.reduce_motion) S.sway_t = 1.6f; /* no sway, the light stays put */
    if (S.sway_t < 1.6f) {
        S.sway_t += dt;
        amp = 1 - S.sway_t / 1.6f;
        amp *= amp;
        anim = 1;
    }
    float ang = 0.6f * amp * sinf(S.sway_t * 2 * (float)M_PI / 1.1f);
    S.light_x = bx * cosf(ang) - by * sinf(ang);
    S.light_y = bx * sinf(ang) + by * cosf(ang);

    float dim = S.idle_stage >= 1 ? 1.0f : 0.0f, black = S.idle_stage >= 2 ? 1.0f : 0.0f;
    approach(&S.dim, dim, dim > S.dim ? 2.5f : 10.0f, dt, &anim);
    approach(&S.black, black, black > S.black ? 2.5f : 10.0f, dt, &anim);
    return anim;
}

/* ------------------------------------------------------------------ drawing */

static void draw_text(Tex *t, int w, int h, float x, float y, Uint8 a)
{
    r_image(t, x, y, (float)w, (float)h, 255, 255, 255, a, 0);
}

/* offset of the focused element while it sways after focus lands */
static void sway(float *dx, float *dy)
{
    float amp = S.sway_t < 1.6f ? 1 - S.sway_t / 1.6f : 0;
    amp *= amp;
    *dx = 3.0f * amp * sinf(S.sway_t * 2 * (float)M_PI / 1.1f);
    *dy = 1.5f * amp * sinf(S.sway_t * 2 * (float)M_PI / 0.9f + 1);
}

/* Liquid Glass (render.h): clear unless what's on it must stay legible. In the
 * backdrop pass glass can't sample itself, so a faint flat panel stands in. */
/* Glass, or with Reduce Transparency a solid dark panel (and a light wash when focused). */
static void glass(const Glass *g)
{
    if (!S.reduce_transparency) {
        r_glass(g);
        return;
    }
    float a = g->alpha * (g->lens < 1 ? g->lens : 1);
    r_shape(g->x, g->y, g->w, g->h, g->radius, 38, 38, 44, (Uint8)(245 * a), 0, 1, 1);
    if (g->focus > 0.01f) r_shape(g->x, g->y, g->w, g->h, g->radius, 255, 255, 255, (Uint8)(60 * g->focus * a), 0, 1, 1);
    r_shape(g->x, g->y, g->w, g->h, g->radius, 255, 255, 255, (Uint8)(50 * a), 1.5f, 1, 1); /* a hairline edge */
}

static void panel(int backdrop, float x, float y, float w, float h, float radius, float legible, float focus, float alpha)
{
    if (alpha <= 0.004f) return;
    if (backdrop) {
        r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(14 * alpha), 0, 1, 1);
        return;
    }
    int lit = focus > 0.01f; /* focused glass catches the moving light */
    Glass g = { x, y, w, h, radius, legible, 1.0f, lit ? S.light_x : -0.6f, lit ? S.light_y : -0.8f, focus, alpha, 0, 0, 0, 0 };
    glass(&g);
}

/* text is made when first shown: a big title is ~0.4 MB of GPU memory */
static void ensure_label(App *a)
{
    if (!a->label) a->label = text_render(S.f_medium, 26, a->title, TILE_W + 60, &a->lw, &a->lh);
}

static void ensure_big(App *a)
{
    if (!a->big) a->big = text_render(S.f_bold, 84, a->title, 1000, &a->bw, &a->bh);
}

/* darkens where text sits: the bottom (dock), the left (titles), the top (top bar) */
static void draw_scrims(Uint8 alpha)
{
    r_image(S.scrim_v, 0, H * 0.55f, W, H * 0.45f, 255, 255, 255, alpha, 0);
    r_image(S.scrim_h, 0, 0, 1150, H, 255, 255, 255, alpha, 0);
    r_image(S.scrim_top, 0, 0, W, 280, 255, 255, 255, alpha, 0);
}

/* An app's tile drawn large: the Top Shelf's big tile and the App Switcher's
 * cards. Its logo grows with it (up to 3.5x); a tile without one shows its name. */
static void draw_big_tile(App *a, float x, float y, float w, float h, float radius, Uint8 alpha, int shadow)
{
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    float k = fminf(fminf(w / TILE_W, h / TILE_H), 3.5f);
    if (shadow) r_shadow(x, y, w, h, radius, 40, 20, 0.45f * alpha / 255);
    r_shape(x, y, w, h, radius, r, g, b, alpha, 0, 1, 1);
    if (0.299f * r + 0.587f * g + 0.114f * b < 32) /* dark tiles keep their shape */
        r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(26 * alpha / 255), 1.5f, 1, 1);
    if (a->logo_tex) {
        float lw = a->logo_w * k, lh = a->logo_h * k;
        r_image(a->logo_tex, x + (w - lw) / 2, y + (h - lh) / 2, lw, lh, 255, 255, 255, alpha, a->logo_r * k);
    } else {
        ensure_big(a);
        if (!a->big) return;
        float f = fminf(fminf(1, w * 0.8f / a->bw), k / 2.5f), bw = a->bw * f, bh = a->bh * f;
        r_image(a->big, x + (w - bw) / 2, y + (h - bh) / 2, bw, bh, 255, 255, 255, alpha, 0);
    }
}

/* The Top Shelf is a picture (artwork, or the app's tile large on a glow, under
 * scrims) and content (titles, the Continue Watching rail). The picture stays
 * behind the grid when you scroll down, blurred; the content scrolls away. */
/* The Top Shelf picture: the app's artwork, else a cover made from its icon. */
static void draw_app_cover(App *a, Uint8 alpha)
{
    if (!a || !alpha) return;
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    if (a->art_tex) r_cover(a->art_tex, 0, 0, W, H, alpha, 0);
    else if (a->cover_tex) r_cover(a->cover_tex, 0, 0, W, H, alpha, 0);
    else r_shape(0, 0, W, H, 0, r, g, b, (Uint8)(alpha * 0.25f), 0, 1, 1);
}

/* Apps without artwork also get their tile, large, on the right. */
static void draw_app_bigtile(App *a, Uint8 alpha)
{
    if (!a || !alpha || a->art_tex || !a->logo_tex) return;
    /* it leaves with the shelf's content, so it never ghosts behind the grid */
    float leave = fmaxf(0, 1 - 2.5f * S.scrolled);
    if (leave <= 0) return;
    float tw = 520, th = tw * TILE_H / TILE_W;
    draw_big_tile(a, RIGHT - tw, SAFE_Y + 120, tw, th, TILE_R * tw / TILE_W, (Uint8)(alpha * leave), 1);
}

static void draw_app_text(App *a, Uint8 alpha, float dy)
{
    if (!a || !alpha) return;
    ensure_big(a);
    draw_text(a->big, a->bw, a->bh, LEFT - 4, 340 + dy, alpha);
    Tex *sub = is_stremio(a) && !S.rail_connected ? S.sub_connect_tex : S.sub_tex;
    int sw = sub == S.sub_tex ? S.sw : S.scw, sh = sub == S.sub_tex ? S.sh : S.sch;
    if (S.zone == ZONE_TOP) { /* while the top bar has focus, the hint is about it */
        sub = S.top_hint[S.top_index];
        sw = S.top_hw[S.top_index];
        sh = S.top_hh[S.top_index];
    }
    draw_text(sub, sw, sh, LEFT, 340 + a->bh + 10 + dy, (Uint8)(alpha * 0.92f));
}

static void draw_rail_art(Uint8 alpha)
{
    RailItem *cur = S.rail_index < S.nrail ? &S.rail[S.rail_index] : NULL;
    if (!cur || !alpha) return;
    RailItem *prev = S.rail_art_prev >= 0 && S.rail_art_prev < S.nrail ? &S.rail[S.rail_art_prev] : NULL;
    Tex *pt = prev ? (prev->bg_tex ? prev->bg_tex : prev->thumb_tex) : NULL;
    Tex *ct = cur->bg_tex ? cur->bg_tex : cur->thumb_tex;
    r_cover(pt, 0, 0, W, H, alpha, 0);
    r_cover(ct, 0, 0, W, H, (Uint8)(alpha * S.rail_art_a), 0);
    draw_scrims(alpha);
}

static void draw_rail_content(Uint8 alpha, int backdrop)
{
    RailItem *cur = S.rail_index < S.nrail ? &S.rail[S.rail_index] : NULL;
    if (!cur || !alpha) return;
    /* heading capsule, title, meta */
    float hy = SAFE_Y + 58, hh = 46;
    panel(backdrop, LEFT, hy, S.hw + 44, hh, hh / 2, 1, 0, alpha / 255.0f);
    draw_text(S.heading_tex, S.hw, S.hh, LEFT + 22, hy + (hh - S.hh) / 2.0f, alpha);
    ensure_rail_texts(cur);
    draw_text(cur->title_tex, cur->tw, cur->th, LEFT - 3, hy + hh + 26, alpha);
    float my = hy + hh + 26 + cur->th + 8;
    draw_text(cur->meta_tex, cur->mw, cur->mh, LEFT, my, (Uint8)(alpha * 0.92f));
    draw_text(S.rail_hint, S.rail_hint_w, S.rail_hint_h, LEFT, my + cur->mh + 6, (Uint8)(alpha * 0.7f)); /* it can't resume from here */

    /* cards, on the grid's columns */
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        float sc = 1 + 0.10f * it->s;
        float cw = RAIL_CARD_W * sc, ch = RAIL_CARD_H * sc, rad = TILE_R * sc;
        float x = LEFT + i * (RAIL_CARD_W + RAIL_GAP) - S.rail_shift - (cw - RAIL_CARD_W) / 2;
        float y = RAIL_Y - (ch - RAIL_CARD_H) / 2 - 6 * it->s;
        if (x > W || x + cw < 0) continue;
        if (it->s > 0.01f) {
            float dx, dy;
            sway(&dx, &dy);
            x += dx * it->s;
            y += dy * it->s;
        }
        /* opaque cards, the unfocused ones darker; those past the dock's edges fade
         * ("more this way") */
        float over = fmaxf(LEFT - x, x + cw - (W - LEFT));
        Uint8 ca = (Uint8)(alpha * (1 - 0.6f * fminf(fmaxf(over / 200, 0), 1)));
        if (it->s > 0.01f && !backdrop) r_shadow(x, y, cw, ch, rad, 26, 14, 0.5f * it->s * alpha / 255.0f);
        if (it->thumb_tex) r_cover(it->thumb_tex, x, y, cw, ch, ca, rad);
        else r_shape(x, y, cw, ch, rad, 255, 255, 255, (Uint8)(ca * 0.08f), 0, 1, 1);
        if (it->s < 0.99f) r_shape(x, y, cw, ch, rad, 0, 0, 0, (Uint8)(ca * 0.28f * (1 - fmaxf(it->s, 0))), 0, 1, 1);
        if (S.zone == ZONE_TOP && i == top_down_card() && !backdrop) /* where Down lands */
            r_shape(x, y, cw, ch, rad, 255, 255, 255, (Uint8)(ca / 2), 2.0f, 1, 1);
        r_image(S.scrim_v, x, y + ch * 0.45f, cw, ch * 0.55f, 255, 255, 255, ca, rad);
        draw_text(it->name_tex, it->nw, it->nh, x + 16 * sc, y + ch - (22 + it->nh) * sc, ca);
        float bx = x + 16 * sc, bw = cw - 32 * sc, by = y + ch - 17 * sc, bh = 5 * sc; /* progress */
        r_shape(bx, by, bw, bh, bh / 2, 255, 255, 255, (Uint8)(ca * 0.28f), 0, 1, 1);
        if (it->progress > 0) r_shape(bx, by, bw * it->progress, bh, bh / 2, 255, 255, 255, ca, 0, 1, 1);
    }
}

/* the shelf shows the rail when Stremio (with something to resume) was the last
 * top-row app focused, even after focus moves on below the dock */
static int shelf_rail(void)
{
    if (rail_active()) return 1;
    App *a = S.focus >= COLS ? item_app(S.shelf_cur) : NULL;
    return a && is_stremio(a) && S.nrail > 0;
}

static void draw_shelf_art(Uint8 alpha)
{
    if (!alpha) return;
    if (shelf_rail()) {
        draw_rail_art(alpha);
    } else {
        /* the new picture fades in over the old one, which stays at full strength
         * underneath (two half-faded pictures let the dark background show through),
         * and the scrim is drawn once over both */
        App *pa = item_app(S.shelf_prev), *ca = item_app(S.shelf_cur);
        float t = S.shelf_a;
        if (t < 1) draw_app_cover(pa, alpha);
        draw_app_cover(ca, (Uint8)(alpha * t));
        draw_scrims(alpha);
        if (t < 1) draw_app_bigtile(pa, (Uint8)(alpha * (1 - smooth01(0, 0.5f, t))));
        draw_app_bigtile(ca, (Uint8)(alpha * smooth01(0.3f, 1, t)));
    }
}

static void draw_shelf_content(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    if (rail_active()) {
        draw_rail_content(alpha, backdrop);
    } else {
        /* one title at a time: the old one leaves quickly, then the new one rises in */
        float t = S.shelf_a, in = smooth01(0.35f, 1, t);
        if (t < 0.4f) draw_app_text(item_app(S.shelf_prev), (Uint8)(alpha * (1 - smooth01(0, 0.4f, t))), 0);
        draw_app_text(item_app(S.shelf_cur), (Uint8)(alpha * in), S.reduce_motion ? 0 : 14 * (1 - in));
    }
}

/* Recents / Settings: round glass buttons left of the clock, BAR_GAP apart */
static void draw_top_bar(float right, Uint8 alpha, int backdrop)
{
    float x = right;
    for (int i = TOP_COUNT - 1; i >= 0; i--) {
        float k = S.top_s[i], d = BAR * (1 + 0.15f * k), g = fminf(fmaxf(k, 0), 1);
        x -= BAR + BAR_GAP;
        float cx = x + BAR / 2.0f, cy = SAFE_Y + BAR / 2.0f;
        if (k > 0.01f) {
            float dx, dy;
            sway(&dx, &dy);
            cx += dx * k;
            cy += dy * k;
        }
        if (k > 0.01f && !backdrop) r_shadow(cx - d / 2, cy - d / 2, d, d, d / 2, 16, 6, 0.35f * k * alpha / 255.0f);
        panel(backdrop, cx - d / 2, cy - d / 2, d, d, d / 2, 1, k, alpha / 255.0f);
        if (g > 0.01f) /* focused: an opaque white platter with a dark glyph, like the sheet buttons */
            r_shape(cx - d / 2, cy - d / 2, d, d, d / 2, 245, 245, 247, (Uint8)(alpha * g), 0, 1, 1);
        float is = S.top_w[i] * (1 + 0.15f * k);
        Uint8 gc = (Uint8)(255 - 227 * g);
        r_image(S.top_tex[i], cx - is / 2, cy - is / 2, is, is, gc, gc, (Uint8)(255 - 225 * g), (Uint8)(alpha * (0.85f + 0.15f * g)), 0);
        if (g > 0.01f) { /* and its name in a pill under it */
            float lw = S.top_lw[i] + 28.0f, lh = S.top_lh[i] + 12.0f, lx = cx - lw / 2, ly = SAFE_Y + BAR + 14;
            panel(backdrop, lx, ly, lw, lh, lh / 2, 1, 0, alpha * g / 255.0f);
            draw_text(S.top_label[i], S.top_lw[i], S.top_lh[i], lx + 14, ly + 6, (Uint8)(alpha * g));
        }
    }
}

static void draw_clock(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float pw = S.clock_w + 48, x = RIGHT - pw;
    panel(backdrop, x, SAFE_Y, pw, BAR, BAR / 2, 1, 0, alpha / 255.0f);
    draw_text(S.clock_tex, S.clock_w, S.clock_h, x + 24, SAFE_Y + (BAR - S.clock_h) / 2.0f, (Uint8)(alpha * TEXT_STATIC / 255));
    draw_top_bar(x, alpha, backdrop);
}

static void draw_tile(int i, int backdrop)
{
    App *a = item_app(i);
    float bx = LEFT + (i % COLS) * (TILE_W + GAP_X);
    float by = GRID_TOP + (i / COLS) * ROW_H + S.scroll;
    if (by > H + 40 || by + ROW_H < -40) return;
    int focused = i == S.focus && S.zone == ZONE_GRID;
    float k = a ? a->s : special_s;
    float sc = 1 + (FOCUS_SCALE - 1) * k;
    int launching = S.launching && i == S.launch_item;
    if (launching) { /* opening: breathes between 1.10 and 1.07 for 2 s, then holds */
        float lt = (SDL_GetTicks() - S.launch_at) / 1000.0f;
        k = 1;
        sc = lt < 2 ? 1.085f + 0.015f * cosf(lt * 2 * (float)M_PI * 1.25f) : FOCUS_SCALE;
    }
    float w = TILE_W * sc, h = TILE_H * sc, rad = TILE_R * sc;
    float x = bx - (w - TILE_W) / 2, y = by - (h - TILE_H) / 2 - 6 * k;
    if (S.moving && focused && !S.reduce_motion) x += sinf(SDL_GetTicks() / 40.0f) * 2.0f; /* gentle wobble while moving */
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        x += dx * k;
        y += dy * k;
    }
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    if (k > 0.01f && !backdrop) r_shadow(x, y, w, h, rad, 28, 16, 0.5f * k);
    r_shape(x, y, w, h, rad, r, g, b, a ? 255 : 34, 0, 1, 1); /* uniform: opaque, the icon's own colour */
    if (a && 0.299f * r + 0.587f * g + 0.114f * b < 32) r_shape(x, y, w, h, rad, 255, 255, 255, 26, 1.5f, 1, 1); /* dark tiles keep their shape */
    if (a && a->logo_tex) {
        float lw = a->logo_w * sc, lh = a->logo_h * sc;
        r_image(a->logo_tex, x + (w - lw) / 2, y + (h - lh) / 2, lw, lh, 255, 255, 255, 255, a->logo_r * sc);
    } else if (a) {
        ensure_label(a);
        if (a->label) draw_text(a->label, a->lw, a->lh, x + (w - a->lw) / 2, y + (h - a->lh) / 2, 230);
    } else {
        static Tex *ht;
        static int hw, hh;
        if (!ht) ht = text_render(S.f_semi, 28, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2   Hidden", 0, &hw, &hh);
        draw_text(ht, hw, hh, x + (w - hw) / 2, y + (h - hh) / 2, 205);
    }
    if (S.moving && focused) r_shape(x, y, w, h, rad, 255, 255, 255, 200, 2.0f, 1, 1); /* the tile you're moving */
    if (S.zone == ZONE_TOP && i == top_down_tile() && !backdrop) /* where Down from the top bar lands */
        r_shape(x, y, w, h, rad, 255, 255, 255, 128, 2.0f, 1, 1);
    if (focused && S.ok_down && !S.ok_long && !S.sheet.open && !S.recents.open && !S.moving && !backdrop) {
        float p = (SDL_GetTicks() - S.ok_down - 250) / (float)(LONG_PRESS_MS - 250);
        if (p > 0) { /* holding OK: a bar fills until the options open */
            float hw = w * 0.5f, hx = x + (w - hw) / 2, hy = y + h + 10;
            r_shape(hx, hy, hw, 4, 2, 255, 255, 255, 60, 0, 1, 1);
            r_shape(hx, hy, hw * fminf(p, 1), 4, 2, 255, 255, 255, 230, 0, 1, 1);
        }
    }
    if (a && k > 0.01f && !backdrop) {
        ensure_label(a);
        float ly = i < COLS ? DOCK_Y + S.scroll + DOCK_H + 14 : by + TILE_H + 22; /* top row: under the dock */
        Tex *lt = launching && S.launch_tex ? S.launch_tex : a->label;
        int lw = lt == a->label ? a->lw : S.launch_w, lh = lt == a->label ? a->lh : S.launch_h;
        draw_text(lt, lw, lh, bx + (TILE_W - lw) / 2, ly, (Uint8)(TEXT_STATIC * fminf(k, 1)));
    }
}

/* Everything below the overlays. backdrop: the copy the glass refracts. */
static void draw_scene(int backdrop)
{
    Uint8 shelf_a = (Uint8)(255 * (1 - S.scrolled));
    draw_shelf_art(shelf_a);
    if (S.scrolled > 0.004f) { /* below the dock the picture stays, blurred and dimmed, so the apps stand out */
        r_backdrop_image(1, (Uint8)(255 * S.scrolled));
        r_fill(0, 0, 0, (Uint8)(110 * S.scrolled));
    }
    /* the shelf's content leaves faster than the grid arrives, drifting up behind it */
    r_translate(0, 0.35f * S.scroll);
    draw_shelf_content((Uint8)(255 * fmaxf(0, 1 - 2.5f * S.scrolled)), backdrop);
    r_translate(0, 0);
    float dock_y = DOCK_Y + S.scroll, dock_a = 1 - S.scrolled; /* the dock leaves with its row */
    if (!backdrop && dock_a > 0.004f) r_shadow(DOCK_X, dock_y, DOCK_W, DOCK_H, TILE_R + PAD, 40, 14, 0.3f * dock_a);
    panel(backdrop, DOCK_X, dock_y, DOCK_W, DOCK_H, TILE_R + PAD, 0, 0, dock_a); /* icons on it are opaque: stays clear */
    int dimmed = !backdrop && S.dim > 0.004f, top = S.zone == ZONE_TOP;
    if (!(dimmed && top)) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    for (int i = 0; i < S.nitems; i++)
        if (i != S.focus) draw_tile(i, backdrop);
    if (dimmed) r_fill(0, 0, 0, (Uint8)(S.dim * 115)); /* OLED: idle dims all but what has focus */
    if (S.launching && !backdrop) { /* the rest steps back while the app opens */
        float d = (SDL_GetTicks() - S.launch_at - 250) / 200.0f;
        if (d > 0) r_fill(0, 0, 0, (Uint8)(77 * fminf(d, 1)));
    }
    if (S.focus < S.nitems) draw_tile(S.focus, backdrop);
    if (dimmed && top) draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    if (S.moving) { /* how to finish, in a capsule at the top while an app is being moved */
        static Tex *hint;
        static int hw, hh;
        if (!hint) hint = text_render(S.f_medium, 26, "Move it with the arrows  \xC2\xB7  OK or Back when it\xE2\x80\x99s in place", 0, &hw, &hh);
        float pw = hw + 56, px = (W - pw) / 2;
        panel(backdrop, px, SAFE_Y, pw, BAR, BAR / 2, 1, 0, 1);
        draw_text(hint, hw, hh, px + 28, SAFE_Y + (BAR - hh) / 2.0f, TEXT_STATIC);
    }
}

/* Cancel and the bulk actions after a list stay put while a long list scrolls. */
static int sheet_footer(void)
{
    int k = 0;
    while (k < S.sheet.n) {
        Action a = S.sheet.it[S.sheet.n - 1 - k].act;
        if (a != ACT_CANCEL && a != ACT_UNHIDE_ALL) break;
        k++;
    }
    return k;
}

static float sheet_body_h(void)
{
    float h = 0;
    for (int i = 0; i < S.sheet.nbody; i++) h += S.sheet.bh[i] + 6;
    return h ? h + 18 : 0;
}

static float sheet_head(void)
{
    return PAD + S.sheet.th + 24 + sheet_body_h() + (S.sheet_big ? S.sheet_big_h + 24 : 0);
}

/* List rows on screen: all of them when they fit, else as many as fit above the
 * footer in a panel no taller than the safe area. */
static int sheet_rows(int nfoot)
{
    int fit = (int)((H - 2 * SAFE_Y - sheet_head() - PAD + SHEET_GAP) / (SHEET_BH + SHEET_GAP)) - nfoot;
    int nlist = S.sheet.n - nfoot;
    return nlist < fit ? nlist : fit < 1 ? 1 : fit;
}

static void draw_sheet_button(const SheetItem *it, int focused, float bx, float y, float bw, float k, Uint8 ca)
{
    const float BH = SHEET_BH;
    Uint8 a = (Uint8)(ca * k);
    float tx = bx + (bw - it->w) / 2, ty = y + (BH - it->h) / 2;
    if (focused) { /* tvOS: the focused button turns opaque white with a dark label */
        r_shadow(bx, y, bw, BH, BH / 2, 18, 8, 0.4f * a / 255);
        r_shape(bx, y, bw, BH, BH / 2, 245, 245, 247, a, 0, 1, 1);
        if (it->destructive) r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 215, 40, 40, a, 0);
        else r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 28, 28, 30, a, 0);
    } else {
        r_shape(bx, y, bw, BH, BH / 2, 255, 255, 255, (Uint8)(26 * a / 255), 0, 1, 1);
        if (it->destructive) r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 255, 138, 128, (Uint8)(235 * a / 255), 0);
        else r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 255, 255, 255, (Uint8)(235 * a / 255), 0);
    }
}

static void draw_sheet(void)
{
    if (!S.sheet.open) return;
    float t = S.sheet.t, tc = t < 0 ? 0 : t > 1 ? 1 : t;
    r_fill(0, 0, 0, (Uint8)(80 * tc));
    const float BH = SHEET_BH, BGAP = SHEET_GAP, rad = BH / 2 + PAD; /* corners concentric with the buttons */
    float big = S.sheet_big ? S.sheet_big_h + 24 : 0;
    int nfoot = sheet_footer(), nlist = S.sheet.n - nfoot, rows = sheet_rows(nfoot);
    float pw = 720;
    if (S.sheet.tw + 2 * PAD > pw) pw = S.sheet.tw + 2 * PAD;
    for (int i = 0; i < S.sheet.nbody; i++) if (S.sheet.bw[i] + 2 * PAD > pw) pw = S.sheet.bw[i] + 2 * PAD;
    float ph = sheet_head() + (rows + nfoot) * (BH + BGAP) - (S.sheet.n ? BGAP : 24) + PAD;
    float px = (W - pw) / 2, py = (H - ph) / 2;
    S.sheet.px = px; S.sheet.py = py; S.sheet.pw = pw; S.sheet.ph = ph; S.sheet.pr = rad;
    /* grows out of the button it came from on a spring, and its lensing
     * materialises as it does (Apple: glass appears by bending light) */
    float gx = S.sheet.fx + (px - S.sheet.fx) * t, gy = S.sheet.fy + (py - S.sheet.fy) * t;
    float gw = S.sheet.fw + (pw - S.sheet.fw) * t, gh = S.sheet.fh + (ph - S.sheet.fh) * t;
    float r0 = fminf(S.sheet.fw, S.sheet.fh) / 2, gr = r0 + (rad - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * tc);
    Glass g = { gx, gy, gw, gh, gr, 1, tc, -0.6f, -0.8f, 0, 1, 0, 0, 0, 0 };
    glass(&g);
    float c = (t - 0.55f) / 0.45f; /* content fades in once the panel is nearly there */
    Uint8 ca = (Uint8)(255 * (c < 0 ? 0 : c > 1 ? 1 : c));
    if (!ca) return;
    draw_text(S.sheet.title, S.sheet.tw, S.sheet.th, px + (pw - S.sheet.tw) / 2, py + PAD, (Uint8)(200 * ca / 255));
    float y = py + PAD + S.sheet.th + 24;
    for (int i = 0; i < S.sheet.nbody; i++) {
        draw_text(S.sheet.body[i], S.sheet.bw[i], S.sheet.bh[i], px + (pw - S.sheet.bw[i]) / 2, y, (Uint8)(185 * ca / 255));
        y += S.sheet.bh[i] + 6;
    }
    if (S.sheet.nbody) y += 18;
    if (S.sheet_big) {
        draw_text(S.sheet_big, S.sheet_big_w, S.sheet_big_h, px + (pw - S.sheet_big_w) / 2, y, ca);
        y += big;
    }
    float bx = px + PAD, bw = pw - 2 * PAD, list_h = rows * (BH + BGAP);
    float more_above = fminf(fmaxf(S.sheet.scroll, 0), 1), more_below = fminf(fmaxf(nlist - rows - S.sheet.scroll, 0), 1);
    for (int i = 0; i < nlist; i++) {
        float ry = y + (i - S.sheet.scroll) * (BH + BGAP);
        /* rows sliding out of the list fade out; the rows at its edges dim while
         * more lie beyond them */
        float out = fmaxf(y - ry, ry + BH - (y + list_h - BGAP));
        float k = 1 - fmaxf(out, 0) / (BH * 0.6f);
        if (k <= 0) continue;
        float top = fminf(fmaxf((ry - y) / (BH + BGAP), 0), 1), bottom = fminf(fmaxf((y + list_h - BGAP - ry - BH) / (BH + BGAP), 0), 1);
        k *= 1 - 0.55f * more_above * (1 - top);
        k *= 1 - 0.55f * more_below * (1 - bottom);
        draw_sheet_button(&S.sheet.it[i], i == S.sheet.index, bx, ry, bw, k, ca);
    }
    y += list_h;
    for (int i = nlist; i < S.sheet.n; i++, y += BH + BGAP) draw_sheet_button(&S.sheet.it[i], i == S.sheet.index, bx, y, bw, 1, ca);
}

/* One App Switcher card: the app's tile, large, with its name above (tvOS). */
static void draw_switcher_card(int i, float t)
{
    __typeof__(S.recents.c[0]) *c = &S.recents.c[i];
    App *a = app_by_id(c->id);
    float fly = fminf(fmaxf(c->fly, 0), 1);
    float sc = (0.84f + 0.16f * c->s) * (0.92f + 0.08f * t);
    float w = SW_W * sc, h = SW_H * sc, rad = TILE_R * w / TILE_W;
    float x = W / 2.0f + c->x * SW_PITCH - w / 2, y = SW_CY + 40 * (1 - t) - fly * 760 - h / 2;
    float alpha = t * (1 - fly) * (0.75f + 0.25f * c->s);
    int zoom = S.recents.zooming && i == S.recents.index;
    if (zoom) { /* it grows to fill the screen while the app opens */
        float z = fminf(fmaxf(S.recents.zoom, 0), 1);
        x -= x * z;
        y -= y * z;
        w += (W - w) * z;
        h += (H - h) * z;
        rad *= 1 - z;
        alpha = t;
    }
    if (alpha < 0.004f || x > W || x + w < 0) return;
    Uint8 al = (Uint8)(255 * alpha);
    if (a) {
        draw_big_tile(a, x, y, w, h, rad, al, c->s > 0.01f && !zoom);
    } else {
        r_shape(x, y, w, h, rad, 58, 58, 64, al, 0, 1, 1);
        draw_text(c->name, c->nw, c->nh, x + (w - c->nw) / 2, y + (h - c->nh) / 2, al);
    }
    if (!zoom) draw_text(c->name, c->nw, c->nh, x + 4, y - 18 - c->nh, (Uint8)(al * 0.9f));
}

static void draw_switcher(void)
{
    float t = fminf(fmaxf(S.recents.t, 0), 1);
    if (t < 0.004f) return;
    r_backdrop_image(1, (Uint8)(255 * t)); /* the home screen behind, blurred */
    r_fill(0, 0, 0, (Uint8)(110 * t));
    Uint8 ca = (Uint8)(255 * t);
    if (!S.recents.n) {
        static Tex *cap[3];
        static int cw[3], ch[3];
        static const char *const text[3] = { "Loading\xE2\x80\xA6", "No apps running", "Couldn\xE2\x80\x99t load recent apps" };
        int which = S.recents.loading ? 0 : S.recents.failed ? 2 : 1;
        if (!cap[which]) cap[which] = text_render(S.f_medium, 30, text[which], 0, &cw[which], &ch[which]);
        draw_text(cap[which], cw[which], ch[which], (W - cw[which]) / 2.0f, SW_CY - ch[which] / 2.0f, (Uint8)(ca * 0.8f));
        return;
    }
    for (int i = 0; i < S.recents.n; i++)
        if (i != S.recents.index) draw_switcher_card(i, t);
    draw_switcher_card(S.recents.index, t); /* the focused card on top */
    if (S.recents.zooming) return;
    static Tex *hint, *close_all;
    static int hw, hh, cw, ch;
    if (!hint) hint = text_render(S.f_medium, 26, "OK opens  \xC2\xB7  Up closes", 0, &hw, &hh);
    if (!close_all) close_all = text_render(S.f_medium, 31, "Close All", 0, &cw, &ch);
    draw_text(hint, hw, hh, (W - hw) / 2.0f, SW_CY + SW_H / 2.0f + 48, (Uint8)(ca * 0.62f));
    SheetItem it = { 0 };
    it.tex = close_all;
    it.w = cw;
    it.h = ch;
    it.destructive = 1;
    draw_sheet_button(&it, S.recents.close_all, (W - 360) / 2.0f, H - SAFE_Y - SHEET_BH, 360, 1, ca);
}

static void draw_toast(void)
{
    float t = S.toast_t < 0 ? 0 : S.toast_t > 1 ? 1 : S.toast_t;
    if (!S.toast_tex || t < 0.01f) return;
    float tw = S.toast_w + 72, th = S.toast_h + 40, tx = (W - tw) / 2, ty = H - SAFE_Y - th + (1 - S.toast_t) * 40;
    r_shadow(tx, ty, tw, th, th / 2, 30, 12, 0.4f * t);
    Glass g = { tx, ty, tw, th, th / 2, 1, t, -0.6f, -0.8f, 0, t, 0, 0, 0, 0 };
    glass(&g);
    draw_text(S.toast_tex, S.toast_w, S.toast_h, (W - S.toast_w) / 2, ty + 20, (Uint8)(245 * t));
}

extern int homeplus_screenshot(const char *path);

static void draw_ghost(void)
{
    if (!S.ghost.on) return;
    float t = S.ghost.t < 0 ? 0 : S.ghost.t;
    r_fill(0, 0, 0, (Uint8)(80 * t));
    float gx = S.ghost.fx + (S.ghost.px - S.ghost.fx) * t, gy = S.ghost.fy + (S.ghost.py - S.ghost.fy) * t;
    float gw = S.ghost.fw + (S.ghost.pw - S.ghost.fw) * t, gh = S.ghost.fh + (S.ghost.ph - S.ghost.fh) * t;
    float r0 = fminf(S.ghost.fw, S.ghost.fh) / 2, gr = r0 + (S.ghost.pr - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * t);
    Glass g = { gx, gy, gw, gh, gr, 1, t, -0.6f, -0.8f, 0, fminf(1, t * 1.5f), 0, 0, 0, 0 };
    glass(&g);
}

static int render(float dt, const char *shot)
{
    int anim = update(dt);
    /* the Top Shelf picture alone: what the dock and top bar refract (the icons
     * sit on the glass), and the blurred background below the dock */
    r_backdrop_begin();
    draw_shelf_art(255);
    r_backdrop_end();

    r_frame_begin(8, 8, 11);
    draw_scene(0);
    if (S.sheet.open || S.ghost.on || S.toast_t > 0.01f || S.recents.t > 0.004f) { /* overlays refract the whole scene */
        r_backdrop_begin();
        draw_scene(1);
        r_backdrop_end();
    }
    draw_switcher();
    draw_ghost();
    draw_sheet();
    draw_toast();
    if (S.black > 0.004f) r_fill(0, 0, 0, (Uint8)(S.black * 255)); /* OLED: idle fades to black */
    if (shot) homeplus_screenshot(shot);
    r_present();
    return anim;
}

/* ------------------------------------------------------------------ setup */

static int load_assets(void)
{
    char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof path, "%sassets/fonts/Inter-Medium.ttf", base ? base : "./");
    S.f_medium = font_load(path);
    snprintf(path, sizeof path, "%sassets/fonts/Inter-SemiBold.ttf", base ? base : "./");
    S.f_semi = font_load(path);
    snprintf(path, sizeof path, "%sassets/fonts/Inter-Bold.ttf", base ? base : "./");
    S.f_bold = font_load(path);
    SDL_free(base);
    if (!S.f_medium || !S.f_semi || !S.f_bold) {
        fprintf(stderr, "homeplus: fonts missing\n");
        return 0;
    }
    S.scrim_v = gfx_gradient(1, 0.0f, 0.6f);
    S.scrim_h = gfx_gradient(0, 0.72f, 0.0f); /* text over daylight artwork stays readable */
    S.scrim_top = gfx_gradient(1, 0.45f, 0.0f);
    S.heading_tex = text_render(S.f_semi, 24, "Continue Watching \xC2\xB7 Stremio", 0, &S.hw, &S.hh);
    S.rail_hint = text_render(S.f_medium, 26, "Opens in Stremio", 0, &S.rail_hint_w, &S.rail_hint_h);
    static const char *const top_names[TOP_COUNT] = { "Recent Apps", "Settings" };
    for (int i = 0; i < TOP_COUNT; i++) {
        char hint[96];
        snprintf(hint, sizeof hint, "OK opens %s  \xC2\xB7  Down returns to your apps", top_names[i]);
        S.top_label[i] = text_render(S.f_medium, 24, top_names[i], 0, &S.top_lw[i], &S.top_lh[i]);
        S.top_hint[i] = text_render(S.f_medium, 28, hint, 0, &S.top_hw[i], &S.top_hh[i]);
    }
    S.sub_tex = text_render(S.f_medium, 28, "Press OK to open \xC2\xB7 Hold for options", 0, &S.sw, &S.sh);
    S.sub_connect_tex = text_render(S.f_medium, 28, "Connect your Stremio account to see Continue Watching", 0, &S.scw, &S.sch);
    static const int kinds[TOP_COUNT] = { ICON_RECENTS, ICON_SETTINGS };
    for (int i = 0; i < TOP_COUNT; i++) {
        S.top_tex[i] = gfx_icon(kinds[i], 64); /* drawn at 34 px; 64 keeps it sharp when focused */
        S.top_w[i] = S.top_h[i] = 34;
    }
    return 1;
}

/* ------------------------------------------------------------------ idle wait */

/* LG's SDL 2.0.14 has no real blocking wait: SDL_WaitEventTimeout checks for
 * events every 10 ms, 100 wake-ups a second even while hidden. On the TV the
 * loop sleeps in poll() on the Wayland socket and on a pipe that queued events
 * (network results) write to. While hidden it still pumps SDL every 100 ms:
 * LG's SDL finishes SAM's relaunch, i.e. the Home button, in SDL_PumpEvents. */
#define HIDDEN_PUMP_MS 100

static int wake_pipe[2] = { -1, -1 };

/* the Wayland socket SDL reads events from, or -1 (not Wayland, e.g. the desktop preview) */
static int wayland_fd(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(S.win, &info) || info.subsystem != SDL_SYSWM_WAYLAND) return -1;
    void *lib = SDL_LoadObject("libwayland-client.so.0"); /* SDL has it loaded already */
    int (*get_fd)(struct wl_display *) = lib ? (int (*)(struct wl_display *))SDL_LoadFunction(lib, "wl_display_get_fd") : NULL;
    return get_fd ? get_fd(info.info.wl.display) : -1;
}

/* runs on whichever thread queues an SDL event and wakes the loop out of poll() */
static int SDLCALL wake_loop(void *userdata, SDL_Event *e)
{
    char c = 1;
    ssize_t n = write(wake_pipe[1], &c, 1); /* fails only when full: the loop is awake then */
    (void)userdata;
    (void)e;
    (void)n;
    return 0;
}

static void start_idle_wait(void)
{
    S.wl_fd = wayland_fd();
    if (S.wl_fd < 0 || pipe(wake_pipe) != 0) {
        S.wl_fd = -1;
        return;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(wake_pipe[i], F_SETFL, O_NONBLOCK);
        fcntl(wake_pipe[i], F_SETFD, FD_CLOEXEC); /* not inherited by luna-send */
    }
    SDL_AddEventWatch(wake_loop, NULL);
}

/* drew: a frame went out since the last wait (drawing can leave events read off
 * the socket but not yet handed to SDL) */
static int wait_event(SDL_Event *e, int timeout_ms, int drew)
{
    if (S.wl_fd < 0) return SDL_WaitEventTimeout(e, timeout_ms < 0 ? -1 : timeout_ms > 0 ? timeout_ms : 1);
    if (drew && SDL_PollEvent(e)) return 1;
    struct pollfd fds[2] = { { S.wl_fd, POLLIN, 0 }, { wake_pipe[0], POLLIN, 0 } };
    poll(fds, 2, timeout_ms);
    char buf[64];
    while (read(wake_pipe[0], buf, sizeof buf) > 0) {
    }
    return SDL_PollEvent(e);
}

/* How long the loop may sleep: until what's due next (a retry, a Trakt check, a
 * long press, the toast ending, the next OLED stage, closing the audio, the
 * next minute for the clock and pixel orbit while something is on screen), or
 * the hidden pump. -1 = until something happens. */
static int next_wake_ms(void)
{
    Uint32 now = SDL_GetTicks();
    Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
    int visible = !S.offscreen && S.idle_stage < 2;
    Uint32 due[8] = { S.setup_retry, S.next_retry, S.trakt_poll, S.ok_down && !S.ok_long ? S.ok_down + LONG_PRESS_MS : 0,
                      S.toast_tex && S.toast_until > now ? S.toast_until : 0,
                      S.offscreen ? 0 : S.idle_stage == 0 ? S.last_input + dim_ms : S.idle_stage == 1 ? S.last_input + off_ms : 0,
                      snd_active() ? now + 1000 : 0, visible ? now + (60000 - now % 60000) : 0 };
    int wait = -1;
    for (int i = 0; i < 8; i++) {
        if (!due[i]) continue;
        int d = (Sint32)(due[i] - now) > 0 ? (int)(due[i] - now) : 0;
        if (wait < 0 || d < wait) wait = d;
    }
    if (visible) { /* the clock's next minute */
        time_t tt = time(NULL);
        int d = (60 - (int)(tt % 60)) * 1000 + 5;
        if (wait < 0 || d < wait) wait = d;
    }
    if (S.offscreen && (wait < 0 || wait > HIDDEN_PUMP_MS)) wait = HIDDEN_PUMP_MS;
    return wait;
}

/* Another app is in front: free the big pictures except the one on show (they
 * come back on demand; ~8 MB each on this GPU). */
static void release_images(void)
{
    App *keep = item_app(S.shelf_cur);
    for (int i = 0; i < S.napps; i++) {
        App *a = &S.apps[i];
        if (a == keep || !a->art_tex) continue;
        tex_free(&a->art_tex);
        a->art_state = 0;
    }
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        if (i == S.rail_index || !it->bg_tex) continue;
        tex_free(&it->bg_tex);
        it->bg_state = 0;
    }
}

/* Another app is in front (LG's SDL may say so as HIDDEN, MINIMIZED or
 * DIDENTERBACKGROUND): stop drawing, release the audio and the big pictures. */
static void go_offscreen(void)
{
    if (S.offscreen) return;
    S.offscreen = 1;
    S.keys_until = 0;
    sw_reset(); /* an app opened from it is in front now */
    S.launching = 0;
    snd_suspend();
    release_images();
}

static void come_back(void)
{
    if (S.away) go_home(); /* back from another app (or the Home button): start at the top */
    S.away = 0;
    S.offscreen = 0;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    if (S.setup == SETUP_DONE) request_data();
    /* only a setup that stopped on an error starts over; one in progress carries on */
    else if (S.setup == SETUP_NO_HBC || S.setup == SETUP_DENIED || S.setup == SETUP_FAILED) setup_retry_now();
}

/* Any input resets the OLED idle timer. Returns 1 when it only woke a dark
 * screen, so that press does nothing else (key: its release is dropped too). */
static int wake(int key)
{
    int dark = S.black > 0.01f;
    S.last_input = SDL_GetTicks();
    S.idle_stage = 0;
    if (dark && key) S.swallow_up = 1;
    return dark;
}

/* window -> logical coordinates (the GL drawable may not be 1920x1080) */
static void to_logical(int *x, int *y)
{
    int ww, wh;
    SDL_GetWindowSize(S.win, &ww, &wh);
    if (ww > 0 && wh > 0) {
        *x = *x * W / ww;
        *y = *y * H / wh;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* LG's SDL: let the app handle Back/Exit itself instead of the system closing it. */
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "5000");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "homeplus: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    r_gl_attributes();
    S.win = SDL_CreateWindow("Home+", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                             SDL_WINDOW_OPENGL | (getenv("HOMEPLUS_SCREENSHOT") ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) | SDL_WINDOW_SHOWN);
    if (!S.win) {
        fprintf(stderr, "homeplus: window: %s\n", SDL_GetError());
        return 1;
    }
    if (!r_init(S.win, W, H)) {
        fprintf(stderr, "homeplus: renderer init failed\n");
        return 1;
    }
    if (!load_assets()) return 1;
    snd_init();

    net_start();
    S.ptr_ax = S.ptr_ay = -1;
    start_idle_wait();
    {
        int dw, dh, ww, wh;
        SDL_GL_GetDrawableSize(S.win, &dw, &dh);
        SDL_GetWindowSize(S.win, &ww, &wh);
        log_remote("native up: renderer=gles2 drawable=%dx%d window=%dx%d idle=%s", dw, dh, ww, wh, S.wl_fd >= 0 ? "poll" : "sdl");
    }
    S.shelf_cur = S.shelf_prev = S.rail_art_cur = S.rail_art_prev = -1;
    S.last_input = SDL_GetTicks();
    S.orbit_k = -1;
    setup_check(); /* loads the data once the service is up and elevated */

    const char *shot = getenv("HOMEPLUS_SCREENSHOT"); /* host preview: render then save a PNG */
    Uint32 shot_at = shot ? SDL_GetTicks() + (Uint32)atoi(getenv("HOMEPLUS_SCREENSHOT_MS") ? getenv("HOMEPLUS_SCREENSHOT_MS") : "2500") : 0;
    extern void homeplus_preview_input(Uint32 elapsed);

    int running = 1, animating = 1, drew = 1;
    Uint32 last = SDL_GetTicks(), start = last;
    while (running) {
        SDL_Event e;
        /* sleep while idle or hidden; wake on input, a queued event or what's due next */
        int got;
        if (shot || (animating && !S.offscreen)) got = SDL_PollEvent(&e);
        else if ((Sint32)(S.keys_until - SDL_GetTicks()) > 0) got = SDL_WaitEventTimeout(&e, 10);
        else got = wait_event(&e, next_wake_ms(), drew);
        drew = 0;
        int dirty = got;
        while (got) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == WEBOS_KEY_CURSOR_HIDE) { /* the pointer fell asleep: not input */
                    keys_take_over();
                    break;
                }
                S.keys_until = SDL_GetTicks() + 600;
                if (wake(1)) break;
                on_key_down(&e.key);
                break;
            case SDL_KEYUP:
                if (S.swallow_up) {
                    S.swallow_up = 0;
                    break;
                }
                on_key_up(&e.key);
                break;
            case SDL_MOUSEMOTION:
                /* the Magic Remote pointer: what it points at gets focus */
                if (wake(0)) break;
                {
                    int mx = e.motion.x, my = e.motion.y;
                    to_logical(&mx, &my);
                    if (pointer_moved(mx, my) && !S.moving && !S.ok_down) pointer_at(mx, my, 1);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (wake(0)) { S.ptr_press = 0; break; }
                if (e.button.button == SDL_BUTTON_LEFT && !S.ok_down) {
                    int mx = e.button.x, my = e.button.y, outside = 0;
                    to_logical(&mx, &my);
                    if (!pointer_moved(mx, my)) { /* the keys are in charge: it's their OK */
                        S.ptr_press = 1;
                        S.ptr_dismiss = 0;
                        S.ok_down = SDL_GetTicks();
                        S.ok_long = 0;
                        break;
                    }
                    S.ptr_press = pointer_at(mx, my, 1);
                    /* a click beside a sheet closes it, as Back would */
                    S.ptr_dismiss = !S.ptr_press && S.sheet.open && !S.sheet.locked && !S.recents.open &&
                                    (sheet_button_at(mx, my, &outside), outside);
                    if (S.ptr_press) { /* held, it opens options like a held OK */
                        S.ok_down = SDL_GetTicks();
                        S.ok_long = 0;
                    }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button != SDL_BUTTON_LEFT) break;
                if (S.ptr_press && S.ok_down) ok_release();
                else if (S.ptr_dismiss && S.sheet.open) sheet_dismiss();
                S.ptr_press = S.ptr_dismiss = 0;
                break;
            case SDL_MOUSEWHEEL:
                if (wake(0)) break;
                if (S.recents.open) sw_key(e.wheel.y < 0 ? SDLK_RIGHT : SDLK_LEFT);
                else if (S.sheet.open) {
                    if (e.wheel.y < 0 && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
                    else if (e.wheel.y > 0 && S.sheet.index > 0) S.sheet.index--;
                } else move_focus(0, e.wheel.y < 0 ? 1 : -1);
                break;
            case SDL_APP_DIDENTERBACKGROUND:
                go_offscreen();
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                come_back();
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_SHOWN ||
                    e.window.event == SDL_WINDOWEVENT_RESTORED)
                    come_back();
                else if (e.window.event == SDL_WINDOWEVENT_HIDDEN || e.window.event == SDL_WINDOWEVENT_MINIMIZED)
                    go_offscreen();
                break;
            default:
                if (e.type == net_event) {
                    Result r;
                    while (net_poll(&r)) {
                        handle_result(&r);
                        net_result_free(&r);
                    }
                }
            }
            got = SDL_PollEvent(&e);
        }

        Uint32 now = SDL_GetTicks();
        snd_idle();
        if (S.ok_down && !S.ok_long && now - S.ok_down >= LONG_PRESS_MS) {
            S.ok_long = 1;
            if (!S.sheet.open && !S.recents.open && !S.moving && S.zone == 0) {
                snd_play(SND_SELECT);
                open_app_sheet();
            }
            dirty = 1;
        }
        if (S.trakt_poll && now >= S.trakt_poll) {
            S.trakt_poll = 0;
            net_get_json("/api/trakt/status", TAG_TRAKT_STATUS, 0, 0);
        }
        if (S.setup_retry && now >= S.setup_retry) {
            S.setup_retry = 0;
            net_wake_service();
            setup_check();
        }
        if (S.next_retry && now >= S.next_retry) {
            S.next_retry = 0;
            net_get_json("/api/apps", TAG_APPS, 0, 0);
            net_get_json("/api/continue", TAG_STREMIO, 0, 0);
            if (!S.layout_loaded) net_get_json("/api/layout", TAG_LAYOUT, 0, 0);
        }
        if (shot) homeplus_preview_input(now - start);
        if (S.toast_tex && now < S.toast_until + 100) dirty = 1;
        {
            time_t tt = time(NULL);
            struct tm *lt = localtime(&tt);
            if (lt->tm_hour * 60 + lt->tm_min != S.clock_min) dirty = 1;
        }
        /* OLED: idle stages, and a 1 px step of the pixel orbit every minute (within +-4 px) */
        {
            Uint32 idle = now - S.last_input;
            Uint32 dim_ms = S.idle_dim_ms ? S.idle_dim_ms : IDLE_DIM_MS, off_ms = S.idle_off_ms ? S.idle_off_ms : IDLE_OFF_MS;
            int stage = idle >= off_ms ? 2 : idle >= dim_ms ? 1 : 0;
            if (stage != S.idle_stage && !shot) {
                S.idle_stage = stage;
                dirty = 1;
            }
            int k = (int)(now / 60000);
            if (k != S.orbit_k) {
                S.orbit_k = k;
                r_set_offset(shot ? 0 : roundf(4 * sinf(k * 0.61f)), shot ? 0 : roundf(4 * sinf(k * 0.37f + 1.0f)));
                dirty = 1;
            }
        }
        /* nothing to draw while hidden behind another app or once the screen is black */
        if ((S.offscreen || (S.idle_stage == 2 && S.black >= 0.999f)) && !shot) dirty = animating = 0;

        if (dirty || animating || shot) {
            float dt = (now - last) / 1000.0f;
            if (dt > 0.1f) dt = 0.1f;
            int take = shot && now >= shot_at;
            animating = render(dt, take ? shot : NULL);
            drew = 1;
            if (take) running = 0;
        }
        last = now;
    }
    net_stop();
    r_shutdown();
    SDL_Quit();
    return 0;
}
