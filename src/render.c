/* Software renderer: draws the scene + UI into an RGBA framebuffer that
 * term.c ships to the terminal. Visuals mirror the web canvas version. */
#include "bashed_earth.h"
#include "font8x16.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static uint8_t *fb = NULL;      /* RGBA */
static uint8_t *sky = NULL;     /* cached background */
static int W = 0, H = 0;
static int OX = 0, OY = 0;      /* camera shake offset for scene drawing */

uint8_t *render_fb(void) { return fb; }

void render_init(int w, int h)
{
    W = w; H = h;
    fb = malloc((size_t)W * H * 4);
    sky = malloc((size_t)W * H * 4);

    /* sky gradient: #0a0a0c -> #12121a -> #1a1a24, plus deterministic stars */
    for (int y = 0; y < H; y++) {
        float t = (float)y / H;
        uint8_t r, g, b;
        if (t < 0.5f) {
            float u = t * 2;
            r = (uint8_t)(0x0a + (0x12 - 0x0a) * u);
            g = (uint8_t)(0x0a + (0x12 - 0x0a) * u);
            b = (uint8_t)(0x0c + (0x1a - 0x0c) * u);
        } else {
            float u = (t - 0.5f) * 2;
            r = (uint8_t)(0x12 + (0x1a - 0x12) * u);
            g = (uint8_t)(0x12 + (0x1a - 0x12) * u);
            b = (uint8_t)(0x1a + (0x24 - 0x1a) * u);
        }
        uint8_t *row = sky + (size_t)y * W * 4;
        for (int x = 0; x < W; x++) {
            row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255;
        }
    }
    for (int i = 0; i < 100; i++) {
        int x = (i * 137) % W;
        int y = (i * 73) % (int)(H * 0.6f);
        int size = 1 + (i % 3);
        float a = 0.15f + (i % 10) * 0.08f;
        uint8_t v = (uint8_t)(255 * a);
        for (int dy = 0; dy < size && y + dy < H; dy++)
            for (int dx = 0; dx < size && x + dx < W; dx++) {
                uint8_t *p = sky + ((size_t)(y + dy) * W + x + dx) * 4;
                if (v > p[0]) { p[0] = p[1] = p[2] = v; }
            }
    }
}

void render_shutdown(void)
{
    free(fb); free(sky);
    fb = sky = NULL;
}

/* ---------- primitives ---------- */
static inline void px_blend(int x, int y, uint32_t rgb, float a)
{
    x += OX; y += OY;
    if (x < 0 || x >= W || y < 0 || y >= H || a <= 0) return;
    uint8_t *p = fb + ((size_t)y * W + x) * 4;
    float ia = 1 - a;
    p[0] = (uint8_t)(((rgb >> 16) & 255) * a + p[0] * ia);
    p[1] = (uint8_t)(((rgb >> 8) & 255) * a + p[1] * ia);
    p[2] = (uint8_t)((rgb & 255) * a + p[2] * ia);
}

static void fill_rect(float fx, float fy, float fw, float fh, uint32_t rgb, float a)
{
    int x0 = (int)fx, y0 = (int)fy, x1 = (int)(fx + fw), y1 = (int)(fy + fh);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            px_blend(x, y, rgb, a);
}

static void fill_circle(float cx, float cy, float r, uint32_t rgb, float a)
{
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1;
    int y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    float r2 = r * r;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r2) px_blend(x, y, rgb, a);
        }
}

/* soft radial glow: alpha fades linearly to 0 at r */
static void glow_circle(float cx, float cy, float r, uint32_t rgb, float a)
{
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1;
    int y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < r) px_blend(x, y, rgb, a * (1 - d / r));
        }
}

static void ring(float cx, float cy, float r, float width, uint32_t rgb,
                 float a, int dashOn, int dashOff)
{
    int steps = (int)(r * 6.283f);
    if (steps < 12) steps = 12;
    int period = dashOn + dashOff;
    for (int i = 0; i < steps; i++) {
        if (period > 0 && (i % period) >= dashOn) continue;
        float t = 6.283f * i / steps;
        float x = cx + cosf(t) * r, y = cy + sinf(t) * r;
        for (float wx = 0; wx < width; wx += 1)
            px_blend((int)(x), (int)(y + wx - width / 2), rgb, a);
    }
}

static void draw_line(float x0, float y0, float x1, float y1, float width,
                      uint32_t rgb, float a, int dashOn, int dashOff)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    int steps = (int)len + 1;
    int period = dashOn + dashOff;
    float hw = width / 2;
    for (int i = 0; i <= steps; i++) {
        if (period > 0 && (i % period) >= dashOn) continue;
        float t = (float)i / steps;
        float x = x0 + dx * t, y = y0 + dy * t;
        if (width <= 1.5f) px_blend((int)x, (int)y, rgb, a);
        else fill_circle(x, y, hw, rgb, a);
    }
}

static int text_width(const char *s, int scale) { return (int)strlen(s) * FONT_W * scale; }

static void draw_text(float fx, float fy, const char *s, uint32_t rgb,
                      float a, int scale)
{
    int x = (int)fx, y = (int)fy;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) c = '?';
        const unsigned char *glyph = font8x16[c - 32];
        for (int gy = 0; gy < FONT_H; gy++) {
            unsigned char bits = glyph[gy];
            for (int gx = 0; gx < FONT_W; gx++) {
                if (!(bits & (0x80 >> gx))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px_blend(x + gx * scale + sx, y + gy * scale + sy, rgb, a);
            }
        }
        x += FONT_W * scale;
    }
}

static void draw_text_outlined(float x, float y, const char *s, uint32_t rgb,
                               float a, int scale)
{
    draw_text(x - 1, y, s, 0x000000, a, scale);
    draw_text(x + 1, y, s, 0x000000, a, scale);
    draw_text(x, y - 1, s, 0x000000, a, scale);
    draw_text(x, y + 1, s, 0x000000, a, scale);
    draw_text(x, y, s, rgb, a, scale);
}

static void draw_text_center(float cx, float y, const char *s, uint32_t rgb,
                             float a, int scale)
{
    draw_text(cx - text_width(s, scale) / 2.0f, y, s, rgb, a, scale);
}

/* ---------- scene pieces ---------- */
static void draw_terrain(void)
{
    const uint8_t *grid = terrain_grid();
    if (!grid) return;
    /* terrain grid is exactly W x H; blit with palette (respect shake) */
    for (int y = 0; y < H; y++) {
        int sy = y + OY;
        if (sy < 0 || sy >= H) continue;
        const uint8_t *src = grid + (size_t)y * W;
        uint8_t *dst = fb + (size_t)sy * W * 4;
        for (int x = 0; x < W; x++) {
            uint8_t c = src[x];
            if (!c) continue;
            int sx = x + OX;
            if (sx < 0 || sx >= W) continue;
            uint32_t rgb = MATERIAL_COLORS[c];
            uint8_t *p = dst + (size_t)sx * 4;
            p[0] = (rgb >> 16) & 255;
            p[1] = (rgb >> 8) & 255;
            p[2] = rgb & 255;
        }
    }
}

static void draw_trajectory(float startX, float startY, float angle,
                            float power, uint32_t rgb, float a)
{
    float angleRad = angle * (float)M_PI / 180;
    float sx = startX, sy = startY;
    float svx = cosf(angleRad) * power * 0.15f;
    float svy = -sinf(angleRad) * power * 0.15f;
    for (int step = 0; step < 15; step++) {
        float nx, ny;
        svy += GRAVITY;
        svx += G.wind * 0.01f;
        nx = sx + svx;
        ny = sy + svy;
        draw_line(sx, sy, nx, ny, 1.5f, rgb, a, 4, 5);
        sx = nx; sy = ny;
        if (sx < 0 || sx > G.W || sy > G.H) break;
        if (terrain_is_ground(sx, sy)) break;
    }
}

static void draw_tank(Tank *tank)
{
    float cx = tank->x;
    float baseY = tank->y + TANK_HEIGHT / 2.0f;
    float bodyY = baseY - 8;
    float turretY = baseY - 12;
    bool isCurrent = tank->id == G.currentPlayer && G.gameState == GS_PLAYING;

    /* treads */
    fill_rect(cx - TANK_WIDTH / 2.0f, bodyY, TANK_WIDTH, 4, 0x222222, 1);
    for (int i = 0; i < 4; i++)
        fill_circle(cx - TANK_WIDTH / 2.0f + 4 + i * 6, bodyY + 4, 2.5f, 0x444444, 1);

    /* body + dome */
    fill_rect(cx - TANK_WIDTH / 2.0f + 2, bodyY - 5, TANK_WIDTH - 4, 5, tank->color, 1);
    fill_circle(cx, turretY + 1, 5, tank->color, 1);

    /* barrel */
    float aimAngle = tank->angle * (float)M_PI / 180;
    float bx = cx + cosf(aimAngle) * BARREL_LENGTH;
    float by = turretY - 2 - sinf(aimAngle) * BARREL_LENGTH;
    draw_line(cx, turretY - 2, bx, by, 3, tank->color, 1, 0, 0);

    if (isCurrent) {
        /* dashed highlight box */
        float hx = cx - TANK_WIDTH / 2.0f - 3, hy = bodyY - 9;
        float hw = TANK_WIDTH + 6, hh = TANK_HEIGHT + 6;
        draw_line(hx, hy, hx + hw, hy, 1, 0xffffff, 0.9f, 3, 3);
        draw_line(hx, hy + hh, hx + hw, hy + hh, 1, 0xffffff, 0.9f, 3, 3);
        draw_line(hx, hy, hx, hy + hh, 1, 0xffffff, 0.9f, 3, 3);
        draw_line(hx + hw, hy, hx + hw, hy + hh, 1, 0xffffff, 0.9f, 3, 3);
        if (!tank->isAI)
            draw_trajectory(cx, turretY - 2, tank->angle, tank->power, 0xffffff, 0.25f);
    }

    /* name + hp bar */
    draw_text_outlined(cx - text_width(tank->name, 1) / 2.0f, bodyY - 36,
                       tank->name, 0xffffff, 1, 1);
    float hpPct = (float)tank->hp / MAX_HP;
    uint32_t hpColor = hpPct > 0.6f ? 0x22c55e : hpPct > 0.3f ? 0xf59e0b : 0xef4444;
    fill_rect(cx - 14, bodyY - 46, 28, 6, 0x000000, 0.5f);
    fill_rect(cx - 13, bodyY - 45, 26 * hpPct, 4, hpColor, 1);

    if (tank->shield > 0) {
        float t = G.frameCount * 0.1f;
        ring(cx, bodyY - 5, 18, 1.5f, 0x4488ff, 0.45f + sinf(t) * 0.15f, 0, 0);
    }
}

static void draw_scene(void)
{
    /* camera shake */
    float totalShake = G.cameraShake;
    OX = (int)((frandf() - 0.5f) * totalShake);
    OY = (int)((frandf() - 0.5f) * totalShake);

    memcpy(fb, sky, (size_t)W * H * 4);
    draw_terrain();

    for (int i = 0; i < G.numPlayers; i++)
        if (G.tanks[i].hp > 0) draw_tank(&G.tanks[i]);

    if (G.hasLastShot && G.gameState == GS_PLAYING)
        draw_trajectory(G.lastShotX, G.lastShotY, G.lastShotAngle,
                        G.lastShotPower, 0xffffff, 0.12f);

    for (int i = 0; i < MAX_PROJECTILES; i++) {
        Projectile *p = &G.projectiles[i];
        if (!p->active) continue;
        uint32_t col = WEAPONS[p->weapon].color;
        glow_circle(p->x, p->y, 10, col, 0.35f);
        fill_circle(p->x, p->y, 4, col, 1);
    }

    for (int i = 0; i < MAX_FLAMES; i++) {
        Flame *f = &G.flames[i];
        if (!f->active) continue;
        float flicker = 0.7f + frandf() * 0.3f;
        float a = fminf(1, f->life) * flicker;
        float r = 5 + frandf() * 3;
        glow_circle(f->x, f->y - 2, r * 2, 0xff3c00, 0.4f * a);
        glow_circle(f->x, f->y - 3, r * 1.2f, 0xff8221, 0.6f * a);
        glow_circle(f->x, f->y - 4, r * 0.6f, 0xffe68c, 0.9f * a);
    }

    for (int i = 0; i < MAX_DEBRIS; i++) {
        Debris *d = &G.debris[i];
        if (!d->active) continue;
        uint32_t col = ((uint32_t)d->r << 16) | ((uint32_t)d->g << 8) | d->b;
        fill_rect(d->x - d->size / 2, d->y - d->size / 2, d->size, d->size,
                  col, fminf(1, d->life));
    }

    for (int i = 0; i < MAX_SHOCKWAVES; i++) {
        Shockwave *s = &G.shockwaves[i];
        if (!s->active) continue;
        ring(s->x, s->y, s->radius, 2, 0xffffff, s->alpha * 0.5f, 0, 0);
    }

    for (int i = 0; i < MAX_MARKERS; i++) {
        ImpactMarker *m = &G.markers[i];
        if (!m->active) continue;
        float a = fminf(1, m->life) * 0.35f;
        float pulse = 1 + sinf(m->life * 6) * 0.08f;
        ring(m->x, m->y, m->radius * pulse, 1.5f, 0xff643c, a, 3, 4);
    }

    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        float a = p->type == P_SMOKE ? p->life * 0.4f : p->life;
        fill_circle(p->x, p->y, p->size, p->color, clampf(a, 0, 1));
    }

    for (int i = 0; i < MAX_DAMAGE_NUMS; i++) {
        DamageNumber *d = &G.damageNums[i];
        if (!d->active) continue;
        float a = fminf(1, d->life);
        draw_text_outlined(d->x - text_width(d->text, 1) / 2.0f, d->y - 8,
                           d->text, d->color, a, 1);
    }

    OX = OY = 0;

    if (G.screenFlash > 0)
        fill_rect(0, 0, W, H, 0xffc896, G.screenFlash * 0.3f);
}

/* ---------- HUD & overlays ---------- */
static const char *SETTING_NAMES[5] = { "Random", "None", "Light", "Medium", "Strong" };
static const char *TERRAIN_NAMES[4] = { "Random", "Grass", "Sand", "Ice" };

static void draw_hud(void)
{
    char buf[128];
    Tank *tank = &G.tanks[G.currentPlayer];

    /* top-left: turn info */
    fill_rect(8, 8, 300, 56, 0x18181b, 0.85f);
    snprintf(buf, sizeof buf, "MATCH %d - ROUND %d", G.matchNumber, G.roundCount + 1);
    draw_text(18, 14, buf, 0x71717a, 1, 1);
    snprintf(buf, sizeof buf, "%s%s", tank->name, tank->isAI ? " (AI)" : "");
    draw_text(18, 34, buf, tank->color, 1, 1);

    /* top-right: wind */
    fill_rect(W - 178, 8, 170, 42, 0x18181b, 0.85f);
    draw_text(W - 168, 12, "WIND", 0x71717a, 1, 1);
    float wmag = fabsf(G.wind);
    snprintf(buf, sizeof buf, "%s %.1f", G.wind > 0.5f ? ">>" : G.wind < -0.5f ? "<<" : "--", wmag);
    draw_text(W - 168, 28, buf, wmag > 6 ? 0xf59e0b : 0xfafafa, 1, 1);

    /* bottom bar */
    int by = H - 58;
    fill_rect(0, by, W, 58, 0x101012, 0.88f);
    if (!tank->isAI && (G.gameState == GS_PLAYING || G.gameState == GS_ANIMATING ||
                        G.gameState == GS_TURN_ENDING)) {
        const Weapon *w = &WEAPONS[G.currentWeapon];
        int ammo = game_player_ammo(G.currentPlayer, G.currentWeapon);
        snprintf(buf, sizeof buf, "%s x%d", w->name, ammo > 900 ? 99 : ammo);
        draw_text(16, by + 8, buf, w->color, 1, 1);
        snprintf(buf, sizeof buf, "ANGLE %3d   POWER %3d/%3d   HP %3d   $%d",
                 (int)tank->angle, (int)tank->power, (int)tank->maxPower,
                 tank->hp, G.wallets[G.currentPlayer]);
        draw_text(16, by + 28, buf, 0xfafafa, 1, 1);
        draw_text(W - 540, by + 8,
                  "ARROWS aim/power  SPACE fire  TAB/1-0/D/R weapon", 0x71717a, 1, 1);
        draw_text(W - 540, by + 28, "Q quit", 0x71717a, 1, 1);
    } else {
        snprintf(buf, sizeof buf, "%s is taking their turn...", tank->name);
        draw_text(16, by + 18, buf, 0xa1a1aa, 1, 1);
    }
}

static void panel(int pw, int ph, int *px, int *py)
{
    *px = (W - pw) / 2;
    *py = (H - ph) / 2;
    fill_rect(*px - 2, *py - 2, pw + 4, ph + 4, 0x3b82f6, 0.4f);
    fill_rect(*px, *py, pw, ph, 0x121215, 0.96f);
}

static void draw_start_menu(void)
{
    char buf[128];
    int px, py;
    panel(620, 480, &px, &py);
    draw_text_center(W / 2.0f, py + 18, "BASHED EARTH", 0x3b82f6, 1, 3);
    draw_text_center(W / 2.0f, py + 68, "terminal artillery -- a Tank Wars port", 0x71717a, 1, 1);

    const char *rows[START_ROWS];
    char rowbuf[START_ROWS][96];
    for (int p = 1; p <= 3; p++) {
        const char *v = !G.pEnabled[p] ? "Off"
            : G.pStrategy[p] < 0 ? "AI: Random"
            : AI_STRATEGIES[G.pStrategy[p]].name;
        snprintf(rowbuf[p - 1], 96, "Player %d          < %s >", p + 1, v);
    }
    snprintf(rowbuf[3], 96, "Terrain           < %s >", TERRAIN_NAMES[G.terrainSetting]);
    snprintf(rowbuf[4], 96, "Wind              < %s >", SETTING_NAMES[G.windSetting]);
    snprintf(rowbuf[5], 96, "Precipitation     < %s >", SETTING_NAMES[G.precipSetting]);
    snprintf(rowbuf[6], 96, "Damage            < %.1fx >", G.damageMultiplier);
    snprintf(rowbuf[7], 96, "Wall bounce       < %s >", G.wallBounce ? "On" : "Off");
    snprintf(rowbuf[8], 96, "        START  ");
    for (int i = 0; i < START_ROWS; i++) rows[i] = rowbuf[i];

    for (int i = 0; i < START_ROWS; i++) {
        int ry = py + 110 + i * 34;
        bool sel = G.startCursor == i;
        if (sel) fill_rect(px + 40, ry - 4, 540, 26, 0x27272a, 1);
        draw_text(px + 60, ry, rows[i], i == 8 ? 0x22c55e : sel ? 0xfafafa : 0xa1a1aa,
                  1, i == 8 ? 2 : 1);
    }
    draw_text_center(W / 2.0f, py + 440,
                     "UP/DOWN select  LEFT/RIGHT change  ENTER start  Q quit",
                     0x71717a, 1, 1);
    snprintf(buf, sizeof buf, "up to 4 tanks, 5 AI personalities");
    draw_text_center(W / 2.0f, py + 92, buf, 0x52525b, 1, 1);
}

static void draw_store(void)
{
    char buf[160];
    int px, py;
    int ph = 120 + STORE_ITEMS * 26 + 60;
    panel(700, ph, &px, &py);
    draw_text_center(W / 2.0f, py + 14, "ARMORY", 0x3b82f6, 1, 2);

    Tank *shopper = &G.tanks[G.storePlayer];
    snprintf(buf, sizeof buf, "%s -- $%d remaining", shopper->name,
             game_money_left(G.storePlayer));
    draw_text_center(W / 2.0f, py + 54, buf, 0xfafafa, 1, 1);
    snprintf(buf, sizeof buf, "match %d", G.matchNumber);
    draw_text_center(W / 2.0f, py + 74, buf, 0x52525b, 1, 1);

    for (int i = 0; i < STORE_ITEMS; i++) {
        int w = STORE_ORDER[i];
        int ry = py + 100 + i * 26;
        bool sel = G.storeCursor == i;
        if (sel) fill_rect(px + 20, ry - 3, 660, 24, 0x27272a, 1);
        int owned = G.purchases[G.storePlayer][w];
        int carried = G.carry[G.storePlayer][w];
        snprintf(buf, sizeof buf, "%-14s $%-5d %s", WEAPONS[w].name,
                 WEAPONS[w].cost, WEAPONS[w].blurb);
        draw_text(px + 36, ry, buf, WEAPONS[w].color, 1, 1);
        if (carried > 0)
            snprintf(buf, sizeof buf, "x%-3d (%d owned)", owned, carried);
        else
            snprintf(buf, sizeof buf, "x%-3d", owned);
        draw_text(px + 540, ry, buf, owned > carried ? 0x22c55e : 0xa1a1aa, 1, 1);
    }
    draw_text_center(W / 2.0f, py + ph - 40,
                     "UP/DOWN select  RIGHT/+ buy  LEFT/- refund  Z reset  ENTER done",
                     0x71717a, 1, 1);
}

static void draw_gameover(void)
{
    char buf[160];
    int px, py;
    panel(560, 300 + G.numPlayers * 30, &px, &py);
    draw_text_center(W / 2.0f, py + 16, "MATCH OVER", 0x3b82f6, 1, 2);

    if (G.lastWinnerId >= 0) {
        Tank *wt = &G.tanks[G.lastWinnerId];
        snprintf(buf, sizeof buf, "%s Wins!", wt->name);
        draw_text_center(W / 2.0f, py + 66, buf, wt->color, 1, 3);
    } else {
        draw_text_center(W / 2.0f, py + 66, "It's a Draw!", 0xfafafa, 1, 3);
    }
    snprintf(buf, sizeof buf, "Match %d - %d round%s", G.matchNumber,
             G.roundCount + 1, G.roundCount == 0 ? "" : "s");
    draw_text_center(W / 2.0f, py + 126, buf, 0xa1a1aa, 1, 1);

    for (int i = 0; i < G.numPlayers; i++) {
        Tank *t = &G.tanks[i];
        int ry = py + 160 + i * 30;
        fill_rect(px + 40, ry - 4, 480, 26, 0x1c1c1f, 1);
        fill_circle(px + 56, ry + 8, 5, t->color, 1);
        int nextMoney = G.wallets[i] + STARTING_MONEY + (i == G.lastWinnerId ? 2000 : 0);
        snprintf(buf, sizeof buf, "%-12s wins:%d   $%d next", t->name,
                 G.matchWins[i], nextMoney);
        draw_text(px + 72, ry, buf, 0xfafafa, 1, 1);
    }
    draw_text_center(W / 2.0f, py + 180 + G.numPlayers * 30,
                     "ENTER next match   Q quit", 0x71717a, 1, 1);
}

void render_frame(void)
{
    if (!fb) return;
    draw_scene();
    switch (G.gameState) {
    case GS_START: draw_start_menu(); break;
    case GS_STORE: draw_store(); break;
    case GS_GAMEOVER: draw_hud(); draw_gameover(); break;
    default: draw_hud(); break;
    }
}
