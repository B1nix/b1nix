#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <b1nix/syscall.h>
#include <b1nix/dirent.h>
#include <tui.h>

/*
 * mc — Mini Commander
 * Two-panel TUI file manager inspired by Midnight Commander
 */

/* ── Globals ── */

#define MAX_FILES 128
#define PANEL_WIDTH 36

struct file_entry {
	char name[64];
	int  is_dir;
	int  is_exec;
	usize size;
};

struct panel {
	char  current_dir[256];
	struct file_entry files[MAX_FILES];
	int   file_count;
	int   selected;      /* Currently highlighted line */
	int   top_line;      /* Scroll offset */
};

static struct panel panels[2];
static int active_panel = 0;  /* 0 or 1 */
static char clipboard_path[256]; /* For copy/move */
static int clipboard_has_path = 0; /* Track if clipboard has content */

/* ── Helper functions ── */

static void resolve_path(const char *cwd, const char *rel, char *abs)
{
	if (rel[0] == '/') {
		strcpy(abs, rel);
		return;
	}
	strcpy(abs, cwd);
	int len = strlen(abs);
	if (abs[len - 1] != '/') {
		strcat(abs, "/");
	}
	strcat(abs, rel);
}

static int read_directory(struct panel *p)
{
	p->file_count = 0;
	p->selected = 0;
	p->top_line = 0;
	
	/* Use SYS_READDIR to get directory listing */
	struct dirent entries[MAX_FILES];
	usize count = syscall_dispatch(SYS_READDIR, (u64)(usize)p->current_dir, (u64)(usize)entries, MAX_FILES, 0, 0, 0);
	
	if (count == 0 || count == (u64)-1) {
		/* Fallback: at least show "." */
		strcpy(p->files[0].name, ".");
		p->files[0].is_dir = 1;
		p->files[0].is_exec = 0;
		p->files[0].size = 0;
		p->file_count = 1;
		return 0;
	}
	
	/* Add "." entry */
	strcpy(p->files[0].name, ".");
	p->files[0].is_dir = 1;
	p->files[0].is_exec = 0;
	p->files[0].size = 0;
	p->file_count = 1;
	
	/* Add ".." entry if not at root */
	if (strcmp(p->current_dir, "/") != 0) {
		strcpy(p->files[1].name, "..");
		p->files[1].is_dir = 1;
		p->files[1].is_exec = 0;
		p->files[1].size = 0;
		p->file_count = 2;
	}
	
	for (usize i = 0; i < count && p->file_count < MAX_FILES; i++) {
		/* Skip "." and ".." as we already added them */
		if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0)
			continue;
		
		strcpy(p->files[p->file_count].name, entries[i].name);
		p->files[p->file_count].is_dir = entries[i].is_dir;
		p->files[p->file_count].is_exec = entries[i].is_exec;
		p->files[p->file_count].size = entries[i].size;
		p->file_count++;
	}
	
	return 0;
}

/* ── Panel drawing ── */

static void draw_panel(int panel_idx, int left_col)
{
	struct panel *p = &panels[panel_idx];
	int fg, bg;
	
	/* Draw border */
	tui_draw_box(2, left_col, TUI_ROWS - 4, PANEL_WIDTH, 7, 0); /* White on black */
	
	/* Draw title */
	char title[64];
	snprintf(title, sizeof(title), " %s ", p->current_dir);
	tui_title_bar(2, title, 0, 7); /* Black on white */
	
	/* Draw file listing */
	int max_visible = TUI_ROWS - 6;
	for (int i = 0; i < max_visible && i + p->top_line < p->file_count; i++) {
		int file_idx = i + p->top_line;
		struct file_entry *fe = &p->files[file_idx];
		
		int row = 3 + i;
		
		if (file_idx == p->selected) {
			/* Highlighted */
			if (panel_idx == active_panel) {
				fg = 0;   /* Black */
				bg = 7;   /* White background */
			} else {
				fg = 7;   /* White */
				bg = 4;   /* Blue background */
			}
		} else if (fe->is_dir) {
			fg = 6;   /* Cyan for directories */
			bg = 0;
		} else if (fe->is_exec) {
			fg = 2;   /* Green for executables */
			bg = 0;
		} else {
			fg = 7;   /* White for normal */
			bg = 0;
		}
		
		/* Draw file name */
		char line_buf[PANEL_WIDTH - 1];
		int len = strlen(fe->name);
		if (len > PANEL_WIDTH - 4) len = PANEL_WIDTH - 4;
		memcpy(line_buf, fe->name, len);
		line_buf[len] = '\0';
		
		/* Add size if not a directory */
		if (!fe->is_dir && fe->size > 0) {
			char size_str[16];
			if (fe->size > 1024 * 1024) {
				snprintf(size_str, sizeof(size_str), " %dM", (int)(fe->size / (1024*1024)));
			} else if (fe->size > 1024) {
				snprintf(size_str, sizeof(size_str), " %dK", (int)(fe->size / 1024));
			} else {
				snprintf(size_str, sizeof(size_str), " %d", (int)fe->size);
			}
			int slen = strlen(size_str);
			if (len + slen < PANEL_WIDTH - 2) {
				memcpy(line_buf + len, size_str, slen);
				len += slen;
				line_buf[len] = '\0';
			}
		}
		
		if (fe->is_dir) {
			int l = strlen(line_buf);
			if (l < PANEL_WIDTH - 3) {
				line_buf[l] = '/';
				line_buf[l + 1] = '\0';
			}
		}
		
		tui_write_at(row, left_col + 1, line_buf, PANEL_WIDTH - 2, fg, bg);
	}
}

/* ── Main ── */

int mc_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	(void)clipboard_path;     /* Reserved for future F5/F6 operations */
	(void)clipboard_has_path; /* Reserved for future F5/F6 operations */
	
	/* Initialize panels */
	strcpy(panels[0].current_dir, "/");
	strcpy(panels[1].current_dir, "/");
	
	read_directory(&panels[0]);
	read_directory(&panels[1]);
	
	tui_clear_screen();
	tui_cursor_hide();
	
	int running = 1;
	
	while (running) {
		/* Draw title bar */
		tui_title_bar(0, " Mini Commander (b1nix) ", 0, 7);
		
		/* Draw function key bar */
		tui_write_at(TUI_ROWS - 1, 1, "F1-Help F2-Menu F3-View F4-Edit F5-Copy F6-Move F7-Mkdir F8-Delete F10-Quit", 
					TUI_COLS - 2, 0, 7);
		
		/* Draw panels */
		draw_panel(0, 1);
		draw_panel(1, 1 + PANEL_WIDTH + 1);
		
		/* Draw info between panels */
		int mid_col = 1 + PANEL_WIDTH + 1;
		tui_write_at(TUI_ROWS / 2 - 2, mid_col + 1, "Info", TUI_COLS - mid_col - 3, 7, 0);
		
		char info[64];
		struct panel *ap = &panels[active_panel];
		if (ap->selected < ap->file_count) {
			struct file_entry *fe = &ap->files[ap->selected];
			snprintf(info, sizeof(info), "File: %s", fe->name);
			tui_write_at(TUI_ROWS / 2, mid_col + 1, info, TUI_COLS - mid_col - 3, 7, 0);
			if (fe->size > 0) {
				snprintf(info, sizeof(info), "Size: %d bytes", (int)fe->size);
				tui_write_at(TUI_ROWS / 2 + 1, mid_col + 1, info, TUI_COLS - mid_col - 3, 7, 0);
			}
		}
		
		/* Read key */
		int key = tui_get_key();
		
		switch (key) {
		case KEY_TAB:
			/* Switch panel */
			active_panel = 1 - active_panel;
			break;
			
		case KEY_UP:
			if (panels[active_panel].selected > 0) {
				panels[active_panel].selected--;
				if (panels[active_panel].selected < panels[active_panel].top_line) {
					panels[active_panel].top_line = panels[active_panel].selected;
				}
			}
			break;
			
		case KEY_DOWN:
			if (panels[active_panel].selected < panels[active_panel].file_count - 1) {
				panels[active_panel].selected++;
				int max_visible = TUI_ROWS - 6;
				if (panels[active_panel].selected >= panels[active_panel].top_line + max_visible) {
					panels[active_panel].top_line = panels[active_panel].selected - max_visible + 1;
				}
			}
			break;
			
		case KEY_HOME:
			panels[active_panel].selected = 0;
			panels[active_panel].top_line = 0;
			break;
			
		case KEY_END:
			panels[active_panel].selected = panels[active_panel].file_count - 1;
			panels[active_panel].top_line = panels[active_panel].selected - (TUI_ROWS - 6) + 1;
			if (panels[active_panel].top_line < 0) panels[active_panel].top_line = 0;
			break;
			
		case KEY_ENTER: {
			struct panel *p = &panels[active_panel];
			if (p->selected < p->file_count && p->files[p->selected].is_dir) {
				const char *name = p->files[p->selected].name;
				if (strcmp(name, ".") == 0) {
					/* Same dir */
				} else if (strcmp(name, "..") == 0) {
					/* Go up */
					char *slash = strrchr(p->current_dir, '/');
					if (slash && slash != p->current_dir) {
						*slash = '\0';
					} else if (slash == p->current_dir) {
						p->current_dir[1] = '\0';
					}
				} else {
					/* Go into directory */
					char new_path[256];
					resolve_path(p->current_dir, name, new_path);
					strcpy(p->current_dir, new_path);
				}
				read_directory(p);
			}
			break;
		}
			
		case KEY_F1:
			tui_clear_screen();
			tui_write_at(1, 1, "Mini Commander Help", TUI_COLS - 2, 7, 0);
			tui_write_at(3, 1, "F1  - This help screen", TUI_COLS - 2, 7, 0);
			tui_write_at(4, 1, "F2  - Menu (not yet implemented)", TUI_COLS - 2, 7, 0);
			tui_write_at(5, 1, "F3  - View file content (cat)", TUI_COLS - 2, 7, 0);
			tui_write_at(6, 1, "F4  - Edit file with nano-like editor", TUI_COLS - 2, 7, 0);
			tui_write_at(7, 1, "F5  - Copy file/dir", TUI_COLS - 2, 7, 0);
			tui_write_at(8, 1, "F6  - Move file/dir", TUI_COLS - 2, 7, 0);
			tui_write_at(9, 1, "F7  - Create directory", TUI_COLS - 2, 7, 0);
			tui_write_at(10, 1, "F8  - Delete file/dir", TUI_COLS - 2, 7, 0);
			tui_write_at(11, 1, "F10 - Quit", TUI_COLS - 2, 7, 0);
			tui_write_at(13, 1, "TAB - Switch panel", TUI_COLS - 2, 7, 0);
			tui_write_at(14, 1, "Arrows - Navigate", TUI_COLS - 2, 7, 0);
			tui_write_at(15, 1, "ENTER - Open directory", TUI_COLS - 2, 7, 0);
			tui_write_at(TUI_ROWS - 1, 1, "Press any key to continue", TUI_COLS - 2, 0, 7);
			tui_get_key();
			break;
			
		case KEY_F10:
		case KEY_ESC:
		case KEY_CTRL_Q:
		case 'q':
		case 'Q':
			running = 0;
			break;
			
		default:
			break;
		}
	}
	
	tui_clear_screen();
	tui_cursor_show();
	printf("Mini Commander exited.\n");
	return 0;
}
