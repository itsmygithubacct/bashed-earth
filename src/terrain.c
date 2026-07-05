/* Falling-sand cellular automaton terrain. Direct port of the JS
 * MatterPhysics module (1 px cells, 4 steps per 60 Hz tick). */
#include "bashed_earth.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define STEPS_PER_FRAME 4
#define DIRT_SLIDE_PROB 0.35f
#define ICE_OVERHANG_LIMIT 100

enum { FALL_NONE, FALL_SAND, FALL_DIRT, FALL_LIQUID };

static const uint8_t fallKind[M_COUNT] = {
    [M_SAND_LIGHT] = FALL_SAND, [M_SAND_MID] = FALL_SAND, [M_SAND_DEEP] = FALL_SAND,
    [M_GRASS] = FALL_DIRT, [M_DIRT] = FALL_DIRT, [M_DIRT_DEEP] = FALL_DIRT,
    [M_SNOW] = FALL_SAND, [M_ICE] = FALL_NONE, [M_WATER] = FALL_LIQUID,
    [M_RAFT_MAT] = FALL_NONE,
};
static const uint8_t meltsTo[M_COUNT] = { [M_ICE] = M_WATER };

static uint8_t *grid = NULL;
/* Active-region tracking: per row, the column span [minX..maxX] that needs
 * simulation this step. Whole-row scans are what made the naive automaton
 * slow — one falling raindrop must not cost a 1000-cell row sweep. */
static int16_t *rowMinX = NULL, *rowMaxX = NULL;      /* current step */
static int16_t *nextMinX = NULL, *nextMaxX = NULL;    /* being built */
static int cols = 0, rows = 0;
static bool anyActive = false;

static inline int idx(int gx, int gy) { return gy * cols + gx; }

const uint8_t *terrain_grid(void) { return grid; }

static inline void span_add(int16_t *minA, int16_t *maxA, int gy, int x0, int x1)
{
    if (gy < 0 || gy >= rows) return;
    if (x0 < 0) x0 = 0;
    if (x1 > cols - 1) x1 = cols - 1;
    if (x0 > x1) return;
    if (minA[gy] > x0) minA[gy] = (int16_t)x0;
    if (maxA[gy] < x1) maxA[gy] = (int16_t)x1;
    anyActive = true;
}

/* wake a horizontal span on row gy (and the row above, whose cells may now
 * be able to fall/slide into freed space) */
static void wake_span(int gy, int x0, int x1)
{
    span_add(rowMinX, rowMaxX, gy, x0, x1);
    span_add(rowMinX, rowMaxX, gy - 1, x0, x1);
}

static void wake_cell(int gx, int gy) { wake_span(gy, gx - 2, gx + 2); }

void terrain_generate(int width, int height, const float *surfaceYs)
{
    cols = width;
    rows = height;
    free(grid); free(rowMinX); free(rowMaxX); free(nextMinX); free(nextMaxX);
    grid = calloc((size_t)cols * rows, 1);
    rowMinX = malloc(rows * sizeof(int16_t));
    rowMaxX = malloc(rows * sizeof(int16_t));
    nextMinX = malloc(rows * sizeof(int16_t));
    nextMaxX = malloc(rows * sizeof(int16_t));
    for (int i = 0; i < rows; i++) {
        rowMinX[i] = nextMinX[i] = (int16_t)cols;   /* empty span */
        rowMaxX[i] = nextMaxX[i] = -1;
    }
    anyActive = false;

    int *surfaceRows = malloc(sizeof(int) * cols);

    for (int gx = 0; gx < cols; gx++) {
        float surfaceY = surfaceYs[gx];
        int surfaceRow = (int)surfaceY;
        if (surfaceRow < 0) surfaceRow = 0;
        surfaceRows[gx] = surfaceRow;
        int totalDepth = rows - surfaceRow;
        if (totalDepth < 1) totalDepth = 1;

        if (G.terrainType == TERRAIN_ICE) {
            /* smoothly-varying snow blanket depth, 2..~26 cells */
            float d = 14 + sinf(gx * 0.012f + 1.7f) * 8 + sinf(gx * 0.04f + 0.4f) * 4;
            int snow = d < 2 ? 2 : (int)d;
            for (int gy = surfaceRow; gy < rows; gy++)
                grid[idx(gx, gy)] = (gy - surfaceRow < snow) ? M_SNOW : M_ICE;
        } else if (G.terrainType == TERRAIN_SAND) {
            for (int gy = surfaceRow; gy < rows; gy++) {
                float t = (float)(gy - surfaceRow) / totalDepth;
                grid[idx(gx, gy)] = t < 0.25f ? M_SAND_LIGHT
                                  : t < 0.65f ? M_SAND_MID : M_SAND_DEEP;
            }
        } else {
            for (int gy = surfaceRow; gy < rows; gy++) {
                int depth = gy - surfaceRow;
                float t = (float)depth / totalDepth;
                grid[idx(gx, gy)] = depth < 3 ? M_GRASS
                                  : t < 0.55f ? M_DIRT : M_DIRT_DEEP;
            }
        }
    }

    /* Grass levels sometimes get a lake or two in surface basins. */
    if (G.terrainType == TERRAIN_GRASS && frandf() < 0.7f) {
        int numLakes = 1 + (int)(frandf() * 2);
        for (int lake = 0; lake < numLakes; lake++) {
            int anchor = (int)(cols * (0.15f + frandf() * 0.7f));
            int half = (int)(cols * 0.08f);
            int lo = anchor, loY = surfaceRows[anchor];
            int from = anchor - half < 0 ? 0 : anchor - half;
            int to = anchor + half > cols ? cols : anchor + half;
            for (int gx = from; gx < to; gx++)
                if (surfaceRows[gx] > loY) { loY = surfaceRows[gx]; lo = gx; }

            const int RIM_LIFT = 4;
            int leftRim = lo, rightRim = lo;
            while (leftRim > 0 && surfaceRows[leftRim] >= loY - RIM_LIFT) leftRim--;
            while (rightRim < cols - 1 && surfaceRows[rightRim] >= loY - RIM_LIFT) rightRim++;

            int waterLevel = surfaceRows[leftRim] > surfaceRows[rightRim]
                           ? surfaceRows[leftRim] + 1 : surfaceRows[rightRim] + 1;
            if (loY - waterLevel < 3) continue;
            for (int gx = leftRim + 1; gx < rightRim; gx++)
                for (int gy = waterLevel; gy < surfaceRows[gx]; gy++)
                    if (gy >= 0 && gy < rows) grid[idx(gx, gy)] = M_WATER;
            for (int gy = waterLevel - 1 < 0 ? 0 : waterLevel - 1; gy <= loY && gy < rows; gy++)
                wake_span(gy, leftRim, rightRim);
        }
    }

    free(surfaceRows);

    G.precipMaterial = G.terrainType == TERRAIN_GRASS ? M_WATER
                     : G.terrainType == TERRAIN_ICE ? M_SNOW : 0;
    G.precipRate = 0;
}

void terrain_tick_precipitation(void)
{
    if (!grid || G.precipRate <= 0 || G.precipMaterial == 0 || G.precipBudget <= 0)
        return;
    float n = cols * G.precipRate;
    while (n > 0 && G.precipBudget > 0) {
        if (n < 1 && frandf() > n) break;
        int gx = (int)(frandf() * cols);
        if (grid[idx(gx, 0)] == 0) {
            grid[idx(gx, 0)] = (uint8_t)G.precipMaterial;
            wake_cell(gx, 0);
            G.precipBudget--;
        }
        n--;
    }
}

/* Ice cells melt-to-snow when (a) their column has no solid chain to the
 * floor, or (b) they're >LIMIT lateral cells from any rooted ice (BFS). */
int terrain_check_ice_stability(void)
{
    if (!grid) return 0;
    int n = cols * rows;
    int16_t *dist = malloc(sizeof(int16_t) * n);
    memset(dist, 0xff, sizeof(int16_t) * n);      /* -1 everywhere */
    int *queue = malloc(sizeof(int) * n * 2);
    int qtail = 0;

    for (int gx = 0; gx < cols; gx++) {
        for (int gy = rows - 1; gy >= 0; gy--) {
            uint8_t c = grid[idx(gx, gy)];
            if (c == 0 || fallKind[c] == FALL_LIQUID) break;
            if (c == M_ICE) {
                dist[idx(gx, gy)] = 0;
                queue[qtail++] = gx; queue[qtail++] = gy;
            }
        }
    }

    int head = 0;
    while (head < qtail) {
        int cx = queue[head++], cy = queue[head++];
        int ci = idx(cx, cy);
        int cd = dist[ci];
        if (cd >= ICE_OVERHANG_LIMIT) continue;
        int nd = cd + 1;
        static const int dx4[4] = { 1, -1, 0, 0 }, dy4[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx4[d], ny = cy + dy4[d];
            if (nx < 0 || nx >= cols || ny < 0 || ny >= rows) continue;
            int ni = idx(nx, ny);
            if (grid[ni] == M_ICE && (dist[ni] == -1 || dist[ni] > nd)) {
                dist[ni] = (int16_t)nd;
                if (qtail + 2 <= n * 2) { queue[qtail++] = nx; queue[qtail++] = ny; }
            }
        }
    }

    int changed = 0;
    for (int i = 0; i < n; i++)
        if (grid[i] == M_ICE && (dist[i] == -1 || dist[i] > ICE_OVERHANG_LIMIT)) {
            grid[i] = M_SNOW;
            changed++;
        }
    if (changed > 0)
        for (int gy = 0; gy < rows; gy++) wake_span(gy, 0, cols - 1);

    free(dist); free(queue);
    return changed;
}

/* Shared circular sweep. mode: 0=remove(melt ice), 1=vaporize liquid, 2=add */
static int radius_op(float cx, float cy, float radius, int mode, uint8_t addMat)
{
    if (!grid) return 0;
    float r2 = radius * radius;
    int minGx = (int)floorf(cx - radius), maxGx = (int)ceilf(cx + radius);
    int minGy = (int)floorf(cy - radius), maxGy = (int)ceilf(cy + radius);
    if (minGx < 0) minGx = 0;
    if (maxGx > cols - 1) maxGx = cols - 1;
    if (minGy < (mode == 2 ? 2 : 0)) minGy = mode == 2 ? 2 : 0;
    if (maxGy > rows - 1) maxGy = rows - 1;
    int n = 0;
    for (int gy = minGy; gy <= maxGy; gy++) {
        for (int gx = minGx; gx <= maxGx; gx++) {
            float dx = gx + 0.5f - cx, dy = gy + 0.5f - cy;
            if (dx * dx + dy * dy > r2) continue;
            int i = idx(gx, gy);
            uint8_t c = grid[i];
            if (mode == 0) {
                if (c == 0) continue;
                grid[i] = meltsTo[c];
                n++;
            } else if (mode == 1) {
                if (c != 0 && fallKind[c] == FALL_LIQUID) { grid[i] = 0; n++; }
            } else {
                if (c == 0) { grid[i] = addMat; n++; }
            }
        }
    }
    if (n > 0)
        for (int gy = minGy - 1 < 0 ? 0 : minGy - 1; gy <= maxGy; gy++)
            wake_span(gy, minGx - 2, maxGx + 2);
    return n;
}

int terrain_remove_radius(float cx, float cy, float radius)
{ return radius_op(cx, cy, radius, 0, 0); }

int terrain_vaporize_liquid(float cx, float cy, float radius)
{ return radius_op(cx, cy, radius, 1, 0); }

int terrain_add_radius(float cx, float cy, float radius)
{
    uint8_t mat = G.terrainType == TERRAIN_SAND ? M_SAND_MID
                : G.terrainType == TERRAIN_ICE ? M_SNOW : M_DIRT;
    return radius_op(cx, cy, radius, 2, mat);
}

/* wake helpers targeting the NEXT step's spans */
static inline void next_wake(int gy, int x0, int x1)
{
    span_add(nextMinX, nextMaxX, gy, x0, x1);
}

static bool simulation_step(void)
{
    if (!anyActive) return false;
    bool moved = false;
    anyActive = false;
    for (int i = 0; i < rows; i++) { nextMinX[i] = (int16_t)cols; nextMaxX[i] = -1; }

    for (int gy = rows - 2; gy >= 0; gy--) {
        if (rowMaxX[gy] < rowMinX[gy]) continue;
        bool leftFirst = (gy & 1) == 0;
        int lo = rowMinX[gy], hi = rowMaxX[gy];
        int start = leftFirst ? lo : hi;
        int stride = leftFirst ? 1 : -1;
        int end = leftFirst ? hi + 1 : lo - 1;

        for (int gx = start; gx != end; gx += stride) {
            int i = idx(gx, gy);
            uint8_t c = grid[i];
            if (c == 0) continue;
            int k = fallKind[c];
            if (k == FALL_NONE) continue;

            if (k == FALL_LIQUID) {
                /* multi-cell drop */
                const int LIQUID_DROP = 6;
                int dropTo = i;
                for (int d = 1; d <= LIQUID_DROP; d++) {
                    int ni = dropTo + cols;
                    if (ni >= cols * rows || grid[ni] != 0) break;
                    dropTo = ni;
                }
                if (dropTo != i) {
                    grid[dropTo] = c;
                    grid[i] = 0;
                    moved = true;
                    next_wake(gy, gx - 2, gx + 2);
                    next_wake(gy - 1, gx - 2, gx + 2);
                    int newGy = dropTo / cols;
                    next_wake(newGy, gx - 2, gx + 2);
                    next_wake(newGy + 1, gx - 2, gx + 2);
                    continue;
                }
                /* diagonal slide */
                int ldA = leftFirst ? -1 : 1;
                bool didSlide = false;
                for (int s = 0; s < 2; s++) {
                    int dir = s == 0 ? ldA : -ldA;
                    int nx = gx + dir;
                    if (nx < 0 || nx >= cols) continue;
                    int sideI = i + dir, diagI = sideI + cols;
                    if (grid[sideI] != 0 || grid[diagI] != 0) continue;
                    grid[diagI] = c;
                    grid[i] = 0;
                    didSlide = true;
                    break;
                }
                if (didSlide) {
                    moved = true;
                    next_wake(gy - 1, gx - 2, gx + 2);
                    next_wake(gy, gx - 2, gx + 2);
                    next_wake(gy + 1, gx - 2, gx + 2);
                    continue;
                }
                /* multi-cell horizontal flow, prefer holes below */
                const int LIQUID_FLOW = 6;
                int bestI = i, bestScore = 0;
                for (int s = 0; s < 2; s++) {
                    int dir = s == 0 ? ldA : -ldA;
                    for (int step = 1; step <= LIQUID_FLOW; step++) {
                        int nx = gx + dir * step;
                        if (nx < 0 || nx >= cols) break;
                        int ni = i + dir * step;
                        if (grid[ni] != 0) break;
                        int score = step;
                        if (ni + cols < cols * rows && grid[ni + cols] == 0) score += 100;
                        if (score > bestScore) { bestScore = score; bestI = ni; }
                    }
                }
                if (bestI != i) {
                    grid[bestI] = c;
                    grid[i] = 0;
                    moved = true;
                    int destX = bestI - gy * cols;
                    int sx0 = gx < destX ? gx : destX, sx1 = gx > destX ? gx : destX;
                    next_wake(gy - 1, sx0 - 2, sx1 + 2);
                    next_wake(gy, sx0 - 2, sx1 + 2);
                    next_wake(gy + 1, sx0 - 2, sx1 + 2);
                }
                continue;
            }

            /* solids: direct fall */
            int below = i + cols;
            if (grid[below] == 0) {
                grid[below] = c;
                grid[i] = 0;
                moved = true;
                next_wake(gy - 1, gx - 2, gx + 2);
                next_wake(gy, gx - 2, gx + 2);
                next_wake(gy + 1, gx - 2, gx + 2);
                continue;
            }
            /* diagonal slide — check geometry first, roll dirt's
             * keep-shape probability only if a slide is actually possible */
            int dirA = leftFirst ? -1 : 1;
            for (int s = 0; s < 2; s++) {
                int dir = s == 0 ? dirA : -dirA;
                int nx = gx + dir;
                if (nx < 0 || nx >= cols) continue;
                int sideI = i + dir, diagI = sideI + cols;
                if (grid[sideI] != 0 || grid[diagI] != 0) continue;
                if (k == FALL_DIRT && frandf() > DIRT_SLIDE_PROB) break;
                grid[diagI] = c;
                grid[i] = 0;
                moved = true;
                next_wake(gy - 1, gx - 2, gx + 2);
                next_wake(gy, gx - 2, gx + 2);
                next_wake(gy + 1, gx - 2, gx + 2);
                break;
            }
        }
    }

    int16_t *t1 = rowMinX; rowMinX = nextMinX; nextMinX = t1;
    int16_t *t2 = rowMaxX; rowMaxX = nextMaxX; nextMaxX = t2;
    anyActive = moved;
    return moved;
}

void terrain_update(void)
{
    if (!grid) return;
    for (int i = 0; i < STEPS_PER_FRAME; i++)
        if (!simulation_step()) break;
}

bool terrain_is_ground(float x, float y)
{
    if (!grid) return false;
    int gx = (int)x, gy = (int)y;
    if (gx < 0 || gx >= cols || gy < 0 || gy >= rows) return false;
    uint8_t c = grid[idx(gx, gy)];
    return c != 0 && fallKind[c] != FALL_LIQUID;
}

bool terrain_is_liquid(float x, float y)
{
    if (!grid) return false;
    int gx = (int)x, gy = (int)y;
    if (gx < 0 || gx >= cols || gy < 0 || gy >= rows) return false;
    uint8_t c = grid[idx(gx, gy)];
    return c != 0 && fallKind[c] == FALL_LIQUID;
}

/* Surface = first solid cell with a solid cell directly beneath (skips
 * in-flight precipitation grains). */
float terrain_get_height(float x)
{
    if (!grid) return (float)G.H;
    int gx = (int)x;
    if (gx < 0 || gx >= cols) return (float)G.H;
    for (int gy = 0; gy < rows - 1; gy++) {
        uint8_t c = grid[idx(gx, gy)];
        if (c == 0 || fallKind[c] == FALL_LIQUID) continue;
        uint8_t below = grid[idx(gx, gy + 1)];
        if (below != 0 && fallKind[below] != FALL_LIQUID) return (float)gy;
    }
    uint8_t last = grid[idx(gx, rows - 1)];
    if (last != 0 && fallKind[last] != FALL_LIQUID) return (float)(rows - 1);
    return (float)G.H;
}

float terrain_get_slope(float x)
{
    float h1 = terrain_get_height(x - 8);
    float h2 = terrain_get_height(x + 8);
    return atan2f(h2 - h1, 16.0f);
}

float terrain_get_water_top(float x)
{
    if (!grid) return -1;
    int gx = (int)x;
    if (gx < 0 || gx >= cols) return -1;
    for (int gy = 0; gy < rows; gy++) {
        uint8_t c = grid[idx(gx, gy)];
        if (c != 0 && fallKind[c] == FALL_LIQUID) return (float)gy;
    }
    return -1;
}

bool terrain_place_raft(float cx, float width)
{
    if (!grid) return false;
    int gcx = (int)cx;
    if (gcx < 0 || gcx >= cols) return false;
    int centreTop = -1;
    for (int gy = 0; gy < rows; gy++) {
        uint8_t c = grid[idx(gcx, gy)];
        if (c != 0 && fallKind[c] == FALL_LIQUID) { centreTop = gy; break; }
    }
    if (centreTop < 0) return false;
    int half = (int)(width / 2);
    const int plankRows = 4;
    for (int dy = 0; dy < plankRows; dy++) {
        int gy = centreTop + dy;
        if (gy < 0 || gy >= rows) continue;
        for (int dx = -half; dx <= half; dx++) {
            int gx = gcx + dx;
            if (gx < 0 || gx >= cols) continue;
            grid[idx(gx, gy)] = M_RAFT_MAT;
        }
    }
    for (int gy = centreTop - 2; gy <= centreTop + plankRows + 2; gy++)
        if (gy >= 0 && gy < rows) wake_span(gy, gcx - half - 2, gcx + half + 2);
    return true;
}
