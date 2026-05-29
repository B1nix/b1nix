#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <b1nix/errno.h>
#include <b1nix/posix.h>
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
static int mc_smoke_mode = 0;

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

/* Read a line of input from user into buf (up to max_len). Returns length. */
static int read_input(char *buf, int max_len)
{
	int pos = 0;
	while (1) {
		int key = tui_get_key();
		if (key == KEY_ENTER || key == '\n') { buf[pos] = '\0'; return pos; }
		if (key == KEY_ESC) { buf[0] = '\0'; return -1; }
		if (key == KEY_BACKSP || key == 0x7F) {
			if (pos > 0) { pos--; tui_write("\b \b"); }
		} else if (key >= 32 && key <= 126 && pos < max_len - 1) {
			buf[pos++] = (char)key;
			char tmp[2] = { (char)key, '\0' };
			tui_write(tmp);
		}
	}
}

/* ── File Operation Helpers (F5-F8) ── */

static int copy_file_op(const char *src_path, const char *dst_path)
{
	u64 src_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)src_path,
	                 B1NIX_O_RDONLY, 0, 0, 0, 0);
	if ((isize)src_fd < 0) return -1;
	u64 dst_fd = syscall_dispatch(SYS_OPEN, (u64)(usize)dst_path,
	                 B1NIX_O_WRONLY | B1NIX_O_CREAT | B1NIX_O_TRUNC,
	                 0666, 0, 0, 0);
	if ((isize)dst_fd < 0) { syscall_dispatch(SYS_CLOSE, src_fd, 0,0,0,0,0); return -1; }
	char buf[512];
	int ret = 0;
	while (1) {
		u64 n = syscall_dispatch(SYS_READ, src_fd, (u64)(usize)buf, sizeof(buf), 0, 0, 0);
		if (n == 0 || (isize)n < 0) break;
		u64 w = syscall_dispatch(SYS_WRITE, dst_fd, (u64)(usize)buf, n, 0, 0, 0);
		if ((isize)w < 0 || w != n) { ret = -1; break; }
	}
	syscall_dispatch(SYS_CLOSE, src_fd, 0,0,0,0,0);
	syscall_dispatch(SYS_CLOSE, dst_fd, 0,0,0,0,0);
	return ret;
}

static int copy_dir_op(const char *src_dir, const char *dst_dir)
{
	u64 mkret = syscall_dispatch(SYS_MKDIR, (u64)(usize)dst_dir, 0755, 0,0,0,0);
	if ((isize)mkret < 0 && (isize)mkret != -EEXIST) return -1;
	u64 fd = syscall_dispatch(SYS_OPEN, (u64)(usize)src_dir, B1NIX_O_RDONLY, 0,0,0,0);
	if ((isize)fd < 0) return -1;
	struct dirent *entries = malloc(64 * sizeof(struct dirent));
	if (!entries) {
		syscall_dispatch(SYS_CLOSE, fd, 0,0,0,0,0);
		return -1;
	}
	u64 count = syscall_dispatch(SYS_GETDENTS, fd, (u64)(usize)entries, 64, 0,0,0);
	syscall_dispatch(SYS_CLOSE, fd, 0,0,0,0,0);
	if (count == 0 || (isize)count < 0) {
		free(entries);
		return 0;
	}
	usize num = count;
	for (usize i = 0; i < num; i++) {
		const char *name = entries[i].name;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
		char child_src[256], child_dst[256];
		snprintf(child_src, sizeof(child_src), "%s/%s", src_dir, name);
		snprintf(child_dst, sizeof(child_dst), "%s/%s", dst_dir, name);
		if (entries[i].is_dir) {
			if (copy_dir_op(child_src, child_dst) != 0) {
				free(entries);
				return -1;
			}
		} else if (copy_file_op(child_src, child_dst) != 0) {
			free(entries);
			return -1;
		}
	}
	free(entries);
	return 0;
}

/* File-manager delete/rmdir ops — scaffolding for a delete key binding not yet
 * wired into the panel input handler. Kept (marked unused) for future use. */
__attribute__((unused)) static int delete_file_op(const char *path) {
	isize ret = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)path, 0,0,0,0,0);
	return (ret < 0 && ret != -ENOENT) ? -1 : 0;
}

__attribute__((unused)) static int remove_dir_op(const char *path) {
	isize ret = (isize)syscall_dispatch(SYS_RMDIR, (u64)(usize)path, 0,0,0,0,0);
	return (ret < 0 && ret != -ENOENT) ? -1 : 0;
}

static int read_directory(struct panel *p)
{
	p->file_count = 0;
	p->selected = 0;
	p->top_line = 0;
	
	/* Keep the directory buffer off the 16 KiB kernel stack. */
	struct dirent *entries = malloc(MAX_FILES * sizeof(struct dirent));
	if (!entries) {
		strcpy(p->files[0].name, ".");
		p->files[0].is_dir = 1;
		p->files[0].is_exec = 0;
		p->files[0].size = 0;
		p->file_count = 1;
		return 0;
	}
	usize count = syscall_dispatch(SYS_READDIR, (u64)(usize)p->current_dir, (u64)(usize)entries, MAX_FILES, 0, 0, 0);
	
	if (count == 0 || count == (u64)-1) {
		/* Fallback: at least show "." */
		strcpy(p->files[0].name, ".");
		p->files[0].is_dir = 1;
		p->files[0].is_exec = 0;
		p->files[0].size = 0;
		p->file_count = 1;
		free(entries);
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
	
	free(entries);
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

static void mc_render_screen(void)
{
	/* Draw title bar */
	tui_title_bar(0, " Mini Commander (b1nix) ", 0, 7);

	/* Draw function key bar */
	tui_write_at(TUI_ROWS - 1, 1, "F1-Help F5-Copy F6-Move F7-MkDir F8-Delete F10-Quit",
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
}

static int mc_handle_key(int key)
{
	switch (key) {
	case KEY_TAB:
		active_panel = 1 - active_panel;
		return 1;

	case KEY_UP:
		if (panels[active_panel].selected > 0) {
			panels[active_panel].selected--;
			if (panels[active_panel].selected < panels[active_panel].top_line) {
				panels[active_panel].top_line = panels[active_panel].selected;
			}
		}
		return 1;

	case KEY_DOWN:
		if (panels[active_panel].selected < panels[active_panel].file_count - 1) {
			panels[active_panel].selected++;
			int max_visible = TUI_ROWS - 6;
			if (panels[active_panel].selected >= panels[active_panel].top_line + max_visible) {
				panels[active_panel].top_line = panels[active_panel].selected - max_visible + 1;
			}
		}
		return 1;

	case KEY_HOME:
		panels[active_panel].selected = 0;
		panels[active_panel].top_line = 0;
		return 1;

	case KEY_END:
		panels[active_panel].selected = panels[active_panel].file_count - 1;
		panels[active_panel].top_line = panels[active_panel].selected - (TUI_ROWS - 6) + 1;
		if (panels[active_panel].top_line < 0) panels[active_panel].top_line = 0;
		return 1;

	case KEY_PGUP:
		panels[active_panel].selected = 0;
		panels[active_panel].top_line = 0;
		return 1;

	case KEY_PGDN:
		panels[active_panel].selected = panels[active_panel].file_count - 1;
		panels[active_panel].top_line = panels[active_panel].selected - (TUI_ROWS - 6) + 1;
		if (panels[active_panel].top_line < 0) panels[active_panel].top_line = 0;
		return 1;

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
		return 1;
	}

	case KEY_F1:
		tui_clear_screen();
		tui_write_at(1, 1, "Mini Commander Help", TUI_COLS - 2, 7, 0);
		tui_write_at(3, 1, "F1  - This help screen", TUI_COLS - 2, 7, 0);
		tui_write_at(4, 1, "F2  - Context menu (deferred)", TUI_COLS - 2, 7, 0);
		tui_write_at(5, 1, "F3  - View file content (deferred)", TUI_COLS - 2, 7, 0);
		tui_write_at(6, 1, "F4  - Edit file (deferred)", TUI_COLS - 2, 7, 0);
		tui_write_at(7, 1, "F5  - Copy file/dir to other panel", TUI_COLS - 2, 7, 0);
		tui_write_at(8, 1, "F6  - Move file/dir to other panel", TUI_COLS - 2, 7, 0);
		tui_write_at(9, 1, "F7  - Create directory", TUI_COLS - 2, 7, 0);
		tui_write_at(10, 1, "F8  - Delete file/dir", TUI_COLS - 2, 7, 0);
		tui_write_at(11, 1, "F10 - Quit", TUI_COLS - 2, 7, 0);
		tui_write_at(13, 1, "TAB - Switch panel", TUI_COLS - 2, 7, 0);
		tui_write_at(14, 1, "Arrows - Navigate", TUI_COLS - 2, 7, 0);
		tui_write_at(15, 1, "ENTER - Open directory", TUI_COLS - 2, 7, 0);
		tui_write_at(TUI_ROWS - 1, 1, "Press any key to continue", TUI_COLS - 2, 0, 7);
		if (!mc_smoke_mode) tui_get_key();
		return 1;

	case KEY_F2:
		tui_write_at(TUI_ROWS - 2, 1, "F2 menu is deferred for M16", TUI_COLS - 2, 0, 7);
		return 1;

	case KEY_F3:
	case KEY_F4:
		tui_write_at(TUI_ROWS - 2, 1, "F3/F4 are deferred for M16", TUI_COLS - 2, 0, 7);
		return 1;

	case KEY_F5: {
		struct panel *src_p = &panels[active_panel];
		struct panel *dst_p = &panels[1 - active_panel];
		if (src_p->selected >= src_p->file_count) return 1;
		const char *name = src_p->files[src_p->selected].name;
		char src_path[256], dst_path[256];
		resolve_path(src_p->current_dir, name, src_path);
		resolve_path(dst_p->current_dir, name, dst_path);
		int ret;
		if (src_p->files[src_p->selected].is_dir) ret = copy_dir_op(src_path, dst_path);
		else ret = copy_file_op(src_path, dst_path);
		tui_write_at(TUI_ROWS - 2, 1, ret == 0 ? "Copy successful." : "Copy failed.",
			TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
		if (!mc_smoke_mode) tui_get_key();
		read_directory(&panels[0]); read_directory(&panels[1]);
		return 1;
	}

	case KEY_F6: {
		struct panel *src_p = &panels[active_panel];
		struct panel *dst_p = &panels[1 - active_panel];
		if (src_p->selected >= src_p->file_count) return 1;
		const char *name = src_p->files[src_p->selected].name;
		char src_path[256], dst_path[256];
		resolve_path(src_p->current_dir, name, src_path);
		resolve_path(dst_p->current_dir, name, dst_path);
		isize ret = (isize)syscall_dispatch(SYS_RENAME, (u64)(usize)src_path, (u64)(usize)dst_path, 0,0,0,0);
		if (ret < 0) {
			int copy_ok;
			if (src_p->files[src_p->selected].is_dir) copy_ok = copy_dir_op(src_path, dst_path);
			else copy_ok = copy_file_op(src_path, dst_path);
			if (copy_ok == 0) {
				if (src_p->files[src_p->selected].is_dir)
					ret = (isize)syscall_dispatch(SYS_RMDIR, (u64)(usize)src_path, 0,0,0,0,0);
				else
					ret = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)src_path, 0,0,0,0,0);
			} else ret = -1;
		}
		tui_write_at(TUI_ROWS - 2, 1, ret == 0 ? "Move successful." : "Move failed.",
			TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
		if (!mc_smoke_mode) tui_get_key();
		read_directory(&panels[0]); read_directory(&panels[1]);
		return 1;
	}

	case KEY_F7: {
		char dirname[128];
		tui_write_at(TUI_ROWS - 2, 1, "Create directory: ", TUI_COLS - 2, 7, 0);
		if (read_input(dirname, sizeof(dirname)) > 0) {
			char full_path[256];
			resolve_path(panels[active_panel].current_dir, dirname, full_path);
			isize ret = (isize)syscall_dispatch(SYS_MKDIR, (u64)(usize)full_path, 0755, 0,0,0,0);
			tui_write_at(TUI_ROWS - 2, 1,
				ret == 0 ? "Directory created." : ret == -EEXIST ? "Already exists." : "Failed.",
				TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
			if (!mc_smoke_mode) tui_get_key();
			read_directory(&panels[0]); read_directory(&panels[1]);
		}
		return 1;
	}

	case KEY_F8: {
		struct panel *p = &panels[active_panel];
		if (p->selected >= p->file_count) return 1;
		char full_path[256];
		resolve_path(p->current_dir, p->files[p->selected].name, full_path);
		isize ret;
		if (p->files[p->selected].is_dir)
			ret = (isize)syscall_dispatch(SYS_RMDIR, (u64)(usize)full_path, 0,0,0,0,0);
		else
			ret = (isize)syscall_dispatch(SYS_UNLINK, (u64)(usize)full_path, 0,0,0,0,0);
		tui_write_at(TUI_ROWS - 2, 1,
			ret == 0 ? "Deleted." : ret == -ENOTEMPTY ? "Dir not empty." : "Delete failed.",
			TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
		if (!mc_smoke_mode) tui_get_key();
		read_directory(&panels[0]); read_directory(&panels[1]);
		return 1;
	}

	case KEY_F10:
	case KEY_ESC:
	case KEY_CTRL_Q:
	case 'q':
	case 'Q':
		return 0;

	default:
		return 1;
	}
}

static int mc_smoke_run(void)
{
	const char *smoke_dir = "/tmp/m16-mc-smoke";
	const char *smoke_subdir = "/tmp/m16-mc-smoke/subdir";
	u64 mkdir_rc = syscall_dispatch(SYS_MKDIR, (u64)(usize)smoke_dir, 0755, 0, 0, 0, 0);
	if ((isize)mkdir_rc < 0 && (isize)mkdir_rc != -EEXIST) {
		return 1;
	}
	mkdir_rc = syscall_dispatch(SYS_MKDIR, (u64)(usize)smoke_subdir, 0755, 0, 0, 0, 0);
	if ((isize)mkdir_rc < 0 && (isize)mkdir_rc != -EEXIST) {
		return 1;
	}

	mc_smoke_mode = 1;
	if (tui_terminal_begin() != 0) {
		mc_smoke_mode = 0;
		return 1;
	}

	active_panel = 0;
	strcpy(panels[0].current_dir, smoke_dir);
	strcpy(panels[1].current_dir, "/");
	read_directory(&panels[0]);
	read_directory(&panels[1]);

	if (panels[0].file_count < 1 || panels[1].file_count < 1) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}

	tui_clear_screen();
	tui_cursor_hide();
	mc_render_screen();

	int initial_active = active_panel;
	int initial_selected = panels[active_panel].selected;
	if (mc_handle_key(KEY_F1) != 1) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}
	mc_render_screen();

	int subdir_idx = -1;
	for (int i = 0; i < panels[0].file_count; i++) {
		if (strcmp(panels[0].files[i].name, "subdir") == 0) {
			subdir_idx = i;
			break;
		}
	}
	if (subdir_idx < 0) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}

	active_panel = 0;
	panels[0].selected = subdir_idx;
	panels[0].top_line = 0;
	if (mc_handle_key(KEY_ENTER) != 1 ||
	    strcmp(panels[0].current_dir, smoke_subdir) != 0) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}

	mc_handle_key(KEY_END);
	if (panels[active_panel].file_count > 0 &&
	    panels[active_panel].selected != panels[active_panel].file_count - 1) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}
	mc_handle_key(KEY_UP);
	if (panels[active_panel].file_count > 1 &&
	    panels[active_panel].selected != panels[active_panel].file_count - 2) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}
	mc_handle_key(KEY_HOME);
	if (panels[active_panel].selected != 0) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}
	mc_handle_key(KEY_TAB);
	if (active_panel == initial_active) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}
	mc_handle_key(KEY_TAB);
	if (active_panel != initial_active) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}

	if (mc_handle_key(KEY_F10) != 0) {
		tui_clear_screen();
		tui_cursor_show();
		tui_terminal_end();
		mc_smoke_mode = 0;
		return 1;
	}

	(void)initial_selected;
	mc_render_screen();
	tui_clear_screen();
	tui_cursor_show();
	tui_terminal_end();
	mc_smoke_mode = 0;
	printf("M16-SMOKE: ok file-explorer-hotkeys\n");
	return 0;
}

/* ── Main ── */

int mc_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	(void)clipboard_path;     /* Reserved for future F5/F6 operations */
	(void)clipboard_has_path; /* Reserved for future F5/F6 operations */

	if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "--smoke") == 0) {
		return mc_smoke_run();
	}
	
	/* Initialize panels */
	active_panel = 0;
	strcpy(panels[0].current_dir, "/");
	strcpy(panels[1].current_dir, "/");
	
	read_directory(&panels[0]);
	read_directory(&panels[1]);
	
	if (tui_terminal_begin() != 0) {
		return 1;
	}

	tui_clear_screen();
	tui_cursor_hide();
	
	int running = 1;
	
	while (running) {
		mc_render_screen();
		if (!mc_handle_key(tui_get_key())) {
			running = 0;
		}
	}
	
	tui_clear_screen();
	tui_cursor_show();
	tui_terminal_end();
	printf("Mini Commander exited.\n");
	return 0;
}
