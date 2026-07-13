#include <b1nix/gui.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "font8x8.h"

#define COLS 76
#define ROWS 28
#define CELL_W 8
#define CELL_H 10

static char cells[ROWS][COLS];
static unsigned cursor_x;
static unsigned cursor_y;
static int term_master = -1; /* pty master fd, for terminal replies (DSR) */

/* ANSI/VT100 escape-sequence parser state. */
static int esc_state;   /* 0 = normal, 1 = saw ESC, 2 = collecting CSI */
static int esc_private; /* CSI '?' private-mode marker (consumed, ignored) */
static int esc_params[8];
static int esc_nparams;

static void clear_row(unsigned row) {
	memset(cells[row], ' ', COLS);
}

static void scroll_up(void) {
	memmove(cells[0], cells[1], (ROWS - 1) * COLS);
	clear_row(ROWS - 1);
	cursor_y = ROWS - 1;
}

/* Execute a completed CSI sequence (ESC [ params final). Implements the cursor
 * movement, erase and line-edit ops bash/readline and curses TUIs depend on, so
 * arrow keys / Home / End / history move the cursor instead of corrupting the
 * line. SGR colour ('m') and private modes are accepted and ignored. */
static void csi_dispatch(char final) {
	int p0 = esc_params[0];
	int p1 = esc_params[1];
	unsigned n;
	switch (final) {
	case 'A': n = p0 ? (unsigned)p0 : 1; cursor_y = cursor_y > n ? cursor_y - n : 0; break;
	case 'B': n = p0 ? (unsigned)p0 : 1; cursor_y += n; if (cursor_y >= ROWS) cursor_y = ROWS - 1; break;
	case 'C': n = p0 ? (unsigned)p0 : 1; cursor_x += n; if (cursor_x >= COLS) cursor_x = COLS - 1; break;
	case 'D': n = p0 ? (unsigned)p0 : 1; cursor_x = cursor_x > n ? cursor_x - n : 0; break;
	case 'G': /* CHA: cursor to 1-based column */
		cursor_x = p0 ? (unsigned)(p0 - 1) : 0;
		if (cursor_x >= COLS) cursor_x = COLS - 1;
		break;
	case 'd': /* VPA: cursor to 1-based row */
		cursor_y = p0 ? (unsigned)(p0 - 1) : 0;
		if (cursor_y >= ROWS) cursor_y = ROWS - 1;
		break;
	case 'H': case 'f': /* CUP: cursor to 1-based row;col */
		cursor_y = p0 ? (unsigned)(p0 - 1) : 0;
		cursor_x = p1 ? (unsigned)(p1 - 1) : 0;
		if (cursor_y >= ROWS) cursor_y = ROWS - 1;
		if (cursor_x >= COLS) cursor_x = COLS - 1;
		break;
	case 'K': /* EL: erase in line */
		if (p0 == 0)      for (unsigned x = cursor_x; x < COLS; x++) cells[cursor_y][x] = ' ';
		else if (p0 == 1) for (unsigned x = 0; x <= cursor_x && x < COLS; x++) cells[cursor_y][x] = ' ';
		else if (p0 == 2) clear_row(cursor_y);
		break;
	case 'J': /* ED: erase in display */
		if (p0 == 0) {
			for (unsigned x = cursor_x; x < COLS; x++) cells[cursor_y][x] = ' ';
			for (unsigned r = cursor_y + 1; r < ROWS; r++) clear_row(r);
		} else if (p0 == 1) {
			for (unsigned r = 0; r < cursor_y; r++) clear_row(r);
			for (unsigned x = 0; x <= cursor_x && x < COLS; x++) cells[cursor_y][x] = ' ';
		} else {
			for (unsigned r = 0; r < ROWS; r++) clear_row(r);
			cursor_x = cursor_y = 0;
		}
		break;
	case 'P': /* DCH: delete chars, shift rest of line left */
		n = p0 ? (unsigned)p0 : 1;
		for (unsigned x = cursor_x; x < COLS; x++)
			cells[cursor_y][x] = (x + n < COLS) ? cells[cursor_y][x + n] : ' ';
		break;
	case '@': /* ICH: insert blanks, shift rest of line right */
		n = p0 ? (unsigned)p0 : 1;
		for (unsigned x = COLS; x-- > cursor_x; )
			cells[cursor_y][x] = (x >= cursor_x + n) ? cells[cursor_y][x - n] : ' ';
		break;
	case 'n': /* DSR: report cursor position as ESC[row;colR */
		if (p0 == 6 && term_master >= 0) {
			char rep[24], tmp[8];
			int l = 0, t;
			unsigned r = cursor_y + 1, c = cursor_x + 1;
			rep[l++] = '\033'; rep[l++] = '[';
			t = 0; do { tmp[t++] = (char)('0' + r % 10); r /= 10; } while (r);
			while (t) rep[l++] = tmp[--t];
			rep[l++] = ';';
			t = 0; do { tmp[t++] = (char)('0' + c % 10); c /= 10; } while (c);
			while (t) rep[l++] = tmp[--t];
			rep[l++] = 'R';
			(void)write(term_master, rep, (size_t)l);
		}
		break;
	default: /* 'm' (SGR colour) and anything unhandled: ignore */
		break;
	}
}

static void put_terminal_char(char ch) {
	unsigned char c = (unsigned char)ch;

	if (esc_state == 1) { /* just saw ESC */
		if (c == '[') {
			esc_state = 2;
			esc_private = 0;
			esc_nparams = 1;
			memset(esc_params, 0, sizeof(esc_params));
		} else {
			/* other ESC sequences (charset select, keypad mode, …):
			 * swallow the single following byte and resume. */
			esc_state = 0;
		}
		return;
	}
	if (esc_state == 2) { /* collecting CSI parameters */
		if (c == '?') {
			esc_private = 1;
			return;
		}
		if (c >= '0' && c <= '9') {
			if (esc_nparams <= 8)
				esc_params[esc_nparams - 1] =
				    esc_params[esc_nparams - 1] * 10 + (c - '0');
			return;
		}
		if (c == ';') {
			if (esc_nparams < 8)
				esc_nparams++;
			return;
		}
		if (c >= 0x40 && c <= 0x7e) { /* final byte */
			if (!esc_private)
				csi_dispatch((char)c);
			esc_state = 0;
			return;
		}
		return; /* intermediate byte: keep collecting */
	}

	/* normal state */
	switch (c) {
	case 0x1b: esc_state = 1; return;        /* ESC */
	case '\r': cursor_x = 0; return;
	case '\n': cursor_x = 0; if (++cursor_y >= ROWS) scroll_up(); return;
	case '\b': if (cursor_x > 0) cursor_x--; return; /* non-destructive */
	case '\t': cursor_x = (cursor_x + 8) & ~7u;
		if (cursor_x >= COLS) cursor_x = COLS - 1; return;
	case 0x07: return;                       /* bell */
	}
	if (c < 32 || c >= 127)
		return;
	cells[cursor_y][cursor_x++] = (char)c;
	if (cursor_x >= COLS) {
		cursor_x = 0;
		if (++cursor_y >= ROWS)
			scroll_up();
	}
}

static void render(struct b1gui_window *win) {
	for (unsigned i = 0; i < win->width * win->height; i++)
		win->pixels[i] = 0x000B1117u;
	for (unsigned row = 0; row < ROWS; row++) {
		for (unsigned col = 0; col < COLS; col++) {
			unsigned char c = (unsigned char)cells[row][col];
			const unsigned char *glyph = font8x8_basic[c < 128 ? c : '?'];
			uint32_t color = row == 0 ? 0x007DD3FCu : 0x00D5E7E8u;
			for (unsigned gy = 0; gy < 8; gy++)
				for (unsigned gx = 0; gx < 8; gx++)
					if (glyph[gy] & (1u << (7 - gx))) {
						unsigned x = col * CELL_W + gx;
						unsigned y = row * CELL_H + gy + 1;
						if (x < win->width && y < win->height)
							win->pixels[y * win->width + x] = color;
					}
		}
	}
	unsigned cx = cursor_x * CELL_W;
	unsigned cy = cursor_y * CELL_H + 9;
	for (unsigned x = cx; x < cx + 7 && x < win->width; x++)
		if (cy < win->height)
			win->pixels[cy * win->width + x] = 0x004DE1A8u;
	b1gui_present(win, 0, 0, win->width, win->height);
}

static char key_char(unsigned scan, int shift) {
	static const char normal[128] = {
	    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
	    [0x07]='6',[0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',
	    [0x0c]='-',[0x0d]='=',[0x10]='q',[0x11]='w',[0x12]='e',
	    [0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',
	    [0x18]='o',[0x19]='p',[0x1a]='[',[0x1b]=']',[0x1e]='a',
	    [0x1f]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
	    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',
	    [0x29]='`',[0x2b]='\\',[0x2c]='z',[0x2d]='x',[0x2e]='c',
	    [0x2f]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',
	    [0x34]='.',[0x35]='/',[0x39]=' '
	};
	static const char shifted[128] = {
	    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',
	    [0x07]='^',[0x08]='&',[0x09]='*',[0x0a]='(',[0x0b]=')',
	    [0x0c]='_',[0x0d]='+',[0x10]='Q',[0x11]='W',[0x12]='E',
	    [0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',
	    [0x18]='O',[0x19]='P',[0x1a]='{',[0x1b]='}',[0x1e]='A',
	    [0x1f]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',
	    [0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',
	    [0x29]='~',[0x2b]='|',[0x2c]='Z',[0x2d]='X',[0x2e]='C',
	    [0x2f]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',
	    [0x34]='>',[0x35]='?',[0x39]=' '
	};
	return scan < 128 ? (shift ? shifted[scan] : normal[scan]) : 0;
}

int main(void) {
	struct b1gui_window win;
	if (b1gui_connect(&win) ||
	    b1gui_create_window(&win, COLS * CELL_W, ROWS * CELL_H, "Terminal"))
		return 1;
	(void)b1gui_checksum(&win);
	struct b1gui_menu_item items[] = {
	    {1, 0, "New Tab", "Ctrl+T"},
	    {0, B1GUI_MENU_SEPARATOR, "", ""},
	    {2, 0, "Copy",  "Ctrl+C"},
	    {3, 0, "Paste", "Ctrl+V"},
	};
	b1gui_register_menu(&win, items, 4); /* ensure objects exist before any fork */

	int shift = 0;
	int running = 1;

	/* Keep one shell alive in this window. If the shell exits (e.g. the user
	 * types `exit`), reset the screen and start a fresh one in place instead
	 * of tearing down the whole terminal — that keeps gdesktop from having to
	 * respawn (and cascade) a new window. The display socket is FD_CLOEXEC
	 * (set in b1gui_connect), so the shell never inherits it. */
	while (running) {
		for (unsigned row = 0; row < ROWS; row++)
			clear_row(row);
		cursor_x = cursor_y = 0;

		int master = -1;
		pid_t child = forkpty(&master, 0, 0, 0);
		if (child == 0) {
			execlp("/bin/bash", "bash", (char *)0);
			_exit(127);
		}
		if (master < 0)
			break; /* pty allocation failed — give up the window */
		fcntl(master, F_SETFL, O_NONBLOCK);
		term_master = master; /* let the escape parser send DSR replies */
		esc_state = 0;        /* reset parser for the fresh shell */
		usleep(100000);
		render(&win);

		int shell_alive = 1;
		while (shell_alive && running) {
			int changed = 0;
			char text[256];
			ssize_t n;
			while ((n = read(master, text, sizeof(text))) > 0) {
				for (ssize_t i = 0; i < n; i++)
					put_terminal_char(text[i]);
				changed = 1;
			}
			if (n == 0) {
				int status;
				if (waitpid(child, &status, WNOHANG) == child)
					shell_alive = 0;
			}

			struct b1gui_event event;
			int rc = b1gui_next_event(&win, &event, 20);
			if (rc < 0) {
				running = 0;
				break;
			}
			if (rc == 1 && event.type == B1GUI_EV_CLOSE) {
				running = 0;
				break;
			}
			if (rc == 1 && event.type == B1GUI_EV_MENU_ITEM) {
				/* Menu actions — for now just ignore (Copy/Paste are
				 * handled by Ctrl+C/V keyboard forwarding already). */
				continue;
			}
			if (rc == 1 && event.type == B1GUI_EV_KEY && event.nargs >= 2) {
				unsigned scan = event.args[0];
				int pressed = event.args[1] != 0;
				if (scan == 0x2a || scan == 0x36) {
					shift = pressed;
				} else if (pressed) {
					char c = key_char(scan, shift);
					if (c)
						write(master, &c, 1);
					else if (scan == 0x1c)
						write(master, "\n", 1);
					else if (scan == 0x0e)
						write(master, "\177", 1);
					else if (scan == 0x0f)
						write(master, "\t", 1);
					/* Extended (0xE0-prefixed) navigation keys ->
					 * ANSI escape sequences so line editing / TUIs
					 * (and shell history) work. */
					else if (scan == 0xE048)
						write(master, "\033[A", 3); /* up */
					else if (scan == 0xE050)
						write(master, "\033[B", 3); /* down */
					else if (scan == 0xE04D)
						write(master, "\033[C", 3); /* right */
					else if (scan == 0xE04B)
						write(master, "\033[D", 3); /* left */
					else if (scan == 0xE047)
						write(master, "\033[H", 3); /* home */
					else if (scan == 0xE04F)
						write(master, "\033[F", 3); /* end */
					else if (scan == 0xE053)
						write(master, "\033[3~", 4); /* delete */
				}
			}
			if (changed)
				render(&win);
		}
		close(master);
		/* Reap the shell if it died, and back off so a shell that exits
		 * instantly can't spin this loop. */
		int status;
		waitpid(child, &status, WNOHANG);
		if (running)
			usleep(400000);
	}
	b1gui_destroy(&win);
	return 0;
}
