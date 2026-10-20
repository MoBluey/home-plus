/* Home+ native: luna bus calls via the TV's libluna-service2. See luna.h. */
#include "luna.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "net.h"

/* The few libluna-service2 / glib types and calls we need (lunaservice.h). */
typedef struct LSHandle LSHandle;
typedef struct LSMessage LSMessage;
typedef unsigned long LSMessageToken;
typedef struct {
    int error_code;
    char *message;
    const char *file;
    int line;
    const char *func;
    void *padding;
    unsigned long magic;
} LSError;
typedef int (*LSFilterFunc)(LSHandle *sh, LSMessage *reply, void *ctx);
typedef struct GMainContext GMainContext;

static struct {
    int tried, ok;
    LSHandle *sh;
    GMainContext *ctx;
    SDL_mutex *lock;
    int (*ErrorInit)(LSError *);
    void (*ErrorFree)(LSError *);
    int (*Register)(const char *, LSHandle **, LSError *);
    int (*Attach)(LSHandle *, GMainContext *, LSError *);
    int (*CallOneReply)(LSHandle *, const char *, const char *, LSFilterFunc, void *, LSMessageToken *, LSError *);
    const char *(*GetPayload)(LSMessage *);
    GMainContext *(*ContextNew)(void);
    int (*Iteration)(GMainContext *, int);
} L;

void luna_diag(const char *fmt, ...)
{
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "homeplus: %s\n", line);
    FILE *f = fopen("/tmp/homeplus-native.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
    fprintf(f, "%s %s\n", ts, line);
    fclose(f);
}

static void *sym(void *lib, const char *name, int *missing)
{
    void *p = lib ? dlsym(lib, name) : NULL;
    if (!p) *missing = 1;
    return p;
}

/* Loads the libraries and registers on the bus; once. Caller holds L.lock. */
static int luna_init(void)
{
    if (L.tried) return L.ok;
    L.tried = 1;
    void *ls = dlopen("libluna-service2.so.3", RTLD_NOW | RTLD_GLOBAL);
    const char *why = ls ? NULL : dlerror();
    void *gl = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!why && !gl) why = dlerror();
    if (!ls || !gl) {
        luna_diag("luna: libraries not available (%s)", why ? why : "?");
        return 0;
    }
    int missing = 0;
    *(void **)&L.ErrorInit = sym(ls, "LSErrorInit", &missing);
    *(void **)&L.ErrorFree = sym(ls, "LSErrorFree", &missing);
    *(void **)&L.Register = sym(ls, "LSRegister", &missing);
    *(void **)&L.Attach = sym(ls, "LSGmainContextAttach", &missing);
    *(void **)&L.CallOneReply = sym(ls, "LSCallOneReply", &missing);
    *(void **)&L.GetPayload = sym(ls, "LSMessageGetPayload", &missing);
    *(void **)&L.ContextNew = sym(gl, "g_main_context_new", &missing);
    *(void **)&L.Iteration = sym(gl, "g_main_context_iteration", &missing);
    if (missing) {
        luna_diag("luna: a libluna-service2/glib function is missing");
        return 0;
    }
    /* anonymous: the app id itself is already registered by LG's runtime for the
     * app ("a service name that already exists"), and anonymous calls are allowed
     * from the sandbox (seen on the C3, webOS 23) */
    const char *names[] = { NULL };
    for (int i = 0; i < 1; i++) {
        LSError err;
        L.ErrorInit(&err);
        if (L.Register(names[i], &L.sh, &err)) {
            L.ctx = L.ContextNew();
            if (L.Attach(L.sh, L.ctx, &err)) {
                luna_diag("luna: registered as %s", names[i] ? names[i] : "(anonymous)");
                L.ok = 1;
                return 1;
            }
        }
        luna_diag("luna: register as %s failed: %s", names[i] ? names[i] : "(anonymous)", err.message ? err.message : "?");
        L.ErrorFree(&err);
        L.sh = NULL;
    }
    return 0;
}

/* On the heap: a reply that arrives after we gave up lands in a later
 * Iteration, and then frees its own state. */
typedef struct {
    int done, abandoned;
    char *payload;
} Reply;

static int on_reply(LSHandle *sh, LSMessage *msg, void *ctx)
{
    (void)sh;
    Reply *r = ctx;
    if (r->abandoned) {
        free(r);
        return 1;
    }
    const char *p = L.GetPayload(msg);
    r->payload = strdup(p ? p : "");
    r->done = 1;
    return 1;
}

void luna_setup(void)
{
    if (!L.lock) L.lock = SDL_CreateMutex();
}

int luna_call(const char *uri, const char *payload, int timeout_ms, char **out)
{
    if (out) *out = NULL;
    SDL_LockMutex(L.lock);
    if (!luna_init()) {
        SDL_UnlockMutex(L.lock);
        return -1;
    }
    Reply *rp = calloc(1, sizeof *rp), r;
    LSError err;
    L.ErrorInit(&err);
    int rc;
    if (!L.CallOneReply(L.sh, uri, payload, on_reply, rp, NULL, &err)) {
        free(rp);
        rp = NULL;
        luna_diag("luna: call %s failed: %s", uri, err.message ? err.message : "?");
        if (out) *out = strdup(err.message ? err.message : "call failed");
        L.ErrorFree(&err);
        rc = 0;
    } else {
        Uint32 until = SDL_GetTicks() + (Uint32)timeout_ms;
        while (!rp->done && SDL_GetTicks() < until) {
            if (!L.Iteration(L.ctx, 0)) SDL_Delay(10);
        }
        if (!rp->done) {
            luna_diag("luna: %s: no reply in %d ms", uri, timeout_ms);
            if (out) *out = strdup("timed out");
            rp->abandoned = 1; /* on_reply frees it if the reply still comes */
            rp = NULL;
            rc = 0;
        } else {
            r = *rp;
            free(rp);
            rp = NULL;
            rc = strstr(r.payload, "\"returnValue\":true") || strstr(r.payload, "\"returnValue\": true") ? 1 : 0;
            luna_diag("luna: %s -> %.300s", uri, r.payload);
            if (out) *out = r.payload; else free(r.payload);
        }
    }
    SDL_UnlockMutex(L.lock);
    return rc;
}
