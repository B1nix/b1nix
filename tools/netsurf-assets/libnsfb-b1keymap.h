/* Shared b1nix PS/2 set-1 scancode -> libnsfb keysym map for the b1nix libnsfb
 * surfaces (the /dev/fb0 and displayd surfaces). Handles printable keys (with
 * shift), and the special keys NetSurf text fields need: backspace, delete, the
 * arrow keys, home/end, page up/down, tab, return and escape. Extended keys
 * arrive as evdev codes 0xE000 | scancode (see kernel input_kbd_scancode). */
#ifndef B1NIX_NSFB_KEYMAP_H
#define B1NIX_NSFB_KEYMAP_H

#include "libnsfb_event.h"

/* shift make/break scancodes (left/right shift). */
static inline int b1nix_is_shift_scancode(unsigned sc)
{
	return sc == 0x2A || sc == 0x36;
}

static enum nsfb_key_code_e b1nix_scancode_to_nsfb(unsigned sc, int shift)
{
	switch (sc) {
	/* extended (0xE0-prefixed) navigation keys */
	case 0xE048: return NSFB_KEY_UP;
	case 0xE050: return NSFB_KEY_DOWN;
	case 0xE04B: return NSFB_KEY_LEFT;
	case 0xE04D: return NSFB_KEY_RIGHT;
	case 0xE047: return NSFB_KEY_HOME;
	case 0xE04F: return NSFB_KEY_END;
	case 0xE049: return NSFB_KEY_PAGEUP;
	case 0xE051: return NSFB_KEY_PAGEDOWN;
	case 0xE053: return NSFB_KEY_DELETE;
	/* control keys that don't live in the printable tables */
	case 0x01: return NSFB_KEY_ESCAPE;
	case 0x0E: return NSFB_KEY_BACKSPACE;
	case 0x0F: return NSFB_KEY_TAB;
	case 0x1C: return NSFB_KEY_RETURN;
	default: break;
	}

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
	if (sc < 128) {
		char c = shift ? shifted[sc] : normal[sc];
		if (c != 0)
			return (enum nsfb_key_code_e)(unsigned char)c;
	}
	return NSFB_KEY_UNKNOWN;
}

#endif
