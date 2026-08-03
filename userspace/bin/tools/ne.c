#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <tui.h>

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
static int editor_smoke_mode = 0;

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
	
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	
	char buf[512];
	int total = 0;
	int line = 0;
	
	while (fgets(buf, sizeof(buf), f)) {
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
	}
	fclose(f);
	
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

	FILE *f = fopen(ed.filename, "w");
	if (!f) return -1;

	size_t written = fwrite(save_buffer, 1, (size_t)pos, f);
	fclose(f);
	if (written != (size_t)pos) return -1;

	ed.dirty = 0;
	return 0;
}

static void editor_refresh_screen(void)
{
	int cols = TUI_COLS - 2;
	int rows = TUI_ROWS - 3;
	
	char title[64];
	snprintf(title, sizeof(title), " ne - %s%s ", ed.filename, ed.dirty ? " (Modified)" : "");
	tui_title_bar(0, title, 0, 7);
	
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
				
				char line_num[8];
				snprintf(line_num, sizeof(line_num), "%4d ", line_idx + 1);
				tui_set_color(8, 0);
				tui_write(line_num);
				tui_reset_color();
				
				tui_set_color(7, 0);
				tui_write_n(text + start, display_len);
				tui_reset_color();
				
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
				char spaces[256];
				int sp = cols;
				if (sp > 255) sp = 255;
				memset(spaces, ' ', sp);
				tui_write_n(spaces, sp);
				tui_reset_color();
			}
		} else {
			char spaces[256];
			int sp = cols + 5;
			if (sp > 255) sp = 255;
			memset(spaces, ' ', sp);
			tui_write_n(spaces, sp);
		}
	}
	
	char status[64];
	snprintf(status, sizeof(status), " Line %d/%d  Col %d ", 
			 ed.cursor_row + 1, ed.line_count, ed.cursor_col + 1);
	tui_status_bar(TUI_ROWS - 2, status, 7, 4);
	tui_write_at(TUI_ROWS - 1, 1, "^X/^Q Exit  ^S/^Y Save  ^G Help", TUI_COLS - 2, 0, 7);
	tui_cursor_goto(1 + ed.cursor_row - ed.top_line, 6 + ed.cursor_col - ed.left_col);
}

static void editor_insert_char(char c)
{
	char *line = ed.lines[ed.cursor_row];
	int len = strlen(line);
	
	if (len < MAX_LINE_LEN - 2) {
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
		for (int i = ed.cursor_col - 1; i < len; i++) {
			line[i] = line[i + 1];
		}
		ed.cursor_col--;
		ed.dirty = 1;
	} else if (ed.cursor_row > 0) {
		int prev_len = strlen(ed.lines[ed.cursor_row - 1]);
		if (prev_len + len < MAX_LINE_LEN - 1) {
			strcpy(ed.lines[ed.cursor_row - 1] + prev_len, line);
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

static void editor_delete_forward(void)
{
	char *line = ed.lines[ed.cursor_row];
	int len = strlen(line);

	if (ed.cursor_col < len) {
		for (int i = ed.cursor_col; i < len; i++) {
			line[i] = line[i + 1];
		}
		ed.dirty = 1;
	} else if (ed.cursor_row < ed.line_count - 1) {
		int next_len = strlen(ed.lines[ed.cursor_row + 1]);
		if (len + next_len < MAX_LINE_LEN - 1) {
			strcpy(line + len, ed.lines[ed.cursor_row + 1]);
			for (int i = ed.cursor_row + 1; i < ed.line_count - 1; i++) {
				strcpy(ed.lines[i], ed.lines[i + 1]);
			}
			ed.line_count--;
			ed.dirty = 1;
		}
	}
}

static void editor_newline(void)
{
	if (ed.line_count >= MAX_LINES - 1) return;
	
	for (int i = ed.line_count; i > ed.cursor_row; i--) {
		strcpy(ed.lines[i], ed.lines[i - 1]);
	}
	
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

static int editor_handle_key(int key)
{
	switch (key) {
	case KEY_CTRL_Q:
	case KEY_CTRL_X:
	case KEY_ESC:
		if (ed.dirty && !editor_smoke_mode) {
			tui_write_at(TUI_ROWS - 1, 1, "Save modified buffer? (y/n): ", TUI_COLS - 2, 0, 7);
			int c = tui_get_key();
			if (c == 'y' || c == 'Y') {
				editor_save();
			}
		}
		return 0;

	case KEY_CTRL_S:
	case KEY_CTRL_Y:
		if (editor_save() == 0) {
			tui_write_at(TUI_ROWS - 1, 1, "Saved successfully.", TUI_COLS - 2, 2, 0);
			if (!editor_smoke_mode) {
				tui_get_key();
			}
		} else {
			tui_write_at(TUI_ROWS - 1, 1, "Error saving file.", TUI_COLS - 2, 1, 0);
			if (!editor_smoke_mode) {
				tui_get_key();
			}
		}
		return 1;

	case KEY_CTRL_G:
		tui_clear_screen();
		tui_write_at(1, 1, "ne Editor Help (Ring 3)", TUI_COLS - 2, 7, 0);
		tui_write_at(3, 1, "Ctrl+X  - Exit editor", TUI_COLS - 2, 7, 0);
		tui_write_at(4, 1, "Ctrl+Q/Esc - Exit editor", TUI_COLS - 2, 7, 0);
		tui_write_at(5, 1, "Ctrl+S/Ctrl+Y - Save file", TUI_COLS - 2, 7, 0);
		tui_write_at(6, 1, "Ctrl+G  - This help", TUI_COLS - 2, 7, 0);
		tui_write_at(7, 1, "Arrows  - Navigate", TUI_COLS - 2, 7, 0);
		tui_write_at(8, 1, "Backspace/Delete - Delete char", TUI_COLS - 2, 7, 0);
		tui_write_at(9, 1, "Enter   - New line", TUI_COLS - 2, 7, 0);
		tui_write_at(14, 1, "Type text to insert characters.", TUI_COLS - 2, 7, 0);
		tui_write_at(TUI_ROWS - 1, 1, "Press any key to continue", TUI_COLS - 2, 0, 7);
		if (!editor_smoke_mode) {
			tui_get_key();
		}
		return 1;

	case KEY_LEFT:
		if (ed.cursor_col > 0) {
			ed.cursor_col--;
			if (ed.cursor_col < ed.left_col) {
				ed.left_col = ed.cursor_col;
			}
		}
		return 1;

	case KEY_RIGHT:
		if (ed.cursor_col < (int)strlen(ed.lines[ed.cursor_row])) {
			ed.cursor_col++;
			if (ed.cursor_col >= ed.left_col + TUI_COLS - 10) {
				ed.left_col = ed.cursor_col - (TUI_COLS - 10) + 1;
			}
		}
		return 1;

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
		return 1;

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
		return 1;

	case KEY_HOME:
		ed.cursor_col = 0;
		ed.left_col = 0;
		return 1;

	case KEY_END:
		ed.cursor_col = strlen(ed.lines[ed.cursor_row]);
		if (ed.cursor_col >= ed.left_col + TUI_COLS - 10) {
			ed.left_col = ed.cursor_col - (TUI_COLS - 10) + 1;
		}
		return 1;

	case KEY_PGUP:
		ed.cursor_row = 0;
		ed.top_line = 0;
		if (ed.cursor_col > (int)strlen(ed.lines[0])) {
			ed.cursor_col = strlen(ed.lines[0]);
		}
		return 1;

	case KEY_PGDN:
		ed.cursor_row = ed.line_count - 1;
		ed.top_line = ed.line_count > 0 ? ed.line_count - 1 : 0;
		if (ed.cursor_col > (int)strlen(ed.lines[ed.cursor_row])) {
			ed.cursor_col = strlen(ed.lines[ed.cursor_row]);
		}
		return 1;

	case KEY_BACKSP:
		editor_delete_char();
		return 1;

	case KEY_DEL:
		editor_delete_forward();
		return 1;

	case KEY_ENTER:
		editor_newline();
		return 1;

	case KEY_TAB:
		editor_insert_char(' ');
		editor_insert_char(' ');
		return 1;

	default:
		if (key >= 32 && key <= 126) {
			editor_insert_char((char)key);
		}
		return 1;
	}
}

static int editor_verify_contents(const char *path, const char *expected)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char buf[512];
	memset(buf, 0, sizeof(buf));
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	(void)n;
	return strcmp(buf, expected) == 0 ? 0 : -1;
}

static int editor_smoke_run(const char *path)
{
	const char *smoke_path = (path && path[0]) ? path : "/tmp/m16-editor-smoke.txt";
	editor_smoke_mode = 1;
	tui_screen_mute();
	if (tui_terminal_begin() != 0) {
		tui_screen_unmute();
		editor_smoke_mode = 0;
		return 1;
	}

	FILE *f = fopen(smoke_path, "w");
	if (f) fclose(f);

	if (editor_load(smoke_path) != 0) {
		tui_terminal_end();
		tui_screen_unmute();
		editor_smoke_mode = 0;
		return 1;
	}

	tui_clear_screen();
	tui_cursor_hide();
	editor_refresh_screen();

	const int keys[] = {
		'n', 'a', 'n', 'o', KEY_LEFT, KEY_DEL, KEY_BACKSP,
		KEY_ENTER, KEY_UP, KEY_DOWN, KEY_PGUP, KEY_PGDN, 'z'
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		editor_handle_key(keys[i]);
	}

	editor_handle_key(KEY_CTRL_S);
	if (editor_verify_contents(smoke_path, "na\nz\n") != 0) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		tui_screen_unmute();
		editor_smoke_mode = 0;
		return 1;
	}

	editor_handle_key(KEY_CTRL_G);
	editor_handle_key(KEY_CTRL_Q);

	tui_clear_screen();
	tui_cursor_show();
	tui_terminal_end();
	tui_screen_unmute();
	editor_smoke_mode = 0;
	printf("M16-SMOKE: ok editor-hotkeys\n");
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("Usage: ne <filename>\n");
		return 1;
	}

	if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "--smoke") == 0) {
		const char *smoke_path = (argc >= 3 && argv[2]) ? argv[2] : 0;
		return editor_smoke_run(smoke_path);
	}

	editor_load(argv[1]);
	
	if (tui_terminal_begin() != 0) {
		return 1;
	}

	tui_clear_screen();
	tui_cursor_hide();
	
	int running = 1;
	while (running) {
		editor_refresh_screen();
		if (!editor_handle_key(tui_get_key())) {
			running = 0;
		}
	}
	
	tui_clear_screen();
	tui_cursor_show();
	tui_terminal_end();
	printf("Editor exited.\n");
	return 0;
}
