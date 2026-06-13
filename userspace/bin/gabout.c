#include <b1nix/gui.h>
#include <b1nix/input.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/utsname.h>

#include "font8x8.h"

/* ---- Palette (0x00RRGGBB) -------------------------------------------- */
#define COL_BG            0x00121821u
#define COL_HEADER_TOP    0x001F2C3Eu
#define COL_PANEL         0x001B2531u
#define COL_PANEL_BD      0x00314358u
#define COL_SHADOW        0x000B0F16u
#define COL_TEXT          0x00FFFFFFu
#define COL_TEXT_DIM      0x00C2C9D1u
#define COL_SUBTLE        0x00828B92u
#define COL_ACCENT        0x006FE7D2u
#define COL_DIVIDER       0x00314358u
#define COL_BADGE_TOP     0x008FF0DEu
#define COL_BADGE_BOT     0x0046C9B0u
#define COL_BADGE_TEXT    0x000F2A24u
#define COL_BTN_TOP       0x00314561u
#define COL_BTN_BOT       0x00243648u
#define COL_BTN_BD        0x004A5B73u
#define COL_BTN_HOVER_TOP 0x004C93E6u
#define COL_BTN_HOVER_BOT 0x00346FC2u
#define COL_BTN_HOVER_BD  0x0066ACF2u

/* ---- Text drawing ---------------------------------------------------- */
static void draw_text(struct b1gui_window *win, int x0, int y0, const char *text, uint32_t color) {
	for (int i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		const unsigned char *glyph = font8x8_basic[c < 128 ? c : '?'];
		for (int gy = 0; gy < 8; gy++) {
			int y = y0 + gy;
			if (y < 0 || y >= (int)win->height) continue;
			for (int gx = 0; gx < 8; gx++) {
				int x = x0 + i * 8 + gx;
				if (x < 0 || x >= (int)win->width) continue;
				if (glyph[gy] & (1u << (7 - gx))) {
					win->pixels[y * win->width + x] = color;
				}
			}
		}
	}
}

static void draw_text_centered(struct b1gui_window *win, int y0, const char *text, uint32_t color) {
	int len = strlen(text);
	int x0 = (int)(win->width - len * 8) / 2;
	draw_text(win, x0, y0, text, color);
}

static void draw_text_scaled(struct b1gui_window *win, int x0, int y0, const char *text, uint32_t color, int char_w) {
	for (int i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		const unsigned char *glyph = font8x8_basic[c < 128 ? c : '?'];
		for (int gy = 0; gy < 8; gy++) {
			int y = y0 + gy;
			if (y < 0 || y >= (int)win->height) continue;
			for (int gx = 0; gx < char_w; gx++) {
				int x = x0 + i * char_w + gx;
				if (x < 0 || x >= (int)win->width) continue;
				int sx = gx * 8 / char_w;
				if (glyph[gy] & (1u << (7 - sx))) {
					win->pixels[y * win->width + x] = color;
				}
			}
		}
	}
}

/* Crisp integer-scaled text (scales BOTH width and height). */
static void draw_text_big(struct b1gui_window *win, int x0, int y0, const char *text, uint32_t color, int scale) {
	for (int i = 0; text[i]; i++) {
		unsigned char c = (unsigned char)text[i];
		const unsigned char *glyph = font8x8_basic[c < 128 ? c : '?'];
		for (int gy = 0; gy < 8; gy++) {
			for (int gx = 0; gx < 8; gx++) {
				if (!(glyph[gy] & (1u << (7 - gx)))) continue;
				for (int sy = 0; sy < scale; sy++) {
					int y = y0 + gy * scale + sy;
					if (y < 0 || y >= (int)win->height) continue;
					for (int sx = 0; sx < scale; sx++) {
						int x = x0 + i * 8 * scale + gx * scale + sx;
						if (x < 0 || x >= (int)win->width) continue;
						win->pixels[y * win->width + x] = color;
					}
				}
			}
		}
	}
}

static void draw_text_big_centered(struct b1gui_window *win, int y0, const char *text, uint32_t color, int scale) {
	int len = strlen(text);
	int x0 = (int)(win->width - len * 8 * scale) / 2;
	draw_text_big(win, x0, y0, text, color, scale);
}

/* ---- Shape drawing --------------------------------------------------- */
static void draw_rect(struct b1gui_window *win, int x0, int y0, int w, int h, uint32_t color) {
	for (int y = y0; y < y0 + h; y++) {
		if (y < 0 || y >= (int)win->height) continue;
		for (int x = x0; x < x0 + w; x++) {
			if (x < 0 || x >= (int)win->width) continue;
			win->pixels[y * win->width + x] = color;
		}
	}
}

/* Linear interpolation between two colors, t in [0,255]. */
static uint32_t blend(uint32_t a, uint32_t b, int t) {
	if (t < 0) t = 0;
	if (t > 255) t = 255;
	int ia = 255 - t;
	uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
	uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
	uint32_t r = (ar * ia + br * t) / 255;
	uint32_t g = (ag * ia + bg * t) / 255;
	uint32_t bl = (ab * ia + bb * t) / 255;
	return (r << 16) | (g << 8) | bl;
}

/* Is local pixel (x,y) inside a w x h rounded-rect with corner radius r? */
static int inside_round(int x, int y, int w, int h, int r) {
	if (x < 0 || y < 0 || x >= w || y >= h) return 0;
	int cx, cy, corner = 0;
	if (x < r && y < r)            { cx = r;         cy = r;         corner = 1; }
	else if (x >= w - r && y < r)  { cx = w - r - 1; cy = r;         corner = 1; }
	else if (x < r && y >= h - r)  { cx = r;         cy = h - r - 1; corner = 1; }
	else if (x >= w - r && y >= h - r) { cx = w - r - 1; cy = h - r - 1; corner = 1; }
	if (!corner) return 1;
	int dx = x - cx, dy = y - cy;
	return dx * dx + dy * dy <= r * r;
}

static void fill_round_grad(struct b1gui_window *win, int x0, int y0, int w, int h, int r, uint32_t top, uint32_t bot) {
	for (int y = 0; y < h; y++) {
		int yy = y0 + y;
		if (yy < 0 || yy >= (int)win->height) continue;
		uint32_t color = blend(top, bot, h > 1 ? (y * 255) / (h - 1) : 0);
		for (int x = 0; x < w; x++) {
			int xx = x0 + x;
			if (xx < 0 || xx >= (int)win->width) continue;
			if (inside_round(x, y, w, h, r))
				win->pixels[yy * win->width + xx] = color;
		}
	}
}

static void fill_round(struct b1gui_window *win, int x0, int y0, int w, int h, int r, uint32_t color) {
	fill_round_grad(win, x0, y0, w, h, r, color, color);
}

static void stroke_round(struct b1gui_window *win, int x0, int y0, int w, int h, int r, uint32_t color) {
	for (int y = 0; y < h; y++) {
		int yy = y0 + y;
		if (yy < 0 || yy >= (int)win->height) continue;
		for (int x = 0; x < w; x++) {
			int xx = x0 + x;
			if (xx < 0 || xx >= (int)win->width) continue;
			if (inside_round(x, y, w, h, r) &&
			    (!inside_round(x - 1, y, w, h, r) || !inside_round(x + 1, y, w, h, r) ||
			     !inside_round(x, y - 1, w, h, r) || !inside_round(x, y + 1, w, h, r)))
				win->pixels[yy * win->width + xx] = color;
		}
	}
}

/* Rounded, flat-filled button with soft shadow and centered label. */
static void draw_button(struct b1gui_window *win, int x, int y, int w, int h, const char *label, int hovered) {
	fill_round(win, x + 1, y + 2, w, h, 7, COL_SHADOW);
	fill_round(win, x, y, w, h, 7, hovered ? COL_BTN_HOVER_TOP : COL_BTN_TOP);
	stroke_round(win, x, y, w, h, 7, hovered ? COL_BTN_HOVER_BD : COL_BTN_BD);
	int len = strlen(label);
	int tx = x + (w - len * 8) / 2;
	int ty = y + (h - 8) / 2;
	draw_text(win, tx, ty, label, COL_TEXT);
}

/* Section header for the detail view: accent bar + label. */
static void draw_section(struct b1gui_window *win, int dy, const char *label) {
	draw_rect(win, 15, dy, 3, 9, COL_ACCENT);
	draw_text(win, 24, dy, label, COL_ACCENT);
}

/* ---- Info rows ------------------------------------------------------- */
static void draw_info_row(struct b1gui_window *win, int y, const char *key, const char *val) {
	draw_text(win, 12, y, key, COL_SUBTLE);

	int val_len = strlen(val);
	int max_val_w = (int)win->width - 120 - 8;
	int char_w = 8;
	if (val_len * 8 > max_val_w) {
		char_w = max_val_w / val_len;
		if (char_w < 5) char_w = 5;
	}
	draw_text_scaled(win, 120, y, val, COL_TEXT, char_w);
}

static void draw_detail_row(struct b1gui_window *win, int y, const char *key, const char *val) {
	draw_text(win, 20, y, key, COL_SUBTLE);

	int val_len = strlen(val);
	int max_val_w = (int)win->width - 84 - 8;
	int char_w = 8;
	if (val_len * 8 > max_val_w) {
		char_w = max_val_w / val_len;
		if (char_w < 5) char_w = 5;
	}
	draw_text_scaled(win, 84, y, val, COL_TEXT, char_w);
}

/* ---- System probes (unchanged behaviour) ----------------------------- */
static void get_mem_stats(unsigned long *total_mb, unsigned long *free_mb) {
	*total_mb = 512;
	*free_mb = 490;
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd >= 0) {
		char buf[256];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			char *p = strstr(buf, "MemTotal:");
			if (p) {
				unsigned long total_kb = 0;
				sscanf(p, "MemTotal: %lu", &total_kb);
				*total_mb = total_kb / 1024;
			}
			p = strstr(buf, "MemFree:");
			if (p) {
				unsigned long free_kb = 0;
				sscanf(p, "MemFree: %lu", &free_kb);
				*free_mb = free_kb / 1024;
			}
		}
		close(fd);
	}
}

static int get_cpu_info(char *model, int max_len) {
	int fd = open("/proc/cpuinfo", O_RDONLY);
	int count = 0;
	strcpy(model, "b1nix virtual CPU");
	if (fd >= 0) {
		char buf[2048];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			char *p = buf;
			while ((p = strstr(p, "processor"))) {
				count++;
				p += 9;
			}
			p = strstr(buf, "model name");
			if (p) {
				char *colon = strchr(p, ':');
				if (colon) {
					colon++;
					while (*colon == ' ' || *colon == '\t') colon++;
					int i = 0;
					while (colon[i] && colon[i] != '\n' && i < max_len - 1) {
						model[i] = colon[i];
						i++;
					}
					model[i] = 0;
				}
			}
		}
		close(fd);
	}
	if (count == 0) count = 1;
	return count;
}

static void get_gpu_info(char *model, int max_model_len, char *vram, int max_vram_len) {
	int fd = open("/proc/gpuinfo", O_RDONLY);
	strcpy(model, "Basic Framebuffer");
	vram[0] = 0;
	if (fd >= 0) {
		char buf[512];
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		if (n > 0) {
			buf[n] = 0;
			char *p = strstr(buf, "model name");
			if (p) {
				char *colon = strchr(p, ':');
				if (colon) {
					colon++;
					while (*colon == ' ' || *colon == '\t') colon++;
					int i = 0;
					while (colon[i] && colon[i] != '\n' && i < max_model_len - 1) {
						model[i] = colon[i];
						i++;
					}
					model[i] = 0;
				}
			}
			p = strstr(buf, "vram");
			if (p) {
				char *colon = strchr(p, ':');
				if (colon) {
					colon++;
					while (*colon == ' ' || *colon == '\t') colon++;
					int i = 0;
					while (colon[i] && colon[i] != '\n' && i < max_vram_len - 1) {
						vram[i] = colon[i];
						i++;
					}
					vram[i] = 0;
				}
			}
		}
		close(fd);
	}
}

int main(int argc, char **argv) {
	struct b1gui_window win;
	const char *mode = "b1nix";
	char title[48] = "About b1nix";
	uint32_t win_width = 280;
	uint32_t win_height = 140;

	if (argc > 1) {
		mode = argv[1];
		if (strcmp(mode, "date") == 0) {
			strcpy(title, "Date & Time");
		} else {
			strcpy(title, "About ");
			strncat(title, mode, 32);
		}
	} else {
		win_height = 300;
		strcpy(title, "About b1nix");
	}

	if (b1gui_connect(&win) || b1gui_create_window(&win, win_width, win_height, title))
		return 1;

	int hover = 0;
	int px = 0, py = 0;
	int needs_redraw = 1;
	int show_details = 0;

	// Query CPU, Memory, and GPU
	char cpu_model[64];
	int cpu_count = get_cpu_info(cpu_model, sizeof(cpu_model));
	unsigned long total_mem = 0, free_mem = 0;
	get_mem_stats(&total_mem, &free_mem);
	char gpu_model[64];
	char gpu_vram[32];
	get_gpu_info(gpu_model, sizeof(gpu_model), gpu_vram, sizeof(gpu_vram));

	// Query kernel version dynamically
	struct utsname uts;
	char os_ver[32] = "unknown";
	char kernel_ver_str[64] = "unknown";
	if (uname(&uts) == 0) {
		snprintf(os_ver, sizeof(os_ver), "%s", uts.release);
		snprintf(kernel_ver_str, sizeof(kernel_ver_str), "%s %s",
		         uts.release, uts.version);
	}

	int has_gpu = (strcmp(gpu_model, "Basic Framebuffer") != 0 || gpu_vram[0]);
	int button_y = (int)win_height - 68; // pinned to the bottom, just above the footer

	for (;;) {
		if (strcmp(mode, "date") == 0) {
			needs_redraw = 1;
		}

		if (needs_redraw) {
			// Clear background
			draw_rect(&win, 0, 0, win.width, win.height, COL_BG);

			if (strcmp(mode, "b1nix") == 0) {
				if (!show_details) {
					// Logo badge
					int badge = 34, bxx = ((int)win.width - badge) / 2;
					fill_round(&win, bxx + 1, 11, badge, badge, 9, COL_SHADOW);
					fill_round(&win, bxx, 9, badge, badge, 9, COL_BADGE_BOT);
					draw_text_big(&win, bxx + 1, 18, "b1", COL_BADGE_TEXT, 2);
					draw_text_centered(&win, 49, "b1nix OS", COL_TEXT);

					// Specs values
					char chip_buf[64];
					snprintf(chip_buf, sizeof(chip_buf), "%dx %s", cpu_count, cpu_model);

					char mem_buf[32];
					if (total_mem >= 1024 && (total_mem % 1024) == 0) {
						snprintf(mem_buf, sizeof(mem_buf), "%lu GB", total_mem / 1024);
					} else {
						snprintf(mem_buf, sizeof(mem_buf), "%lu MB", total_mem);
					}

					char graphics_buf[128];
					if (gpu_vram[0]) {
						snprintf(graphics_buf, sizeof(graphics_buf), "%s %s", gpu_model, gpu_vram);
					} else {
						snprintf(graphics_buf, sizeof(graphics_buf), "%s", gpu_model);
					}

					int y = 74;
					draw_info_row(&win, y, "Processor", chip_buf);
					y += 18;
					if (has_gpu) {
						draw_info_row(&win, y, "Graphics", graphics_buf);
						y += 18;
					}
					draw_info_row(&win, y, "Memory", mem_buf);
					y += 18;
					draw_info_row(&win, y, "b1nix Version", os_ver);

					// More Info button
					draw_button(&win, 92, button_y, 96, 24, "More Info...", hover);

					// Footer
					draw_text_centered(&win, (int)win.height - 34, "b1nix OS  -  2026", COL_SUBTLE);
				} else {
					// Detail report view
					draw_text_centered(&win, 15, "System Report", COL_ACCENT);
					draw_rect(&win, 15, 30, win.width - 30, 1, COL_DIVIDER);

					int dy = 38;
					draw_section(&win, dy, "Processor");
					dy += 14;
					char cores_buf[16];
					snprintf(cores_buf, sizeof(cores_buf), "%d", cpu_count);
					draw_detail_row(&win, dy, "Cores  :", cores_buf);
					dy += 12;
					draw_detail_row(&win, dy, "Model  :", cpu_model);
					dy += 18;

					draw_section(&win, dy, "Graphics");
					dy += 14;
					draw_detail_row(&win, dy, "Model  :", gpu_model);
					if (gpu_vram[0]) {
						dy += 12;
						draw_detail_row(&win, dy, "VRAM   :", gpu_vram);
					}
					dy += 18;

					draw_section(&win, dy, "Memory");
					dy += 14;
					char total_buf[32], free_buf[32], used_buf[32];
					snprintf(total_buf, sizeof(total_buf), "%lu MB", total_mem);
					snprintf(free_buf, sizeof(free_buf), "%lu MB", free_mem);
					snprintf(used_buf, sizeof(used_buf), "%lu MB", total_mem > free_mem ? total_mem - free_mem : 0);
					draw_detail_row(&win, dy, "Total  :", total_buf);
					dy += 12;
					draw_detail_row(&win, dy, "Free   :", free_buf);
					dy += 12;
					draw_detail_row(&win, dy, "Used   :", used_buf);
					dy += 18;

					draw_section(&win, dy, "System");
					dy += 14;
					draw_detail_row(&win, dy, "OS     :", "b1nix");
					dy += 12;
					draw_detail_row(&win, dy, "Kernel :", kernel_ver_str);
					dy += 12;
#ifdef __x86_64__
					draw_detail_row(&win, dy, "Arch   :", "x86_64");
#else
					draw_detail_row(&win, dy, "Arch   :", "i686");
#endif

					// Back button
					draw_button(&win, 108, 255, 64, 24, "Back", hover);
				}
			} else if (strcmp(mode, "date") == 0) {
				time_t now = time(0);
				struct tm tmv;
				struct tm *tm = localtime_r(&now, &tmv);
				char date_str[64] = "Unknown Date";
				char time_str[64] = "00:00:00";
				if (tm) {
					strftime(date_str, sizeof(date_str), "%A, %b %d, %Y", tm);
					strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);
				}
				draw_text_centered(&win, 16, date_str, COL_TEXT_DIM);
				draw_text_big_centered(&win, 38, time_str, COL_ACCENT, 2);
				draw_text_centered(&win, 66, "b1nix local time", COL_SUBTLE);

				// OK button
				draw_button(&win, 108, 104, 64, 24, "OK", hover);
			} else {
				char app_info[64];
				snprintf(app_info, sizeof(app_info), "About %s", mode);
				draw_text_centered(&win, 18, app_info, COL_ACCENT);
				draw_text_centered(&win, 44, "A graphical b1nix application.", COL_TEXT_DIM);
				draw_text_centered(&win, 64, "Part of the b1nix userland.", COL_SUBTLE);

				// OK button
				draw_button(&win, 108, 104, 64, 24, "OK", hover);
			}

			b1gui_present(&win, 0, 0, win.width, win.height);
			needs_redraw = 0;
		}

		struct b1gui_event event;
		int status = b1gui_next_event(&win, &event, 500);
		if (status == 1) {
			if (event.type == B1GUI_EV_CLOSE) {
				break;
			}

			{
				if ((event.type == B1GUI_EV_POINTER_ENTER ||
				     event.type == B1GUI_EV_POINTER_MOTION) &&
				    event.nargs >= 2) {
					int ex = (int)event.args[0];
					int ey = (int)event.args[1];
					px = ex;
					py = ey;

					int new_hover = 0;
					if (strcmp(mode, "b1nix") == 0) {
						if (!show_details) {
							new_hover = (px >= 92 && px < 188 && py >= button_y && py < button_y + 24);
						} else {
							new_hover = (px >= 108 && px < 172 && py >= 255 && py < 279);
						}
					} else {
						new_hover = (px >= 108 && px < 172 && py >= 104 && py < 128);
					}

					if (new_hover != hover) {
						hover = new_hover;
						needs_redraw = 1;
					}
				} else if (event.type == B1GUI_EV_POINTER_BUTTON &&
				           event.nargs >= 2 && event.args[0] == B1NIX_BTN_LEFT && event.args[1] != 0) {
					if (strcmp(mode, "b1nix") == 0) {
						if (!show_details) {
							if (px >= 92 && px < 188 && py >= button_y && py < button_y + 24) {
								show_details = 1;
								hover = 0;
								needs_redraw = 1;
							}
						} else {
							if (px >= 108 && px < 172 && py >= 255 && py < 279) {
								show_details = 0;
								hover = 0;
								needs_redraw = 1;
							}
						}
					} else {
						if (px >= 108 && px < 172 && py >= 104 && py < 128) {
							break;
						}
					}
				} else if (event.type == B1GUI_EV_KEY && event.nargs >= 2) {
					uint32_t keycode = event.args[0];
					uint32_t state = event.args[1];
					if (state != 0 && (keycode == 0x01 || keycode == 0x1c)) { // ESC or Enter
						if (strcmp(mode, "b1nix") == 0) {
							if (show_details) {
								show_details = 0;
								hover = 0;
								needs_redraw = 1;
							} else {
								break;
							}
						} else {
							break;
						}
					}
				}
			}
		}
	}

	b1gui_destroy(&win);
	return 0;
}
