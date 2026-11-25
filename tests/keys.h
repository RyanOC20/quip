/* Byte sequences a terminal sends for keys, as quip reads them. */
#ifndef QUIP_TEST_KEYS_H
#define QUIP_TEST_KEYS_H

#define K_ENTER     "\r"
#define K_TAB       "\t"
#define K_BACKSPACE "\x7f"
#define K_ESC       "\x1b"
#define K_UP        "\x1b[A"
#define K_DOWN      "\x1b[B"
#define K_RIGHT     "\x1b[C"
#define K_LEFT      "\x1b[D"
#define K_HOME      "\x1b[H"
#define K_END       "\x1b[F"
#define K_DEL       "\x1b[3~"
#define K_PGUP      "\x1b[5~"
#define K_PGDN      "\x1b[6~"
#define K_SHIFT_TAB "\x1b[Z"

#define K_SHIFT_UP         "\x1b[1;2A"
#define K_SHIFT_DOWN       "\x1b[1;2B"
#define K_SHIFT_RIGHT      "\x1b[1;2C"
#define K_SHIFT_LEFT       "\x1b[1;2D"
#define K_ALT_UP           "\x1b[1;3A"
#define K_ALT_DOWN         "\x1b[1;3B"
#define K_ALT_RIGHT        "\x1b[1;3C"
#define K_ALT_LEFT         "\x1b[1;3D"
#define K_META_B           "\033b" /* ESC b: Alt+Left in terminals that send it. */
#define K_META_F           "\033f" /* ESC f: Alt+Right. */
#define K_SHIFT_ALT_RIGHT  "\x1b[1;4C"
#define K_SHIFT_ALT_LEFT   "\x1b[1;4D"
#define K_CTRL_UP          "\x1b[1;5A"
#define K_CTRL_DOWN        "\x1b[1;5B"
#define K_CTRL_RIGHT       "\x1b[1;5C"
#define K_CTRL_LEFT        "\x1b[1;5D"
#define K_SHIFT_CTRL_UP    "\x1b[1;6A"
#define K_SHIFT_CTRL_DOWN  "\x1b[1;6B"
#define K_SHIFT_CTRL_RIGHT "\x1b[1;6C"
#define K_SHIFT_CTRL_LEFT  "\x1b[1;6D"

#define K_CTRL_C     "\x03"
#define K_CTRL_E     "\x05"
#define K_CTRL_F     "\x06"
#define K_CTRL_Q     "\x11"
#define K_CTRL_S     "\x13"
#define K_CTRL_V     "\x16"
#define K_CTRL_X     "\x18"
#define K_CTRL_Y     "\x19"
#define K_CTRL_Z     "\x1a"
#define K_CTRL_SLASH "\x1f"

#endif
