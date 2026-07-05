/* Weapon, AI, and color data tables. */
#include "bashed_earth.h"

const Weapon WEAPONS[WEAPON_COUNT] = {
    [W_NORMAL]  = { "Baby Missile", 30, 30,    0, 0xffffff, .blurb = "starter pea-shooter" },
    [W_MISSILE] = { "Missile",      42, 40,  250, 0xcccccc, .blurb = "42 dmg" },
    [W_BIG]     = { "Heavy Missile",60, 55,  500, 0xff8800, .blurb = "60 dmg" },
    [W_NUKE]    = { "Nuke",         96, 90, 2000, 0xff0000, .blurb = "96 dmg, huge blast" },
    [W_TRIPLE]  = { "Triple",       24, 28,  600, 0x88ccff, .spread = 12, .blurb = "3x24 dmg spread" },
    [W_BOUNCY]  = { "Bouncy",       36, 32,  400, 0xff44ff, .bounces = 4, .blurb = "36 dmg, bounces 4x" },
    [W_ROLLER]  = { "Roller",       50, 40,  700, 0x4ade80, .roller = true, .blurb = "50 dmg, rolls downhill" },
    [W_DRILL]   = { "Drill",        48, 22,  800, 0x888888, .drillDepth = 70, .blurb = "48 dmg, tunnels deep" },
    [W_DIGGER]  = { "Digger",       12, 15,  350, 0xa0522d, .digger = true, .blurb = "digs a wide tunnel" },
    [W_NAPALM]  = { "Napalm",       18, 35, 1200, 0xff6600, .fire = true, .blurb = "18 dmg + burning fire" },
    [W_DIRT]    = { "Dirt",          0, 45,  300, 0x5a8f3c, .addTerrain = true, .blurb = "adds terrain" },
    [W_MIRV]    = { "MIRV",         30, 30, 1500, 0xff00ff, .mirv = true, .blurb = "splits at apex" },
    [W_RAFT]    = { "Raft",          0,  0,  800, 0x8b5a2b, .isRaft = true, .blurb = "float on water" },
    [W_PARACHUTE]={ "Parachute",     0,  0,  600, 0xe85d3d, .isParachute = true, .blurb = "negates fall damage" },
    [W_SHIELD]  = { "Shield",        0,  0, 2000, 0x4488ff, .isShield = true, .blurb = "blocks one hit" },
};

/* buy[]: desired stock per weapon. priority[]: firing order, -1 terminated. */
const AIStrategy AI_STRATEGIES[STRAT_COUNT] = {
    [STRAT_AGGRESSIVE] = { "Aggressive", 0.75f,
        .buy = { [W_NUKE]=2, [W_MIRV]=3, [W_BIG]=8, [W_MISSILE]=15 },
        .priority = { W_NUKE, W_MIRV, W_BIG, W_MISSILE, W_TRIPLE, W_NORMAL, -1 } },
    [STRAT_DEFENSIVE] = { "Defensive", 0.60f,
        .buy = { [W_DIRT]=10, [W_DIGGER]=8, [W_BIG]=5, [W_MISSILE]=10 },
        .priority = { W_DIRT, W_BIG, W_MISSILE, W_NAPALM, W_DRILL, W_NORMAL, -1 } },
    [STRAT_TACTICAL] = { "Tactical", 0.80f,
        .buy = { [W_DRILL]=5, [W_DIGGER]=8, [W_NAPALM]=5, [W_ROLLER]=4,
                 [W_TRIPLE]=8, [W_BOUNCY]=6, [W_MISSILE]=12 },
        .priority = { W_DRILL, W_ROLLER, W_DIGGER, W_NAPALM, W_TRIPLE,
                      W_BOUNCY, W_MISSILE, W_NORMAL, -1 } },
    [STRAT_BALANCED] = { "Balanced", 0.70f,
        .buy = { [W_NUKE]=1, [W_BIG]=6, [W_TRIPLE]=5, [W_ROLLER]=2,
                 [W_MISSILE]=12, [W_DIRT]=3, [W_DRILL]=3 },
        .priority = { W_NUKE, W_BIG, W_ROLLER, W_TRIPLE, W_MISSILE,
                      W_DRILL, W_NORMAL, -1 } },
    [STRAT_TRICKSTER] = { "Trickster", 0.55f,
        .buy = { [W_BOUNCY]=10, [W_ROLLER]=6, [W_DIRT]=8, [W_DIGGER]=6,
                 [W_NAPALM]=4, [W_MIRV]=2 },
        .priority = { W_ROLLER, W_BOUNCY, W_DIRT, W_DIGGER, W_MIRV,
                      W_NAPALM, W_NORMAL, -1 } },
};

const char *AI_TAUNTS[10] = {
    "I shall smash your ugly tank!",
    "You fight like a dairy farmer!",
    "Say hello to my little friend!",
    "Incoming!",
    "Time to die!",
    "Kiss your tank goodbye!",
    "I've got you in my sights!",
    "This is going to hurt!",
    "Fire in the hole!",
    "Prepare to be scorched!",
};

const char *AI_DEATH_MESSAGES[10] = {
    "Join the army, see the world they said...",
    "I'll be back!",
    "Medic!",
    "So close...",
    "The humanity!",
    "Tell my crew I love them!",
    "Not fair!",
    "Curse you!",
    "I see a bright light...",
    "Avenge me!",
};

const char *AI_NAMES[STRAT_COUNT][7] = {
    [STRAT_AGGRESSIVE] = { "Blaze", "Havoc", "Fury", "Doom", "Rampage", "Scorch", "Inferno" },
    [STRAT_DEFENSIVE]  = { "Bunker", "Bastion", "Moat", "Turtle", "Bulwark", "Fortress", "Rampart" },
    [STRAT_TACTICAL]   = { "Ghost", "Viper", "Sniper", "Fox", "Hawk", "Spectre", "Cipher" },
    [STRAT_BALANCED]   = { "Atlas", "Titan", "Nomad", "Rex", "Storm", "Bolt", "Tank" },
    [STRAT_TRICKSTER]  = { "Jinx", "Chaos", "Prank", "Rascal", "Mayhem", "Joker", "Wildcard" },
};

const uint32_t TANK_COLORS[MAX_PLAYERS] = { 0xef4444, 0x3b82f6, 0x22c55e, 0xf59e0b };

const uint32_t MATERIAL_COLORS[M_COUNT] = {
    [M_EMPTY]      = 0x000000,
    [M_SAND_LIGHT] = 0xe8d28a,
    [M_SAND_MID]   = 0xd4b870,
    [M_SAND_DEEP]  = 0xa08a50,
    [M_GRASS]      = 0x5a8f3c,
    [M_DIRT]       = 0x6b4226,
    [M_DIRT_DEEP]  = 0x3d2817,
    [M_SNOW]       = 0xf0f8ff,
    [M_ICE]        = 0x9fd8e8,
    [M_WATER]      = 0x3a78c8,
    [M_RAFT_MAT]   = 0x8b5a2b,
};
