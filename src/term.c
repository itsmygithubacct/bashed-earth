/* Game-facing terminal API over the shared Kitty framebuffer presenter. */
#include "bashed_earth.h"
#include "kitty_framebuffer.h"

#include <unistd.h>

static kittyfb_session framebuffer;
static bool framebuffer_active;
static volatile int shutdown_claimed;

bool term_init(int *outW, int *outH)
{
    kittyfb_options options;

    kittyfb_session_init(&framebuffer);
    kittyfb_options_init(&options);
    options.install_winch_handler = false;
    if (kittyfb_start(&framebuffer, STDIN_FILENO, STDOUT_FILENO,
                      &options) != 0)
        return false;
    framebuffer_active = true;
    shutdown_claimed = 0;
    *outW = kittyfb_width(&framebuffer);
    *outH = kittyfb_height(&framebuffer);
    return true;
}

void term_present(const uint8_t *rgba, int w, int h)
{
    if (framebuffer_active)
        (void)kittyfb_present(&framebuffer, rgba, w, h);
}

static bool claim_shutdown(void)
{
    if (!framebuffer_active) return false;
    return !__sync_lock_test_and_set(&shutdown_claimed, 1);
}

void term_shutdown(void)
{
    if (!claim_shutdown()) return;
    kittyfb_stop(&framebuffer);
    framebuffer_active = false;
}

void term_emergency_restore(void)
{
    if (!claim_shutdown()) return;
    kittyfb_emergency_restore(&framebuffer);
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
    if (c == 3) { G.quit = true; return -1; }

    if (c == 0x1b) {
        unsigned char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return KEY_ESC;
        if (seq[0] != '[' && seq[0] != 'O') return KEY_ESC;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return KEY_ESC;
        switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default:
            while (seq[1] >= '0' && seq[1] <= ';') {
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) break;
            }
            return -1;
        }
    }
    return c;
}
