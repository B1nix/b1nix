#include <b1nix/console.h>
#include <b1nix/fb_console.h>
#include <b1nix/types.h>
#include <string.h>
#include "font8x8.h"

static const char *splash_lines[] = {
	".                                                 .   . .                     . .          .        ",
	".       .        .      .                                                                           ",
	"       .      .       .             .     .       ..    .               .            .              ",
	"..   ..                .      .          .              .                                    . .    ",
	".                             .     . .       .         .       .                                   ",
	"         .       .            ..    .        .             .             .       .      .      .    ",
	"..   ....     ..            .      ...    .. .  . . ...  ..            . .    ..       .     ..     ",
	"..   .     .               .     ..    ..@@@@@@@@@@@@@@@@@                   .             .        ",
	"              .                  @@@@@@@@+++++++++++++++++@@@@  @@@@@....              .            ",
	"         ...                .. .@+++@@++++++++++++++++++++++++@@+++++@    .     .      . .          ",
	". .     .     ...    .     ... .@+@@=+++++++++++++++++++++++++++@@=++@   .   .      .. .            ",
	"     .       .              .   @@@+++++++++++++++++++++++++++++@@+++@          .                  .",
	".             .     .   .     .  @+++@+++++++++++++++++++++++++++++@@   .   .  .. .          .      ",
	"..     .  .  .@#@.     ..      .@+++@@@+++++++++++++@@@++++++++++++@@     .     .           . .     ",
	". .       @@@..@##@. .       ..@@+++++++:::::::::+++++++++++++++++++@@         .     .   .    .     ",
	"       . ..@##@..@@#@@.. .   .@@+:::::::@@@@@@@::::::::::+++++++++++@@         ..                   ",
	".     .%%%. -@#%%%-@@%#%..%%@-@@==::::::==@@@==::::::::::::-+++++++++@%. .                 .        ",
	".      @#@@. .@##@@.@@##@. .  @::@:::::::::@:::::::::::::@@@@@@++++++@@   .       . .   .     .     ",
	"     ...@@##@@ .@@##@.@##@..@@@@:::::@@:::@@@:::::@@:::::::::++++++++@@       .            .       .",
	"    @@@  .@@###@. @@#####@  . .@:::::::::::::@@@@@:::::::::@@++++++++@@     .                       ",
	"    @@##@@.. @@##@@.@###@@  .  .@@:::::::::::::::::::::::::::+@@++++++@@ .   .      . ..            ",
	"    ..@@##@@.. @@#######@..    ..@@@::::::::::::::::::::::::++++++++++@@.                           ",
	".    . .@@##@. ..@#######@ ..   ..@@@::::::::::::::::::::::++++++++++++@@.              .      .    ",
	".       ..@@##@@@####@@##@@@...   @@++::::::::::::::::::++++++++++++++++@   .   .     .     . .     ",
	".            @@###@@.  .@@####@@.@@++:::::::::::::::::::::+++++++++++++++@.                .        ",
	".    .       .   ..        @@@++@@@+::::::::::::::::::::::+++++++++++++++@@                         ",
	"               .   .      @@++@@+++@@::::::::::::::::::::::+++++++++++++++@@   .      .        .    ",
	"        .      .         .@+++@+++@@@@-::::::::::::::::::::+@+++++++++++++@@     .    .             ",
	"    .                   .@+++++++@++@##@@:::::::::::::::::@@+++++++++++++++@.                    .  ",
	".. .    ...   ....    .  @++++++++++@@@##@@:::::::::::@@@@+++++++++++++++++@@  ..     ...    ...    ",
	"     .. .      .    ...   @++++++++@@::@@###@@::::@@@@++++++++++++++++++++++@ .    ... .    ..      ",
	"                   ..     .@@@@@@@@:::::::@@###@@@@++++++++++++++++@@+++++++@@.     . .          .  ",
	"   .             .         .@@+=::::::::::::@@#@@++++++++++++++++@@=++++++++@@          .           ",
	".     .  ..   ...    .. .  @@++:::::::::::::::@@@=@++=+++++++++@@@++++++++++@@....     .     ...    ",
	"                   .  ..  .@++::::::::::::::::::@@++@@+++@@@@@+++++++++++++++@.     ....   ...    . ",
	"       ..     .    .   .  @@++:::::::::::::::::::@@@@@@@#@@:+++++++++++++++++@        .    .        ",
	"..                .  .   .@++:::::::::::::::::::::::::@@####@++++++++++++++++@                      ",
	"       . .      .        @@++::::::::::::::::::::::::::::@@###@@+++++++++++++@.               .     ",
	" ..  ...      .:--:  ..  @@++::::::::::::::::::::::::::::::@%%##@**+++++++++@@ .      .             ",
	".            .@@@@@@....@@@++:::::::::::::::::::::-:::::::-++@@###@@++++++++@@@.               .    ",
	"..            @@++++@@@@++@@++:::::::::::::::::@@@+@@@:::@@+++++@@###@+++++@@+@@@@. .  .      . .   ",
	"..       .   .@++++++@@++++@++::::::::::::::::@@++++++@@@++++++++++@##@++++@+++++++@@@@.    . .     ",
	"            . @@++++++++++++@++:::::::::::::::@@+++++++@++++++++++++++++++@@++++++++++++@@@@        ",
	" . .         ..@@++++++++++++@@+++::::::::::::@@++++++++@++++++++++++++++@@+++++++++++++++++@@      ",
	".. .      ..    @@++++++++++++@@+=++:::::::::::@=++++++++++++++++++++++=@@++++++++++++++++++@@ . .  ",
	"         ..    . @@++++++++++++++@@@+++++++++++@@+++++++++++++++++++++@@++++++++++++++@@@@@@.       ",
	"..               .@@@++@@@@@@@@.      @@@@@@@@@@@@++++++++++++++++@@@@@@@@@@@@@@@@@...              ",
	"..     .      .       .. ..  .                  .@@+++++++++@@@@@..             . .   .             ",
	"   .     ..      .     .                           @@@@@@@@@  .           .     ..   .    .      .  ",
	" .      .    .  .    .            . .       .       ****                ..           .              ",
	" .                                        .                                                .        ",
	". . .    .     .    .       .   .   .       .     .                    .                            ",
	"..       .                  .          .           .      .             .             .             ",
	".  .                 .              .     ...       ..         .        . .   .       .       ..    ",
	"                           .                                   .             .                      ",
};

#define SPLASH_LINE_COUNT (sizeof(splash_lines) / sizeof(splash_lines[0]))

static u32 get_char_color(char c)
{
	switch (c) {
	case '@': return 0xFFFF3333; /* Demon body: Crimson Red */
	case '#': return 0xFFFFCC00; /* Horns/Core: Bright Amber */
	case '+': return 0xFF00E5FF; /* Aura: Neon Cyan */
	case '=': return 0xFFFFAA00; /* Borders: Gold */
	case ':': return 0xFF88AAFF; /* Atmosphere: Sky Blue */
	case '-': return 0xFF5588FF; /* Shading: Blue */
	case '%':
	case '*': return 0xFF55FF88; /* Eyes/Sparks: Neon Green */
	case '.': return 0xFF556688; /* Starfield: Slate Blue */
	default:  return 0xFFFFFFFF; /* Text: Crisp White */
	}
}

static void draw_title_centered(const char *str, u32 cy, u32 scale, u32 color, u32 w, u32 pitch, volatile u8 *fb)
{
	usize len = strlen(str);
	u32 str_w = (u32)len * 8 * scale;
	u32 cx = (w > str_w) ? (w - str_w) / 2 : 0;

	for (usize i = 0; i < len; i++) {
		char c = str[i];
		if ((unsigned char)c <= ' ') {
			cx += 8 * scale;
			continue;
		}
		const unsigned char *glyph = font8x8_basic[(unsigned char)c];
		for (u32 gy = 0; gy < 8; gy++) {
			for (u32 gx = 0; gx < 8; gx++) {
				if (!((glyph[gy] >> (7 - gx)) & 1))
					continue;
				for (u32 dy = 0; dy < scale; dy++) {
					u32 py = cy + gy * scale + dy;
					u64 row_off = (u64)py * pitch;
					for (u32 dx = 0; dx < scale; dx++) {
						u32 px = cx + gx * scale + dx;
						if (px < w) {
							*(volatile u32 *)(fb + row_off + (u64)px * 4) = color;
						}
					}
				}
			}
		}
		cx += 8 * scale;
	}
}

void demon_splash_show(void)
{
	if (!fb_console_ready()) {
		console_write("\n=== HELLO WORLD ===\n");
		for (usize i = 0; i < SPLASH_LINE_COUNT; i++) {
			console_write(splash_lines[i]);
			console_write("\n");
		}
		console_write("=== b1nix arm64 ===\n\n");
		return;
	}

	u32 w = fb_console_width();
	u32 h = fb_console_height();
	u32 pitch = fb_console_pitch();
	volatile u8 *fb = (volatile u8 *)fb_console_frontbuffer();

	if (!fb || w == 0 || h == 0)
		return;

	u32 num_lines = (u32)SPLASH_LINE_COUNT;
	u32 max_cols = 100;

	u32 cell_w = (w > 20) ? (w - 20) / max_cols : 8;
	if (cell_w < 6) cell_w = 6;

	u32 cell_h = 19;
	u32 art_total_w = max_cols * cell_w;
	u32 art_total_h = num_lines * cell_h;
	u32 x_start = (w > art_total_w) ? (w - art_total_w) / 2 : 0;

	u32 top_title_y = 12;
	u32 top_bar_y = top_title_y + 28;
	u32 art_y_start = top_bar_y + 10;
	u32 art_y_end = art_y_start + art_total_h;

	u32 bot_bar_y = art_y_end + 8;
	u32 bot_title_y = bot_bar_y + 8;
	u32 split_y = bot_title_y + 32;

	if (split_y >= h - 200)
		split_y = h / 2;

	/* Clear top artwork region to black */
	for (u32 py = 0; py < split_y; py++) {
		u64 row_off = (u64)py * pitch;
		for (u32 px = 0; px < w; px++) {
			*(volatile u32 *)(fb + row_off + (u64)px * 4) = 0x00000000u;
		}
	}

	/* Render Large Top Title: "HELLO WORLD" (Scale 3 = 24px height) */
	draw_title_centered("H E L L O   W O R L D", top_title_y, 3, 0xFFFFAA00u, w, pitch, fb);

	/* Top decorative accent line */
	for (u32 py = top_bar_y; py < top_bar_y + 2; py++) {
		u64 row_off = (u64)py * pitch;
		for (u32 px = x_start; px < x_start + art_total_w && px < w; px++) {
			*(volatile u32 *)(fb + row_off + (u64)px * 4) = 0xFF00E5FFu;
		}
	}

	/* Render stylized colored ASCII art */
	for (u32 line_idx = 0; line_idx < num_lines; line_idx++) {
		const char *line = splash_lines[line_idx];
		usize len = strlen(line);

		for (u32 col = 0; col < len && col < max_cols; col++) {
			char c = line[col];
			if ((unsigned char)c <= ' ')
				continue;

			u32 color = get_char_color(c);
			const unsigned char *glyph = font8x8_basic[(unsigned char)c];

			for (u32 gy = 0; gy < 8; gy++) {
				u32 py_0 = art_y_start + line_idx * cell_h + (gy * cell_h) / 8;
				u32 py_1 = art_y_start + line_idx * cell_h + ((gy + 1) * cell_h) / 8;
				if (py_0 >= bot_bar_y) break;
				if (py_1 > bot_bar_y) py_1 = bot_bar_y;

				for (u32 gx = 0; gx < 8; gx++) {
					if (!((glyph[gy] >> (7 - gx)) & 1))
						continue;

					u32 px_0 = x_start + col * cell_w + (gx * cell_w) / 8;
					u32 px_1 = x_start + col * cell_w + ((gx + 1) * cell_w) / 8;
					if (px_0 >= w) break;
					if (px_1 > w) px_1 = w;

					for (u32 py = py_0; py < py_1; py++) {
						u64 row_off = (u64)py * pitch;
						for (u32 px = px_0; px < px_1; px++) {
							*(volatile u32 *)(fb + row_off + (u64)px * 4) = color;
						}
					}
				}
			}
		}
	}

	/* Bottom decorative accent line */
	for (u32 py = bot_bar_y; py < bot_bar_y + 2; py++) {
		u64 row_off = (u64)py * pitch;
		for (u32 px = x_start; px < x_start + art_total_w && px < w; px++) {
			*(volatile u32 *)(fb + row_off + (u64)px * 4) = 0xFF00E5FFu;
		}
	}

	/* Render Large Bottom Title: "b1nix arm64" (Scale 3 = 24px height) */
	draw_title_centered("b 1 n i x   a r m 6 4", bot_title_y, 3, 0xFF00E5FFu, w, pitch, fb);

	/* Sleek dividing neon accent line between artwork and terminal */
	u32 split_bar_y = split_y - 4;
	for (u32 py = split_bar_y; py < split_bar_y + 2; py++) {
		u64 row_off = (u64)py * pitch;
		for (u32 px = x_start; px < x_start + art_total_w && px < w; px++) {
			*(volatile u32 *)(fb + row_off + (u64)px * 4) = 0xFFFFAA00u;
		}
	}

	/* Freeze top half so the terminal console scrolls only in the bottom half */
	fb_console_set_top(split_y);
	fb_console_set_font_scale(3);
	fb_console_flush();
}
