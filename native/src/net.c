/* Home+ native: HTTP to the service + background workers. See net.h. */
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "stb_image.h"

#define WORKERS 2
#define MAX_PATH_LEN 512

/* ------------------------------------------------------------------ HTTP */

/* The service answers only the launcher: each request carries the token it
 * writes into our folder when it starts (see service.js). It's read again
 * when the service has restarted and refuses the old one (403). */
static char token_path[600];
static char token[128];
static SDL_mutex *token_lock;

static void load_token(void)
{
    char buf[128] = "";
    FILE *f = token_path[0] ? fopen(token_path, "r") : NULL;
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        buf[strcspn(buf, "\r\n")] = 0;
    }
    SDL_LockMutex(token_lock);
    snprintf(token, sizeof token, "%s", buf);
    SDL_UnlockMutex(token_lock);
}

static void current_token(char *out, size_t cap)
{
    SDL_LockMutex(token_lock);
    snprintf(out, cap, "%s", token);
    SDL_UnlockMutex(token_lock);
}

static int http_once(const char *path, const char *post_body, const char *tok, char **body, size_t *len)
{
    *body = NULL;
    if (len) *len = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct timeval tv = { 20, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(SERVICE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return 0;
    }

    char head[MAX_PATH_LEN + 384];
    size_t blen = post_body ? strlen(post_body) : 0;
    int hn = snprintf(head, sizeof head,
                      "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nX-Homeplus-Token: %s\r\n%s%s%zu\r\n\r\n",
                      post_body ? "POST" : "GET", path, tok,
                      post_body ? "Content-Type: application/json\r\n" : "",
                      "Content-Length: ", blen);
    if (send(fd, head, hn, 0) != hn || (blen && send(fd, post_body, blen, 0) != (ssize_t)blen)) {
        close(fd);
        return 0;
    }

    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap + 1);
        }
        ssize_t k = recv(fd, buf + n, cap - n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        n += k;
    }
    close(fd);
    buf[n] = 0;

    int status = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) status = 0;
    char *sep = strstr(buf, "\r\n\r\n");
    size_t off = sep ? (size_t)(sep + 4 - buf) : n;
    size_t bn = n - off;
    char *out = malloc(bn + 1);
    memcpy(out, buf + off, bn);
    out[bn] = 0;
    free(buf);
    *body = out;
    if (len) *len = bn;
    return status;
}

int http_request(const char *path, const char *post_body, char **body, size_t *len)
{
    char tok[128];
    current_token(tok, sizeof tok);
    int status = http_once(path, post_body, tok, body, len);
    if (status == 403) { /* a new token: the service restarted, or our folder was reinstalled */
        free(*body);
        char *st = NULL;
        http_once("/api/status", NULL, "", &st, NULL); /* makes the service write it again */
        free(st);
        load_token();
        current_token(tok, sizeof tok);
        status = http_once(path, post_body, tok, body, len);
    }
    return status;
}

/* ------------------------------------------------------------------ icon colour */

/* If the icon's edges are one solid colour, the tile can extend it so the icon
 * reads as full-bleed. Otherwise use a dimmed average. Same idea as the web UI. */
static void sample_icon(const unsigned char *px, int w, int h, Result *r)
{
    enum { N = 48, INSET = 10 };
    int edge_n = 0;
    double em[3] = { 0 }, all[4] = { 0 };
    unsigned char edge[4 * N][3]; /* per call: two workers run this concurrently */
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            const unsigned char *p = px + ((size_t)(y * h / N) * w + (x * w / N)) * 4;
            if (p[3] < 220) continue;
            all[0] += p[0]; all[1] += p[1]; all[2] += p[2]; all[3]++;
            int ring = (x == INSET || x == N - 1 - INSET || y == INSET || y == N - 1 - INSET) &&
                       x >= INSET && x <= N - 1 - INSET && y >= INSET && y <= N - 1 - INSET;
            if (ring && edge_n < 4 * N) {
                memcpy(edge[edge_n++], p, 3);
                em[0] += p[0]; em[1] += p[1]; em[2] += p[2];
            }
        }
    }
    int ring_size = 4 * (N - 2 * INSET) - 4;
    r->sampled = 1;
    if (edge_n > ring_size * 0.85) {
        for (int c = 0; c < 3; c++) em[c] /= edge_n;
        double dev = 0;
        for (int i = 0; i < edge_n; i++)
            dev += fabs(edge[i][0] - em[0]) + fabs(edge[i][1] - em[1]) + fabs(edge[i][2] - em[2]);
        if (dev / edge_n < 30) {
            r->fullbleed = 1;
            for (int c = 0; c < 3; c++) r->rgb[c] = (unsigned char)(em[c] + 0.5);
            return;
        }
    }
    if (all[3] > 0) {
        for (int c = 0; c < 3; c++) r->rgb[c] = (unsigned char)(all[c] / all[3] * 0.6);
    } else {
        r->sampled = 0;
    }
}

/* ------------------------------------------------------------------ shrinking */

/* Adds w x one source row's colour to an output row: alpha-weighted r, g, b sums and alpha. */
static void add_row(float *acc, const float *row, int n, float w)
{
    for (int i = 0; i < n * 4; i++) acc[i] += w * row[i];
}

static void put_row(unsigned char *out, const float *acc, int n, float area)
{
    for (int i = 0; i < n; i++, acc += 4, out += 4) {
        float a = acc[3];
        for (int c = 0; c < 3; c++) out[c] = a > 0 ? (unsigned char)fminf(acc[c] / a + 0.5f, 255) : 0;
        out[3] = (unsigned char)fminf(a / area + 0.5f, 255);
    }
}

/* Shrinks RGBA pixels to nw x nh (each no larger than the source) by averaging the area of
 * the source each new pixel covers. Colour is weighted by alpha, so the transparent pixels
 * around an icon don't darken its edges. NULL when out of memory. */
static unsigned char *shrink(const unsigned char *px, int w, int h, int nw, int nh)
{
    float sx = (float)w / nw, sy = (float)h / nh;
    float *row = malloc(sizeof(float) * 4 * nw), *acc = calloc((size_t)nw * 4, sizeof(float));
    unsigned char *out = malloc((size_t)nw * nh * 4);
    if (!row || !acc || !out) {
        free(row);
        free(acc);
        free(out);
        return NULL;
    }
    int oy = 0;
    for (int y = 0; y < h && oy < nh; y++) {
        /* this source row averaged across into nw pixels */
        const unsigned char *s = px + (size_t)y * w * 4;
        for (int ox = 0; ox < nw; ox++) {
            float x0 = ox * sx, x1 = x0 + sx, r = 0, g = 0, b = 0, a = 0;
            for (int x = (int)x0; x < w && x < x1; x++) {
                float k = (fminf(x + 1, x1) - fmaxf(x, x0)) * s[x * 4 + 3];
                r += k * s[x * 4];
                g += k * s[x * 4 + 1];
                b += k * s[x * 4 + 2];
                a += k;
            }
            float *d = row + ox * 4;
            d[0] = r; d[1] = g; d[2] = b; d[3] = a;
        }
        /* ...then shared between the output rows it overlaps (at most two when shrinking) */
        float top = (float)y;
        while (oy < nh) {
            float end = (oy + 1) * sy;
            if (y + 1 < end) {
                add_row(acc, row, nw, y + 1 - top);
                break;
            }
            add_row(acc, row, nw, end - top);
            put_row(out + (size_t)oy * nw * 4, acc, nw, sx * sy);
            memset(acc, 0, sizeof(float) * 4 * nw);
            oy++;
            top = end;
            if (top >= y + 1) break;
        }
    }
    if (oy < nh) put_row(out + (size_t)oy * nw * 4, acc, nw, sx * sy); /* float rounding left the last row open */
    free(row);
    free(acc);
    return out;
}

/* ------------------------------------------------------------------ job queue */

typedef struct Job {
    JobKind kind;
    int tag, index, generation, sample, fit_w, fit_h;
    char path[MAX_PATH_LEN];
    char *post;
    struct Job *next;
} Job;

typedef struct Done {
    Result r;
    struct Done *next;
} Done;

static SDL_mutex *lock;
static SDL_cond *wake;
static Job *jobs_head, *jobs_tail;
static Done *done_head, *done_tail;
static SDL_Thread *threads[WORKERS];
static int running;
Uint32 net_event = (Uint32)-1;

static void push_job(Job *j)
{
    SDL_LockMutex(lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
}

static void push_done(Result *r)
{
    Done *d = calloc(1, sizeof *d);
    d->r = *r;
    SDL_LockMutex(lock);
    if (done_tail) done_tail->next = d; else done_head = d;
    done_tail = d;
    SDL_UnlockMutex(lock);
    /* wake the render loop, which sleeps while nothing is animating */
    SDL_Event e = { 0 };
    e.type = net_event;
    SDL_PushEvent(&e);
}

static void run_job(Job *j)
{
    Result r = { 0 };
    r.kind = j->kind;
    r.tag = j->tag;
    r.index = j->index;
    r.generation = j->generation;

    if (j->kind == JOB_LUNA) {
        char cmd[MAX_PATH_LEN + 600];
        snprintf(cmd, sizeof cmd, "luna-send -n 1 -w 20000 '%s' '%s' 2>&1", j->path, j->post ? j->post : "{}");
        FILE *p = popen(cmd, "r");
        size_t cap = 4096, n = 0;
        char *out = malloc(cap + 1);
        if (p) {
            size_t k;
            while (n < cap && (k = fread(out + n, 1, cap - n, p)) > 0) n += k;
            pclose(p);
        }
        out[n] = 0;
        r.body = out;
        r.status = strstr(out, "\"returnValue\":true") || strstr(out, "\"returnValue\": true") ? 200 : 0;
        push_done(&r);
        return;
    }
    if (j->kind == JOB_WAKE) {
        /* luna-send is how a webOS service gets started; ignore its output. */
        int rc = system("luna-send -n 1 -w 5000 luna://" SERVICE_ID "/ping '{}' >/dev/null 2>&1");
        r.status = rc == 0 ? 200 : 0;
        push_done(&r);
        return;
    }

    char *body = NULL;
    size_t len = 0;
    r.status = http_request(j->path, j->kind == JOB_POST ? (j->post ? j->post : "{}") : NULL, &body, &len);
    if (j->kind == JOB_IMAGE) {
        if (r.status == 200 && body && len) {
            int n;
            r.rgba = stbi_load_from_memory((unsigned char *)body, (int)len, &r.w, &r.h, &n, 4);
            if (r.rgba && j->sample) sample_icon(r.rgba, r.w, r.h, &r);
            /* bigger than it's ever drawn: shrink to just cover the fit box */
            float s = r.rgba && j->fit_w > 0 ? fmaxf((float)j->fit_w / r.w, (float)j->fit_h / r.h) : 1;
            if (s < 1) {
                int nw = (int)lroundf(r.w * s), nh = (int)lroundf(r.h * s);
                if (nw < j->fit_w) nw = j->fit_w;
                if (nh < j->fit_h) nh = j->fit_h;
                unsigned char *small = shrink(r.rgba, r.w, r.h, nw, nh);
                if (small) {
                    stbi_image_free(r.rgba);
                    r.rgba = small; /* malloc'd like stb's own buffers: net_result_free frees either */
                    r.w = nw;
                    r.h = nh;
                }
            }
        }
        free(body);
    } else {
        r.body = body;
    }
    push_done(&r);
}

static int worker(void *arg)
{
    (void)arg;
    for (;;) {
        SDL_LockMutex(lock);
        while (running && !jobs_head) SDL_CondWait(wake, lock);
        if (!running) {
            SDL_UnlockMutex(lock);
            return 0;
        }
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        SDL_UnlockMutex(lock);
        run_job(j);
        free(j->post);
        free(j);
    }
}

void net_start(void)
{
    net_event = SDL_RegisterEvents(1);
    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    /* the token sits next to the binary on the TV; HOMEPLUS_TOKEN_FILE for the desktop preview */
    const char *env = getenv("HOMEPLUS_TOKEN_FILE");
    char *base = env ? NULL : SDL_GetBasePath();
    snprintf(token_path, sizeof token_path, "%s%s", env ? env : base ? base : "./", env ? "" : "api-token");
    SDL_free(base);
    token_lock = SDL_CreateMutex();
    load_token();
    running = 1;
    for (int i = 0; i < WORKERS; i++) threads[i] = SDL_CreateThread(worker, "homeplus-net", NULL);
}

void net_stop(void)
{
    SDL_LockMutex(lock);
    running = 0;
    SDL_CondBroadcast(wake);
    SDL_UnlockMutex(lock);
    for (int i = 0; i < WORKERS; i++) SDL_WaitThread(threads[i], NULL);
}

static Job *new_job(JobKind kind, const char *path, int tag, int index, int generation)
{
    Job *j = calloc(1, sizeof *j);
    j->kind = kind;
    j->tag = tag;
    j->index = index;
    j->generation = generation;
    if (path) snprintf(j->path, sizeof j->path, "%s", path);
    return j;
}

void net_get_json(const char *path, int tag, int index, int generation)
{
    push_job(new_job(JOB_JSON, path, tag, index, generation));
}

void net_post(const char *path, const char *body, int tag)
{
    Job *j = new_job(JOB_POST, path, tag, 0, 0);
    j->post = body ? strdup(body) : NULL;
    push_job(j);
}

void net_get_image(const char *path, int tag, int index, int generation, int sample, int fit_w, int fit_h)
{
    Job *j = new_job(JOB_IMAGE, path, tag, index, generation);
    j->sample = sample;
    j->fit_w = fit_w;
    j->fit_h = fit_h;
    push_job(j);
}

void net_luna(const char *uri, const char *payload, int tag)
{
    Job *j = new_job(JOB_LUNA, uri, tag, 0, 0);
    j->post = payload ? strdup(payload) : NULL;
    push_job(j);
}

void net_wake_service(void)
{
    push_job(new_job(JOB_WAKE, NULL, 0, 0, 0));
}

int net_poll(Result *out)
{
    SDL_LockMutex(lock);
    Done *d = done_head;
    if (d) {
        done_head = d->next;
        if (!done_head) done_tail = NULL;
    }
    SDL_UnlockMutex(lock);
    if (!d) return 0;
    *out = d->r;
    free(d);
    return 1;
}

void net_result_free(Result *r)
{
    free(r->body);
    if (r->rgba) stbi_image_free(r->rgba);
    r->body = NULL;
    r->rgba = NULL;
}
