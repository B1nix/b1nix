#ifndef TUI_H
#define TUI_H

/* ── ANSI Escape Sequences ── */

/* Cursor movement */
#define TUI_CURSOR_HOME    "\033[H"
#define TUI_CURSOR_UP(n)   "\033[" #n "A"
#define TUI_CURSOR_DOWN(n) "\033[" #n "B"
#define TUI_CURSOR_RIGHT(n) "\033[" #n "C"
#define TUI_CURSOR_LEFT(n) "\033[" #n "D"
#define TUI_CURSOR_GOTO(r,c) "\033[" #r ";" #c "H"
#define TUI_CURSOR_SAVE    "\033[s"
#define TUI_CURSOR_RESTORE "\033[u"
#define TUI_CURSOR_HIDE    "\033[?25l"
#define TUI_CURSOR_SHOW    "\033[?25h"

/* Clear screen */
#define TUI_CLEAR          "\033[2J"
#define TUI_CLEAR_LINE     "\033[K"
#define TUI_CLEAR_TO_EOL   "\033[K"
#define TUI_CLEAR_TO_BOL   "\033[1K"
#define TUI_CLEAR_DOWN     "\033[J"

/* Text attributes */
#define TUI_RESET          "\033[0m"
#define TUI_BOLD           "\033[1m"
#define TUI_DIM            "\033[2m"
#define TUI_UNDERLINE      "\033[4m"
#define TUI_BLINK          "\033[5m"
#define TUI_REVERSE        "\033[7m"

/* Foreground colors */
#define TUI_FG_BLACK       "\033[30m"
#define TUI_FG_RED         "\033[31m"
#define TUI_FG_GREEN       "\033[32m"
#define TUI_FG_YELLOW      "\033[33m"
#define TUI_FG_BLUE        "\033[34m"
#define TUI_FG_MAGENTA     "\033[35m"
#define TUI_FG_CYAN        "\033[36m"
#define TUI_FG_WHITE       "\033[37m"
#define TUI_FG_DEFAULT     "\033[39m"

/* Background colors */
#define TUI_BG_BLACK       "\033[40m"
#define TUI_BG_RED         "\033[41m"
#define TUI_BG_GREEN       "\033[42m"
#define TUI_BG_YELLOW      "\033[43m"
#define TUI_BG_BLUE        "\033[44m"
#define TUI_BG_MAGENTA     "\033[45m"
#define TUI_BG_CYAN        "\033[46m"
#define TUI_BG_WHITE       "\033[47m"
#define TUI_BG_DEFAULT     "\033[49m"

/* Bright foreground */
#define TUI_FG_BRIGHT_BLACK   "\033[90m"
#define TUI_FG_BRIGHT_RED     "\033[91m"
#define TUI_FG_BRIGHT_GREEN   "\033[92m"
#define TUI_FG_BRIGHT_YELLOW  "\033[93m"
#define TUI_FG_BRIGHT_BLUE    "\033[94m"
#define TUI_FG_BRIGHT_MAGENTA "\033[95m"
#define TUI_FG_BRIGHT_CYAN    "\033[96m"
#define TUI_FG_BRIGHT_WHITE   "\033[97m"

/* Key codes (returned by read_kbd, extended with escape sequences) */
#define KEY_UP     0xE001
#define KEY_DOWN   0xE002
#define KEY_LEFT   0xE003
#define KEY_RIGHT  0xE004
#define KEY_HOME   0xE005
#define KEY_END    0xE006
#define KEY_PGUP   0xE007
#define KEY_PGDN   0xE008
#define KEY_DEL    0xE009
#define KEY_INS    0xE00A
#define KEY_F1     0xE010
#define KEY_F2     0xE011
#define KEY_F3     0xE012
#define KEY_F4     0xE013
#define KEY_F5     0xE014
#define KEY_F6     0xE015
#define KEY_F7     0xE016
#define KEY_F8     0xE017
#define KEY_F9     0xE018
#define KEY_F10    0xE019
#define KEY_TAB    0xE01A
#define KEY_ESC    0xE01B
#define KEY_ENTER  0x0A
#define KEY_BACKSP 0x08

/* Ctrl+letter key codes */
#define KEY_CTRL_A 1
#define KEY_CTRL_B 2
#define KEY_CTRL_C 3
#define KEY_CTRL_D 4
#define KEY_CTRL_E 5
#define KEY_CTRL_F 6
#define KEY_CTRL_G 7
#define KEY_CTRL_H 8
#define KEY_CTRL_I 9
#define KEY_CTRL_J 10
#define KEY_CTRL_K 11
#define KEY_CTRL_L 12
#define KEY_CTRL_M 13
#define KEY_CTRL_N 14
#define KEY_CTRL_O 15
#define KEY_CTRL_P 16
#define KEY_CTRL_Q 17
#define KEY_CTRL_R 18
#define KEY_CTRL_S 19
#define KEY_CTRL_T 20
#define KEY_CTRL_U 21
#define KEY_CTRL_V 22
#define KEY_CTRL_W 23
#define KEY_CTRL_X 24
#define KEY_CTRL_Y 25
#define KEY_CTRL_Z 26

/* ── TUI Helper Macros ── */

/* Screen geometry — default 80x25 */
#define TUI_COLS 80
#define TUI_ROWS 25

/* ── TUI Common Functions ── */

/* Terminal control */
void tui_clear_screen(void);
void tui_cursor_goto(int row, int col);
void tui_cursor_hide(void);
void tui_cursor_show(void);
void tui_set_color(int fg, int bg);
void tui_reset_color(void);
void tui_reverse(int on);
void tui_bold(int on);
void tui_write(const char *s);
void tui_write_n(const char *s, int n);

/* Drawing */
void tui_draw_hline(int row, int col, int len, char ch, int fg, int bg);
void tui_draw_vline(int row, int col, int len, char ch, int fg, int bg);
void tui_draw_box(int row, int col, int height, int width, int fg, int bg);
void tui_write_at(int row, int col, const char *text, int max_len, int fg, int bg);
void tui_status_bar(int row, const char *text, int fg, int bg);
void tui_title_bar(int row, const char *title, int fg, int bg);

/* Input */
int tui_get_key(void);

/* Color aliases */
#define TUI_FG_INDEX(fg) fg
#define TUI_BG_INDEX(bg) bg

/* Color pairs for file manager */
#define TUI_COLOR_NORMAL    0
#define TUI_COLOR_DIR       1
#define TUI_COLOR_EXEC      2
#define TUI_COLOR_SELECTED  3
#define TUI_COLOR_HIGHLIGHT 4
#define TUI_COLOR_MENU      5
#define TUI_COLOR_STATUS    6
#define TUI_COLOR_TITLE     7

#endif /* TUI_H */
