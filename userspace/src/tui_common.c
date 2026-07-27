#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <tui.h>

/* ── Terminal manipulation ── */

static void t_write(const char *s)
{
	write(1, s, strlen(s));
}

static void t_write_n(const char *s, int n)
{
	write(1, s, n);
}

/* fd 1 saved across tui_screen_mute()/tui_screen_unmute(); -1 = not muted. */
static int tui_saved_stdout = -1;

void tui_screen_mute(void)
{
	if (tui_saved_stdout >= 0)
		return;
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull < 0)
		return;
	int saved = dup(1);
	if (saved < 0) {
		close(devnull);
		return;
	}
	fflush(stdout);
	dup2(devnull, 1);
	close(devnull);
	tui_saved_stdout = saved;
}

void tui_screen_unmute(void)
{
	if (tui_saved_stdout < 0)
		return;
	fflush(stdout);
	dup2(tui_saved_stdout, 1);
	close(tui_saved_stdout);
	tui_saved_stdout = -1;
}

static int tui_stdio_ready = 0;
static int tui_raw_depth = 0;
static int tui_raw_active = 0;
static struct termios tui_saved_termios;

static void tui_ensure_stdio(void)
{
	if (tui_stdio_ready) {
		return;
	}

	int tty = open("/dev/tty", O_RDWR);
	if (tty < 0) {
		return;
	}

	dup2(tty, 0);
	dup2(tty, 1);
	dup2(tty, 2);
	if (tty > 2) {
		close(tty);
	}
	tui_stdio_ready = 1;
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
	char buf[32];
	int pos = 0;
	buf[pos++] = '\033';
	buf[pos++] = '[';
	
	if (fg < 8) {
		buf[pos++] = '3';
		buf[pos++] = '0' + fg;
	} else {
		buf[pos++] = '9';
		buf[pos++] = '0' + (fg - 8);
	}
	
	buf[pos++] = ';';
	
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

int tui_terminal_begin(void)
{
	tui_ensure_stdio();

	if (tui_raw_depth++ > 0) {
		return 0;
	}

	if (tcgetattr(0, &tui_saved_termios) < 0) {
		tui_raw_depth = 0;
		return -1;
	}

	struct termios raw = tui_saved_termios;
	raw.c_lflag &= ~(ICANON | ECHO | ISIG);
	if (tcsetattr(0, TCSANOW, &raw) < 0) {
		tui_raw_depth = 0;
		return -1;
	}

	tui_raw_active = 1;
	return 0;
}

void tui_terminal_end(void)
{
	if (tui_raw_depth <= 0) {
		return;
	}

	tui_raw_depth--;
	if (tui_raw_depth > 0 || !tui_raw_active) {
		return;
	}

	tcsetattr(0, TCSANOW, &tui_saved_termios);
	tui_raw_active = 0;
}

int tui_decode_key_sequence(const char *seq, usize len)
{
	if (!seq || len == 0) {
		return KEY_ESC;
	}

	unsigned char c = (unsigned char)seq[0];
	if (c == 0x1B) {
		if (len == 1) {
			return KEY_ESC;
		}

		if (seq[1] == '[') {
			if (len >= 3) {
				switch ((unsigned char)seq[2]) {
				case 'A': return KEY_UP;
				case 'B': return KEY_DOWN;
				case 'C': return KEY_RIGHT;
				case 'D': return KEY_LEFT;
				case 'H': return KEY_HOME;
				case 'F': return KEY_END;
				case 'M':
					if (len >= 4) {
						switch ((unsigned char)seq[3]) {
						case 1: return KEY_F1;
						case 2: return KEY_F2;
						case 3: return KEY_F3;
						case 4: return KEY_F4;
						case 5: return KEY_F5;
						case 6: return KEY_F6;
						case 7: return KEY_F7;
						case 8: return KEY_F8;
						case 9: return KEY_F9;
						case 10: return KEY_F10;
						case 11: return KEY_F11;
						case 12: return KEY_F12;
						default: break;
						}
					}
					break;
				case 'O':
					if (len >= 4) {
						switch ((unsigned char)seq[3]) {
						case 'P': return KEY_F1;
						case 'Q': return KEY_F2;
						case 'R': return KEY_F3;
						case 'S': return KEY_F4;
						case 'A': return KEY_UP;
						case 'B': return KEY_DOWN;
						case 'C': return KEY_RIGHT;
						case 'D': return KEY_LEFT;
						case 'H': return KEY_HOME;
						case 'F': return KEY_END;
						default: break;
						}
					}
					break;
				default:
					break;
				}

				if (seq[2] >= '0' && seq[2] <= '9') {
					int value = 0;
					usize i = 2;
					while (i < len && seq[i] != '~') {
						if (seq[i] < '0' || seq[i] > '9') {
							return KEY_ESC;
						}
						value = (value * 10) + (seq[i] - '0');
						i++;
					}
					switch (value) {
					case 1: return KEY_HOME;
					case 2: return KEY_INS;
					case 3: return KEY_DEL;
					case 4: return KEY_END;
					case 5: return KEY_PGUP;
					case 6: return KEY_PGDN;
					case 7: return KEY_HOME;
					case 8: return KEY_END;
					case 11: return KEY_F1;
					case 12: return KEY_F2;
					case 13: return KEY_F3;
					case 14: return KEY_F4;
					case 15: return KEY_F5;
					case 17: return KEY_F6;
					case 18: return KEY_F7;
					case 19: return KEY_F8;
					case 20: return KEY_F9;
					case 21: return KEY_F10;
					case 23: return KEY_F11;
					case 24: return KEY_F12;
					default:
						return KEY_ESC;
					}
				}
			}
		} else if (seq[1] == 'O') {
			if (len >= 3) {
				switch ((unsigned char)seq[2]) {
				case 'P': return KEY_F1;
				case 'Q': return KEY_F2;
				case 'R': return KEY_F3;
				case 'S': return KEY_F4;
				case 'A': return KEY_UP;
				case 'B': return KEY_DOWN;
				case 'C': return KEY_RIGHT;
				case 'D': return KEY_LEFT;
				case 'H': return KEY_HOME;
				case 'F': return KEY_END;
				default:
					return KEY_ESC;
				}
			}
		}

		return KEY_ESC;
	}

	if (c == '\t') return KEY_TAB;
	if (c == '\n' || c == '\r') return KEY_ENTER;
	if (c == 0x7F || c == '\b') return KEY_BACKSP;

	return (int)c;
}

int tui_get_key(void)
{
	char ch = 0;
	if (read(0, &ch, 1) <= 0) return KEY_ESC;

	if (ch == 0x1B) {
		char seq[6];
		int len = 0;
		seq[len++] = ch;

		char seq1 = 0;
		if (read(0, &seq1, 1) > 0) {
			seq[len++] = seq1;
			if (seq1 == '[') {
				char seq2 = 0;
				if (read(0, &seq2, 1) > 0) {
					seq[len++] = seq2;
					if (seq2 >= '0' && seq2 <= '9') {
						char seq3 = 0;
						if (read(0, &seq3, 1) > 0) {
							seq[len++] = seq3;
						}
					}
				}
			}
		}
		return tui_decode_key_sequence(seq, (usize)len);
	}

	return tui_decode_key_sequence(&ch, 1);
}
