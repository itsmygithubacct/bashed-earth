/*
 * Bashed Earth — a terminal artillery game for kitty-protocol terminals.
 * C port of the Tank Wars web game (tankwars.domainreviews.org).
 *
 * Rendering: RGBA framebuffer pushed to the terminal via the kitty
 * graphics protocol (zlib-compressed, base64-chunked APC sequences).
 * Terrain: falling-sand cellular automaton, one byte per pixel cell.
 */
#ifndef BASHED_EARTH_H
#define BASHED_EARTH_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- Core constants (identical to the JS Config module) ---------- */
#define GRAVITY        0.25f
#define TANK_WIDTH     24
#define TANK_HEIGHT    12
#define BARREL_LENGTH  18
#define MAX_HP         100
#define WIND_MAX       10
#define STARTING_MONEY 10000
#define TICK_MS        16.666f   /* logic tick = one 60 Hz frame of the original */

#define MAX_PLAYERS       4
#define MAX_PROJECTILES   64
#define MAX_PARTICLES     320
#define MAX_DEBRIS        160
#define MAX_SHOCKWAVES    16
#define MAX_MARKERS       32
#define MAX_DAMAGE_NUMS   100
#define MAX_FLAMES        128

/* ---------- Weapons ---------- */
enum {
    W_NORMAL, W_MISSILE, W_BIG, W_NUKE, W_TRIPLE, W_BOUNCY, W_ROLLER,
    W_DRILL, W_DIGGER, W_NAPALM, W_DIRT, W_MIRV,
    W_RAFT, W_PARACHUTE, W_SHIELD,
    WEAPON_COUNT
};

typedef struct {
    const char *name;
    int damage;
    int blastRadius;
    int cost;
    uint32_t color;      /* 0xRRGGBB */
    /* behaviour flags / params */
    int spread;          /* triple */
    int bounces;         /* bouncy */
    int drillDepth;      /* drill */
    bool digger, fire, addTerrain, mirv, roller;
    bool isRaft, isParachute, isShield;
    const char *blurb;   /* store description */
} Weapon;

extern const Weapon WEAPONS[WEAPON_COUNT];

/* ---------- AI ---------- */
enum { STRAT_AGGRESSIVE, STRAT_DEFENSIVE, STRAT_TACTICAL, STRAT_BALANCED,
       STRAT_TRICKSTER, STRAT_COUNT };

typedef struct {
    const char *name;
    float accuracy;
    int buy[WEAPON_COUNT];        /* desired counts per weapon */
    int priority[WEAPON_COUNT];   /* firing priority, terminated by -1 */
} AIStrategy;

extern const AIStrategy AI_STRATEGIES[STRAT_COUNT];
extern const char *AI_TAUNTS[10];
extern const char *AI_DEATH_MESSAGES[10];
extern const char *AI_NAMES[STRAT_COUNT][7];
extern const uint32_t TANK_COLORS[MAX_PLAYERS];

/* ---------- Terrain ---------- */
enum {
    M_EMPTY = 0, M_SAND_LIGHT, M_SAND_MID, M_SAND_DEEP, M_GRASS,
    M_DIRT, M_DIRT_DEEP, M_SNOW, M_ICE, M_WATER, M_RAFT_MAT, M_COUNT
};
enum { TERRAIN_GRASS, TERRAIN_SAND, TERRAIN_ICE };

void terrain_generate(int width, int height, const float *surfaceYs);
void terrain_update(void);               /* STEPS_PER_FRAME automaton steps */
void terrain_tick_precipitation(void);
int  terrain_check_ice_stability(void);
int  terrain_vaporize_liquid(float cx, float cy, float radius);
int  terrain_remove_radius(float cx, float cy, float radius);
int  terrain_add_radius(float cx, float cy, float radius);
bool terrain_is_ground(float x, float y);
bool terrain_is_liquid(float x, float y);
float terrain_get_height(float x);
float terrain_get_slope(float x);
float terrain_get_water_top(float x);    /* -1 if none */
bool terrain_place_raft(float cx, float width);
const uint8_t *terrain_grid(void);       /* for the renderer */
extern const uint32_t MATERIAL_COLORS[M_COUNT];  /* 0xRRGGBB, [0] unused */

/* ---------- Entities ---------- */
typedef struct {
    int id;
    float x, y, vx, vy;
    int hp, maxHp, shield;
    float angle, power, maxPower;
    float groundAngle;
    uint32_t color;
    char name[24];
    bool isAI, falling, dead, onGround;
    bool parachuteActive, fallShielded;
    int strategy;
    int buriedTimer;
    int selectedWeapon;
} Tank;

typedef struct {
    bool active;
    float x, y, vx, vy, rvx;
    int weapon, bounces, age;
    float radius;
    bool rolling, mirvSplit, inLiquid;
    int drillDepth, digDepth, stall;
} Projectile;

enum { P_TRAIL, P_FIRE, P_SMOKE, P_SPARK };
typedef struct { bool active; float x,y,vx,vy,life,decay,size; uint32_t color; int type; } Particle;
typedef struct { bool active; float x,y,vx,vy,life,size,rot,rotSpeed; uint8_t r,g,b; } Debris;
typedef struct { bool active; float x,y,radius,maxRadius,alpha; } Shockwave;
typedef struct { bool active; float x,y,radius,life; } ImpactMarker;
typedef struct { bool active; float x,y,vy,life; char text[64]; uint32_t color; bool isText; } DamageNumber;
typedef struct { bool active; float x,y,vx,vy,life; bool settled; } Flame;

/* ---------- Game state ---------- */
enum { GS_START, GS_STORE, GS_PLAYING, GS_ANIMATING, GS_TURN_ENDING, GS_GAMEOVER };
enum { SET_RANDOM, SET_NONE, SET_LIGHT, SET_MEDIUM, SET_STRONG };  /* wind/precip */

typedef struct {
    int gameState;
    int W, H;                 /* playfield pixel size */

    Tank tanks[MAX_PLAYERS];
    int numPlayers;
    Projectile projectiles[MAX_PROJECTILES];
    Particle particles[MAX_PARTICLES];
    Debris debris[MAX_DEBRIS];
    Shockwave shockwaves[MAX_SHOCKWAVES];
    ImpactMarker markers[MAX_MARKERS];
    DamageNumber damageNums[MAX_DAMAGE_NUMS];
    Flame flames[MAX_FLAMES];

    int currentPlayer, roundCount, frameCount;
    float wind, cameraShake, screenFlash;
    int currentWeapon;
    int ammo[MAX_PLAYERS][WEAPON_COUNT];
    bool wallBounce;
    float damageMultiplier;
    /* last-shot ghost trajectory */
    bool hasLastShot;
    float lastShotX, lastShotY, lastShotAngle, lastShotPower;
    uint32_t lastShotColor;

    /* turn-flow countdowns, ms */
    float pendingNextTurn, pendingAIStart, pendingAIFire;

    /* terrain / weather */
    int terrainSetting, terrainType;   /* setting may be SET-style random */
    int windSetting, precipSetting;
    int precipMaterial;
    float precipRate;
    int precipBudget;

    /* stalemate breaker: turns since any tank lost hp */
    int staleTurns, lastTotalHp;

    /* match / economy */
    int wallets[MAX_PLAYERS];
    int carry[MAX_PLAYERS][WEAPON_COUNT];
    int purchases[MAX_PLAYERS][WEAPON_COUNT];
    int matchWins[MAX_PLAYERS];
    int matchNumber, lastWinnerId;

    /* menus */
    int startCursor;                  /* start-menu row */
    bool pEnabled[MAX_PLAYERS];       /* [0] always true (P1) */
    int pStrategy[MAX_PLAYERS];       /* STRAT_* or -1 = random */
    int storePlayer, storeCursor;
    int gameoverCursor;
    bool optionsOpen;
    int optionsCursor;

    bool quit;
    bool headless;                    /* selftest: no rendering */
} GameState;

extern GameState G;

/* store display order (game.c) */
extern const int STORE_ORDER[];
extern const int STORE_ITEMS;
#define START_ROWS 9   /* start-menu rows incl. START button */

/* ---------- game.c ---------- */
void game_reset_to_start(void);
void game_start_from_menu(void);      /* start menu -> store */
void game_store_confirm(void);        /* next store player / launch */
void game_store_reset(void);
void game_launch(void);
void game_next_round(void);           /* gameover -> store */
void game_tick(void);                 /* one 60 Hz logic tick */
void game_fire(void);
void game_select_weapon(int w);
void game_handle_key(int key);        /* routed by current state */
int  game_player_ammo(int player, int w);
int  game_money_left(int player);
void ai_buy_weapons(int player);
void ai_do_turn(void);
void create_explosion(float x, float y, float radius, float damage, int weaponType);
void add_damage_text(float x, float y, const char *text, uint32_t color);

/* ---------- render.c ---------- */
void render_init(int w, int h);
void render_frame(void);              /* draws scene + UI into framebuffer */
uint8_t *render_fb(void);             /* RGBA buffer, W*H*4 */
void render_shutdown(void);

/* ---------- term.c ---------- */
bool term_init(int *outW, int *outH); /* raw mode + alt screen; pixel size */
void term_shutdown(void);
void term_present(const uint8_t *rgba, int w, int h);
int  term_poll_key(void);             /* -1 if none; else KEY_* or ascii */
enum { KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ESC, KEY_ENTER,
       KEY_BACKSPACE, KEY_TAB };

/* ---------- util ---------- */
float frandf(void);                   /* [0,1), fast xorshift */
void frand_seed(uint32_t s);
float clampf(float v, float lo, float hi);
void options_load(void);
void options_save(void);

#endif
