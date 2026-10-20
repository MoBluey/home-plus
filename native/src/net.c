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

int http_request(const char *path, const char *post_body, char **body, size_t *len)
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

    char head[MAX_PATH_LEN + 256];
    size_t blen = post_body ? strlen(post_body) : 0;
    int hn = snprintf(head, sizeof head,
                      "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\n%s%s%zu\r\n\r\n",
                      post_body ? "POST" : "GET", path,
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

/* ------------------------------------------------------------------ job queue */

typedef struct Job {
    JobKind kind;
    int tag, index, generation, sample;
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

    if (j->kind == JOB_WAKE) {
        /* luna-send is how a webOS service gets started; ignore its output. */
        int rc = system("luna-send -n 1 -w 5000 luna://com.atvlauncher.app.service/ping '{}' >/dev/null 2>&1");
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

void net_get_image(const char *path, int tag, int index, int generation, int sample)
{
    Job *j = new_job(JOB_IMAGE, path, tag, index, generation);
    j->sample = sample;
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
