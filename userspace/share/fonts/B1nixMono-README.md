# B1nix Mono

Experimental terminal font built from scratch for a strict Unix-like look.

## Current status

`v0.6` is a generated monospace TrueType font family with:

- `Regular` and `Bold` styles
- printable ASCII coverage (`U+0020` to `U+007E`)
- basic Russian Cyrillic coverage
- essential box-drawing glyphs for terminal UI
- refined lowercase Latin and Cyrillic forms
- half-segment box glyphs for denser terminal layouts
- multiscript fallback workflow for CJK, Arabic, and Indic scripts
- manually reworked Latin bowls and core text glyphs toward a calmer terminal rhythm
- consistent cell metrics for terminal use
- clearly separated `0/O`, `1/l/I`
- square, low-contrast strokes inspired by classic Unix terminal fonts

This version is intentionally compact and test-first.
For large writing systems such as Chinese, Japanese, Arabic, and Indic scripts, the project currently uses system fallback fonts instead of drawing thousands of native glyphs from scratch.

## Build

```bash
.venv/bin/python tools/build_font.py
.venv/bin/python tools/report_fallbacks.py
```

The generated font is written to:

- `dist/B1nixMono-Regular.ttf`
- `dist/B1nixMono-Bold.ttf`

## Install on macOS

1. Open both font files from `dist/`
2. Click `Install Font`
3. Select `B1nix Mono` in your terminal settings

## Quick visual test

Use the sample text from `specimens/terminal-sample.txt` in your terminal editor or preview pane.
Use `specimens/multiscript-sample.txt` to verify system fallback behavior.

## Project layout

- `tools/build_font.py` - font generator
- `tools/report_fallbacks.py` - system fallback coverage report
- `dist/` - generated binaries
- `specimens/terminal-sample.txt` - sample terminal text
- `specimens/multiscript-sample.txt` - Unicode fallback sample
- `docs/unicode-strategy.md` - multiscript support approach
