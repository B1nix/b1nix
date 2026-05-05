#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <b1nix/syscall.h>
#include <tui.h>

/* ── Terminal manipulation ── */

static void t_write(const char *s)
{
	syscall_dispatch(SYS_WRITE, (u64)(usize)s, strlen(s), 0, 0);
}

static void t_write_n(const char *s, int n)
{
	syscall_dispatch(SYS_WRITE, (u64)(usize)s, n, 0, 0);
}

void tui_write(const char *s)
{
	t_write(s);
}

void tui_write_n(const char *s, int n)
{
	t_write_n(s, n);
}

void tui_clear_screen(void)
{
	t_write(TUI_CLEAR);
	t_write(TUI_CURSOR_HOME);
}

void tui_cursor_goto(int row, int col)
{
	char buf[32];
	int pos = 0;
	buf[pos++] = '\033';
	buf[pos++] = '[';
	
	/* Row */
	if (row >= 100) buf[pos++] = '0' + (row / 100);
	if (row >= 10) buf[pos++] = '0' + ((row / 10) % 10);
	buf[pos++] = '0' + (row % 10);
	
	buf[pos++] = ';';
	
	/* Col */
	if (col >= 100) buf[pos++] = '0' + (col / 100);
	if (col >= 10) buf[pos++] = '0' + ((col / 10) % 10);
	buf[pos++] = '0' + (col % 10);
	
	buf[pos++] = 'H';
	buf[pos] = '\0';
	t_write(buf);
}

void tui_cursor_hide(void)
{
	t_write("\033[?25l");
}

void tui_cursor_show(void)
{
	t_write("\033[?25h");
}

void tui_set_color(int fg, int bg)
{
	/* Simplified: set fg and bg using ANSI */
	char buf[32];
	int pos = 0;
	buf[pos++] = '\033';
	buf[pos++] = '[';
	
	/* Foreground: 30-37 for standard, 90-97 for bright */
	if (fg < 8) {
		buf[pos++] = '3';
		buf[pos++] = '0' + fg;
	} else {
		buf[pos++] = '9';
		buf[pos++] = '0' + (fg - 8);
	}
	
	buf[pos++] = ';';
	
	/* Background: 40-47 */
	if (bg < 8) {
		buf[pos++] = '4';
		buf[pos++] = '0' + bg;
	} else {
		buf[pos++] = '1';
		buf[pos++] = '0';
		buf[pos++] = '0' + (bg - 8);
	}
	
	buf[pos++] = 'm';
	buf[pos] = '\0';
	t_write(buf);
}

void tui_reset_color(void)
{
	t_write(TUI_RESET);
}

void tui_reverse(int on)
{
	t_write(on ? TUI_REVERSE : TUI_RESET);
}

void tui_bold(int on)
{
	t_write(on ? TUI_BOLD : TUI_RESET);
}

/* ── Drawing primitives ── */

void tui_draw_hline(int row, int col, int len, char ch, int fg, int bg)
{
	tui_cursor_goto(row, col);
	tui_set_color(fg, bg);
	
	/* Draw line using repeated character */
	char buf[256];
	int max = len < 255 ? len : 255;
	memset(buf, ch, max);
	buf[max] = '\0';
	t_write_n(buf, max);
	tui_reset_color();
}

void tui_draw_vline(int row, int col, int len, char ch, int fg, int bg)
{
	tui_set_color(fg, bg);
	for (int i = 0; i < len; i++) {
		tui_cursor_goto(row + i, col);
		t_write_n(&ch, 1);
	}
	tui_reset_color();
}

void tui_draw_box(int row, int col, int height, int width, int fg, int bg)
{
	/* Top */
	tui_cursor_goto(row, col);
	tui_set_color(fg, bg);
	t_write("+");
	for (int i = 0; i < width - 2; i++) t_write("-");
	t_write("+");
	
	/* Sides */
	for (int i = 1; i < height - 1; i++) {
		tui_cursor_goto(row + i, col);
		t_write("|");
		tui_cursor_goto(row + i, col + width - 1);
		t_write("|");
	}
	
	/* Bottom */
	tui_cursor_goto(row + height - 1, col);
	t_write("+");
	for (int i = 0; i < width - 2; i++) t_write("-");
	t_write("+");
	
	tui_reset_color();
}

void tui_write_at(int row, int col, const char *text, int max_len, int fg, int bg)
{
	tui_cursor_goto(row, col);
	tui_set_color(fg, bg);
	
	int len = strlen(text);
	if (max_len > 0 && len > max_len) len = max_len;
	
	t_write_n(text, len);
	
	/* Clear to end of line if needed */
	if (max_len > 0 && len < max_len) {
		char spaces[256];
		int space_count = max_len - len;
		if (space_count > 255) space_count = 255;
		memset(spaces, ' ', space_count);
		t_write_n(spaces, space_count);
	}
	
	tui_reset_color();
}

void tui_status_bar(int row, const char *text, int fg, int bg)
{
	tui_write_at(row, 1, text, TUI_COLS - 2, fg, bg);
}

void tui_title_bar(int row, const char *title, int fg, int bg)
{
	char buf[TUI_COLS];
	int len = strlen(title);
	int pad = (TUI_COLS - 2 - len) / 2;
	
	memset(buf, ' ', TUI_COLS - 1);
	buf[TUI_COLS - 1] = '\0';
	
	if (pad > 0 && pad < TUI_COLS - 2) {
		memcpy(buf + pad, title, len > (TUI_COLS - 2 - pad) ? (TUI_COLS - 2 - pad) : len);
	}
	
	tui_write_at(row, 1, buf, TUI_COLS - 2, fg, bg);
}

/* ── Input reading ── */

/* Read a single key, return extended key code */
int tui_get_key(void)
{
	int c = (int)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
	
	if (c == 0x1B) {
		int seq1 = (int)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
		if (seq1 == '[') {
			int seq2 = (int)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
			if (seq2 == 'A') return KEY_UP;
			if (seq2 == 'B') return KEY_DOWN;
			if (seq2 == 'C') return KEY_RIGHT;
			if (seq2 == 'D') return KEY_LEFT;
			if (seq2 == 'H') return KEY_HOME;
			if (seq2 == 'F') return KEY_END;
			if (seq2 == 'M') {
				int f = (int)syscall_dispatch(SYS_READ_KBD, 0, 0, 0, 0);
				if (f >= 1 && f <= 10) return KEY_F1 + (f - 1);
				if (f == 11) return KEY_F11;
				if (f == 12) return KEY_F12;
			}
		}
		return KEY_ESC;
	}
	
	if (c == '\t') return KEY_TAB;
	if (c == '\n') return KEY_ENTER;
	if (c == 0x7F || c == '\b') return KEY_BACKSP;
	
	return c;
}
