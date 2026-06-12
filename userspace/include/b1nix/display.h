#ifndef _B1NIX_DISPLAY_H
#define _B1NIX_DISPLAY_H

#include <stdint.h>

/* M47 Phase 2 — the b1display v1 wire protocol.
 *
 * Deliberately Wayland-shaped (see docs/display-server.md): object-id wire
 * framing, client-allocated objects and buffers, attach/damage/commit
 * surface lifecycle, frame callbacks, a seat for input. The differences
 * from real Wayland (no registry, fixed well-known objects, SysV-SHM
 * buffer transport) are exactly the parts M48/M49 replace.
 *
 * Wire format, little-endian:
 *   struct b1d_hdr { u32 object_id; u16 opcode; u16 size; }
 * `size` is the total message length in bytes including the header and is
 * always a multiple of 4. Payload is a sequence of 32-bit words.
 *
 * Well-known objects: 1 = display, 2 = seat. Client-allocated ids
 * (surfaces, buffers, callbacks) start at B1D_CLIENT_ID_BASE.
 */

#define B1D_SOCKET_PATH "/run/b1display.sock"
#define B1D_CLIENT_ID_BASE 0x10
#define B1D_MAX_MSG 256

#define B1D_OBJ_DISPLAY 1
#define B1D_OBJ_SEAT 2

struct b1d_hdr {
  uint32_t object_id;
  uint16_t opcode;
  uint16_t size;
};

/* ── requests (client → server) ── */

/* display */
#define B1D_REQ_DISPLAY_HELLO 0          /* () → EV_DISPLAY_INFO */
#define B1D_REQ_DISPLAY_CREATE_SURFACE 1 /* (new_id) */
#define B1D_REQ_DISPLAY_CREATE_BUFFER 2  /* (new_id, shm_key, offset, w, h, stride) */
#define B1D_REQ_DISPLAY_SYNC 3           /* (new_callback_id) → EV_CALLBACK_DONE */
#define B1D_REQ_DISPLAY_SHUTDOWN 4       /* () — server exits (admin/test) */
#define B1D_REQ_DISPLAY_CREATE_TOPLEVEL 5 /* (new_id, surface_id) */
#define B1D_REQ_DISPLAY_CHECKSUM 6        /* (new_callback_id) → EV_CALLBACK_VALUE */

/* surface */
#define B1D_REQ_SURFACE_ATTACH 0   /* (buffer_id) — pending until commit */
#define B1D_REQ_SURFACE_DAMAGE 1   /* (x, y, w, h) — pending until commit */
#define B1D_REQ_SURFACE_FRAME 2    /* (new_callback_id) — fires after commit */
#define B1D_REQ_SURFACE_COMMIT 3   /* () — atomically apply pending state */
#define B1D_REQ_SURFACE_DESTROY 4  /* () */
#define B1D_REQ_SURFACE_SET_POSITION 5 /* (x, y) — request top-left screen pos;
                                          overrides server placement. Honored
                                          before first map; after map it moves
                                          the window. x/y are signed int32. */

/* buffer */
#define B1D_REQ_BUFFER_DESTROY 0 /* () */

/* toplevel */
#define B1D_REQ_TOPLEVEL_SET_TITLE 0 /* (length, bytes...) UTF-8, max 31 bytes */
#define B1D_REQ_TOPLEVEL_MOVE 1      /* (input_serial, dx, dy) */
#define B1D_REQ_TOPLEVEL_RESIZE 2    /* (input_serial, width, height) */
#define B1D_REQ_TOPLEVEL_DESTROY 3   /* () */

/* ── events (server → client) ── */

/* display */
#define B1D_EV_DISPLAY_INFO 0  /* (width, height, format) */
#define B1D_EV_DISPLAY_ERROR 1 /* (object_id, code) */

/* any callback object */
#define B1D_EV_CALLBACK_DONE 0 /* (timestamp_lo32) — callback id is then dead */
#define B1D_EV_CALLBACK_VALUE 1 /* (value) — callback id is then dead */

/* seat */
#define B1D_EV_SEAT_POINTER_ENTER 0  /* (surface_id, x, y) surface-local */
#define B1D_EV_SEAT_POINTER_LEAVE 1  /* (surface_id) */
#define B1D_EV_SEAT_POINTER_MOTION 2 /* (x, y) surface-local */
#define B1D_EV_SEAT_POINTER_BUTTON 3 /* (button, state) — B1NIX_BTN_*, 1=press */
#define B1D_EV_SEAT_KEY 4            /* (scancode, state) — raw set-1 code */
#define B1D_EV_SEAT_FOCUS_ENTER 5    /* (surface_id) */
#define B1D_EV_SEAT_FOCUS_LEAVE 6    /* (surface_id) */

/* toplevel */
#define B1D_EV_TOPLEVEL_CONFIGURE 0 /* (x, y, width, height) */
#define B1D_EV_TOPLEVEL_CLOSE 1     /* () */

/* buffer */
#define B1D_EV_BUFFER_RELEASE 0 /* () — server is done reading the buffer */

/* pixel formats */
#define B1D_FORMAT_XRGB8888 0

/* error codes for B1D_EV_DISPLAY_ERROR */
#define B1D_ERR_BAD_OBJECT 1
#define B1D_ERR_BAD_REQUEST 2
#define B1D_ERR_NO_RESOURCE 3
#define B1D_ERR_BAD_BUFFER 4

#endif
