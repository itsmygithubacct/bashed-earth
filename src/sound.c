/* Procedural sound. Every effect is synthesized at startup (no asset
 * files) into an s16 PCM buffer; a mixer thread resamples/mixes active
 * voices and streams raw s16le mono 44.1 kHz down a pipe to the first
 * CLI audio sink that stays alive (pacat / pw-play / aplay / sox play).
 * No sink, or the sink dying mid-game, just means the game runs silent —
 * sound_play() is always safe to call, including from the headless
 * selftest where sound_init() never runs. */
#include "bashed_earth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/wait.h>

#define SR 44100
#define MAX_VOICES 16
#define MIX_FRAMES 512          /* ~11.6 ms per mixer iteration */

typedef struct { int16_t *data; int len; } Sfx;
typedef struct {
    const int16_t *data;
    int len;
    float pos, step, vol;
    bool active;
} Voice;

static Sfx sfx[SFX_COUNT];
static Voice voices[MAX_VOICES];
static pthread_mutex_t voiceLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static volatile bool running = false;
static bool enabled = true;
static int sinkFd = -1;
static pid_t sinkPid = -1;

/* private rng so sound never perturbs the game's frandf()/rand() streams */
static uint32_t srng = 0x51ed2701u;
static float srandf(void)
{
    srng ^= srng << 13; srng ^= srng >> 17; srng ^= srng << 5;
    return (srng >> 8) * (1.0f / 16777216.0f);
}
static float snoise(void) { return srandf() * 2 - 1; }

/* ---------- synthesis ---------- */
/* one-pole lowpass coefficient for cutoff c (Hz) */
static float lpk(float c) { return 1 - expf(-6.2832f * c / SR); }

/* normalize to `peak`, apply de-click fades, convert to s16 */
static void bake(int id, const float *s, int n, float peak)
{
    float m = 1e-6f;
    for (int i = 0; i < n; i++) if (fabsf(s[i]) > m) m = fabsf(s[i]);
    float g = peak / m;
    int16_t *out = malloc((size_t)n * sizeof *out);
    if (!out) return;
    int fadeIn = 32 < n ? 32 : n, fadeOut = 512 < n ? 512 : n;
    for (int i = 0; i < n; i++) {
        float v = s[i] * g;
        if (i < fadeIn) v *= (float)i / fadeIn;
        if (n - i < fadeOut) v *= (float)(n - i) / fadeOut;
        v = clampf(v, -1, 1);
        out[i] = (int16_t)(v * 32767);
    }
    sfx[id].data = out;
    sfx[id].len = n;
}

/* shot / explosion family: filtered noise burst + pitch-swept sine body */
static void gen_boom(int id, float dur, float cut0, float cutRate,
                     float envTau, float f0, float f1, float subTau,
                     float noiseAmt, float subAmt, bool clip, float peak)
{
    int n = (int)(dur * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float lp = 0, ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float c = (cut0 - 80) * expf(-t * cutRate) + 80;
        lp += lpk(c) * (snoise() - lp);
        float f = f1 + (f0 - f1) * expf(-t * 8);
        ph += 6.2832f * f / SR;
        float v = lp * expf(-t / envTau) * noiseAmt
                + sinf(ph) * expf(-t / subTau) * subAmt;
        s[i] = clip ? tanhf(v * 1.8f) : v;
    }
    bake(id, s, n, peak);
    free(s);
}

static void gen_fire(void)      /* launch: sharp crack + descending body */
{
    gen_boom(SFX_FIRE, 0.30f, 4200, 16, 0.055f, 340, 70, 0.09f,
             0.9f, 0.8f, false, 0.42f);
}

static void gen_splash(void)
{
    int n = (int)(0.5f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float lp = 0, lp2 = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float c = (2600 - 350) * expf(-t * 9) + 350;
        float x = snoise();
        lp += lpk(c) * (x - lp);
        lp2 += lpk(180) * (lp - lp2);
        float env = fminf(t / 0.004f, 1) * expf(-t / 0.13f);
        s[i] = (lp - lp2) * env;    /* band-passed: watery, not rumbling */
    }
    bake(SFX_SPLASH, s, n, 0.4f);
    free(s);
}

static void gen_mirv(void)      /* three quick rising chirps */
{
    int n = (int)(0.34f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    for (int k = 0; k < 3; k++) {
        int start = (int)(k * 0.09f * SR);
        float ph = 0;
        for (int i = 0; i < (int)(0.08f * SR) && start + i < n; i++) {
            float t = (float)i / SR;
            ph += 6.2832f * (500 + 5000 * t) / SR;
            s[start + i] += sinf(ph) * expf(-t / 0.03f);
        }
    }
    bake(SFX_MIRV, s, n, 0.35f);
    free(s);
}

static void gen_drill(void)     /* rattly saw buzz with tremolo */
{
    int n = (int)(0.45f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        ph += 85.0f / SR;
        ph -= (int)ph;
        float saw = 2 * ph - 1;
        lp += lpk(900) * (snoise() - lp);
        float am = 0.55f + 0.45f * sinf(6.2832f * 29 * t);
        float env = t > 0.33f ? (0.45f - t) / 0.12f : 1;
        s[i] = (saw * 0.7f + lp * 0.6f) * am * env;
    }
    bake(SFX_DRILL, s, n, 0.32f);
    free(s);
}

static void gen_shield(void)    /* metallic inharmonic ping */
{
    int n = (int)(0.3f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    static const float F[3] = { 523, 784, 1178 };
    static const float A[3] = { 0.6f, 0.4f, 0.28f };
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR, v = 0;
        for (int k = 0; k < 3; k++)
            v += A[k] * sinf(6.2832f * F[k] * t) * expf(-t * (10 + k * 5));
        s[i] = v;
    }
    bake(SFX_SHIELD, s, n, 0.35f);
    free(s);
}

static void gen_death(void)     /* long falling groan + noise */
{
    int n = (int)(0.8f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = 45 + (280 - 45) * expf(-t / 0.35f);
        ph += f / SR;
        ph -= (int)ph;
        lp += lpk(700) * (snoise() - lp);
        float v = (2 * ph - 1) * expf(-t * 3.2f) + lp * expf(-t * 6) * 0.5f;
        s[i] = tanhf(v * 1.5f);
    }
    bake(SFX_DEATH, s, n, 0.42f);
    free(s);
}

/* short tonal blip: freq sweep f0->f1 over dur with decay tau */
static void gen_blip(int id, float dur, float f0, float f1, float tau,
                     float harm, float peak)
{
    int n = (int)(dur * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    float ph = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float f = f0 + (f1 - f0) * (t / dur);
        ph += 6.2832f * f / SR;
        s[i] = (sinf(ph) + harm * sinf(ph * 2)) * expf(-t / tau);
    }
    bake(id, s, n, peak);
    free(s);
}

static void gen_win(void)       /* little victory arpeggio */
{
    static const float NOTES[4] = { 523.25f, 659.25f, 783.99f, 1046.5f };
    int n = (int)(0.9f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    for (int k = 0; k < 4; k++) {
        int start = (int)(k * 0.15f * SR);
        for (int i = 0; i < (int)(0.35f * SR) && start + i < n; i++) {
            float t = (float)i / SR;
            s[start + i] += (sinf(6.2832f * NOTES[k] * t)
                           + 0.3f * sinf(6.2832f * NOTES[k] * 2 * t))
                          * fminf(t / 0.008f, 1) * expf(-t / 0.12f);
        }
    }
    bake(SFX_WIN, s, n, 0.35f);
    free(s);
}

static void gen_deny(void)      /* flat low buzz */
{
    int n = (int)(0.16f * SR);
    float *s = calloc((size_t)n, sizeof *s);
    if (!s) return;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SR;
        float sq = sinf(6.2832f * 120 * t) > 0 ? 1.0f : -1.0f;
        s[i] = sq * (t > 0.12f ? (0.16f - t) / 0.04f : 1);
    }
    bake(SFX_DENY, s, n, 0.2f);
    free(s);
}

static void synth_all(void)
{
    gen_fire();
    gen_boom(SFX_EXPL_SMALL, 0.60f, 1600, 11, 0.16f, 110, 42, 0.14f,
             1.0f, 0.7f, false, 0.5f);
    gen_boom(SFX_EXPL_BIG, 1.40f, 950, 6, 0.38f, 75, 28, 0.30f,
             1.0f, 0.9f, true, 0.6f);
    gen_boom(SFX_DIRT, 0.35f, 320, 10, 0.09f, 60, 38, 0.09f,
             1.0f, 0.8f, false, 0.42f);
    gen_blip(SFX_BOUNCE, 0.13f, 740, 370, 0.045f, 0.2f, 0.3f);
    gen_splash();
    gen_mirv();
    gen_drill();
    gen_shield();
    gen_death();
    gen_win();
    gen_blip(SFX_MENU_MOVE, 0.06f, 660, 660, 0.018f, 0, 0.22f);
    gen_blip(SFX_MENU_SELECT, 0.16f, 550, 880, 0.05f, 0.2f, 0.28f);
    gen_blip(SFX_BUY, 0.18f, 988, 1319, 0.06f, 0.3f, 0.28f);
    gen_deny();
}

/* ---------- mixer thread ---------- */
static void *mixer_main(void *arg)
{
    (void)arg;
    int16_t out[MIX_FRAMES];
    int32_t acc[MIX_FRAMES];

    while (running) {
        memset(acc, 0, sizeof acc);
        pthread_mutex_lock(&voiceLock);
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice *vo = &voices[v];
            if (!vo->active) continue;
            for (int i = 0; i < MIX_FRAMES; i++) {
                int ip = (int)vo->pos;
                if (ip >= vo->len - 1) { vo->active = false; break; }
                float fr = vo->pos - ip;
                float smp = vo->data[ip] * (1 - fr) + vo->data[ip + 1] * fr;
                acc[i] += (int32_t)(smp * vo->vol);
                vo->pos += vo->step;
            }
        }
        pthread_mutex_unlock(&voiceLock);
        for (int i = 0; i < MIX_FRAMES; i++)
            out[i] = (int16_t)(acc[i] > 32767 ? 32767
                             : acc[i] < -32768 ? -32768 : acc[i]);

        /* blocking write against a small pipe paces us to the DAC clock;
         * we keep streaming silence between effects so latency is fixed */
        const char *p = (const char *)out;
        size_t left = sizeof out;
        while (left > 0 && running) {
            ssize_t n = write(sinkFd, p, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { running = false; break; }   /* sink died: go mute */
            p += n;
            left -= (size_t)n;
        }
    }
    return NULL;
}

/* ---------- sink discovery ---------- */
static bool in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) return false;
    char buf[512];
    while (*path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        if (n > 0 && n < sizeof buf - strlen(name) - 2) {
            memcpy(buf, path, n);
            snprintf(buf + n, sizeof buf - n, "/%s", name);
            if (access(buf, X_OK) == 0) return true;
        }
        if (!sep) break;
        path = sep + 1;
    }
    return false;
}

static const char *const SINKS[][14] = {
    { "pacat", "--rate=44100", "--channels=1", "--format=s16le",
      "--latency-msec=60", NULL },
    { "pw-play", "--raw", "--rate=44100", "--channels=1", "--format=s16", "-", NULL },
    { "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "1", "-", NULL },
    { "play", "-q", "-t", "raw", "-r", "44100", "-e", "signed", "-b", "16",
      "-c", "1", "-", NULL },
};
#define NUM_SINKS (int)(sizeof SINKS / sizeof SINKS[0])

/* spawn candidate sink reading PCM from a pipe; NULL-fd stdout/stderr */
static bool spawn_sink(int idx)
{
    if (!in_path(SINKS[idx][0])) return false;

    int pfd[2];
    if (pipe(pfd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return false; }
    if (pid == 0) {
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, STDOUT_FILENO); dup2(nul, STDERR_FILENO); }
        execvp(SINKS[idx][0], (char *const *)SINKS[idx]);
        _exit(127);
    }
    close(pfd[0]);

#ifdef F_SETPIPE_SZ
    /* small pipe = the mixer can't run far ahead of the DAC (latency) */
    fcntl(pfd[1], F_SETPIPE_SZ, 8192);
#endif

    /* give it a moment; a sink with no server to talk to exits immediately */
    usleep(80 * 1000);
    int st;
    if (waitpid(pid, &st, WNOHANG) == pid) { close(pfd[1]); return false; }

    sinkFd = pfd[1];
    sinkPid = pid;
    return true;
}

/* ---------- public API ---------- */
bool sound_init(void)
{
    if (running) return true;
    signal(SIGPIPE, SIG_IGN);   /* sink death must not kill the game */

    int i;
    for (i = 0; i < NUM_SINKS; i++)
        if (spawn_sink(i)) break;
    if (i == NUM_SINKS) return false;

    synth_all();
    memset(voices, 0, sizeof voices);
    running = true;
    if (pthread_create(&mixer, NULL, mixer_main, NULL) != 0) {
        running = false;
        close(sinkFd);
        sinkFd = -1;
        return false;
    }
    return true;
}

void sound_shutdown(void)
{
    if (sinkFd < 0) return;
    if (running) {
        running = false;
        pthread_join(mixer, NULL);
    }
    close(sinkFd);
    sinkFd = -1;
    if (sinkPid > 0) {
        kill(sinkPid, SIGTERM);
        waitpid(sinkPid, NULL, 0);
        sinkPid = -1;
    }
    for (int i = 0; i < SFX_COUNT; i++) {
        free(sfx[i].data);
        sfx[i].data = NULL;
    }
}

void sound_set_enabled(bool on) { enabled = on; }
bool sound_is_enabled(void) { return enabled; }

void sound_play(int id, float vol, float pitch)
{
    if (!running || !enabled || id < 0 || id >= SFX_COUNT || !sfx[id].data)
        return;
    if (pitch <= 0.05f) pitch = 1;
    pitch *= 0.97f + srandf() * 0.06f;   /* tiny jitter so repeats vary */

    pthread_mutex_lock(&voiceLock);
    int slot = -1;
    float most = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) { slot = i; break; }
        float done = voices[i].pos / voices[i].len;
        if (done > most) { most = done; slot = i; }   /* steal oldest */
    }
    voices[slot] = (Voice){ sfx[id].data, sfx[id].len, 0, pitch,
                            clampf(vol, 0, 1), true };
    pthread_mutex_unlock(&voiceLock);
}
