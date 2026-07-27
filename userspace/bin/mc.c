#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tui.h>

#define MAX_FILES 128
#define PANEL_WIDTH 36

struct file_entry {
	char name[64];
	int  is_dir;
	int  is_exec;
	size_t size;
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
static int mc_smoke_mode = 0;

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

static int copy_file_op(const char *src_path, const char *dst_path)
{
	int src_fd = open(src_path, O_RDONLY);
	if (src_fd < 0) return -1;
	int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (dst_fd < 0) { close(src_fd); return -1; }
	char buf[512];
	int ret = 0;
	while (1) {
		ssize_t n = read(src_fd, buf, sizeof(buf));
		if (n <= 0) break;
		ssize_t w = write(dst_fd, buf, (size_t)n);
		if (w < 0 || w != n) { ret = -1; break; }
	}
	close(src_fd);
	close(dst_fd);
	return ret;
}

static int copy_dir_op(const char *src_dir, const char *dst_dir)
{
	if (mkdir(dst_dir, 0755) < 0 && errno != EEXIST) return -1;
	DIR *dir = opendir(src_dir);
	if (!dir) return -1;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
		char child_src[256], child_dst[256];
		snprintf(child_src, sizeof(child_src), "%s/%s", src_dir, de->d_name);
		snprintf(child_dst, sizeof(child_dst), "%s/%s", dst_dir, de->d_name);
		struct stat st;
		if (stat(child_src, &st) == 0 && S_ISDIR(st.st_mode)) {
			if (copy_dir_op(child_src, child_dst) != 0) { closedir(dir); return -1; }
		} else {
			if (copy_file_op(child_src, child_dst) != 0) { closedir(dir); return -1; }
		}
	}
	closedir(dir);
	return 0;
}

static int read_directory(struct panel *p)
{
	p->file_count = 0;
	p->selected = 0;
	p->top_line = 0;
	
	strcpy(p->files[0].name, ".");
	p->files[0].is_dir = 1;
	p->files[0].is_exec = 0;
	p->files[0].size = 0;
	p->file_count = 1;

	if (strcmp(p->current_dir, "/") != 0) {
		strcpy(p->files[1].name, "..");
		p->files[1].is_dir = 1;
		p->files[1].is_exec = 0;
		p->files[1].size = 0;
		p->file_count = 2;
	}

	DIR *dir = opendir(p->current_dir);
	if (!dir) return 0;

	struct dirent *de;
	while ((de = readdir(dir)) != NULL && p->file_count < MAX_FILES) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;
		
		strcpy(p->files[p->file_count].name, de->d_name);
		char full_path[256];
		snprintf(full_path, sizeof(full_path), "%s/%s", p->current_dir, de->d_name);
		struct stat st;
		if (stat(full_path, &st) == 0) {
			p->files[p->file_count].is_dir = S_ISDIR(st.st_mode);
			p->files[p->file_count].is_exec = (st.st_mode & S_IXUSR) ? 1 : 0;
			p->files[p->file_count].size = (size_t)st.st_size;
		} else {
			p->files[p->file_count].is_dir = 0;
			p->files[p->file_count].is_exec = 0;
			p->files[p->file_count].size = 0;
		}
		p->file_count++;
	}

	closedir(dir);
	return 0;
}

static void draw_panel(int panel_idx, int left_col)
{
	struct panel *p = &panels[panel_idx];
	int fg, bg;
	
	tui_draw_box(2, left_col, TUI_ROWS - 4, PANEL_WIDTH, 7, 0);
	
	char title[64];
	snprintf(title, sizeof(title), " %s ", p->current_dir);
	tui_title_bar(2, title, 0, 7);
	
	int max_visible = TUI_ROWS - 6;
	for (int i = 0; i < max_visible && i + p->top_line < p->file_count; i++) {
		int file_idx = i + p->top_line;
		struct file_entry *fe = &p->files[file_idx];
		
		int row = 3 + i;
		
		if (file_idx == p->selected) {
			if (panel_idx == active_panel) {
				fg = 0; bg = 7;
			} else {
				fg = 7; bg = 4;
			}
		} else if (fe->is_dir) {
			fg = 6; bg = 0;
		} else if (fe->is_exec) {
			fg = 2; bg = 0;
		} else {
			fg = 7; bg = 0;
		}
		
		char line_buf[PANEL_WIDTH - 1];
		int len = strlen(fe->name);
		if (len > PANEL_WIDTH - 4) len = PANEL_WIDTH - 4;
		memcpy(line_buf, fe->name, len);
		line_buf[len] = '\0';
		
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
	tui_title_bar(0, " Mini Commander (b1nix Ring 3) ", 0, 7);
	tui_write_at(TUI_ROWS - 1, 1, "F1-Help F5-Copy F6-Move F7-MkDir F8-Delete F10-Quit", TUI_COLS - 2, 0, 7);

	draw_panel(0, 1);
	draw_panel(1, 1 + PANEL_WIDTH + 1);

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

	case KEY_ENTER: {
		struct panel *p = &panels[active_panel];
		if (p->selected < p->file_count && p->files[p->selected].is_dir) {
			const char *name = p->files[p->selected].name;
			if (strcmp(name, ".") == 0) {
			} else if (strcmp(name, "..") == 0) {
				char *slash = strrchr(p->current_dir, '/');
				if (slash && slash != p->current_dir) {
					*slash = '\0';
				} else if (slash == p->current_dir) {
					p->current_dir[1] = '\0';
				}
			} else {
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
		tui_write_at(1, 1, "Mini Commander Help (Ring 3)", TUI_COLS - 2, 7, 0);
		tui_write_at(3, 1, "F1  - This help screen", TUI_COLS - 2, 7, 0);
		tui_write_at(5, 1, "F5  - Copy file/dir to other panel", TUI_COLS - 2, 7, 0);
		tui_write_at(6, 1, "F6  - Move file/dir to other panel", TUI_COLS - 2, 7, 0);
		tui_write_at(7, 1, "F7  - Create directory", TUI_COLS - 2, 7, 0);
		tui_write_at(8, 1, "F8  - Delete file/dir", TUI_COLS - 2, 7, 0);
		tui_write_at(9, 1, "F10 - Quit", TUI_COLS - 2, 7, 0);
		tui_write_at(11, 1, "TAB - Switch panel", TUI_COLS - 2, 7, 0);
		tui_write_at(12, 1, "ENTER - Open directory", TUI_COLS - 2, 7, 0);
		tui_write_at(TUI_ROWS - 1, 1, "Press any key to continue", TUI_COLS - 2, 0, 7);
		if (!mc_smoke_mode) tui_get_key();
		return 1;

	case KEY_F5: {
		struct panel *src_p = &panels[active_panel];
		struct panel *dst_p = &panels[1 - active_panel];
		if (src_p->selected >= src_p->file_count) return 1;
		const char *name = src_p->files[src_p->selected].name;
		char src_path[256], dst_path[256];
		resolve_path(src_p->current_dir, name, src_path);
		resolve_path(dst_p->current_dir, name, dst_path);
		int ret = src_p->files[src_p->selected].is_dir ? copy_dir_op(src_path, dst_path) : copy_file_op(src_path, dst_path);
		tui_write_at(TUI_ROWS - 2, 1, ret == 0 ? "Copy successful." : "Copy failed.", TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
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
		int ret = rename(src_path, dst_path);
		if (ret < 0) {
			int copy_ok = src_p->files[src_p->selected].is_dir ? copy_dir_op(src_path, dst_path) : copy_file_op(src_path, dst_path);
			if (copy_ok == 0) {
				if (src_p->files[src_p->selected].is_dir) rmdir(src_path);
				else unlink(src_path);
				ret = 0;
			}
		}
		tui_write_at(TUI_ROWS - 2, 1, ret == 0 ? "Move successful." : "Move failed.", TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
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
			int ret = mkdir(full_path, 0755);
			tui_write_at(TUI_ROWS - 2, 1, ret == 0 ? "Directory created." : "Failed.", TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
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
		int ret = p->files[p->selected].is_dir ? rmdir(full_path) : unlink(full_path);
		tui_write_at(TUI_ROWS - 2, 1, ret == 0 ? "Deleted." : "Delete failed.", TUI_COLS - 2, ret == 0 ? 2 : 1, 0);
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
	if (mkdir(smoke_dir, 0755) < 0 && errno != EEXIST) return 1;
	if (mkdir(smoke_subdir, 0755) < 0 && errno != EEXIST) return 1;

	mc_smoke_mode = 1;
	tui_screen_mute();
	if (tui_terminal_begin() != 0) {
		tui_screen_unmute();
		mc_smoke_mode = 0;
		return 1;
	}

	active_panel = 0;
	strcpy(panels[0].current_dir, smoke_dir);
	strcpy(panels[1].current_dir, "/");
	read_directory(&panels[0]);
	read_directory(&panels[1]);

	tui_clear_screen();
	tui_cursor_hide();
	mc_render_screen();

	mc_handle_key(KEY_F1);
	mc_render_screen();

	int subdir_idx = -1;
	for (int i = 0; i < panels[0].file_count; i++) {
		if (strcmp(panels[0].files[i].name, "subdir") == 0) {
			subdir_idx = i;
			break;
		}
	}
	if (subdir_idx >= 0) {
		panels[0].selected = subdir_idx;
		mc_handle_key(KEY_ENTER);
	}

	tui_clear_screen();
	tui_cursor_show();
	tui_terminal_end();
	tui_screen_unmute();
	mc_smoke_mode = 0;
	printf("M16-SMOKE: ok file-explorer-hotkeys\n");
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && argv && argv[1] && strcmp(argv[1], "--smoke") == 0) {
		return mc_smoke_run();
	}
	
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
