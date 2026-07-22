/* Banked PCM audio with a procedural fallback. Every effect is synthesized at
 * startup, then replaced by its reviewed WAV when that file is present. No
 * sink, missing/invalid assets, or a sink dying mid-game remain safe: the game
 * either uses the fallback or plays silently. */
#include "bashed_earth.h"
#include "pcmmix_bank.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SR 44100

static pcmmix_bank sound_bank;
static pcmmix mixer;
static bool mixer_started;
static bool enabled = true;

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
    pcmmix_bank_clear_cue(&sound_bank, (uint32_t)id);
    if (!pcmmix_bank_take(&sound_bank, (uint32_t)id, 0u, out,
                          (size_t)n, 1.0f, 1.0f))
        free(out);
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

static const char *const sfx_files[SFX_COUNT] = {
    [SFX_FIRE] = "sfx/fire.wav",
    [SFX_EXPL_SMALL] = "sfx/expl_small.wav",
    [SFX_EXPL_BIG] = "sfx/expl_big.wav",
    [SFX_DIRT] = "sfx/dirt.wav",
    [SFX_BOUNCE] = "sfx/bounce.wav",
    [SFX_SPLASH] = "sfx/splash.wav",
    [SFX_MIRV] = "sfx/mirv.wav",
    [SFX_DRILL] = "sfx/drill.wav",
    [SFX_SHIELD] = "sfx/shield.wav",
    [SFX_DEATH] = "sfx/death.wav",
    [SFX_WIN] = "sfx/win.wav",
    [SFX_MENU_MOVE] = "sfx/menu_move.wav",
    [SFX_MENU_SELECT] = "sfx/menu_select.wav",
    [SFX_BUY] = "sfx/buy.wav",
    [SFX_DENY] = "sfx/deny.wav",
};

static char sound_asset_root[512] = "assets";

static void sound_asset_paths_init(void)
{
    const char *override = getenv("BASHED_EARTH_ASSETS");
    char executable[400];
    char candidate[512];
    char *slash;
    ssize_t length;

    if (override && *override) {
        snprintf(sound_asset_root, sizeof sound_asset_root, "%s", override);
        return;
    }
    length = readlink("/proc/self/exe", executable, sizeof executable - 1);
    if (length <= 0) return;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(candidate, sizeof candidate, "%s/assets", executable);
    if (access(candidate, F_OK) == 0) {
        snprintf(sound_asset_root, sizeof sound_asset_root, "%s", candidate);
        return;
    }
    snprintf(candidate, sizeof candidate,
             "%s/../share/bashed-earth/assets", executable);
    if (access(candidate, F_OK) == 0)
        snprintf(sound_asset_root, sizeof sound_asset_root, "%s", candidate);
}

static void load_external_sounds(void)
{
    sound_asset_paths_init();
    for (int id = 0; id < SFX_COUNT; id++) {
        char full[768];
        char err[128];
        if (!sfx_files[id]) continue;
        if (snprintf(full, sizeof full, "%s/%s", sound_asset_root,
                     sfx_files[id]) >= (int)sizeof full)
            continue;
        (void)pcmmix_bank_load_wav(&sound_bank, (uint32_t)id, 0u,
                                   full, 1.0f, 1.0f,
                                   err, sizeof err);
    }
}

/* ---------- public API ---------- */
bool sound_init(void)
{
    pcmmix_options options;

    if (mixer_started) return true;
    (void)pcmmix_bank_init(&sound_bank, SFX_COUNT, 0x51ed2701u);
    synth_all();
    load_external_sounds();
    pcmmix_options_init(&options);
    options.max_voices = 16;
    options.latency_ms = 60;
    if (!pcmmix_start(&mixer, &options)) return false;
    mixer_started = true;
    pcmmix_set_enabled(&mixer, enabled);
    return true;
}

void sound_shutdown(void)
{
    if (mixer_started) pcmmix_stop(&mixer);
    mixer_started = false;
    pcmmix_bank_clear(&sound_bank);
}

void sound_set_enabled(bool on)
{
    enabled = on;
    if (mixer_started) pcmmix_set_enabled(&mixer, on);
}
bool sound_is_enabled(void) { return enabled; }

void sound_play(int id, float vol, float pitch)
{
    if (!mixer_started || !enabled || id < 0 || id >= SFX_COUNT) return;
    if (pitch <= 0.05f) pitch = 1;
    pitch *= 0.97f + srandf() * 0.06f;   /* tiny jitter so repeats vary */
    (void)pcmmix_bank_play(&mixer, &sound_bank, (uint32_t)id,
                           clampf(vol, 0, 1), pitch);
}
