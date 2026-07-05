/* Entry point: terminal setup, 30 fps render loop with 2 logic ticks per
 * frame (physics runs at 60 Hz), and a headless selftest mode that plays
 * full AI-vs-AI matches to validate the game logic. */
#include "bashed_earth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>

static void on_signal(int sig)
{
    (void)sig;
    term_emergency_restore();
    _exit(1);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sleep_ms(double ms)
{
    if (ms <= 0) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)(fmod(ms, 1000.0) * 1e6) };
    nanosleep(&ts, NULL);
}

static void set_defaults(void)
{
    memset(&G, 0, sizeof G);
    G.damageMultiplier = 1.0f;
    G.wallBounce = true;
    G.soundOn = true;
    G.pEnabled[0] = true;
    G.pEnabled[1] = true;
    G.pStrategy[1] = G.pStrategy[2] = G.pStrategy[3] = -1;
}

/* ---------- selftest: headless AI-vs-AI matches ---------- */
extern const char *g_phase;

static void watchdog(int sig)
{
    (void)sig;
    printf("WATCHDOG: stuck in phase '%s' (state=%d frame=%d)\n",
           g_phase, G.gameState, G.frameCount);
    fflush(stdout);
    _exit(2);
}

static int selftest(unsigned seed, int matches)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, watchdog);
    srand(seed);
    frand_seed(seed * 2654435761u + 1);
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    G.pEnabled[1] = G.pEnabled[2] = G.pEnabled[3] = true;

    printf("bashed-earth selftest: seed=%u matches=%d field=%dx%d\n",
           seed, matches, G.W, G.H);

    game_reset_to_start();
    game_start_from_menu();   /* headless => all-AI, store auto-runs, launches */

    if (G.gameState != GS_PLAYING && G.gameState != GS_ANIMATING) {
        printf("FAIL: expected match to launch, state=%d\n", G.gameState);
        return 1;
    }

    int match = 0;
    long ticks = 0;
    const long MAX_TICKS = 60L * 60 * 30;   /* 30 minutes of game time per run */
    int shotsFired = 0, lastAmmoSum = -1;

    while (match < matches && ticks < MAX_TICKS) {
        alarm(5);          /* any single tick taking 5s = wedged */
        game_tick();
        ticks++;

        if (getenv("BE_DEBUG") && ticks % 600 == 0) {
            int nproj = 0, alive = 0;
            for (int i = 0; i < MAX_PROJECTILES; i++) nproj += G.projectiles[i].active;
            for (int i = 0; i < G.numPlayers; i++) alive += G.tanks[i].hp > 0;
            printf("  t=%ld state=%d player=%d alive=%d proj=%d hp=[%d %d %d %d] timers=%.0f/%.0f/%.0f\n",
                   ticks, G.gameState, G.currentPlayer, alive, nproj,
                   G.tanks[0].hp, G.tanks[1].hp, G.tanks[2].hp, G.tanks[3].hp,
                   G.pendingNextTurn, G.pendingAIStart, G.pendingAIFire);
        }

        /* count shots by watching total fireable ammo (cheap heuristic) */
        int ammoSum = 0;
        for (int i = 0; i < G.numPlayers; i++)
            for (int w = 0; w < WEAPON_COUNT; w++)
                if (w != W_NORMAL) ammoSum += G.ammo[i][w];
        if (lastAmmoSum >= 0 && ammoSum < lastAmmoSum) shotsFired += lastAmmoSum - ammoSum;
        lastAmmoSum = ammoSum;

        /* invariants */
        for (int i = 0; i < G.numPlayers; i++) {
            Tank *t = &G.tanks[i];
            if (isnan(t->x) || isnan(t->y)) {
                printf("FAIL: tank %d position is NaN at tick %ld\n", i, ticks);
                return 1;
            }
            if (t->hp < 0 || t->hp > MAX_HP) {
                printf("FAIL: tank %d hp out of range: %d\n", i, t->hp);
                return 1;
            }
        }

        if (G.gameState == GS_GAMEOVER) {
            match++;
            printf("  match %d done: winner=%s rounds=%d ticks=%ld terrain=%s\n",
                   match,
                   G.lastWinnerId >= 0 ? G.tanks[G.lastWinnerId].name : "(draw)",
                   G.roundCount + 1, ticks,
                   G.terrainType == TERRAIN_GRASS ? "grass"
                   : G.terrainType == TERRAIN_SAND ? "sand" : "ice");
            if (match < matches) {
                game_next_round();
                lastAmmoSum = -1;
                if (G.gameState != GS_PLAYING && G.gameState != GS_ANIMATING) {
                    printf("FAIL: next round did not launch, state=%d\n", G.gameState);
                    return 1;
                }
            }
        }
    }

    if (match < matches) {
        printf("FAIL: only %d/%d matches finished in %ld ticks (state=%d, alive=",
               match, matches, ticks, G.gameState);
        for (int i = 0; i < G.numPlayers; i++) printf("%d ", G.tanks[i].hp);
        printf(")\n");
        return 1;
    }

    printf("PASS: %d matches, %ld ticks total, ~%d special shots consumed\n",
           matches, ticks, shotsFired);
    return 0;
}

/* ---------- render test: dump framebuffer screenshots as PPM ---------- */
static void dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", G.W, G.H);
    const uint8_t *fb = render_fb();
    for (int i = 0; i < G.W * G.H; i++)
        fwrite(fb + i * 4, 1, 3, f);
    fclose(f);
    printf("wrote %s\n", path);
}

static int render_test(unsigned seed)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    srand(seed);
    frand_seed(seed * 2654435761u + 1);
    set_defaults();
    G.headless = true;
    G.W = 1000;
    G.H = 640;
    render_init(G.W, G.H);

    game_reset_to_start();
    render_frame();
    dump_ppm("render_start.ppm");

    G.pEnabled[1] = G.pEnabled[2] = G.pEnabled[3] = true;

    /* store screen: enter as a human shopper, screenshot, then hand the
     * reins back to the AI flow */
    G.headless = false;
    game_start_from_menu();
    if (G.gameState == GS_STORE) {
        render_frame();
        dump_ppm("render_store.ppm");
    }
    G.headless = true;
    G.tanks[0].isAI = true;
    G.tanks[0].strategy = rand() % STRAT_COUNT;
    game_store_confirm();
    for (int i = 0; i < 400; i++) game_tick();
    render_frame();
    dump_ppm("render_mid.ppm");

    /* run until we catch a frame with a projectile or explosion in flight */
    for (int tries = 0; tries < 20000; tries++) {
        game_tick();
        bool action = false;
        for (int i = 0; i < MAX_PROJECTILES; i++)
            if (G.projectiles[i].active) action = true;
        if (action && G.frameCount % 7 == 0) {
            for (int j = 0; j < 12; j++) game_tick();  /* mid-flight/explosion */
            break;
        }
    }
    render_frame();
    dump_ppm("render_action.ppm");

    for (long i = 0; i < 200000 && G.gameState != GS_GAMEOVER; i++) game_tick();
    render_frame();
    dump_ppm("render_gameover.ppm");
    render_shutdown();
    return 0;
}

/* ---------- interactive ---------- */
static int run(void)
{
    set_defaults();
    options_load();

    int w, h;
    if (!term_init(&w, &h)) {
        fprintf(stderr, "bashed-earth: needs an interactive terminal with the kitty\n");
        fprintf(stderr, "graphics protocol (kitty/kilix). Or try --selftest.\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(term_shutdown);

    srand((unsigned)time(NULL));
    G.W = w;
    G.H = h;
    render_init(w, h);
    sound_init();
    sound_set_enabled(G.soundOn);
    game_reset_to_start();

    const double FRAME_MS = 1000.0 / 30;   /* 30 fps render, 60 Hz logic */
    double next = now_ms();

    while (!G.quit) {
        int key;
        while ((key = term_poll_key()) != -1)
            game_handle_key(key);

        game_tick();
        game_tick();

        render_frame();
        term_present(render_fb(), G.W, G.H);

        next += FRAME_MS;
        double wait = next - now_ms();
        if (wait < -100) next = now_ms();   /* fell behind badly; resync */
        sleep_ms(wait);
    }

    sound_shutdown();
    render_shutdown();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        int matches = argc > 3 ? atoi(argv[3]) : 3;
        return selftest(seed, matches);
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 1337;
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("bashed-earth 1.0.0\n");
        return 0;
    }
    return run();
}
