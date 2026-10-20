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
#include <time.h>

#include <SDL.h>

#include "cJSON.h"
#include "gfx.h"
#include "net.h"

/* ------------------------------------------------------------------ layout */

/* Spacing system (Apple's tvOS layout guidance): an 80/60 px safe area, five
 * columns of 5:3 tiles 40 px apart, containers pad their content 40 px and keep
 * their corners concentric with it (outer radius = inner radius + padding). */
#define W 1920
#define H 1080
#define SAFE_X 80
#define SAFE_Y 60
#define COLS 5
#define TILE_W 304
#define TILE_H 182
#define TILE_R 24
#define GAP_X 40
#define PAD 40
#define LEFT (SAFE_X + PAD)            /* first column */
#define DOCK_Y 712                     /* glass platform behind the top row */
#define DOCK_H (TILE_H + 2 * PAD)
#define GRID_TOP (DOCK_Y + PAD)
#define ROW_H (TILE_H + 100)           /* rows at least 100 px apart; labels sit in the gap */
#define FOCUS_SCALE 1.10f
#define LONG_PRESS_MS 650
#define RAIL_CARD_W TILE_W             /* Continue Watching cards sit on the same columns */
#define RAIL_CARD_H 171                /* 16:9 */
#define RAIL_GAP GAP_X
#define RAIL_Y 486
#define RAIL_VISIBLE 5
#define BAR 56                         /* clock capsule and round top-bar buttons */
#define BAR_GAP 16
#define TEXT_STATIC 218                /* OLED: static text stays at ~85% white */

#define MAX_APPS 128
#define MAX_RAIL 16
#define MAX_SHEET 24
#define ID_LEN 160

enum { TAG_APPS = 1, TAG_LAYOUT, TAG_STREMIO, TAG_ICON, TAG_ART, TAG_RAIL_THUMB, TAG_RAIL_BG, TAG_LAUNCH, TAG_WAKE,
       TAG_SYSTEM, TAG_TRAKT_LINK, TAG_TRAKT_STATUS, TAG_RUNNING, TAG_CLOSED, TAG_HOME, TAG_HOME_SET };

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
    int icon_state, art_state; /* 0 idle, 1 loading, 2 ready, 3 failed */
    int lw, lh, bw, bh;
    float s, sv; /* focus animation 0..1, and its spring velocity */
} App;

typedef struct {
    char id[64], type[16], name[200], link[400], bg[40], thumb[40];
    float progress;
    int season, episode, remaining;
    Tex *thumb_tex, *bg_tex, *name_tex, *title_tex, *meta_tex;
    int thumb_state, bg_state;
    int nw, nh, tw, th, mw, mh;
    float s, sv;
} RailItem;

typedef enum { ACT_NONE, ACT_MOVE, ACT_FRONT, ACT_HIDE, ACT_UNHIDE, ACT_UNHIDE_ALL, ACT_STREMIO_HELP, ACT_CANCEL,
               ACT_LAUNCH_ID, ACT_TRAKT_LINK, ACT_TRAKT_UNLINK,
               ACT_RECENT_APP, ACT_RECENTS, ACT_CLOSE_ID, ACT_CLOSE_ALL, ACT_HOME_MODE } Action;

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
    Tex *glow, *scrim_v, *scrim_h;

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
    int noflash_active, noflash_available; /* available: LG's home is a web app we can replace */
    Uint32 trakt_poll; /* while showing a Trakt code: next status check */
    int moving;
    float scroll, scroll_v, scroll_target; /* grid y offset */
    float scrolled;              /* 0 at the top row, 1 below it */
    int shelf_cur, shelf_prev;   /* item indices shown in the Top Shelf */
    float shelf_a;               /* crossfade: 0 = prev, 1 = cur */
    int rail_art_cur, rail_art_prev;
    float rail_art_a, rail_shift, rail_shift_v;

    struct {
        int open, index, n, tw, th;
        float t, tv;          /* morph: 0 = still the button it grew from, 1 = open */
        float fx, fy, fw, fh; /* that button's rect */
        Action back; /* what Back does besides closing: ACT_RECENTS goes up to the Recents list */
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
    Uint32 next_retry;
    int logged_keys;
    /* OLED protection: idle stages, fades, pixel orbit */
    Uint32 last_input;
    int idle_stage, swallow_up, offscreen, orbit_k; /* offscreen: another app is in front */
    float dim, black;
    /* motion: the focused element sways briefly and its highlight follows a moving light */
    float sway_t;           /* seconds since focus last moved */
    int focus_key;          /* changes whenever focus moves */
    float ptr_x, ptr_y;     /* light direction from the Magic Remote pointer */
    Uint32 ptr_at;          /* when the pointer last moved */
    float light_x, light_y; /* light for focused glass and rims this frame */
    float toast_t, toast_v; /* toast rising in */
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
        strncat(sig, jstr(v, "id"), sizeof sig - strlen(sig) - 2);
        strncat(sig, ",", sizeof sig - strlen(sig) - 1);
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
            net_get_image(a->icon, TAG_ICON, i, S.generation, 1);
        }
    }
    S.shelf_cur = S.shelf_prev = -1;
}

static void make_rail_texts(RailItem *it)
{
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
        strncat(meta, left, sizeof meta - strlen(meta) - 1);
    }
    it->name_tex = text_render(S.f_semi, 20, it->name, RAIL_CARD_W - 32, &it->nw, &it->nh);
    it->title_tex = text_render(S.f_bold, 66, it->name, 1100, &it->tw, &it->th);
    it->meta_tex = text_render(S.f_medium, 27, meta, 1100, &it->mw, &it->mh);
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
        S.heading_tex = text_render(S.f_semi, 22, heading, 0, &S.hw, &S.hh);
    }
    cJSON *arr = cJSON_GetObjectItem(o, "items"), *v;
    char sig[2048] = "";
    cJSON_ArrayForEach(v, arr) {
        char part[96];
        snprintf(part, sizeof part, "%s:%.3f,", jstr(v, "id"), jnum(v, "progress"));
        strncat(sig, part, sizeof sig - strlen(sig) - 1);
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
        snprintf(it->link, sizeof it->link, "%s", jstr(v, "deepLink"));
        snprintf(it->bg, sizeof it->bg, "%s", jstr(v, "background"));
        snprintf(it->thumb, sizeof it->thumb, "%s", jstr(v, "thumbnail"));
        it->progress = (float)jnum(v, "progress");
        it->season = (int)jnum(v, "season");
        it->episode = (int)jnum(v, "episode");
        it->remaining = (int)jnum(v, "remainingMin");
        make_rail_texts(it);
        if (it->thumb[0]) {
            it->thumb_state = 1;
            net_get_image(it->thumb, TAG_RAIL_THUMB, S.nrail, S.rail_generation, 0);
        }
        S.nrail++;
    }
    cJSON_Delete(o);
    if (S.rail_index >= S.nrail) S.rail_index = 0;
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
}

static void sheet_open(const char *title);
static void open_recents_sheet(const char *json);
static void sheet_close(void);
static void sheet_add(const char *label, Action act, int arg, int destructive);

/* ------------------------------------------------------------------ results */

static void handle_result(Result *r)
{
    switch (r->tag) {
    case TAG_WAKE:
        break;
    case TAG_LAYOUT:
        if (r->status == 200) parse_layout(r->body);
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
        if (r->status == 200) open_recents_sheet(r->body);
        break;
    case TAG_CLOSED:
        toast(r->status == 200 ? "Closed" : "Couldn\xE2\x80\x99t close that app", 2000);
        net_get_json("/api/running", TAG_RUNNING, 0, 0); /* back to the list to close more */
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
            if (S.sheet.open && S.sheet_big) sheet_close();
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
                a->icon_tex = r_tex(r->rgba, r->w, r->h);
                a->icon_state = 2;
                if (r->sampled) {
                    a->fullbleed = r->fullbleed;
                    if (r->fullbleed || !a->has_col) {
                        memcpy(a->col, r->rgb, 3);
                        if (!r->fullbleed) a->has_col = 1;
                    }
                }
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

static void launch(const char *id, const char *content_target)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", id);
    if (content_target && content_target[0]) {
        cJSON *p = cJSON_AddObjectToObject(o, "params");
        cJSON_AddStringToObject(p, "contentTarget", content_target);
    }
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
    S.trakt_poll = 0;
    for (int i = 0; i < S.sheet.n; i++) tex_free(&S.sheet.it[i].tex);
    S.sheet.open = 0;
    S.sheet.n = 0;
    S.sheet.back = ACT_NONE;
}

/* rect of the focused element: where a sheet grows from */
static void focus_rect(float *x, float *y, float *w, float *h)
{
    if (S.zone == ZONE_TOP) {
        float right = W - SAFE_X - (S.clock_w + 48);
        *x = right - (TOP_COUNT - S.top_index) * (BAR + BAR_GAP);
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

static void sheet_open(const char *title)
{
    int was_open = S.sheet.open;
    sheet_close();
    if (was_open) {
        S.sheet.t = 1; /* swapping one sheet for another: no morph */
        S.sheet.tv = 0;
    } else {
        S.sheet.t = S.sheet.tv = 0;
        focus_rect(&S.sheet.fx, &S.sheet.fy, &S.sheet.fw, &S.sheet.fh);
    }
    S.sheet.open = 1;
    S.sheet.index = 0;
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

static void sheet_add_id(const char *label, const char *id)
{
    sheet_add(label, ACT_LAUNCH_ID, 0, 0);
    copy_id(S.sheet.it[S.sheet.n - 1].id, id);
}

static const char *title_for(const char *id)
{
    for (int i = 0; i < S.napps; i++) if (!strcmp(S.apps[i].id, id)) return S.apps[i].title;
    return id;
}

/* Control Center: recent (running) apps -> open or close */
static void open_recents_sheet(const char *json)
{
    cJSON *o = cJSON_Parse(json), *v;
    sheet_open("Recent Apps");
    int n = 0;
    cJSON *list = o ? cJSON_GetObjectItem(o, "apps") : NULL;
    cJSON_ArrayForEach(v, list) {
        if (n >= MAX_RUNNING) break;
        sheet_add(jstr(v, "title"), ACT_RECENT_APP, 0, 0);
        copy_id(S.sheet.it[S.sheet.n - 1].id, jstr(v, "id"));
        n++;
    }
    cJSON_Delete(o);
    if (n) sheet_add("Close All", ACT_CLOSE_ALL, 0, 1);
    else sheet_add("No apps running", ACT_CANCEL, 0, 0);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

static void open_recent_app_sheet(const char *id)
{
    sheet_open(title_for(id));
    sheet_add("Open", ACT_LAUNCH_ID, 0, 0);
    copy_id(S.sheet.it[S.sheet.n - 1].id, id);
    sheet_add("Close", ACT_CLOSE_ID, 0, 1);
    copy_id(S.sheet.it[S.sheet.n - 1].id, id);
    sheet_add("Back", ACT_RECENTS, 0, 0);
    S.sheet.back = ACT_RECENTS;
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

/* Control Center: TV settings + accounts */
static void open_settings_sheet(void)
{
    sheet_open("Settings");
    for (int i = 0; i < S.nsettings; i++) sheet_add_id(S.settings[i].label, S.settings[i].id);
    /* no-flash needs LG's home to be a web app; on TVs where it's native (the C3)
     * Home+ always uses standard mode, so there's nothing to switch */
    if (S.noflash_available)
        sheet_add(!strcmp(S.home_mode, "watch") ? "Home Button: Standard  (switch to No-flash)"
                                                 : "Home Button: No-flash  (switch to Standard)", ACT_HOME_MODE, 0, 0);
    if (S.trakt_connected) sheet_add("Disconnect Trakt", ACT_TRAKT_UNLINK, 0, 1);
    else sheet_add("Connect Trakt", ACT_TRAKT_LINK, 0, 0);
    if (!S.rail_stremio) sheet_add("Connect Stremio", ACT_STREMIO_HELP, 0, 0);
    sheet_add("Cancel", ACT_CANCEL, 0, 0);
}

/* Control Center: what a top-bar button (or its colour key) opens */
static void open_top(int which)
{
    if (which == TOP_RECENTS) net_get_json("/api/running", TAG_RUNNING, 0, 0); /* opens when the list arrives */
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
            copy_id(S.hidden[S.nhidden++], a->id);
            save_layout();
            rebuild_items();
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
        toast("Connect Stremio from your Mac: luna://com.atvlauncher.app.service/stremioLogin (see README)", 8000);
        break;
    case ACT_RECENTS: open_top(TOP_RECENTS); break;
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
        if (a && S.rail_index < S.nrail) launch(a->id, S.rail[S.rail_index].link);
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
    if (a) launch(a->id, NULL);
}

static void move_focus(int dx, int dy)
{
    if (S.zone == ZONE_TOP) {
        if (dy > 0) S.zone = rail_active() ? ZONE_RAIL : ZONE_GRID;
        else if (dx && S.top_index + dx >= 0 && S.top_index + dx < TOP_COUNT) S.top_index += dx;
        return;
    }
    if (S.zone == ZONE_RAIL) {
        if (dy > 0) S.zone = ZONE_GRID;
        else if (dy < 0) { S.zone = ZONE_TOP; S.top_index = TOP_COUNT - 1; }
        else if (dx && S.rail_index + dx >= 0 && S.rail_index + dx < S.nrail) S.rail_index += dx;
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
            /* up from the top row: into the Stremio shelf, else the top bar (Recents / Settings) */
            if (!S.moving) {
                if (rail_active()) S.zone = ZONE_RAIL;
                else { S.zone = ZONE_TOP; S.top_index = TOP_COUNT - 1; }
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

static void on_key_down(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if (k == WEBOS_KEY_CURSOR_SHOW || k == WEBOS_KEY_CURSOR_HIDE) return;
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
    if (S.sheet.open) {
        if (k == SDLK_UP && S.sheet.index > 0) S.sheet.index--;
        else if (k == SDLK_DOWN && S.sheet.index < S.sheet.n - 1) S.sheet.index++;
        else if (is_back(k)) {
            Action back = S.sheet.back;
            sheet_close();
            run_action(back, 0); /* one level up (an app's Recents sheet -> the list), else just closed */
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
            else S.focus = 0;
        }
    }
}

static void on_key_up(SDL_KeyboardEvent *e)
{
    SDL_Keycode k = remote_key(&e->keysym);
    if ((k != SDLK_RETURN && k != SDLK_KP_ENTER) || !S.ok_down) return;
    int was_long = S.ok_long;
    S.ok_down = 0;
    if (was_long) return;
    if (S.sheet.open) {
        SheetItem it = S.sheet.it[S.sheet.index];
        sheet_close();
        if (it.act == ACT_LAUNCH_ID) launch(it.id, NULL);
        else if (it.act == ACT_RECENT_APP) open_recent_app_sheet(it.id);
        else if (it.act == ACT_CLOSE_ID) close_apps(it.id);
        else if (it.act == ACT_CLOSE_ALL) close_apps(NULL);
        else run_action(it.act, it.arg);
    } else {
        activate();
    }
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


/* ------------------------------------------------------------------ animation */

/* OLED protection: after a while without input the screen dims (all but the
 * focused tile, like tvOS), then fades to black and stops drawing. */
#define IDLE_DIM_MS (2 * 60 * 1000)
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
    spring(&S.scroll, &S.scroll_v, S.scroll_target, 0.5f, 1.0f, dt, &anim);

    if (rail_active()) {
        RailItem *cur = S.rail_index < S.nrail ? &S.rail[S.rail_index] : NULL;
        /* background art: lazy-load the focused one, crossfade between items */
        if (cur && cur->bg[0] && cur->bg_state == 0) {
            cur->bg_state = 1;
            net_get_image(cur->bg, TAG_RAIL_BG, S.rail_index, S.rail_generation, 0);
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
        float shift = (float)(S.rail_index > RAIL_VISIBLE - 1 ? S.rail_index - (RAIL_VISIBLE - 1) : 0) * (RAIL_CARD_W + RAIL_GAP);
        spring(&S.rail_shift, &S.rail_shift_v, shift, 0.45f, 1.0f, dt, &anim);
        for (int i = 0; i < S.nrail; i++)
            spring(&S.rail[i].s, &S.rail[i].sv, S.zone == ZONE_RAIL && i == S.rail_index ? 1.0f : 0.0f, 0.35f, 0.7f, dt, &anim);
    } else {
        int cur = S.focus < COLS ? S.focus : S.shelf_cur;
        if (cur != S.shelf_cur) {
            S.shelf_prev = S.shelf_cur;
            S.shelf_cur = cur;
            S.shelf_a = 0;
        }
        approach(&S.shelf_a, 1, 9, dt, &anim);
        App *ca = item_app(S.shelf_cur);
        if (ca && ca->art[0] && ca->art_state == 0) {
            ca->art_state = 1;
            net_get_image(ca->art, TAG_ART, S.items[S.shelf_cur], S.generation, 0);
        }
    }
    for (int i = 0; i < S.nitems; i++) {
        App *a = item_app(i);
        spring(a ? &a->s : &special_s, a ? &a->sv : &special_v, i == S.focus && S.zone == ZONE_GRID ? 1.0f : 0.0f, 0.35f, 0.7f, dt, &anim);
    }
    for (int i = 0; i < TOP_COUNT; i++)
        spring(&S.top_s[i], &S.top_v[i], S.zone == ZONE_TOP && S.top_index == i ? 1.0f : 0.0f, 0.35f, 0.7f, dt, &anim);
    if (S.moving) anim = 1; /* wobble */
    if (S.sheet.open) spring(&S.sheet.t, &S.sheet.tv, 1, 0.42f, 0.82f, dt, &anim);
    spring(&S.toast_t, &S.toast_v, toast_on() ? 1.0f : 0.0f, 0.4f, 0.85f, dt, &anim);

    /* when focus lands the element sways for a moment and its highlight follows
     * a moving light (from the pointer if it moved lately), then it settles */
    int key = S.zone * 100000 + S.focus * 1000 + S.top_index * 100 + S.rail_index;
    if (key != S.focus_key) {
        S.focus_key = key;
        S.sway_t = 0;
    }
    float bx = -0.6f, by = -0.8f;
    if (S.ptr_at && SDL_GetTicks() - S.ptr_at < 3000) {
        bx = S.ptr_x;
        by = S.ptr_y;
    }
    float amp = 0;
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
static void panel(int backdrop, float x, float y, float w, float h, float radius, float legible, float focus, float alpha)
{
    if (alpha <= 0.004f) return;
    if (backdrop) {
        r_shape(x, y, w, h, radius, 255, 255, 255, (Uint8)(14 * alpha), 0, 1, 1);
        return;
    }
    int lit = focus > 0.01f; /* focused glass catches the moving light */
    Glass g = { x, y, w, h, radius, legible, 1.0f, lit ? S.light_x : -0.6f, lit ? S.light_y : -0.8f, focus, alpha, 0, 0, 0, 0 };
    r_glass(&g);
}

static void ensure_app_texts(App *a)
{
    if (!a->label) a->label = text_render(S.f_medium, 26, a->title, TILE_W + 60, &a->lw, &a->lh);
    if (!a->big) a->big = text_render(S.f_bold, 84, a->title, 1000, &a->bw, &a->bh);
}

static void draw_app_shelf(App *a, Uint8 alpha)
{
    if (!a || !alpha) return;
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    if (a->art_tex) {
        r_cover(a->art_tex, 0, 0, W, H, alpha, 0);
        r_image(S.scrim_v, 0, H * 0.55f, W, H * 0.45f, 255, 255, 255, alpha, 0);
        r_image(S.scrim_h, 0, 0, W * 0.5f, H, 255, 255, 255, alpha, 0);
    } else {
        /* a tiny glow texture, stretched by the GPU */
        r_image(S.glow, W * 0.77f - 900, H * 0.33f - 700, 1800, 1400, r, g, b, (Uint8)(alpha * 0.8f), 0);
        if (a->icon_tex) {
            float ls = 400, lx = W - SAFE_X - PAD - ls, ly = SAFE_Y + 60;
            r_image(a->icon_tex, lx + 30, ly + 30, ls - 60, ls - 60, 255, 255, 255, alpha, 0);
            r_shape(lx, ly, ls, ls, 96, 255, 255, 255, (Uint8)(alpha * 0.07f), 0, 1, 1);
            r_shape(lx, ly, ls, ls, 96, 255, 255, 255, (Uint8)(alpha * 0.55f), 2.0f, 1.0f, 0.3f);
        }
    }
    ensure_app_texts(a);
    draw_text(a->big, a->bw, a->bh, LEFT - 4, 340, alpha);
    Tex *sub = is_stremio(a) && !S.rail_connected ? S.sub_connect_tex : S.sub_tex;
    int sw = sub == S.sub_tex ? S.sw : S.scw, sh = sub == S.sub_tex ? S.sh : S.sch;
    draw_text(sub, sw, sh, LEFT, 340 + a->bh + 10, (Uint8)(alpha * 0.72f));
}

static void draw_rail_shelf(Uint8 alpha, int backdrop)
{
    RailItem *cur = S.rail_index < S.nrail ? &S.rail[S.rail_index] : NULL;
    if (!cur) return;
    RailItem *prev = S.rail_art_prev >= 0 && S.rail_art_prev < S.nrail ? &S.rail[S.rail_art_prev] : NULL;
    Tex *pt = prev ? (prev->bg_tex ? prev->bg_tex : prev->thumb_tex) : NULL;
    Tex *ct = cur->bg_tex ? cur->bg_tex : cur->thumb_tex;
    r_cover(pt, 0, 0, W, H, alpha, 0);
    r_cover(ct, 0, 0, W, H, (Uint8)(alpha * S.rail_art_a), 0);
    r_image(S.scrim_v, 0, H * 0.55f, W, H * 0.45f, 255, 255, 255, alpha, 0);
    r_image(S.scrim_h, 0, 0, W * 0.5f, H, 255, 255, 255, alpha, 0);

    /* heading capsule, title, meta */
    float hy = SAFE_Y + 58, hh = 46;
    panel(backdrop, LEFT, hy, S.hw + 44, hh, hh / 2, 1, 0, alpha / 255.0f);
    draw_text(S.heading_tex, S.hw, S.hh, LEFT + 22, hy + (hh - S.hh) / 2.0f, alpha);
    draw_text(cur->title_tex, cur->tw, cur->th, LEFT - 3, hy + hh + 26, alpha);
    draw_text(cur->meta_tex, cur->mw, cur->mh, LEFT, hy + hh + 26 + cur->th + 8, (Uint8)(alpha * 0.8f));

    /* cards, on the grid's columns */
    for (int i = 0; i < S.nrail; i++) {
        RailItem *it = &S.rail[i];
        float sc = 1 + 0.10f * it->s;
        float cw = RAIL_CARD_W * sc, ch = RAIL_CARD_H * sc, rad = 20 * sc;
        float x = LEFT + i * (RAIL_CARD_W + RAIL_GAP) - S.rail_shift - (cw - RAIL_CARD_W) / 2;
        float y = RAIL_Y - (ch - RAIL_CARD_H) / 2 - 6 * it->s;
        if (x > W || x + cw < 0) continue;
        if (it->s > 0.01f) {
            float dx, dy;
            sway(&dx, &dy);
            x += dx * it->s;
            y += dy * it->s;
        }
        Uint8 ca = (Uint8)(alpha * (0.75f + 0.25f * it->s));
        if (it->s > 0.01f && !backdrop) r_shadow(x, y, cw, ch, rad, 26, 14, 0.5f * it->s * alpha / 255.0f);
        if (it->thumb_tex) r_cover(it->thumb_tex, x, y, cw, ch, ca, rad);
        else r_shape(x, y, cw, ch, rad, 255, 255, 255, (Uint8)(ca * 0.08f), 0, 1, 1);
        r_image(S.scrim_v, x, y + ch * 0.45f, cw, ch * 0.55f, 255, 255, 255, ca, rad);
        r_shape(x, y, cw, ch, rad, 255, 255, 255, (Uint8)(ca * 0.3f * (1 - it->s)), 1.5f, 1.0f, 0.25f);
        if (it->s > 0.01f) r_rim(x, y, cw, ch, rad, 1.5f + 1.5f * it->s, (Uint8)(ca * 0.92f * it->s), S.light_x, S.light_y);
        draw_text(it->name_tex, it->nw, it->nh, x + 16 * sc, y + ch - (22 + it->nh) * sc, ca);
        float bx = x + 16 * sc, bw = cw - 32 * sc, by = y + ch - 17 * sc, bh = 5 * sc; /* progress */
        r_shape(bx, by, bw, bh, bh / 2, 255, 255, 255, (Uint8)(ca * 0.28f), 0, 1, 1);
        if (it->progress > 0) r_shape(bx, by, bw * it->progress, bh, bh / 2, 255, 255, 255, ca, 0, 1, 1);
    }
}

static void draw_shelf(int backdrop)
{
    Uint8 alpha = (Uint8)(255 * (1 - S.scrolled));
    if (!alpha) return;
    if (rail_active()) {
        draw_rail_shelf(alpha, backdrop);
    } else {
        draw_app_shelf(item_app(S.shelf_prev), (Uint8)(alpha * (1 - S.shelf_a)));
        draw_app_shelf(item_app(S.shelf_cur), (Uint8)(alpha * S.shelf_a));
    }
}

/* Recents / Settings: round glass buttons left of the clock, BAR_GAP apart */
static void draw_top_bar(float right, Uint8 alpha, int backdrop)
{
    float x = right;
    for (int i = TOP_COUNT - 1; i >= 0; i--) {
        float k = S.top_s[i], d = BAR * (1 + 0.14f * k);
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
        float is = S.top_w[i] * (1 + 0.14f * k);
        r_image(S.top_tex[i], cx - is / 2, cy - is / 2, is, is, 255, 255, 255, (Uint8)(alpha * (0.85f + 0.15f * k)), 0);
    }
}

static void draw_clock(Uint8 alpha, int backdrop)
{
    if (!alpha) return;
    float pw = S.clock_w + 48, x = W - SAFE_X - pw;
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
    float w = TILE_W * sc, h = TILE_H * sc, rad = TILE_R * sc;
    float x = bx - (w - TILE_W) / 2, y = by - (h - TILE_H) / 2 - 6 * k;
    if (S.moving && focused) x += sinf(SDL_GetTicks() / 40.0f) * 2.0f; /* gentle wobble while moving */
    if (k > 0.01f) {
        float dx, dy;
        sway(&dx, &dy);
        x += dx * k;
        y += dy * k;
    }
    Uint8 r, g, b;
    app_color(a, &r, &g, &b);
    if (k > 0.01f && !backdrop) r_shadow(x, y, w, h, rad, 28, 16, 0.5f * k);
    r_shape(x, y, w, h, rad, r, g, b, a ? (a->fullbleed ? 255 : 153) : 18, 0, 1, 1);
    if (a && a->icon_tex) {
        float is = (a->fullbleed ? 156 : 124) * sc;
        r_image(a->icon_tex, x + (w - is) / 2, y + (h - is) / 2, is, is, 255, 255, 255, 255, 0);
    } else if (a) {
        ensure_app_texts(a);
        if (a->label) draw_text(a->label, a->lw, a->lh, x + (w - a->lw) / 2, y + (h - a->lh) / 2, 230);
    } else {
        static Tex *ht;
        static int hw, hh;
        if (!ht) ht = text_render(S.f_semi, 28, "\xE2\x80\xA2 \xE2\x80\xA2 \xE2\x80\xA2   Hidden", 0, &hw, &hh);
        draw_text(ht, hw, hh, x + (w - hw) / 2, y + (h - hh) / 2, 205);
    }
    /* rim: a focused tile turns to glass and catches a light that moves as it sways */
    r_shape(x, y, w, h, rad, 255, 255, 255, (Uint8)(90 * (1 - k) + (S.moving && focused ? 35 : 0)), 1.5f, 1.0f, 0.2f);
    if (k > 0.01f) r_rim(x, y, w, h, rad, 1.5f + 1.5f * k, (Uint8)(235 * k), S.light_x, S.light_y);
    if (a && k > 0.01f && !backdrop) {
        ensure_app_texts(a);
        float ly = i < COLS ? DOCK_Y - 160 * S.scrolled + DOCK_H + 14 : by + TILE_H + 22; /* top row: under the dock */
        draw_text(a->label, a->lw, a->lh, bx + (TILE_W - a->lw) / 2, ly, (Uint8)(TEXT_STATIC * k));
    }
}

/* Everything below the overlays. backdrop: the copy the glass refracts. */
static void draw_scene(int backdrop)
{
    draw_shelf(backdrop);
    float dock_y = DOCK_Y - 160 * S.scrolled, dock_a = 1 - S.scrolled;
    if (!backdrop && dock_a > 0.004f) r_shadow(SAFE_X, dock_y, W - 2 * SAFE_X, DOCK_H, TILE_R + PAD, 40, 14, 0.3f * dock_a);
    panel(backdrop, SAFE_X, dock_y, W - 2 * SAFE_X, DOCK_H, TILE_R + PAD, 0, 0, dock_a); /* icons on it are opaque: stays clear */
    draw_clock((Uint8)(255 * (1 - S.scrolled)), backdrop);
    for (int i = 0; i < S.nitems; i++)
        if (i != S.focus) draw_tile(i, backdrop);
    if (!backdrop && S.dim > 0.004f) r_fill(0, 0, 0, (Uint8)(S.dim * 115)); /* OLED: idle dims all but the focused tile */
    if (S.focus < S.nitems) draw_tile(S.focus, backdrop);
}

static void draw_sheet(void)
{
    if (!S.sheet.open) return;
    float t = S.sheet.t, tc = t < 0 ? 0 : t > 1 ? 1 : t;
    r_fill(0, 0, 0, (Uint8)(80 * tc));
    const float BH = 80, BGAP = 16, rad = BH / 2 + PAD; /* corners concentric with the buttons */
    float big = S.sheet_big ? S.sheet_big_h + 24 : 0;
    float pw = 720;
    if (S.sheet.tw + 2 * PAD > pw) pw = S.sheet.tw + 2 * PAD;
    float ph = PAD + S.sheet.th + 24 + big + S.sheet.n * (BH + BGAP) - BGAP + PAD;
    float px = (W - pw) / 2, py = (H - ph) / 2;
    /* grows out of the button it came from on a spring, and its lensing
     * materialises as it does (Apple: glass appears by bending light) */
    float gx = S.sheet.fx + (px - S.sheet.fx) * t, gy = S.sheet.fy + (py - S.sheet.fy) * t;
    float gw = S.sheet.fw + (pw - S.sheet.fw) * t, gh = S.sheet.fh + (ph - S.sheet.fh) * t;
    float r0 = fminf(S.sheet.fw, S.sheet.fh) / 2, gr = r0 + (rad - r0) * t;
    if (gw < 1 || gh < 1) return;
    r_shadow(gx, gy, gw, gh, gr, 60, 24, 0.45f * tc);
    Glass g = { gx, gy, gw, gh, gr, 1, tc, -0.6f, -0.8f, 0, 1, 0, 0, 0, 0 };
    r_glass(&g);
    float c = (t - 0.55f) / 0.45f; /* content fades in once the panel is nearly there */
    Uint8 ca = (Uint8)(255 * (c < 0 ? 0 : c > 1 ? 1 : c));
    if (!ca) return;
    draw_text(S.sheet.title, S.sheet.tw, S.sheet.th, px + (pw - S.sheet.tw) / 2, py + PAD, (Uint8)(200 * ca / 255));
    float y = py + PAD + S.sheet.th + 24;
    if (S.sheet_big) {
        draw_text(S.sheet_big, S.sheet_big_w, S.sheet_big_h, px + (pw - S.sheet_big_w) / 2, y, ca);
        y += big;
    }
    for (int i = 0; i < S.sheet.n; i++, y += BH + BGAP) {
        SheetItem *it = &S.sheet.it[i];
        float bx = px + PAD, bw = pw - 2 * PAD, tx = bx + (bw - it->w) / 2, ty = y + (BH - it->h) / 2;
        if (i == S.sheet.index) { /* tvOS: the focused button turns opaque white with a dark label */
            r_shadow(bx, y, bw, BH, BH / 2, 18, 8, 0.4f * ca / 255);
            r_shape(bx, y, bw, BH, BH / 2, 245, 245, 247, ca, 0, 1, 1);
            if (it->destructive) r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 215, 40, 40, ca, 0);
            else r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 28, 28, 30, ca, 0);
        } else {
            r_shape(bx, y, bw, BH, BH / 2, 255, 255, 255, (Uint8)(26 * ca / 255), 0, 1, 1);
            if (it->destructive) r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 255, 138, 128, (Uint8)(235 * ca / 255), 0);
            else r_image(it->tex, tx, ty, (float)it->w, (float)it->h, 255, 255, 255, (Uint8)(235 * ca / 255), 0);
        }
    }
}

static void draw_toast(void)
{
    float t = S.toast_t < 0 ? 0 : S.toast_t > 1 ? 1 : S.toast_t;
    if (!S.toast_tex || t < 0.01f) return;
    float tw = S.toast_w + 72, th = S.toast_h + 40, tx = (W - tw) / 2, ty = H - SAFE_Y - th + (1 - S.toast_t) * 40;
    r_shadow(tx, ty, tw, th, th / 2, 30, 12, 0.4f * t);
    Glass g = { tx, ty, tw, th, th / 2, 1, t, -0.6f, -0.8f, 0, t, 0, 0, 0, 0 };
    r_glass(&g);
    draw_text(S.toast_tex, S.toast_w, S.toast_h, (W - S.toast_w) / 2, ty + 20, (Uint8)(245 * t));
}

extern int homeplus_screenshot(const char *path);

static int render(float dt, const char *shot)
{
    int anim = update(dt);
    /* what the dock and top bar refract: the Top Shelf only (the icons sit on the glass) */
    r_backdrop_begin();
    draw_shelf(1);
    r_backdrop_end();

    r_frame_begin(8, 8, 11);
    draw_scene(0);
    if (S.sheet.open || S.toast_t > 0.01f) { /* overlays refract the whole scene */
        r_backdrop_begin();
        draw_scene(1);
        r_backdrop_end();
    }
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
    S.glow = gfx_glow(192, 192);
    S.scrim_v = gfx_gradient(1, 0.0f, 0.6f);
    S.scrim_h = gfx_gradient(0, 0.5f, 0.0f);
    S.heading_tex = text_render(S.f_semi, 22, "Continue Watching \xC2\xB7 Stremio", 0, &S.hw, &S.hh);
    S.sub_tex = text_render(S.f_medium, 28, "Press OK to open \xC2\xB7 Hold for options", 0, &S.sw, &S.sh);
    S.sub_connect_tex = text_render(S.f_medium, 28, "Connect your Stremio account to see Continue Watching", 0, &S.scw, &S.sch);
    static const int kinds[TOP_COUNT] = { ICON_RECENTS, ICON_SETTINGS };
    for (int i = 0; i < TOP_COUNT; i++) {
        S.top_tex[i] = gfx_icon(kinds[i], 64); /* drawn at 34 px; 64 keeps it sharp when focused */
        S.top_w[i] = S.top_h[i] = 34;
    }
    return 1;
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

    net_start();
    {
        int dw, dh, ww, wh;
        SDL_GL_GetDrawableSize(S.win, &dw, &dh);
        SDL_GetWindowSize(S.win, &ww, &wh);
        log_remote("native up: renderer=gles2 drawable=%dx%d window=%dx%d", dw, dh, ww, wh);
    }
    S.shelf_cur = S.shelf_prev = S.rail_art_cur = S.rail_art_prev = -1;
    S.last_input = SDL_GetTicks();
    S.orbit_k = -1;
    request_data();

    const char *shot = getenv("HOMEPLUS_SCREENSHOT"); /* host preview: render then save a PNG */
    Uint32 shot_at = shot ? SDL_GetTicks() + (Uint32)atoi(getenv("HOMEPLUS_SCREENSHOT_MS") ? getenv("HOMEPLUS_SCREENSHOT_MS") : "2500") : 0;
    extern void homeplus_preview_input(Uint32 elapsed);

    int running = 1, animating = 1;
    Uint32 last = SDL_GetTicks(), start = last;
    while (running) {
        SDL_Event e;
        /* sleep while idle; wake on input, a finished network job, a due retry or the clock */
        int got;
        if (animating || S.ok_down || shot) {
            got = SDL_PollEvent(&e);
        } else {
            int wait = 1000;
            Uint32 t0 = SDL_GetTicks();
            if (S.next_retry) wait = S.next_retry > t0 ? (int)(S.next_retry - t0) : 0;
            if (S.trakt_poll && (int)(S.trakt_poll - t0) < wait) wait = S.trakt_poll > t0 ? (int)(S.trakt_poll - t0) : 0;
            if (S.toast_tex && S.toast_until > t0 && (int)(S.toast_until - t0) < wait) wait = (int)(S.toast_until - t0);
            got = SDL_WaitEventTimeout(&e, wait > 0 ? wait : 1);
        }
        int dirty = got;
        while (got) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == WEBOS_KEY_CURSOR_HIDE) break; /* the pointer fell asleep: not input */
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
                if (wake(0)) break;
                if (!S.sheet.open && !S.moving) {
                    int mx = e.motion.x, my = e.motion.y;
                    to_logical(&mx, &my);
                    int h = hit_item(mx, my);
                    if (h >= 0) {
                        S.focus = h;
                        S.zone = 0;
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
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (wake(0)) break;
                if (!S.sheet.open) {
                    int mx = e.button.x, my = e.button.y;
                    to_logical(&mx, &my);
                    if (hit_item(mx, my) >= 0) activate();
                }
                break;
            case SDL_MOUSEWHEEL:
                if (wake(0)) break;
                if (!S.sheet.open) move_focus(0, e.wheel.y < 0 ? 1 : -1);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_SHOWN) {
                    if (S.away) go_home(); /* back from another app (or the Home button): start at the top */
                    S.away = 0;
                    S.offscreen = 0;
                    wake(0);
                    request_data();
                } else if (e.window.event == SDL_WINDOWEVENT_HIDDEN) {
                    S.offscreen = 1;
                }
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
        if (S.ok_down && !S.ok_long && now - S.ok_down >= LONG_PRESS_MS) {
            S.ok_long = 1;
            if (!S.sheet.open && !S.moving && S.zone == 0) open_app_sheet();
            dirty = 1;
        }
        if (S.trakt_poll && now >= S.trakt_poll) {
            S.trakt_poll = 0;
            net_get_json("/api/trakt/status", TAG_TRAKT_STATUS, 0, 0);
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
            int stage = idle >= IDLE_OFF_MS ? 2 : idle >= IDLE_DIM_MS ? 1 : 0;
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
            if (take) running = 0;
        }
        last = now;
    }
    net_stop();
    r_shutdown();
    SDL_Quit();
    return 0;
}
