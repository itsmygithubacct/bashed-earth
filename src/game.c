/* Game logic: tanks, projectiles, flames, explosions, AI, store, turn flow. */
#include "bashed_earth.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

GameState G;
const char *g_phase = "init";   /* selftest watchdog marker */

/* fast inline xorshift32 — the automaton rolls this millions of times per
 * second, so libc rand() is too slow for it */
static uint32_t xs_state = 0x9e3779b9u;
void frand_seed(uint32_t s) { xs_state = s ? s : 0x9e3779b9u; }
float frandf(void)
{
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 17;
    xs_state ^= xs_state << 5;
    return (xs_state >> 8) * (1.0f / 16777216.0f);
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------- entity pools ---------- */
static void push_particle(float x, float y, float vx, float vy, float life,
                          float decay, uint32_t color, float size, int type)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!G.particles[i].active) {
            G.particles[i] = (Particle){ true, x, y, vx, vy, life, decay, size, color, type };
            return;
        }
    }
}

static void push_debris(float x, float y, float vx, float vy, float life,
                        uint8_t r, uint8_t g, uint8_t b, float size)
{
    for (int i = 0; i < MAX_DEBRIS; i++) {
        if (!G.debris[i].active) {
            G.debris[i] = (Debris){ true, x, y, vx, vy, life, size,
                                    frandf() * 6.283f, (frandf() - 0.5f) * 0.3f, r, g, b };
            return;
        }
    }
}

static void push_damage_num(float x, float y, int value, uint32_t color)
{
    for (int i = 0; i < MAX_DAMAGE_NUMS; i++) {
        if (!G.damageNums[i].active) {
            G.damageNums[i].active = true;
            G.damageNums[i].x = x; G.damageNums[i].y = y;
            G.damageNums[i].vy = -1; G.damageNums[i].life = 1;
            G.damageNums[i].color = color;
            G.damageNums[i].isText = false;
            snprintf(G.damageNums[i].text, sizeof G.damageNums[i].text, "%d", value);
            return;
        }
    }
}

void add_damage_text(float x, float y, const char *text, uint32_t color)
{
    for (int i = 0; i < MAX_DAMAGE_NUMS; i++) {
        if (!G.damageNums[i].active) {
            G.damageNums[i].active = true;
            G.damageNums[i].x = x; G.damageNums[i].y = y;
            G.damageNums[i].vy = -0.4f; G.damageNums[i].life = 1.8f;
            G.damageNums[i].color = color;
            G.damageNums[i].isText = true;
            snprintf(G.damageNums[i].text, sizeof G.damageNums[i].text, "%s", text);
            return;
        }
    }
}

static Projectile *push_projectile(void)
{
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (!G.projectiles[i].active) {
            memset(&G.projectiles[i], 0, sizeof(Projectile));
            G.projectiles[i].active = true;
            return &G.projectiles[i];
        }
    return NULL;
}

static int live_projectiles(void)
{
    int n = 0;
    for (int i = 0; i < MAX_PROJECTILES; i++) n += G.projectiles[i].active;
    return n;
}

static int live_flames(void)
{
    int n = 0;
    for (int i = 0; i < MAX_FLAMES; i++) n += G.flames[i].active;
    return n;
}

static void clear_effects(void)
{
    memset(G.projectiles, 0, sizeof G.projectiles);
    memset(G.particles, 0, sizeof G.particles);
    memset(G.debris, 0, sizeof G.debris);
    memset(G.shockwaves, 0, sizeof G.shockwaves);
    memset(G.markers, 0, sizeof G.markers);
    memset(G.damageNums, 0, sizeof G.damageNums);
    memset(G.flames, 0, sizeof G.flames);
    G.hasLastShot = false;
}

/* ---------- terrain interaction ---------- */
static void check_tanks_falling(void)
{
    for (int i = 0; i < G.numPlayers; i++) {
        Tank *t = &G.tanks[i];
        if (t->hp <= 0) continue;
        float surf = terrain_get_height(t->x);
        if (t->y < surf - TANK_HEIGHT * 2 && !t->falling) {
            t->falling = true;
            t->onGround = false;
        }
    }
}

static void explode_terrain(float x, float y, float radius, bool addTerrain)
{
    if (addTerrain) {
        terrain_add_radius(x, y, radius);
    } else {
        terrain_remove_radius(x, y, radius);
        if (G.terrainType == TERRAIN_ICE) terrain_check_ice_stability();
        float vapR = radius * 1.6f;
        int cleared = terrain_vaporize_liquid(x, y, vapR);
        if (cleared > 0) {
            int steamCount = 6 + cleared / 8;
            if (steamCount > 40) steamCount = 40;
            for (int i = 0; i < steamCount; i++) {
                float a = frandf() * 6.283f, r = frandf() * vapR * 0.8f;
                push_particle(x + cosf(a) * r, y + sinf(a) * r,
                              (frandf() - 0.5f) * 0.6f, -1 - frandf() * 1.8f,
                              1, 0.008f + frandf() * 0.008f, 0xdfeef8,
                              4 + frandf() * 6, P_SMOKE);
            }
        }
    }
    check_tanks_falling();
}

/* ---------- explosion ---------- */
static const uint32_t FIRE_COLORS[3] = { 0xff6b6b, 0xffa502, 0xffdd59 };
static const uint32_t SMOKE_COLORS[3] = { 0x888888, 0x666666, 0x444444 };

void create_explosion(float x, float y, float radius, float damage, int weaponType)
{
    const Weapon *weapon = &WEAPONS[weaponType];

    if (weaponType == W_DIRT)
        sound_play(SFX_DIRT, 0.8f, 1);
    else
        sound_play(radius >= 50 ? SFX_EXPL_BIG : SFX_EXPL_SMALL,
                   clampf(0.4f + radius / 90.0f, 0, 1), 1);

    explode_terrain(x, y, radius, weaponType == W_DIRT);

    int fireCount = (int)(radius * 0.8f);
    for (int i = 0; i < fireCount; i++) {
        float angle = 6.283f * i / fireCount + (frandf() - 0.5f) * 0.5f;
        float speed = 1 + frandf() * 4;
        push_particle(x, y, cosf(angle) * speed, sinf(angle) * speed - 2,
                      1, 0.015f + frandf() * 0.015f,
                      FIRE_COLORS[rand() % 3], 3 + frandf() * 5, P_FIRE);
    }
    int smokeCount = (int)(radius * 0.5f);
    for (int i = 0; i < smokeCount; i++) {
        float angle = frandf() * 6.283f, speed = 0.5f + frandf() * 2;
        push_particle(x, y, cosf(angle) * speed,
                      sinf(angle) * speed - 1 - frandf(),
                      1, 0.005f + frandf() * 0.005f,
                      SMOKE_COLORS[rand() % 3], 5 + frandf() * 8, P_SMOKE);
    }
    for (int i = 0; i < 15; i++) {
        float angle = frandf() * 6.283f, speed = 3 + frandf() * 6;
        push_particle(x, y, cosf(angle) * speed, sinf(angle) * speed - 1,
                      1, 0.03f + frandf() * 0.02f, 0xffff88,
                      1 + frandf() * 2, P_SPARK);
    }
    for (int i = 0; i < 20; i++) {
        float angle = frandf() * 6.283f, speed = 2 + frandf() * 6;
        push_debris(x, y, cosf(angle) * speed, sinf(angle) * speed - 3,
                    1 + frandf(),
                    (uint8_t)(100 + frandf() * 100), (uint8_t)(80 + frandf() * 60),
                    (uint8_t)(40 + frandf() * 40), 2 + frandf() * 4);
    }
    for (int i = 0; i < MAX_SHOCKWAVES; i++)
        if (!G.shockwaves[i].active) {
            G.shockwaves[i] = (Shockwave){ true, x, y, 0, radius * 2, 1 };
            break;
        }
    for (int i = 0; i < MAX_MARKERS; i++)
        if (!G.markers[i].active) {
            G.markers[i] = (ImpactMarker){ true, x, y, radius * 0.7f, 3 };
            break;
        }

    G.cameraShake = fminf(G.cameraShake + radius / 15, 15);
    if (radius > 50) G.screenFlash = fminf(G.screenFlash + 0.5f, 1);

    /* fire weapons scatter burning globs */
    if (weapon->fire) {
        int spawned = 0;
        for (int i = 0; i < MAX_FLAMES && spawned < 26; i++) {
            if (G.flames[i].active) continue;
            G.flames[i] = (Flame){ true,
                x + (frandf() - 0.5f) * radius * 1.4f,
                y - frandf() * radius * 0.5f,
                (frandf() - 0.5f) * 4, -1 - frandf() * 2.5f,
                2.2f + frandf() * 1.3f, false };
            spawned++;
        }
    }

    /* damage tanks */
    for (int ti = 0; ti < G.numPlayers; ti++) {
        Tank *tank = &G.tanks[ti];
        if (tank->hp <= 0) continue;
        float dx = tank->x - x;
        float dy = (tank->y - TANK_HEIGHT / 2.0f) - y;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist >= radius + TANK_WIDTH) continue;

        float dmgFactor = 1 - dist / (radius + TANK_WIDTH);
        int actualDmg = (int)(damage * dmgFactor * dmgFactor * G.damageMultiplier);
        if (actualDmg <= 0) continue;

        if (tank->shield > 0) {
            tank->shield--;
            G.ammo[tank->id][W_SHIELD] = tank->shield;
            add_damage_text(tank->x, tank->y - 40, "[BLOCKED]", 0x4488ff);
            sound_play(SFX_SHIELD, 0.8f, 1);
            continue;
        }

        tank->hp -= actualDmg;
        if (tank->hp < 0) tank->hp = 0;
        push_damage_num(tank->x, tank->y - 30, actualDmg, 0xff6666);

        /* damage reduces max power: 4 damage = -1 max power */
        float powerLoss = actualDmg / 4.0f;
        tank->maxPower = fmaxf(10, tank->maxPower - powerLoss);
        tank->power = fminf(tank->power, tank->maxPower);

        float knockback = dmgFactor * 3;
        if (dist > 0) {
            tank->vx += (dx / dist) * knockback;
            tank->vy -= knockback;
        }
        tank->falling = true;

        if (tank->hp == 0 && !tank->dead) {
            tank->dead = true;
            if (tank->isAI)
                add_damage_text(tank->x, tank->y - 50,
                                AI_DEATH_MESSAGES[rand() % 10], 0xffff00);
            sound_play(SFX_DEATH, 1, 1);
            create_explosion(tank->x, tank->y, 45, 0, W_NORMAL);
        }
    }
}

/* ---------- tanks ---------- */
static void spawn_tanks(void)
{
    float spacing = (float)G.W / (G.numPlayers + 1);
    for (int i = 0; i < G.numPlayers; i++) {
        Tank *t = &G.tanks[i];
        float x = spacing * (i + 1) + (frandf() - 0.5f) * spacing * 0.3f;
        x = clampf(x, 50, G.W - 50);
        float surfY = terrain_get_height(x);
        t->id = i;
        t->x = x;
        t->y = surfY - TANK_HEIGHT / 2.0f;
        t->vx = t->vy = 0;
        t->hp = t->maxHp = MAX_HP;
        t->shield = 0;
        t->angle = x < G.W / 2.0f ? 45 : 135;
        t->power = 50;
        t->maxPower = 100;
        t->groundAngle = terrain_get_slope(x);
        t->color = TANK_COLORS[i];
        t->falling = false;
        t->dead = false;
        t->onGround = true;
        t->parachuteActive = t->fallShielded = false;
        t->buriedTimer = 0;
        t->selectedWeapon = W_NORMAL;
    }
}

static void check_buried_tanks(void)
{
    for (int ti = 0; ti < G.numPlayers; ti++) {
        Tank *tank = &G.tanks[ti];
        if (tank->hp <= 0) continue;
        int covered = 0, total = 0;
        for (int dx = -TANK_WIDTH / 2; dx <= TANK_WIDTH / 2; dx += 3)
            for (int dy = -TANK_HEIGHT; dy <= 0; dy += 3) {
                total++;
                if (terrain_is_ground(tank->x + dx, tank->y + dy)) covered++;
            }
        if ((float)covered / total > 0.6f) {
            tank->buriedTimer++;
            if (tank->buriedTimer > 30) {
                int buriedDmg = (int)(5 * G.damageMultiplier);
                tank->hp -= buriedDmg;
                if (tank->hp < 0) tank->hp = 0;
                tank->buriedTimer = 0;
                char msg[32];
                snprintf(msg, sizeof msg, "BURIED! -%d", buriedDmg);
                add_damage_text(tank->x, tank->y - 50, msg, 0x8b4513);
                sound_play(SFX_DIRT, 0.5f, 1.2f);
                tank->maxPower = fmaxf(10, tank->maxPower - buriedDmg / 4.0f);
                tank->power = fminf(tank->power, tank->maxPower);
                if (tank->hp <= 0 && !tank->dead) {
                    tank->dead = true;
                    sound_play(SFX_DEATH, 1, 1);
                    create_explosion(tank->x, tank->y, 40, 0, W_NORMAL);
                }
            }
        } else {
            tank->buriedTimer = 0;
        }
    }
}

static void update_tanks(void)
{
    for (int ti = 0; ti < G.numPlayers; ti++) {
        Tank *tank = &G.tanks[ti];
        if (tank->hp <= 0) continue;

        if (fabsf(tank->vx) > 0.1f || fabsf(tank->vy) > 0.1f || tank->falling) {
            /* auto-deploy parachute on the first frame of a real fall */
            if (!tank->parachuteActive && !tank->fallShielded && tank->vy > 1.5f) {
                if (G.ammo[tank->id][W_PARACHUTE] > 0) {
                    G.ammo[tank->id][W_PARACHUTE]--;
                    tank->parachuteActive = true;
                    add_damage_text(tank->x, tank->y - 60, "PARACHUTE!", 0xe85d3d);
                }
            }

            tank->x += tank->vx;
            tank->y += tank->vy;
            if (tank->parachuteActive) {
                tank->vy = fminf(tank->vy, 1.2f);
                tank->vx *= 0.92f;
            } else {
                tank->vy += GRAVITY;
                tank->vx *= 0.95f;
            }
            tank->falling = true;

            float surfY = terrain_get_height(tank->x);
            if (tank->y >= surfY - TANK_HEIGHT / 2.0f) {
                float fallDist = (surfY - TANK_HEIGHT / 2.0f) - (tank->y - tank->vy);
                tank->y = surfY - TANK_HEIGHT / 2.0f;
                tank->groundAngle = terrain_get_slope(tank->x);

                if (fallDist > 20 && !tank->parachuteActive && tank->shield > 0) {
                    tank->shield--;
                    G.ammo[tank->id][W_SHIELD] = tank->shield;
                    add_damage_text(tank->x, tank->y - 40, "[BLOCKED]", 0x4488ff);
                    sound_play(SFX_SHIELD, 0.7f, 1);
                    tank->fallShielded = true;
                }
                if (fallDist > 20 && !tank->parachuteActive && !tank->fallShielded) {
                    int dmg = (int)(((fallDist - 20) / 3) * 2.5f * G.damageMultiplier);
                    tank->hp -= dmg;
                    if (tank->hp < 0) tank->hp = 0;
                    if (dmg > 0) {
                        push_damage_num(tank->x, tank->y - 30, dmg, 0xff6666);
                        tank->maxPower = fmaxf(10, tank->maxPower - dmg / 4.0f);
                        tank->power = fminf(tank->power, tank->maxPower);
                    }
                    if (dmg > 5) create_explosion(tank->x, tank->y, 20, 0, W_NORMAL);
                    if (tank->hp <= 0 && !tank->dead) {
                        tank->dead = true;
                        sound_play(SFX_DEATH, 1, 1);
                        create_explosion(tank->x, tank->y, 45, 0, W_NORMAL);
                    }
                }

                tank->vx = tank->vy = 0;
                tank->falling = false;
                tank->onGround = true;
                tank->parachuteActive = false;
                tank->fallShielded = false;
            }
            tank->x = clampf(tank->x, TANK_WIDTH / 2.0f, G.W - TANK_WIDTH / 2.0f);
        }

        if (!tank->falling) {
            float surfY = terrain_get_height(tank->x);
            tank->groundAngle = terrain_get_slope(tank->x);
            if (tank->y < surfY - TANK_HEIGHT * 2) {
                tank->falling = true;
                tank->onGround = false;
            } else if (fabsf(tank->groundAngle) > 0.4f) {
                tank->x += sinf(tank->groundAngle) * 0.5f;
                tank->y = surfY - TANK_HEIGHT / 2.0f;
            } else {
                tank->y = surfY - TANK_HEIGHT / 2.0f;
            }
        }
    }
}

/* ---------- weapons / firing ---------- */
int game_player_ammo(int player, int w)
{
    if (w == W_NORMAL) return 999;
    return G.ammo[player][w];
}

void game_select_weapon(int w)
{
    if (w < 0 || w >= WEAPON_COUNT) return;
    if (WEAPONS[w].isRaft || WEAPONS[w].isParachute || WEAPONS[w].isShield) return;
    if (w == W_NORMAL || game_player_ammo(G.currentPlayer, w) > 0) {
        G.currentWeapon = w;
        G.tanks[G.currentPlayer].selectedWeapon = w;
    }
}

void game_fire(void)
{
    if (G.gameState != GS_PLAYING) return;
    if (G.currentWeapon != W_NORMAL &&
        game_player_ammo(G.currentPlayer, G.currentWeapon) <= 0) {
        game_select_weapon(W_NORMAL);
        return;
    }

    Tank *tank = &G.tanks[G.currentPlayer];
    const Weapon *weapon = &WEAPONS[G.currentWeapon];

    if (G.currentWeapon != W_NORMAL && G.ammo[G.currentPlayer][G.currentWeapon] > 0)
        G.ammo[G.currentPlayer][G.currentWeapon]--;

    G.gameState = GS_ANIMATING;

    float angleRad = tank->angle * (float)M_PI / 180;
    float firePower = tank->power;
    float vx = cosf(angleRad) * firePower * 0.15f;
    float vy = -sinf(angleRad) * firePower * 0.15f;
    float barrelX = tank->x + cosf(angleRad) * BARREL_LENGTH;
    float barrelY = tank->y - TANK_HEIGHT / 2.0f - sinf(angleRad) * BARREL_LENGTH;

    G.hasLastShot = true;
    G.lastShotX = barrelX; G.lastShotY = barrelY;
    G.lastShotAngle = tank->angle; G.lastShotPower = firePower;
    G.lastShotColor = weapon->color;

    sound_play(SFX_FIRE, 0.85f, 0.85f + 0.3f * firePower / 100);

    if (G.currentWeapon == W_TRIPLE) {
        for (int i = -1; i <= 1; i++) {
            Projectile *p = push_projectile();
            if (!p) break;
            p->x = barrelX; p->y = barrelY;
            p->vx = vx + i * weapon->spread * 0.05f; p->vy = vy;
            p->weapon = G.currentWeapon;
            p->radius = weapon->blastRadius;
        }
    } else {
        Projectile *p = push_projectile();
        if (p) {
            p->x = barrelX; p->y = barrelY;
            p->vx = vx; p->vy = vy;
            p->weapon = G.currentWeapon;
            p->radius = weapon->blastRadius;
        }
    }
}

/* ---------- weather ---------- */
static void change_wind(void)
{
    float maxWind = WIND_MAX;
    switch (G.windSetting) {
    case SET_NONE:   G.wind = 0; return;
    case SET_LIGHT:  maxWind = 5; break;
    case SET_MEDIUM: maxWind = 10; break;
    case SET_STRONG: maxWind = 20; break;
    default: break;
    }
    G.wind = (frandf() - 0.5f) * 2 * maxWind;
}

static void change_precip(void)
{
    if (!G.precipMaterial) { G.precipRate = 0; G.precipBudget = 0; return; }
    float max; int budget;
    switch (G.precipSetting) {
    case SET_NONE:   max = 0;       budget = 0;    break;
    case SET_LIGHT:  max = 0.0015f; budget = 1500; break;
    case SET_MEDIUM: max = 0.004f;  budget = 4000; break;
    case SET_STRONG: max = 0.009f;  budget = 8000; break;
    default:         max = 0.006f;  budget = 5000; break;
    }
    G.precipRate = max <= 0 ? 0 : frandf() * max;
    G.precipBudget = budget;
}

/* ---------- turn flow ---------- */
static int alive_count(int *lastAlive)
{
    int n = 0;
    for (int i = 0; i < G.numPlayers; i++)
        if (G.tanks[i].hp > 0) { n++; if (lastAlive) *lastAlive = i; }
    return n;
}

static void next_turn(void)
{
    int lastAlive = -1;
    if (alive_count(&lastAlive) <= 1) {
        G.lastWinnerId = lastAlive;
        if (lastAlive >= 0) G.matchWins[lastAlive]++;
        G.gameState = GS_GAMEOVER;
        G.pendingAIStart = G.pendingAIFire = G.pendingNextTurn = 0;
        sound_play(SFX_WIN, 0.9f, 1);
        return;
    }

    /* Stalemate breaker: if no tank has taken damage for 60 consecutive
     * turns (both sides dug in / out of useful ammo), end the match in
     * favour of the healthiest tank rather than looping forever. */
    int totalHp = 0;
    for (int i = 0; i < G.numPlayers; i++) totalHp += G.tanks[i].hp;
    if (totalHp < G.lastTotalHp) G.staleTurns = 0;
    else G.staleTurns++;
    G.lastTotalHp = totalHp;
    if (G.staleTurns > 60) {
        int best = -1;
        for (int i = 0; i < G.numPlayers; i++)
            if (G.tanks[i].hp > 0 && (best < 0 || G.tanks[i].hp > G.tanks[best].hp))
                best = i;
        G.lastWinnerId = best;
        if (best >= 0) {
            G.matchWins[best]++;
            add_damage_text(G.tanks[best].x, G.tanks[best].y - 60,
                            "STALEMATE!", 0xf59e0b);
        }
        G.gameState = GS_GAMEOVER;
        G.pendingAIStart = G.pendingAIFire = G.pendingNextTurn = 0;
        sound_play(SFX_WIN, 0.9f, 1);
        return;
    }

    do {
        G.currentPlayer = (G.currentPlayer + 1) % G.numPlayers;
    } while (G.tanks[G.currentPlayer].hp <= 0);

    if (G.currentPlayer == 0) {
        G.roundCount++;
        change_wind();
        change_precip();
    }

    G.gameState = GS_PLAYING;
    G.hasLastShot = G.hasLastShot; /* ghost stays visible during aiming */

    Tank *tank = &G.tanks[G.currentPlayer];
    int restored = tank->selectedWeapon;
    if (WEAPONS[restored].isRaft ||
        (restored != W_NORMAL && game_player_ammo(G.currentPlayer, restored) <= 0))
        restored = W_NORMAL;
    G.currentWeapon = restored;
    tank->selectedWeapon = restored;

    /* auto-deploy raft when submerged */
    if (G.ammo[tank->id][W_RAFT] > 0) {
        float waterTop = terrain_get_water_top(tank->x);
        float solidTop = terrain_get_height(tank->x);
        if (waterTop >= 0 && (solidTop - waterTop) >= TANK_HEIGHT) {
            if (terrain_place_raft(tank->x, TANK_WIDTH * 1.5f)) {
                G.ammo[tank->id][W_RAFT]--;
                add_damage_text(tank->x, tank->y - 60, "RAFT DEPLOYED", 0x88ddff);
                sound_play(SFX_SPLASH, 0.6f, 0.8f);
                tank->falling = true;
            }
        }
    }

    if (!tank->isAI) {
        if (tank->x < G.W / 2.0f && tank->angle > 120) tank->angle = 120;
        else if (tank->x > G.W / 2.0f && tank->angle < 60) tank->angle = 60;
    }

    G.pendingAIStart = tank->isAI ? 1000 : 0;
}

/* ---------- projectiles ---------- */
static void update_projectiles(void)
{
    for (int pi = 0; pi < MAX_PROJECTILES; pi++) {
        Projectile *p = &G.projectiles[pi];
        if (!p->active) continue;
        const Weapon *weapon = &WEAPONS[p->weapon];

        if (!p->rolling) {
            p->vy += GRAVITY;
            p->vx += G.wind * 0.01f;
            if (terrain_is_liquid(p->x, p->y)) {
                if (!p->inLiquid) {
                    p->inLiquid = true;
                    sound_play(SFX_SPLASH, 0.65f, 1);
                    for (int s = 0; s < 8; s++)
                        push_particle(p->x, p->y, (frandf() - 0.5f) * 3,
                                      -frandf() * 2 - 1, 0.7f, 0.05f,
                                      0x88bbee, 1.5f + frandf() * 1.5f, P_SPARK);
                }
                p->vx *= 0.82f;
                p->vy = p->vy * 0.82f + 0.05f;
            } else {
                p->inLiquid = false;
            }
            p->x += p->vx;
            p->y += p->vy;
        }
        p->age++;

        if (frandf() < 0.4f)
            push_particle(p->x, p->y, (frandf() - 0.5f) * 0.5f,
                          (frandf() - 0.5f) * 0.5f, 0.5f, 0.05f,
                          weapon->color, 1 + frandf() * 2, P_TRAIL);

        /* bouncy */
        if (p->weapon == W_BOUNCY && p->bounces < weapon->bounces) {
            if (terrain_is_ground(p->x, p->y + 3)) {
                p->vy = -p->vy * 0.6f;
                p->vx *= 0.8f;
                p->y -= 5;
                p->bounces++;
                sound_play(SFX_BOUNCE, 0.7f, 1 + p->bounces * 0.08f);
                for (int j = 0; j < 5; j++)
                    push_particle(p->x, p->y, (frandf() - 0.5f) * 4,
                                  -frandf() * 3, 0.5f, 0.05f, 0xff88ff, 2, P_SPARK);
                continue;
            }
        }

        /* roller */
        if (p->weapon == W_ROLLER) {
            if (!p->rolling) {
                if (terrain_is_ground(p->x, p->y + 3)) {
                    p->rolling = true;
                    p->rvx = clampf(p->vx, -4, 4);
                    if (p->rvx == 0) p->rvx = frandf() < 0.5f ? -0.5f : 0.5f;
                    p->vy = 0;
                    p->y = terrain_get_height(p->x) - 3;
                }
            } else {
                if (terrain_is_liquid(p->x, p->y + 4)) {
                    create_explosion(p->x, p->y, p->radius, weapon->damage, p->weapon);
                    p->active = false;
                    continue;
                }
                float slope = terrain_get_slope(p->x);
                p->rvx = (p->rvx + sinf(slope) * 0.3f) * 0.985f;
                float prevY = p->y;
                p->x += p->rvx;
                float gh = terrain_get_height(p->x);
                if (gh < prevY - 12) {         /* wall too steep — rebound */
                    p->x -= p->rvx;
                    p->rvx = -p->rvx * 0.5f;
                }
                p->y = terrain_get_height(p->x) - 3;
                if (p->x < 2) { p->x = 2; p->rvx = fabsf(p->rvx) * 0.8f; }
                if (p->x > G.W - 2) { p->x = G.W - 2; p->rvx = -fabsf(p->rvx) * 0.8f; }
                p->stall = fabsf(p->rvx) < 0.3f ? p->stall + 1 : 0;
                if (p->stall > 25 || p->age > 600) {
                    create_explosion(p->x, p->y, p->radius, weapon->damage, p->weapon);
                    p->active = false;
                    continue;
                }
            }
        }

        /* MIRV split at apex */
        if (p->weapon == W_MIRV && !p->mirvSplit && p->vy > 0) {
            p->mirvSplit = true;
            sound_play(SFX_MIRV, 0.7f, 1);
            for (int j = -1; j <= 1; j++) {
                Projectile *np = push_projectile();
                if (!np) break;
                np->x = p->x; np->y = p->y;
                np->vx = p->vx + j * 2; np->vy = p->vy * 0.8f;
                np->weapon = W_NORMAL;
                np->radius = WEAPONS[W_NORMAL].blastRadius;
                np->age = p->age;
            }
            p->active = false;
            continue;
        }

        /* drill: tunnels */
        if (p->weapon == W_DRILL && terrain_is_ground(p->x, p->y)) {
            terrain_remove_radius(p->x, p->y, 8);
            p->drillDepth++;
            if (p->drillDepth == 1) sound_play(SFX_DRILL, 0.7f, 1);
            if (p->drillDepth > weapon->drillDepth) {
                create_explosion(p->x, p->y, p->radius, weapon->damage, p->weapon);
                p->active = false;
            }
            continue;
        }
        /* digger: wider, shorter */
        if (p->weapon == W_DIGGER && terrain_is_ground(p->x, p->y)) {
            terrain_remove_radius(p->x, p->y, 14);
            p->digDepth++;
            if (p->digDepth == 1) sound_play(SFX_DRILL, 0.7f, 0.72f);
            if (p->digDepth > 15) {
                create_explosion(p->x, p->y, p->radius, weapon->damage, p->weapon);
                p->active = false;
            }
            continue;
        }

        /* side-wall bounce */
        if (!p->rolling && G.wallBounce && (p->x < 0 || p->x >= G.W)) {
            p->vx = -p->vx * 0.8f;
            p->x = p->x < 0 ? 0 : G.W - 1;
            sound_play(SFX_BOUNCE, 0.45f, 0.8f);
            continue;
        }

        /* out of bounds */
        int px = (int)p->x, py = (int)p->y;
        if (px < 0 || px >= G.W || py > G.H || py < -100) {
            float ey = py < G.H - 5 ? p->y : G.H - 5;
            if (py > G.H) ey = G.H - 5;
            create_explosion(p->x, ey, p->radius, weapon->damage, p->weapon);
            p->active = false;
            continue;
        }
        /* terrain hit */
        if (py >= 0 && py < G.H && p->weapon != W_ROLLER) {
            if (terrain_is_ground(p->x, p->y)) {
                create_explosion(p->x, p->y, p->radius, weapon->damage, p->weapon);
                p->active = false;
                continue;
            }
        }
        /* tank hit */
        for (int ti = 0; ti < G.numPlayers; ti++) {
            Tank *tank = &G.tanks[ti];
            if (tank->hp <= 0) continue;
            if (tank->id == G.currentPlayer && p->age < 20) continue;
            float dx = p->x - tank->x;
            float dy = p->y - (tank->y - TANK_HEIGHT / 2.0f);
            if (sqrtf(dx * dx + dy * dy) < TANK_WIDTH / 2.0f) {
                create_explosion(p->x, p->y, p->radius, weapon->damage, p->weapon);
                p->active = false;
                break;
            }
        }
    }

    if (live_projectiles() == 0 && live_flames() == 0 && G.gameState == GS_ANIMATING) {
        G.gameState = GS_TURN_ENDING;
        G.pendingNextTurn = 1000;
    }
}

/* ---------- flames ---------- */
static void update_flames(void)
{
    for (int i = 0; i < MAX_FLAMES; i++) {
        Flame *f = &G.flames[i];
        if (!f->active) continue;
        f->life -= 0.016f;
        if (f->life <= 0) { f->active = false; continue; }
        if (!f->settled) {
            f->vy += GRAVITY * 0.6f;
            f->x += f->vx;
            f->y += f->vy;
            if (f->y > G.H || f->x < 0 || f->x >= G.W) { f->active = false; continue; }
            float gh = terrain_get_height(f->x);
            if (f->y >= gh - 2) { f->y = gh - 2; f->settled = true; }
        } else {
            float slope = terrain_get_slope(f->x);
            f->x += sinf(slope) * 1.1f;
            if (f->x < 0 || f->x >= G.W) { f->active = false; continue; }
            f->y = terrain_get_height(f->x) - 2;
        }
        if (terrain_is_liquid(f->x, f->y + 1)) {
            push_particle(f->x, f->y, (frandf() - 0.5f) * 0.4f, -1.5f,
                          0.8f, 0.02f, 0xdfeef8, 4, P_SMOKE);
            f->active = false;
            continue;
        }
        if (frandf() < 0.3f)
            push_particle(f->x + (frandf() - 0.5f) * 6, f->y,
                          (frandf() - 0.5f) * 0.6f + G.wind * 0.02f,
                          -0.5f - frandf() * 1.2f, 0.6f, 0.04f,
                          FIRE_COLORS[rand() % 3], 2 + frandf() * 3, P_FIRE);
    }

    if (live_flames() > 0 && (G.frameCount % 20) == 0) {
        for (int ti = 0; ti < G.numPlayers; ti++) {
            Tank *tank = &G.tanks[ti];
            if (tank->hp <= 0 || tank->shield > 0) continue;
            int near = 0;
            for (int i = 0; i < MAX_FLAMES; i++) {
                Flame *f = &G.flames[i];
                if (f->active && fabsf(f->x - tank->x) < 20 &&
                    fabsf(f->y - tank->y) < 24)
                    near++;
            }
            if (!near) continue;
            int burn = near < 4 ? near : 4;
            tank->hp -= burn;
            if (tank->hp < 0) tank->hp = 0;
            push_damage_num(tank->x + (frandf() - 0.5f) * 10, tank->y - 30,
                            burn, 0xffa502);
            if (tank->hp == 0 && !tank->dead) {
                tank->dead = true;
                if (tank->isAI)
                    add_damage_text(tank->x, tank->y - 50,
                                    AI_DEATH_MESSAGES[rand() % 10], 0xffff00);
                sound_play(SFX_DEATH, 1, 1);
                create_explosion(tank->x, tank->y, 45, 0, W_NORMAL);
            }
        }
    }
}

/* ---------- lightweight effect updates ---------- */
static void update_effects(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &G.particles[i];
        if (!p->active) continue;
        p->x += p->vx;
        p->y += p->vy;
        if (p->type == P_FIRE) { p->vy -= 0.05f; p->vx *= 0.98f; }
        else if (p->type == P_SMOKE) { p->vy -= 0.03f; p->vx += G.wind * 0.005f; p->size *= 1.01f; }
        else if (p->type == P_SPARK) p->vy += GRAVITY;
        p->life -= p->decay;
        if (p->life <= 0) p->active = false;
    }
    for (int i = 0; i < MAX_DEBRIS; i++) {
        Debris *d = &G.debris[i];
        if (!d->active) continue;
        d->vy += GRAVITY;
        d->vx *= 0.99f;
        d->x += d->vx;
        d->y += d->vy;
        d->rot += d->rotSpeed;
        d->life -= 0.01f;
        if (d->life <= 0 || d->y > G.H) d->active = false;
    }
    for (int i = 0; i < MAX_SHOCKWAVES; i++) {
        Shockwave *s = &G.shockwaves[i];
        if (!s->active) continue;
        s->radius += 5;
        s->alpha -= 0.05f;
        if (s->alpha <= 0) s->active = false;
    }
    for (int i = 0; i < MAX_MARKERS; i++) {
        ImpactMarker *m = &G.markers[i];
        if (!m->active) continue;
        m->life -= 0.016f;
        if (m->life <= 0) m->active = false;
    }
    for (int i = 0; i < MAX_DAMAGE_NUMS; i++) {
        DamageNumber *d = &G.damageNums[i];
        if (!d->active) continue;
        d->y += d->vy;
        d->life -= 0.02f;
        if (d->life <= 0) d->active = false;
    }
}

/* ---------- AI ---------- */
void ai_buy_weapons(int player)
{
    const AIStrategy *strategy = &AI_STRATEGIES[G.tanks[player].strategy >= 0
        ? G.tanks[player].strategy : STRAT_BALANCED];
    /* purchases already primed with carry-over by the store flow */
    int *purchases = G.purchases[player];
    int moneyLeft = G.wallets[player];

    /* recompute money after carry (carry costs nothing) */
    if (G.terrainType == TERRAIN_GRASS && frandf() < 0.6f &&
        moneyLeft >= WEAPONS[W_RAFT].cost && purchases[W_RAFT] < 1) {
        purchases[W_RAFT]++;
        moneyLeft -= WEAPONS[W_RAFT].cost;
    }
    if (frandf() < 0.4f && moneyLeft >= WEAPONS[W_PARACHUTE].cost &&
        purchases[W_PARACHUTE] < 1) {
        purchases[W_PARACHUTE]++;
        moneyLeft -= WEAPONS[W_PARACHUTE].cost;
    }
    if (moneyLeft >= 4000 && frandf() < 0.25f && purchases[W_SHIELD] < 1) {
        purchases[W_SHIELD]++;
        moneyLeft -= WEAPONS[W_SHIELD].cost;
    }

    for (int w = 0; w < WEAPON_COUNT; w++) {
        if (w == W_NORMAL || strategy->buy[w] == 0) continue;
        if (WEAPONS[w].isRaft || WEAPONS[w].isParachute || WEAPONS[w].isShield) continue;
        int maxAffordable = WEAPONS[w].cost > 0 ? moneyLeft / WEAPONS[w].cost : 0;
        int toBuy = strategy->buy[w] < maxAffordable ? strategy->buy[w] : maxAffordable;
        if (toBuy > 0) {
            purchases[w] += toBuy;
            moneyLeft -= toBuy * WEAPONS[w].cost;
        }
    }
    int extraMissiles = moneyLeft / WEAPONS[W_MISSILE].cost;
    if (extraMissiles > 0) purchases[W_MISSILE] += extraMissiles;
}

static bool predict_landing(float lx, float ly, float angleDeg, float power,
                            float *outX, float *outY)
{
    float aRad = angleDeg * (float)M_PI / 180;
    float vx = cosf(aRad) * power * 0.15f;
    float vy = -sinf(aRad) * power * 0.15f;
    float sx = lx, sy = ly;
    for (int step = 0; step < 200; step++) {
        vy += GRAVITY;
        vx += G.wind * 0.01f;
        sx += vx;
        sy += vy;
        if (sx < 0 || sx > G.W || sy > G.H) return false;
        if (terrain_is_ground(sx, sy)) { *outX = sx; *outY = sy; return true; }
    }
    return false;
}

void ai_do_turn(void)
{
    if (G.gameState != GS_PLAYING) return;
    Tank *tank = &G.tanks[G.currentPlayer];

    /* pick target: lowest hp among living others */
    Tank *target = NULL;
    for (int i = 0; i < G.numPlayers; i++) {
        Tank *t = &G.tanks[i];
        if (t->hp <= 0 || t->id == tank->id) continue;
        if (!target || t->hp < target->hp) target = t;
    }
    if (!target) return;

    if (frandf() < 0.3f)
        add_damage_text(tank->x, tank->y - 60, AI_TAUNTS[rand() % 10], 0xff88ff);

    float dist = fabsf(target->x - tank->x);
    const int *ammo = G.ammo[G.currentPlayer];
    int strat = tank->strategy >= 0 && tank->strategy < STRAT_COUNT
              ? tank->strategy : STRAT_BALANCED;
    const AIStrategy *strategy = &AI_STRATEGIES[strat];

    int selected = W_NORMAL;
    for (int i = 0; strategy->priority[i] != -1; i++) {
        int w = strategy->priority[i];
        if (w != W_NORMAL && ammo[w] <= 0) continue;
        if (w == W_NUKE && dist < 200) continue;
        if (w == W_MIRV && dist < 150) continue;
        if (w == W_DIRT && tank->y > target->y - 50) continue;
        if (w == W_BOUNCY && dist > 400) continue;
        if (w == W_DRILL && dist > 300) continue;
        if (w == W_ROLLER && target->y < tank->y - 40) continue; /* can't roll uphill */
        selected = w;
        break;
    }
    if (target->hp < 30 &&
        (selected == W_NUKE || selected == W_MIRV || selected == W_BIG)) {
        const int cheap[3] = { W_MISSILE, W_TRIPLE, W_NORMAL };
        for (int i = 0; i < 3; i++)
            if (cheap[i] == W_NORMAL || ammo[cheap[i]] > 0) { selected = cheap[i]; break; }
    }
    game_select_weapon(selected);

    float accuracy = strategy->accuracy + frandf() * 0.15f;
    float errorFactor = (1 - accuracy) * 0.5f;

    float barrelX = target->x;
    float barrelY = target->y - TANK_HEIGHT / 2.0f;
    float angleRad = tank->angle * (float)M_PI / 180;
    float launchX = tank->x + cosf(angleRad) * BARREL_LENGTH;
    float launchY = tank->y - TANK_HEIGHT / 2.0f - sinf(angleRad) * BARREL_LENGTH;
    float dx = barrelX - launchX, dy = barrelY - launchY;
    float rawDist = sqrtf(dx * dx + dy * dy);

    float aimPower = clampf(rawDist / 6 + 20, 30, tank->maxPower);
    int direction = dx > 0 ? 1 : -1;
    float lowBound = direction > 0 ? 5 : 95;
    float highBound = direction > 0 ? 85 : 175;

    float bestAngle = direction > 0 ? 45 : 135;
    float bestPower = aimPower;

    for (int pw = 0; pw < 3; pw++) {
        float pwr = aimPower + (pw - 1) * 15;
        float clampedPwr = clampf(pwr, 25, 100);
        float lo = lowBound, hi = highBound;
        bool hit = false;
        for (int iter = 0; iter < 12; iter++) {
            float mid = (lo + hi) / 2;
            float lx1, ly1;
            if (!predict_landing(launchX, launchY, mid, clampedPwr, &lx1, &ly1)) break;
            if (fabsf(lx1 - barrelX) < 20 && fabsf(ly1 - barrelY) < 80) {
                bestAngle = mid;
                bestPower = clampedPwr;
                hit = true;
                break;
            }
            float lx2, ly2;
            if (!predict_landing(launchX, launchY, mid + 1, clampedPwr, &lx2, &ly2)) break;
            bool goingRight = lx2 > lx1;
            if (goingRight == (direction > 0)) {
                if (lx1 < barrelX) lo = mid; else hi = mid;
            } else {
                if (lx1 > barrelX) hi = mid; else lo = mid;
            }
        }
        if (hit) break;
    }

    tank->angle = clampf(bestAngle + (frandf() - 0.5f) * 20 * errorFactor, 5, 175);
    tank->power = clampf(bestPower, 10, tank->maxPower);
    G.pendingAIFire = 500;
}

/* ---------- store / match flow ---------- */
int game_money_left(int player)
{
    int spent = 0;
    for (int w = 0; w < WEAPON_COUNT; w++) {
        if (WEAPONS[w].cost <= 0) continue;
        int added = G.purchases[player][w] - G.carry[player][w];
        if (added > 0) spent += added * WEAPONS[w].cost;
    }
    int left = G.wallets[player] - spent;
    return left > 0 ? left : 0;
}

/* which weapons appear in the store, in display order */
const int STORE_ORDER[] = { W_MISSILE, W_BIG, W_NUKE, W_TRIPLE, W_BOUNCY,
    W_ROLLER, W_DRILL, W_DIGGER, W_NAPALM, W_DIRT, W_MIRV,
    W_RAFT, W_PARACHUTE, W_SHIELD };
const int STORE_ITEMS = (int)(sizeof STORE_ORDER / sizeof STORE_ORDER[0]);

static void terrain_roll_and_generate(void)
{
    if (G.terrainSetting == 0)
        G.terrainType = rand() % 3;
    else
        G.terrainType = G.terrainSetting - 1;

    float *surface = malloc(sizeof(float) * G.W);
    float baseHeight = G.H * 0.65f;
    float phase = frandf() * 1000.0f;
    for (int x = 0; x < G.W; x++) {
        float y = baseHeight;
        y += sinf((x + phase) * 0.003f) * 80;
        y += sinf((x + phase) * 0.01f) * 30;
        y += sinf((x + phase) * 0.02f) * 15;
        y += sinf((x + phase) * 0.05f) * 5;
        y += (frandf() - 0.5f) * 10;
        surface[x] = clampf(y, G.H * 0.3f, (float)(G.H - 20));
    }
    terrain_generate(G.W, G.H, surface);
    free(surface);
}

void game_launch(void)
{
    /* lock wallets at post-shopping value; leftover carries forward */
    for (int i = 0; i < G.numPlayers; i++)
        G.wallets[i] = game_money_left(i);

    terrain_roll_and_generate();
    spawn_tanks();

    for (int i = 0; i < G.numPlayers; i++) {
        memcpy(G.ammo[i], G.purchases[i], sizeof G.ammo[i]);
        G.tanks[i].shield = G.ammo[i][W_SHIELD];
    }

    change_wind();
    change_precip();
    G.currentPlayer = 0;
    G.roundCount = 0;
    G.currentWeapon = W_NORMAL;
    G.staleTurns = 0;
    G.lastTotalHp = G.numPlayers * MAX_HP;
    clear_effects();
    G.gameState = GS_PLAYING;
    G.pendingNextTurn = G.pendingAIFire = 0;
    G.pendingAIStart = G.tanks[0].isAI ? 1000 : 0;
}

void game_store_confirm(void)
{
    /* advance past current (human) shopper; batch AI buys; launch at end */
    G.storePlayer++;
    while (G.storePlayer < G.numPlayers) {
        if (G.tanks[G.storePlayer].isAI) {
            ai_buy_weapons(G.storePlayer);
            G.storePlayer++;
        } else {
            G.storeCursor = 0;
            return;   /* next human shops */
        }
    }
    game_launch();
}

void game_store_reset(void)
{
    memcpy(G.purchases[G.storePlayer], G.carry[G.storePlayer],
           sizeof G.purchases[G.storePlayer]);
}

static void enter_store(void)
{
    G.gameState = GS_STORE;
    G.storePlayer = 0;
    G.storeCursor = 0;
    /* prime purchases with carry-over */
    for (int i = 0; i < G.numPlayers; i++)
        memcpy(G.purchases[i], G.carry[i], sizeof G.purchases[i]);
    /* if player 0 is AI (selftest), batch straight through */
    if (G.tanks[0].isAI) {
        for (G.storePlayer = 0; G.storePlayer < G.numPlayers; G.storePlayer++)
            ai_buy_weapons(G.storePlayer);
        game_launch();
    }
}

void game_start_from_menu(void)
{
    static bool usedName[STRAT_COUNT][7];
    memset(usedName, 0, sizeof usedName);

    G.numPlayers = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i > 0 && !G.pEnabled[i]) continue;
        Tank *t = &G.tanks[G.numPlayers];
        memset(t, 0, sizeof *t);
        t->id = G.numPlayers;
        t->isAI = (i > 0) || G.headless;
        if (t->isAI) {
            int strat = G.pStrategy[i] >= 0 ? G.pStrategy[i] : rand() % STRAT_COUNT;
            t->strategy = strat;
            int pick = rand() % 7, tries = 0;
            while (usedName[strat][pick] && tries++ < 7) pick = (pick + 1) % 7;
            usedName[strat][pick] = true;
            snprintf(t->name, sizeof t->name, "%s", AI_NAMES[strat][pick]);
        } else {
            t->strategy = -1;
            snprintf(t->name, sizeof t->name, "Player %d", G.numPlayers + 1);
        }
        G.numPlayers++;
    }
    if (G.numPlayers < 2) return;   /* need at least one opponent */

    for (int i = 0; i < G.numPlayers; i++) {
        G.wallets[i] = STARTING_MONEY;
        memset(G.carry[i], 0, sizeof G.carry[i]);
        G.carry[i][W_NORMAL] = 999;
        G.matchWins[i] = 0;
    }
    G.matchNumber = 1;
    G.lastWinnerId = -1;

    /* terrain type must be rolled before the store so AI raft-buying can
     * see whether there's water; generation itself waits until launch */
    if (G.terrainSetting == 0) G.terrainType = rand() % 3;
    else G.terrainType = G.terrainSetting - 1;

    options_save();
    enter_store();
}

void game_next_round(void)
{
    G.pendingNextTurn = G.pendingAIStart = G.pendingAIFire = 0;
    clear_effects();
    for (int i = 0; i < G.numPlayers; i++) {
        G.wallets[i] += STARTING_MONEY;
        if (i == G.lastWinnerId) G.wallets[i] += 2000;
        /* carry surviving ammo into the next match */
        memcpy(G.carry[i], G.ammo[i], sizeof G.carry[i]);
        G.carry[i][W_NORMAL] = 999;
    }
    G.matchNumber++;
    G.currentPlayer = 0;
    G.roundCount = 0;
    if (G.terrainSetting == 0) G.terrainType = rand() % 3;
    else G.terrainType = G.terrainSetting - 1;
    enter_store();
}

void game_reset_to_start(void)
{
    memset(&G.tanks, 0, sizeof G.tanks);
    clear_effects();
    G.gameState = GS_START;
    G.startCursor = 0;
    G.pEnabled[0] = true;
    if (!G.pEnabled[1] && !G.pEnabled[2] && !G.pEnabled[3])
        G.pEnabled[1] = true;
}

/* ---------- input ---------- */
static const int HOTKEYS[10] = { W_NORMAL, W_MISSILE, W_BIG, W_TRIPLE, W_BOUNCY,
                                 W_DIGGER, W_NAPALM, W_DIRT, W_MIRV, W_NUKE };

/* fireable weapons in Tab-cycle order */
static const int CYCLE[12] = { W_NORMAL, W_MISSILE, W_BIG, W_TRIPLE, W_BOUNCY,
    W_ROLLER, W_DIGGER, W_DRILL, W_NAPALM, W_DIRT, W_MIRV, W_NUKE };

static void handle_key_playing(int key)
{
    Tank *tank = &G.tanks[G.currentPlayer];
    if (tank->isAI) return;
    int weaponBefore = G.currentWeapon;

    switch (key) {
    case KEY_LEFT:
        tank->angle = fminf(180, tank->angle + 2);
        break;
    case KEY_RIGHT:
        tank->angle = fmaxf(0, tank->angle - 2);
        break;
    case KEY_UP:
        tank->power = fminf(tank->maxPower, tank->power + 2);
        break;
    case KEY_DOWN:
        tank->power = fmaxf(10, tank->power - 2);
        break;
    case ' ':
    case KEY_ENTER:
        game_fire();
        break;
    case KEY_TAB: {
        int cur = 0;
        for (int i = 0; i < 12; i++) if (CYCLE[i] == G.currentWeapon) cur = i;
        for (int i = 1; i <= 12; i++) {
            int w = CYCLE[(cur + i) % 12];
            if (w == W_NORMAL || game_player_ammo(G.currentPlayer, w) > 0) {
                game_select_weapon(w);
                break;
            }
        }
        break;
    }
    case 'd': case 'D': game_select_weapon(W_DRILL); break;
    case 'r': case 'R': game_select_weapon(W_ROLLER); break;
    default:
        if (key >= '0' && key <= '9') {
            int slot = key == '0' ? 9 : key - '1';
            game_select_weapon(HOTKEYS[slot]);
        }
        break;
    }
    if (G.currentWeapon != weaponBefore)
        sound_play(SFX_MENU_MOVE, 0.4f, 1.4f);
}

/* start menu rows: 0..2 opponents, 3 terrain, 4 wind, 5 precip, 6 damage,
 * 7 wall bounce, 8 sound, 9 START */
static void try_start(void)
{
    game_start_from_menu();
    sound_play(G.gameState != GS_START ? SFX_MENU_SELECT : SFX_DENY, 0.7f, 1);
}

static void handle_key_start(int key)
{
    int dir = key == KEY_LEFT ? -1 : key == KEY_RIGHT ? 1 : 0;
    switch (key) {
    case KEY_UP:
        G.startCursor = (G.startCursor + START_ROWS - 1) % START_ROWS;
        sound_play(SFX_MENU_MOVE, 0.5f, 1);
        return;
    case KEY_DOWN:
        G.startCursor = (G.startCursor + 1) % START_ROWS;
        sound_play(SFX_MENU_MOVE, 0.5f, 1);
        return;
    case KEY_ENTER:
    case ' ':
        if (G.startCursor == START_ROWS - 1 || key == KEY_ENTER) { try_start(); return; }
        dir = 1;
        break;
    case 's': case 'S': try_start(); return;
    default: break;
    }
    if (!dir) return;
    int row = G.startCursor;
    if (row >= 0 && row <= 2) {           /* opponent slots P2..P4 */
        int p = row + 1;
        /* cycle: off, random, 5 strategies */
        int v = !G.pEnabled[p] ? 0 : G.pStrategy[p] < 0 ? 1 : G.pStrategy[p] + 2;
        v = (v + dir + 7) % 7;
        G.pEnabled[p] = v != 0;
        G.pStrategy[p] = v <= 1 ? -1 : v - 2;
    } else if (row == 3) {
        G.terrainSetting = (G.terrainSetting + dir + 4) % 4;
    } else if (row == 4) {
        G.windSetting = (G.windSetting + dir + 5) % 5;
    } else if (row == 5) {
        G.precipSetting = (G.precipSetting + dir + 5) % 5;
    } else if (row == 6) {
        const float steps[4] = { 0.5f, 1.0f, 1.5f, 2.0f };
        int cur = 1;
        for (int i = 0; i < 4; i++) if (fabsf(G.damageMultiplier - steps[i]) < 0.01f) cur = i;
        G.damageMultiplier = steps[(cur + dir + 4) % 4];
    } else if (row == 7) {
        G.wallBounce = !G.wallBounce;
    } else if (row == 8) {
        G.soundOn = !G.soundOn;
        sound_set_enabled(G.soundOn);
    } else {
        return;                       /* START row: left/right is a no-op */
    }
    sound_play(SFX_MENU_MOVE, 0.5f, 1.3f);
}

static void handle_key_store(int key)
{
    int w = STORE_ORDER[G.storeCursor];
    switch (key) {
    case KEY_UP:
        G.storeCursor = (G.storeCursor + STORE_ITEMS - 1) % STORE_ITEMS;
        sound_play(SFX_MENU_MOVE, 0.5f, 1);
        break;
    case KEY_DOWN:
        G.storeCursor = (G.storeCursor + 1) % STORE_ITEMS;
        sound_play(SFX_MENU_MOVE, 0.5f, 1);
        break;
    case KEY_RIGHT: case '+': case '=':
        if (game_money_left(G.storePlayer) >= WEAPONS[w].cost) {
            G.purchases[G.storePlayer][w]++;
            sound_play(SFX_BUY, 0.6f, 1);
        } else {
            sound_play(SFX_DENY, 0.6f, 1);
        }
        break;
    case KEY_LEFT: case '-': case KEY_BACKSPACE:
        if (G.purchases[G.storePlayer][w] > G.carry[G.storePlayer][w]) {
            G.purchases[G.storePlayer][w]--;
            sound_play(SFX_MENU_MOVE, 0.5f, 0.8f);
        }
        break;
    case 'z': case 'Z':
        game_store_reset();
        sound_play(SFX_MENU_MOVE, 0.5f, 0.7f);
        break;
    case KEY_ENTER: case 's': case 'S':
        sound_play(SFX_MENU_SELECT, 0.7f, 1);
        game_store_confirm();
        break;
    default: break;
    }
}

static void handle_key_gameover(int key)
{
    if (key == KEY_ENTER || key == ' ' || key == 'n' || key == 'N') {
        sound_play(SFX_MENU_SELECT, 0.7f, 1);
        game_next_round();
    }
}

void game_handle_key(int key)
{
    if (key == 'q' || key == 'Q') { G.quit = true; return; }
    if (key == 'm' || key == 'M') {       /* mute toggle, any screen */
        G.soundOn = !G.soundOn;
        sound_set_enabled(G.soundOn);
        sound_play(SFX_MENU_SELECT, 0.7f, 1);
        options_save();
        return;
    }
    switch (G.gameState) {
    case GS_START:    handle_key_start(key); break;
    case GS_STORE:    handle_key_store(key); break;
    case GS_PLAYING:  handle_key_playing(key); break;
    case GS_GAMEOVER: handle_key_gameover(key); break;
    default: break;   /* animating / turn_ending: ignore */
    }
}

/* ---------- master tick (one 60 Hz logic frame) ---------- */
void game_tick(void)
{
    if (G.gameState == GS_START || G.gameState == GS_STORE) return;

    G.frameCount++;

    if (G.pendingNextTurn > 0) {
        G.pendingNextTurn -= TICK_MS;
        if (G.pendingNextTurn <= 0) {
            G.pendingNextTurn = 0;
            if (G.gameState == GS_TURN_ENDING) next_turn();
        }
    }
    if (G.pendingAIStart > 0) {
        G.pendingAIStart -= TICK_MS;
        if (G.pendingAIStart <= 0) {
            G.pendingAIStart = 0;
            Tank *t = &G.tanks[G.currentPlayer];
            if (G.gameState == GS_PLAYING && t->isAI && t->hp > 0) ai_do_turn();
        }
    }
    if (G.pendingAIFire > 0) {
        G.pendingAIFire -= TICK_MS;
        if (G.pendingAIFire <= 0) {
            G.pendingAIFire = 0;
            Tank *t = &G.tanks[G.currentPlayer];
            if (G.gameState == GS_PLAYING && t->isAI && t->hp > 0) game_fire();
        }
    }

    if (G.gameState != GS_GAMEOVER) {
        g_phase = "precip";  terrain_tick_precipitation();
        g_phase = "terrain"; terrain_update();
        g_phase = "proj";    update_projectiles();
        g_phase = "flames";  update_flames();
        g_phase = "tanks";   update_tanks();
        g_phase = "buried";
        if ((G.frameCount % 10) == 0) check_buried_tanks();
    }
    g_phase = "effects";
    update_effects();
    g_phase = "idle";

    G.cameraShake = G.cameraShake > 0.5f ? G.cameraShake * 0.9f : 0;
    G.screenFlash = G.screenFlash > 0.01f ? G.screenFlash * 0.9f : 0;
}

/* ---------- options persistence ---------- */
/* $XDG_CONFIG_HOME/bashed-earth.conf, defaulting to ~/.config */
#include <sys/stat.h>

static void conf_dir(char *buf, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, n, "%s", xdg);
        return;
    }
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, n, "%s/.config", home);
}

static void conf_path(char *buf, size_t n)
{
    char dir[448];
    conf_dir(dir, sizeof dir);
    snprintf(buf, n, "%s/bashed-earth.conf", dir);
}

void options_load(void)
{
    char path[512];
    conf_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char key[64];
    float val;
    while (fscanf(f, "%63[^=]=%f\n", key, &val) == 2) {
        if (!strcmp(key, "terrain")) G.terrainSetting = (int)val % 4;
        else if (!strcmp(key, "wind")) G.windSetting = (int)val % 5;
        else if (!strcmp(key, "precip")) G.precipSetting = (int)val % 5;
        else if (!strcmp(key, "damage")) G.damageMultiplier = clampf(val, 0.5f, 2.0f);
        else if (!strcmp(key, "wallBounce")) G.wallBounce = val != 0;
        else if (!strcmp(key, "sound")) G.soundOn = val != 0;
        else if (!strcmp(key, "p2")) { G.pEnabled[1] = val >= 0; G.pStrategy[1] = val >= 1 ? ((int)val - 1) % STRAT_COUNT : -1; }
        else if (!strcmp(key, "p3")) { G.pEnabled[2] = val >= 0; G.pStrategy[2] = val >= 1 ? ((int)val - 1) % STRAT_COUNT : -1; }
        else if (!strcmp(key, "p4")) { G.pEnabled[3] = val >= 0; G.pStrategy[3] = val >= 1 ? ((int)val - 1) % STRAT_COUNT : -1; }
    }
    fclose(f);
}

void options_save(void)
{
    char dir[448], path[512];
    conf_dir(dir, sizeof dir);
    mkdir(dir, 0755);            /* ensure the config dir exists */
    conf_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "terrain=%d\n", G.terrainSetting);
    fprintf(f, "wind=%d\n", G.windSetting);
    fprintf(f, "precip=%d\n", G.precipSetting);
    fprintf(f, "damage=%g\n", G.damageMultiplier);
    fprintf(f, "wallBounce=%d\n", G.wallBounce ? 1 : 0);
    fprintf(f, "sound=%d\n", G.soundOn ? 1 : 0);
    for (int p = 1; p < MAX_PLAYERS; p++)
        fprintf(f, "p%d=%d\n", p + 1,
                !G.pEnabled[p] ? -1 : G.pStrategy[p] < 0 ? 0 : G.pStrategy[p] + 1);
    fclose(f);
}
