#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <b1nix/posix.h>
#include <b1nix/syscall.h>
#include <tui.h>

/* ── Editor state ── */

#define MAX_LINES 1024
#define MAX_LINE_LEN 256

struct editor {
	char filename[256];
	char lines[MAX_LINES][MAX_LINE_LEN];
	int  line_count;
	int  cursor_row;   /* 0-based current line */
	int  cursor_col;   /* 0-based current column */
	int  top_line;     /* Scroll offset: first visible line */
	int  left_col;     /* Scroll offset: first visible column */
	int  dirty;
};

static struct editor ed;
static char save_buffer[MAX_LINES * (MAX_LINE_LEN + 1)];

/* ── File I/O ── */

static int editor_load(const char *path)
{
	strcpy(ed.filename, path);
	ed.line_count = 1;
	ed.lines[0][0] = '\0';
	ed.cursor_row = 0;
	ed.cursor_col = 0;
	ed.top_line = 0;
	ed.left_col = 0;
	ed.dirty = 0;
	
	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)path, 0, 0, 0);
	if (fd == (u64)-1) return 0; /* New file */
	
	char buf[512];
	int total = 0;
	int line = 0;
	
	/* Read file content */
	while (1) {
		u64 n = syscall_dispatch(SYS_READ, fd, (u64)(usize)buf, 255, 0);
		if (n == 0 || n == (u64)-1) break;
		buf[n] = '\0';
		
		/* Split into lines */
		for (int i = 0; buf[i]; i++) {
			if (buf[i] == '\n') {
				ed.lines[line][total] = '\0';
				total = 0;
				line++;
				if (line >= MAX_LINES - 1) break;
				ed.lines[line][0] = '\0';
			} else {
				if (total < MAX_LINE_LEN - 2) {
					ed.lines[line][total++] = buf[i];
				}
			}
		}
		/* Continue the current line across buffer reads */
	}
	
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	
	if (line > 0 || total > 0) {
		ed.lines[line][total] = '\0';
		ed.line_count = line + 1;
	}
	
	return 0;
}

static int editor_save(void)
{
	if (ed.filename[0] == '\0') return -1;

	int pos = 0;
	
	for (int i = 0; i < ed.line_count && pos < (int)sizeof(save_buffer) - MAX_LINE_LEN - 2; i++) {
		int len = strlen(ed.lines[i]);
		if (len > 0) {
			memcpy(save_buffer + pos, ed.lines[i], len);
			pos += len;
		}
		save_buffer[pos++] = '\n';
	}
	save_buffer[pos] = '\0';

	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)ed.filename,
	                          B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC, 0, 0);
	if (fd == (u64)-1) {
		return -1;
	}

	u64 written = syscall_dispatch(SYS_WRITE, (u64)fd, (u64)(usize)save_buffer, (u64)pos, 0);
	syscall_dispatch(SYS_CLOSE, fd, 0, 0, 0);
	if (written == (u64)-1 || (usize)written != (usize)pos) {
		return -1;
	}
	
	ed.dirty = 0;
	return 0;
}

/* ── Editor display ── */

static void editor_refresh_screen(void)
{
	int cols = TUI_COLS - 2;
	int rows = TUI_ROWS - 3;  /* -1 for title, -1 for status, -1 for function bar */
	
	/* Draw title */
	char title[64];
	snprintf(title, sizeof(title), " ne - %s%s ", ed.filename, ed.dirty ? " (Modified)" : "");
	tui_title_bar(0, title, 0, 7);
	
	/* Draw editor content */
	for (int i = 0; i < rows; i++) {
		int line_idx = i + ed.top_line;
		
		tui_cursor_goto(1 + i, 1);
		
		if (line_idx < ed.line_count) {
			const char *text = ed.lines[line_idx];
			int len = strlen(text);
			int start = ed.left_col;
			
			if (start < len) {
				int display_len = len - start;
				if (display_len > cols) display_len = cols;
				
				/* Show line numbers in grey on leftmost col */
				char line_num[8];
				snprintf(line_num, sizeof(line_num), "%4d ", line_idx + 1);
				tui_set_color(8, 0);  /* Bright black (grey) */
				tui_write(line_num);
				tui_reset_color();
				
				/* Determine line color */
				int fg = 7;  /* White */
				tui_set_color(fg, 0);
				tui_write_n(text + start, display_len);
				tui_reset_color();
				
				/* Clear rest of line */
				if (display_len < cols - 6) {
					char spaces[256];
					int sp = cols - 6 - display_len;
					if (sp > 255) sp = 255;
					memset(spaces, ' ', sp);
					tui_write_n(spaces, sp);
				}
			} else {
				char line_num[8];
				snprintf(line_num, sizeof(line_num), "%4d ", line_idx + 1);
				tui_set_color(8, 0);
				tui_write(line_num);
				tui_reset_color();
				tui_set_color(7, 0);
				/* Clear line */
				char spaces[256];
				int sp = cols;
				if (sp > 255) sp = 255;
				memset(spaces, ' ', sp);
				tui_write_n(spaces, sp);
				tui_reset_color();
			}
		} else {
			/* Empty line */
			char spaces[256];
			int sp = cols + 5;
			if (sp > 255) sp = 255;
			memset(spaces, ' ', sp);
			tui_write_n(spaces, sp);
		}
	}
	
	/* Draw cursor position */
	char status[64];
	snprintf(status, sizeof(status), " Line %d/%d  Col %d ", 
			 ed.cursor_row + 1, ed.line_count, ed.cursor_col + 1);
	tui_status_bar(TUI_ROWS - 2, status, 7, 4);  /* White on blue */
	
	/* Draw function keys */
	tui_write_at(TUI_ROWS - 1, 1, "^X/^Q Exit  ^S/^Y Save  ^G Help", TUI_COLS - 2, 0, 7);
	
	/* Position cursor */
	tui_cursor_goto(1 + ed.cursor_row - ed.top_line, 6 + ed.cursor_col - ed.left_col);
}

/* ── Editor operations ── */

static void editor_insert_char(char c)
{
	char *line = ed.lines[ed.cursor_row];
	int len = strlen(line);
	
	if (len < MAX_LINE_LEN - 2) {
		/* Shift characters right */
		for (int i = len; i >= ed.cursor_col; i--) {
			line[i + 1] = line[i];
		}
		line[ed.cursor_col] = c;
		ed.cursor_col++;
		ed.dirty = 1;
	}
}

static void editor_delete_char(void)
{
	char *line = ed.lines[ed.cursor_row];
	int len = strlen(line);
	
	if (ed.cursor_col > 0) {
		/* Delete character before cursor */
		for (int i = ed.cursor_col - 1; i < len; i++) {
			line[i] = line[i + 1];
		}
		ed.cursor_col--;
		ed.dirty = 1;
	} else if (ed.cursor_row > 0) {
		/* Join with previous line */
		int prev_len = strlen(ed.lines[ed.cursor_row - 1]);
		if (prev_len + len < MAX_LINE_LEN - 1) {
			strcpy(ed.lines[ed.cursor_row - 1] + prev_len, line);
			/* Shift lines up */
			for (int i = ed.cursor_row; i < ed.line_count - 1; i++) {
				strcpy(ed.lines[i], ed.lines[i + 1]);
			}
			ed.line_count--;
			ed.cursor_row--;
			ed.cursor_col = prev_len;
			ed.dirty = 1;
		}
	}
}

static void editor_newline(void)
{
	if (ed.line_count >= MAX_LINES - 1) return;
	
	/* Shift lines down */
	for (int i = ed.line_count; i > ed.cursor_row; i--) {
		strcpy(ed.lines[i], ed.lines[i - 1]);
	}
	
	/* Split line */
	char *line = ed.lines[ed.cursor_row];
	int after_newline = strlen(line) - ed.cursor_col;
	
	if (after_newline > 0) {
		memcpy(ed.lines[ed.cursor_row + 1], line + ed.cursor_col, after_newline);
		ed.lines[ed.cursor_row + 1][after_newline] = '\0';
		line[ed.cursor_col] = '\0';
	} else {
		ed.lines[ed.cursor_row + 1][0] = '\0';
	}
	
	ed.line_count++;
	ed.cursor_row++;
	ed.cursor_col = 0;
	ed.dirty = 1;
}

/* ── Main editor loop ── */

int editor_main(int argc, const char **argv)
{
	if (argc < 2) {
		printf("Usage: ne <filename>\n");
		return 1;
	}
	
	editor_load(argv[1]);
	
	tui_clear_screen();
	tui_cursor_hide();
	
	int running = 1;
	
	while (running) {
		editor_refresh_screen();
		
		int key = tui_get_key();
		
		switch (key) {
		case KEY_CTRL_Q:
		case KEY_CTRL_X:  /* Ctrl+X — Exit */
		case KEY_ESC:
			if (ed.dirty) {
				tui_write_at(TUI_ROWS - 1, 1, "Save modified buffer? (y/n): ", TUI_COLS - 2, 0, 7);
				int c = tui_get_key();
				if (c == 'y' || c == 'Y') {
					editor_save();
				}
			}
			running = 0;
			break;
			
		case KEY_CTRL_S:
		case KEY_CTRL_Y:  /* Ctrl+Y — Save, friendlier on keyboards where Ctrl+S is awkward */
			if (editor_save() == 0) {
				tui_write_at(TUI_ROWS - 1, 1, "Saved successfully.", TUI_COLS - 2, 2, 0);
				tui_get_key(); /* Wait for keypress */
			} else {
				tui_write_at(TUI_ROWS - 1, 1, "Error saving file.", TUI_COLS - 2, 1, 0);
				tui_get_key();
			}
			break;
			
		case KEY_CTRL_G:  /* Ctrl+G — Help */
			tui_clear_screen();
			tui_write_at(1, 1, "ne Editor Help", TUI_COLS - 2, 7, 0);
			tui_write_at(3, 1, "Ctrl+X  - Exit editor", TUI_COLS - 2, 7, 0);
			tui_write_at(4, 1, "Ctrl+Q/Esc - Exit editor", TUI_COLS - 2, 7, 0);
			tui_write_at(5, 1, "Ctrl+S/Ctrl+Y - Save file", TUI_COLS - 2, 7, 0);
			tui_write_at(6, 1, "Ctrl+G  - This help", TUI_COLS - 2, 7, 0);
			tui_write_at(7, 1, "Arrows  - Navigate", TUI_COLS - 2, 7, 0);
			tui_write_at(8, 1, "Backspace - Delete char", TUI_COLS - 2, 7, 0);
			tui_write_at(9, 1, "Enter   - New line", TUI_COLS - 2, 7, 0);
			tui_write_at(10, 1, "Home    - Line start", TUI_COLS - 2, 7, 0);
			tui_write_at(11, 1, "End     - Line end", TUI_COLS - 2, 7, 0);
			tui_write_at(13, 1, "Type text to insert characters.", TUI_COLS - 2, 7, 0);
			tui_write_at(TUI_ROWS - 1, 1, "Press any key to continue", TUI_COLS - 2, 0, 7);
			tui_get_key();
			break;
			
		case KEY_LEFT:
			if (ed.cursor_col > 0) {
				ed.cursor_col--;
				if (ed.cursor_col < ed.left_col) {
					ed.left_col = ed.cursor_col;
				}
			}
			break;
			
		case KEY_RIGHT:
			if (ed.cursor_col < (int)strlen(ed.lines[ed.cursor_row])) {
				ed.cursor_col++;
				if (ed.cursor_col >= ed.left_col + TUI_COLS - 10) {
					ed.left_col = ed.cursor_col - (TUI_COLS - 10) + 1;
				}
			}
			break;
			
		case KEY_UP:
			if (ed.cursor_row > 0) {
				ed.cursor_row--;
				int line_len = strlen(ed.lines[ed.cursor_row]);
				if (ed.cursor_col > line_len) {
					ed.cursor_col = line_len;
				}
				if (ed.cursor_row < ed.top_line) {
					ed.top_line = ed.cursor_row;
				}
			}
			break;
			
		case KEY_DOWN:
			if (ed.cursor_row < ed.line_count - 1) {
				ed.cursor_row++;
				int line_len = strlen(ed.lines[ed.cursor_row]);
				if (ed.cursor_col > line_len) {
					ed.cursor_col = line_len;
				}
				int max_rows = TUI_ROWS - 3;
				if (ed.cursor_row >= ed.top_line + max_rows) {
					ed.top_line = ed.cursor_row - max_rows + 1;
				}
			}
			break;
			
		case KEY_HOME:
			ed.cursor_col = 0;
			ed.left_col = 0;
			break;
			
		case KEY_END:
			ed.cursor_col = strlen(ed.lines[ed.cursor_row]);
			break;
			
		case KEY_BACKSP:
			editor_delete_char();
			break;
			
		case KEY_ENTER:
			editor_newline();
			break;
			
		case KEY_TAB:
			/* Insert 2 spaces */
			editor_insert_char(' ');
			editor_insert_char(' ');
			break;
			
		default:
			/* Regular character */
			if (key >= 32 && key <= 126) {
				editor_insert_char((char)key);
			}
			break;
		}
	}
	
	/* Cleanup */
	tui_clear_screen();
	tui_cursor_show();
	printf("Editor exited.\n");
	return 0;
}
