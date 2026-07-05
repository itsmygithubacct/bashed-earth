/* Terminal layer: raw mode, kitty graphics protocol output, key input.
 *
 * Frames are RGBA buffers zlib-compressed (o=z) and pushed as image id 1
 * with a=T (transmit + display), chunked into 4 KB base64 payloads. Each
 * retransmission of id 1 replaces the previous placement, giving us
 * flicker-free ~30 fps animation on kitty-family terminals (kitty, kilix).
 */
#include "bashed_earth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <zlib.h>

static struct termios origTermios;
static bool rawActive = false;

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

bool term_init(int *outW, int *outH)
{
    if (!isatty(STDIN_FILENO)) return false;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return false;
    int px = ws.ws_xpixel, py = ws.ws_ypixel;
    if (px <= 0 || py <= 0) {
        /* terminal doesn't report pixels; assume 9x18 cells */
        px = ws.ws_col * 9;
        py = ws.ws_row * 18;
    }
    /* leave one cell row free at the bottom so the shell prompt after exit
     * doesn't scroll the image */
    int cellH = ws.ws_row > 0 ? py / ws.ws_row : 18;
    py -= cellH;

    if (px < 640) px = 640;
    if (py < 400) py = 400;
    if (px > 1600) px = 1600;
    if (py > 1000) py = 1000;
    *outW = px & ~1;
    *outH = py & ~1;

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
    write_all("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", 24);
    return true;
}

void term_shutdown(void)
{
    if (!rawActive) return;
    /* delete our image, leave alt screen, show cursor */
    write_all("\x1b_Ga=d,d=A,q=2\x1b\\", 16);
    write_all("\x1b[?25h\x1b[?1049l", 14);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios);
    rawActive = false;
}

void term_present(const uint8_t *rgba, int w, int h)
{
    static uint8_t *zbuf = NULL;
    static char *b64buf = NULL, *outbuf = NULL;
    static size_t zcap = 0;

    size_t rawLen = (size_t)w * h * 4;
    size_t need = compressBound(rawLen);
    if (need > zcap) {
        zcap = need;
        zbuf = realloc(zbuf, zcap);
        b64buf = realloc(b64buf, ((zcap + 2) / 3) * 4 + 8);
        outbuf = realloc(outbuf, ((zcap + 2) / 3) * 4 + (zcap / 4096 + 2) * 64 + 128);
    }

    uLongf zLen = (uLongf)zcap;
    if (compress2(zbuf, &zLen, rgba, rawLen, 1) != Z_OK) return;

    size_t bLen = b64_encode(zbuf, zLen, b64buf);

    /* assemble the whole frame (cursor home + chunked APCs) in one buffer
     * so it goes out in as few writes as possible */
    char *o = outbuf;
    o += sprintf(o, "\x1b[H");
    const size_t CHUNK = 4096;
    size_t off = 0;
    bool first = true;
    while (off < bLen) {
        size_t n = bLen - off > CHUNK ? CHUNK : bLen - off;
        int more = off + n < bLen ? 1 : 0;
        if (first) {
            o += sprintf(o, "\x1b_Ga=T,f=32,i=1,q=2,o=z,s=%d,v=%d,m=%d;", w, h, more);
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
    write_all(outbuf, (size_t)(o - outbuf));
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
