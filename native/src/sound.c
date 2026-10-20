/* Home+ native: focus sounds. See sound.h. */
#include "sound.h"

#include <math.h>
#include <stdlib.h>

#include <SDL.h>

#define RATE 48000
#define QUIET_MS 5000

static struct {
    SDL_AudioDeviceID dev;
    Sint16 *buf[SND_COUNT];
    int len[SND_COUNT];
    int ready, on;
    Uint32 last;
} A;

/* A decaying two-tone ping with a short noise transient on top; a 0.5 ms
 * attack keeps the start from clicking. */
static void synth(int which, float ms, float f1, float f2, float decay_ms, float noise, float gain)
{
    int n = (int)(RATE * ms / 1000);
    Sint16 *b = malloc((size_t)n * sizeof *b);
    if (!b) return;
    unsigned seed = 2463534242u;
    for (int i = 0; i < n; i++) {
        float t = (float)i / RATE;
        float env = expf(-t * 1000 / decay_ms) * fminf(1, t * RATE / 24);
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        float nz = (float)(seed & 0xffff) / 32768.0f - 1;
        float v = sinf(2 * (float)M_PI * f1 * t) * 0.7f + sinf(2 * (float)M_PI * f2 * t) * 0.3f;
        v = (v + nz * noise * expf(-t * 1000 / 2.0f)) * env * gain;
        b[i] = (Sint16)(fmaxf(-1, fminf(1, v)) * 32767);
    }
    A.buf[which] = b;
    A.len[which] = n;
}

void snd_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    synth(SND_TICK, 30, 2600, 5200, 4.5f, 0.35f, 0.16f);
    synth(SND_SELECT, 90, 880, 1320, 22, 0.15f, 0.2f);
    A.ready = A.on = 1;
}

void snd_enable(int on) { A.on = on; }

void snd_play(int which)
{
    if (!A.ready || !A.on || which < 0 || which >= SND_COUNT || !A.buf[which]) return;
    if (!A.dev) {
        SDL_AudioSpec want = { 0 }, have;
        want.freq = RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 512;
        A.dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!A.dev) return;
        SDL_PauseAudioDevice(A.dev, 0);
    }
    SDL_ClearQueuedAudio(A.dev);
    SDL_QueueAudio(A.dev, A.buf[which], (Uint32)(A.len[which] * sizeof(Sint16)));
    A.last = SDL_GetTicks();
}

void snd_idle(void)
{
    if (A.dev && SDL_GetTicks() - A.last > QUIET_MS) snd_suspend();
}

void snd_suspend(void)
{
    if (!A.dev) return;
    SDL_CloseAudioDevice(A.dev);
    A.dev = 0;
}
