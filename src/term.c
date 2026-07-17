/* Game-facing terminal API over the shared Kitty framebuffer presenter. */
#include "bashed_earth.h"
#include "kitty_framebuffer.h"
#include "kitty_keyboard_posix.h"

#include <errno.h>
#include <unistd.h>

static kittyfb_session framebuffer;
static kittykb_terminal keyboard;
static bool framebuffer_active;
static bool keyboard_active;
static volatile int shutdown_claimed;

bool term_init(int *outW, int *outH)
{
    kittyfb_options options;
    kittykb_terminal_options key_options;

    kittyfb_session_init(&framebuffer);
    kittyfb_options_init(&options);
    options.install_winch_handler = false;
    if (kittyfb_start(&framebuffer, STDIN_FILENO, STDOUT_FILENO,
                      &options) != 0)
        return false;
    framebuffer_active = true;
    shutdown_claimed = 0;
    kittykb_terminal_init(&keyboard);
    kittykb_terminal_options_init(&key_options);
    key_options.flags = KITTYKB_FLAGS_KEY_STATE;
    key_options.make_raw = false;
    key_options.make_nonblocking = false;
    if (kittykb_terminal_start(&keyboard, STDIN_FILENO, STDOUT_FILENO,
                               &key_options) != 0) {
        int error = errno;
        kittyfb_stop(&framebuffer);
        framebuffer_active = false;
        errno = error;
        return false;
    }
    keyboard_active = true;
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
    if (keyboard_active) {
        (void)kittykb_terminal_stop(&keyboard);
        keyboard_active = false;
    }
    kittyfb_stop(&framebuffer);
    framebuffer_active = false;
}

void term_emergency_restore(void)
{
    static const char keyboard_pop[] = "\x1b\\\x1b[<u";

    if (!claim_shutdown()) return;
    if (keyboard_active)
        (void)write(STDOUT_FILENO, keyboard_pop, sizeof keyboard_pop - 1);
    kittyfb_emergency_restore(&framebuffer);
}

static int game_key(uint32_t key)
{
    switch (key) {
    case KITTYKB_KEY_ENTER: return KEY_ENTER;
    case KITTYKB_KEY_BACKSPACE: return KEY_BACKSPACE;
    case KITTYKB_KEY_TAB: return KEY_TAB;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP: return KEY_UP;
    case KITTYKB_KEY_DOWN: return KEY_DOWN;
    case KITTYKB_KEY_LEFT: return KEY_LEFT;
    case KITTYKB_KEY_RIGHT: return KEY_RIGHT;
    default: return key <= 0x7fU ? (int)key : -1;
    }
}

int term_poll_key(void)
{
    kittykb_event event;
    if (!keyboard_active || kittykb_terminal_read(&keyboard) < 0) return -1;
    while (kittykb_input_next(&keyboard.input, &event)) {
        if (event.action == KITTYKB_ACTION_RELEASE) continue;
        if ((event.modifiers & KITTYKB_MOD_CTRL) &&
            (event.key == 'c' || event.key == 'C')) {
            G.quit = true;
            return -1;
        }
        int key = game_key(event.key);
        if (key >= 0) return key;
    }
    return -1;
}
