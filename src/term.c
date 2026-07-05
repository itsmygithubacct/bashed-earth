/* Terminal layer: raw mode, kitty graphics protocol output, key input.
 *
 * Frames are packed to RGB, zlib-compressed (o=z) and pushed with a=T
 * (transmit + display), chunked into 4 KB base64 payloads. Two image ids
 * are used alternately: the new frame is transmitted and placed under one
 * id, then the previous frame's id is deleted. Retransmitting a single id
 * would flicker — kitty drops the old placement (blank screen) before the
 * replacement finishes decoding. Each frame is additionally wrapped in a
 * DEC 2026 synchronized update so the terminal applies it atomically.
 *
 * Compression + base64 + the pty write run on a presenter thread so a
 * slow-to-encode frame overlaps the next frame's logic and rasterization
 * instead of stalling the 30 fps loop; if the encoder is still busy when
 * the next frame arrives, the newest frame simply replaces the pending
 * one (frame drop, never a stall).
 */
#include "bashed_earth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <zlib.h>

static struct termios origTermios;
static bool rawActive = false;
static volatile int shutdownClaimed = 0;

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t inLen, char *out)
{
    size_t o = 0;
    size_t i = 0;
    for (; i + 2 < inLen; i += 3) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (i + 1 == inLen) {
        uint32_t v = in[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (i + 2 == inLen) {
        uint32_t v = (in[i] << 16) | (in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = '=';
    }
    return o;
}

static void write_all(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, buf, len);
        if (n <= 0) return;
        buf += n;
        len -= (size_t)n;
    }
}

static void write_str(const char *s) { write_all(s, strlen(s)); }

static char originSeq[32] = "\x1b[H";   /* cursor move to centered origin */

bool term_init(int *outW, int *outH)
{
    if (!isatty(STDIN_FILENO)) return false;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return false;
    int cols = ws.ws_col > 0 ? ws.ws_col : 80;
    int rows = ws.ws_row > 0 ? ws.ws_row : 24;
    /* cell size in pixels; assume 9x18 if the terminal doesn't report it */
    int cellW = ws.ws_xpixel > 0 ? ws.ws_xpixel / cols : 9;
    int cellH = ws.ws_ypixel > 0 ? ws.ws_ypixel / rows : 18;
    if (cellW <= 0) cellW = 9;
    if (cellH <= 0) cellH = 18;

    /* leave one cell row free at the bottom so the shell prompt after exit
     * doesn't scroll the image */
    int gridRows = rows - 1;
    int px = cols * cellW;
    int py = gridRows * cellH;
    if (px < 640) px = 640;
    if (py < 400) py = 400;
    if (px > 1600) px = 1600;
    if (py > 1000) py = 1000;
    /* snap to whole cells so the image doesn't end in a ragged
     * partially-covered cell column/row */
    px -= px % cellW;
    py -= py % cellH;
    *outW = px & ~1;
    *outH = py & ~1;

    /* center the playfield instead of pinning it top-left */
    int imgCols = (*outW + cellW - 1) / cellW;
    int imgRows = (*outH + cellH - 1) / cellH;
    int oc = 1 + (cols - imgCols) / 2;
    int or_ = 1 + (gridRows - imgRows) / 2;
    if (oc < 1) oc = 1;
    if (or_ < 1) or_ = 1;
    snprintf(originSeq, sizeof originSeq, "\x1b[%d;%dH", or_, oc);

    tcgetattr(STDIN_FILENO, &origTermios);
    struct termios raw = origTermios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    rawActive = true;

    /* alt screen, hide cursor, clear */
    write_str("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
    return true;
}

static void presenter_stop(void);

/* one-shot guard shared by the normal and signal-handler exit paths */
static bool claim_shutdown(void)
{
    if (!rawActive) return false;
    return !__sync_lock_test_and_set(&shutdownClaimed, 1);
}

static void restore_terminal(void)
{
    /* leading ST closes any APC a killed presenter left half-written */
    write_str("\x1b\\\x1b_Ga=d,d=A,q=2\x1b\\");
    write_str("\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
    rawActive = false;
}

void term_shutdown(void)
{
    if (!claim_shutdown()) return;
    /* stop the presenter first so no frame write interleaves the cleanup */
    presenter_stop();
    restore_terminal();
}

/* async-signal path: no locks, no pthread_join — a handler that touches
 * the presenter mutex can deadlock against its own interrupted thread */
void term_emergency_restore(void)
{
    if (!claim_shutdown()) return;
    restore_terminal();
}

/* runs on the presenter thread only */
static void encode_and_write(const uint8_t *rgba, int w, int h)
{
    static uint8_t *zbuf = NULL, *rgbbuf = NULL;
    static char *b64buf = NULL, *outbuf = NULL;
    static size_t zcap = 0, rgbcap = 0;

    /* strip the (always-opaque) alpha channel: 25% less data to compress,
     * encode and push down the pty every frame */
    size_t npx = (size_t)w * h;
    size_t rawLen = npx * 3;
    if (rawLen > rgbcap) {
        rgbcap = rawLen;
        rgbbuf = realloc(rgbbuf, rgbcap);
    }
    const uint8_t *s = rgba;
    uint8_t *d = rgbbuf;
    for (size_t i = 0; i < npx; i++, s += 4, d += 3) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
    }

    size_t need = compressBound(rawLen);
    if (need > zcap) {
        zcap = need;
        zbuf = realloc(zbuf, zcap);
        b64buf = realloc(b64buf, ((zcap + 2) / 3) * 4 + 8);
        outbuf = realloc(outbuf, ((zcap + 2) / 3) * 4 + (zcap / 4096 + 2) * 64 + 256);
    }

    uLongf zLen = (uLongf)zcap;
    if (compress2(zbuf, &zLen, rgbbuf, rawLen, 1) != Z_OK) return;

    size_t bLen = b64_encode(zbuf, zLen, b64buf);

    /* Double buffer: transmit the new frame under the id NOT currently on
     * screen, then delete the old id. Inside a synchronized update the swap
     * is atomic, so the screen never shows a half-drawn or blank state. */
    static int shown = 2;
    int newId = shown == 1 ? 2 : 1;

    /* assemble the whole frame (sync begin + cursor move + chunked APCs +
     * old-frame delete + sync end) in one buffer so it goes out in as few
     * writes as possible */
    char *o = outbuf;
    o += sprintf(o, "\x1b[?2026h%s", originSeq);
    const size_t CHUNK = 4096;
    size_t off = 0;
    bool first = true;
    while (off < bLen) {
        size_t n = bLen - off > CHUNK ? CHUNK : bLen - off;
        int more = off + n < bLen ? 1 : 0;
        if (first) {
            o += sprintf(o, "\x1b_Ga=T,f=24,i=%d,q=2,o=z,s=%d,v=%d,m=%d;",
                         newId, w, h, more);
            first = false;
        } else {
            o += sprintf(o, "\x1b_Gm=%d;", more);
        }
        memcpy(o, b64buf + off, n);
        o += n;
        *o++ = '\x1b';
        *o++ = '\\';
        off += n;
    }
    /* d=I frees the old frame's pixel data in the terminal, not just the
     * placement, so memory use stays at two frames */
    o += sprintf(o, "\x1b_Ga=d,d=I,i=%d,q=2\x1b\\\x1b[?2026l", shown);
    shown = newId;
    write_all(outbuf, (size_t)(o - outbuf));
}

/* ---------- presenter thread ---------- */
static pthread_t presenter;
static pthread_mutex_t frameLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frameCond = PTHREAD_COND_INITIALIZER;
static uint8_t *pendingBuf = NULL, *encodeBuf = NULL;
static size_t frameCap = 0;
static int frameW = 0, frameH = 0;
static bool framePending = false, presenterRun = false;

static void *presenter_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&frameLock);
        while (!framePending && presenterRun)
            pthread_cond_wait(&frameCond, &frameLock);
        if (!presenterRun) { pthread_mutex_unlock(&frameLock); break; }
        /* swap buffers: the game keeps writing new frames into pendingBuf
         * while we encode this one from encodeBuf */
        uint8_t *t = pendingBuf; pendingBuf = encodeBuf; encodeBuf = t;
        int w = frameW, h = frameH;
        framePending = false;
        pthread_mutex_unlock(&frameLock);

        encode_and_write(encodeBuf, w, h);
    }
    return NULL;
}

void term_present(const uint8_t *rgba, int w, int h)
{
    size_t need = (size_t)w * h * 4;
    pthread_mutex_lock(&frameLock);
    if (!presenterRun) {
        presenterRun = true;
        if (pthread_create(&presenter, NULL, presenter_main, NULL) != 0) {
            presenterRun = false;
            pthread_mutex_unlock(&frameLock);
            encode_and_write(rgba, w, h);   /* fall back to synchronous */
            return;
        }
    }
    if (need > frameCap) {
        frameCap = need;
        pendingBuf = realloc(pendingBuf, frameCap);
        encodeBuf = realloc(encodeBuf, frameCap);
    }
    /* overwriting an undelivered frame = dropping it in favor of this one */
    memcpy(pendingBuf, rgba, need);
    frameW = w; frameH = h;
    framePending = true;
    pthread_cond_signal(&frameCond);
    pthread_mutex_unlock(&frameLock);
}

static void presenter_stop(void)
{
    pthread_mutex_lock(&frameLock);
    if (!presenterRun) { pthread_mutex_unlock(&frameLock); return; }
    presenterRun = false;
    framePending = false;
    pthread_cond_signal(&frameCond);
    pthread_mutex_unlock(&frameLock);
    pthread_join(presenter, NULL);
}

/* Decode one key from stdin; returns -1 when no input is pending. */
int term_poll_key(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 127 || c == 8) return KEY_BACKSPACE;
    if (c == '\t') return KEY_TAB;
    if (c == 3) { G.quit = true; return -1; }   /* ctrl-c */

    if (c == 0x1b) {
        unsigned char seq[4];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return KEY_ESC;
        if (seq[0] != '[' && seq[0] != 'O') return KEY_ESC;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return KEY_ESC;
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:
            /* swallow the rest of longer CSI sequences (e.g. \e[1;5A) */
            while (seq[1] >= '0' && seq[1] <= ';') {
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) break;
            }
            return -1;
        }
    }
    return c;
}
