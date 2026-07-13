/*
 * displayd.h — shared types and globals for the b1nix display server.
 *
 * Split from the monolithic displayd.c for maintainability. Each module
 * includes this header and owns a specific subsystem.
 */
#ifndef DISPLAYD_H
#define DISPLAYD_H

#include <b1nix/fb.h>
#include <b1nix/input.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

/* ── limits ── */
#define MAX_CLIENTS   32
#define MAX_SURFACES  32
#define MAX_BUFFERS   32
#define MAX_TOPLEVELS 16
#define MAX_MSG       256
#define MAX_WOBJECTS  256
#define MAX_WPOOLS    32

/* ── visual constants ── */
#define CURSOR_W  11
#define CURSOR_H  19
#define TITLE_H   14
#define PANEL_H   28
#define MENU_W    176
#define MENU_ITEM_H 18
#define TASKBTN_W 96

#define BG_COLOR            0x00202830u
#define BORDER_COLOR        0x00E0E0E0u
#define TITLE_COLOR         0x00355085u
#define TITLE_FOCUS_COLOR   0x005078B0u
#define PANEL_COLOR         0x00111924u
#define PANEL_ACTIVE_COLOR  0x00354A5Du
#define MENU_BG_COLOR       0x00E8EDF1u
#define MENU_TEXT_COLOR     0x00131A20u
#define MENU_DISABLED_COLOR 0x00828B92u
#define MENU_HOVER_COLOR    0x003D78B5u
#define CLOSE_COLOR         0x00E05263u

/* ── bottom dock (macOS-style) ── */
#define DOCK_H           52
#define DOCK_ICON_W      40
#define DOCK_ICON_H      40
#define DOCK_PAD         6
#define DOCK_BG_COLOR    0x001A2230u
#define DOCK_BORDER_COLOR 0x003A4A5Au
#define DOCK_DOT_COLOR   0x007DD3FCu
#define DOCK_ACTIVE_COLOR 0x005078B0u
#define DOCK_ITEM_W      (DOCK_ICON_W + DOCK_PAD * 2)

/* ── app menu protocol constants ── */
#define APP_MENU_MAX_ITEMS  16
#define APP_MENU_LABEL_MAX  32
#define APP_MENU_ACCEL_MAX  12

#define WAYLAND_SOCKET_PATH "/run/wayland-0"

/* ── wire header ── */
struct wl_hdr {
	uint32_t object_id;
	uint16_t opcode;
	uint16_t size;
};

/* ── object types ── */
enum wobject_type {
	WOBJ_REGISTRY, WOBJ_COMPOSITOR, WOBJ_SHM, WOBJ_SEAT,
	WOBJ_POINTER, WOBJ_KEYBOARD, WOBJ_REGION, WOBJ_XDG_WM_BASE,
	WOBJ_XDG_SURFACE, WOBJ_OUTPUT, WOBJ_DDM, WOBJ_DATA_SOURCE,
	WOBJ_DATA_DEVICE, WOBJ_DATA_OFFER, WOBJ_DECORATION_MANAGER,
	WOBJ_TOPLEVEL_DECORATION, WOBJ_VIEWPORTER, WOBJ_VIEWPORT,
	WOBJ_SUBCOMPOSITOR, WOBJ_SUBSURFACE, WOBJ_PRESENTATION,
	WOBJ_PRESENTATION_FEEDBACK, WOBJ_DMABUF, WOBJ_DMABUF_PARAMS,
	WOBJ_TOUCH,
};

/* ── seat events ── */
enum seat_event {
	SEAT_POINTER_ENTER, SEAT_POINTER_LEAVE, SEAT_POINTER_MOTION,
	SEAT_POINTER_BUTTON, SEAT_KEY, SEAT_FOCUS_ENTER, SEAT_FOCUS_LEAVE,
};

/* ── panel menus ── */
enum panel_menu {
	MENU_NONE, MENU_SYSTEM, MENU_APP, MENU_FILE, MENU_EDIT,
	MENU_VIEW, MENU_WINDOW, MENU_CLOCK, MENU_DESKTOP,
};

/* ── core structs ── */
struct wobject {
	int used;
	uint32_t id;
	int client;
	enum wobject_type type;
	uint32_t link;
	int configured;
	char mime[64];
};

struct wpool {
	int used;
	uint32_t id;
	int client;
	int fd;
	uint32_t size;
};

struct dbuffer {
	int used;
	uint32_t id;
	int client;
	void *mem;
	void *map_base;
	size_t map_size;
	uint32_t w, h, stride;
};

struct dsurface {
	int used;
	uint32_t id;
	int client;
	int x, y;
	int mapped;
	unsigned placement;
	struct dbuffer *buf;
	uint32_t pend_buffer_id;
	int pend_attach;
	int pend_dmg_valid;
	uint32_t dx0, dy0, dx1, dy1;
	uint32_t frame_cb;
	int has_frame_cb;
	struct dsurface *parent;
	int sub_x, sub_y;
	int vp_dst_w, vp_dst_h;
};

struct dtoplevel {
	int used;
	uint32_t id;
	int client;
	struct dsurface *surface;
	char title[32];
	char app_id[32];
	int maximized;
	int fullscreen;
	int minimized;
	int geom_dirty;
	int restoring;
	int saved_x, saved_y;
};

struct dclient {
	int used;
	int fd;
	uint8_t inbuf[512];
	unsigned inlen;
	int pending_fd;
	int ping_pending;
	uint32_t ping_serial;
};

struct app_menu_item {
	uint16_t id;
	uint16_t flags;
	char label[APP_MENU_LABEL_MAX];
	char accel[APP_MENU_ACCEL_MAX];
};

struct app_menu {
	int toplevel_slot;
	int count;
	struct app_menu_item items[APP_MENU_MAX_ITEMS];
};

struct panel_layout {
	int system_x, system_w;
	int app_x, app_w;
	int file_x, file_w;
	int edit_x, edit_w;
	int view_x, view_w;
	int window_x, window_w;
	int clock_x, clock_w;
};

/* ── shared globals (defined in displayd_main.c) ── */
extern struct dclient clients[MAX_CLIENTS];
extern struct dsurface surfaces[MAX_SURFACES];
extern struct dbuffer buffers[MAX_BUFFERS];
extern struct dtoplevel toplevels[MAX_TOPLEVELS];
extern struct wobject wobjects[MAX_WOBJECTS];
extern struct wpool wpools[MAX_WPOOLS];
extern struct app_menu app_menus[MAX_TOPLEVELS];
extern int zorder[MAX_SURFACES];
extern int zcount;

extern int fb_fd;
extern uint32_t *fb;
extern uint32_t scr_w, scr_h;
extern int ev_fds[3];
extern int listen_fd;
extern int running;
extern unsigned frame_serial;
extern int surfaces_created;

extern char clock_hhmm[10];
extern int clock_last_min;
extern int clock_24h;

extern enum panel_menu open_menu;
extern int menu_hover;
extern int desktop_menu_x, desktop_menu_y;

extern int px, py;
extern int enter_slot;
extern int focus_slot;
extern int drag_slot;
extern int resize_slot;
extern uint32_t resize_edges;
extern int resize_ox, resize_oy, resize_ow, resize_oh, resize_wx, resize_wy;
extern int left_alt;
extern uint32_t input_serial;
extern uint32_t kbd_mods_depressed;
extern uint32_t kbd_mods_locked;
extern int ptr_acc_dx, ptr_acc_dy;
extern int ptr_abs_x, ptr_abs_y;
extern int ptr_have_abs;
extern int ptr_moved;
extern int tch_down, tch_x, tch_y, tch_client;
extern uint32_t tch_surface_id;
extern int btn_on_decoration;
extern int btn_on_panel;

/* ── clipboard / DnD state ── */
extern int sel_client;
extern uint32_t sel_source;
extern char sel_mime[64];
extern uint32_t server_id_next;
extern int dnd_active;
extern int dnd_src_client;
extern uint32_t dnd_source;
extern char dnd_mime[64];
extern uint32_t dnd_src_actions;
extern int dnd_target_client;
extern uint32_t dnd_target_offer;
extern uint32_t dnd_accepted_action;
extern int dnd_in_surface;

/* ── module API ── */

/* protocol: wire helpers + message dispatch */
void send_msg(int client, uint32_t obj, uint16_t opcode,
              const uint32_t *words, unsigned nwords);
void send_msg_fd(int client, uint32_t obj, uint16_t opcode,
                 const uint32_t *words, unsigned nwords, int fd);
void wl_delete_id(int client, uint32_t id);
void handle_wayland_msg(int ci, const struct wl_hdr *h,
                        const uint32_t *a, unsigned n);
void client_data(int ci);
void accept_client(int fd);
void client_disconnect(int ci);

/* window management */
struct dsurface *slot_surface(int slot);
struct dsurface *find_surface(int client, uint32_t id);
struct dbuffer *find_buffer(int client, uint32_t id);
struct dtoplevel *find_toplevel(int client, uint32_t id);
struct dtoplevel *surface_toplevel(struct dsurface *s);
void create_surface(int ci, uint32_t id);
void create_toplevel(int ci, uint32_t id, uint32_t surface_id);
void surface_destroy(struct dsurface *s);
void buffer_destroy(struct dbuffer *b);
void zorder_remove(int slot);
void zorder_raise(int slot);
void minimize_toplevel(struct dtoplevel *t);
void restore_toplevel(struct dtoplevel *t);
void toplevel_set_state(struct dtoplevel *t, int maximized, int fullscreen);
void send_state_configure(struct dtoplevel *t, uint32_t w, uint32_t h,
                          const uint32_t *states, unsigned nstates);
void focus_cycle(void);
void close_focused_window(void);
int surface_at(int x, int y);
uint32_t work_w(void);
uint32_t work_h(void);

/* rendering */
void composite_rect(int rx, int ry, int rw, int rh);
void composite_surface_region(struct dsurface *s);
void blit_surface_at(struct dsurface *s, int ax, int ay, int rx, int ry,
                     int rw, int rh);
void draw_text_clipped(uint32_t *row, int screen_y, int rx, int rw,
                       int x0, int y0, const char *text, uint32_t color);
struct panel_layout get_panel_layout(void);
int text_len(const char *s, int limit);
const char *active_app_title(void);

/* rendering extras */
extern const char *const cursor_bitmap[CURSOR_H];
void draw_panel_overlay(int rx, int ry, int rw, int rh);

/* menu */
int menu_item_count(enum panel_menu menu);
const char *menu_item_label(enum panel_menu menu, int item);
int menu_item_is_separator(enum panel_menu menu, int item);
int menu_item_enabled(enum panel_menu menu, int item);
int menu_x(enum panel_menu menu);
int menu_h(enum panel_menu menu);
enum panel_menu panel_menu_at(int x);
int menu_item_at(int x, int y);
void open_panel_menu(enum panel_menu menu);
void close_panel_menu(void);
void activate_menu_item(enum panel_menu menu, int item);
int taskbar_x0(void);
int taskbar_slot(int n);
int taskbar_button_at(int x, int y);

/* input */
void keyboard_init(int client, uint32_t id);
void input_drain(int which);
void touch_drain(void);
void pointer_button(uint16_t code, int state);
void pointer_moved(void);
void apply_pointer_motion(void);

/* main */
void spawn_app(const char *path, const char *arg);
int update_clock(void);
void ping_clients(void);
void send_focused_shortcut(uint32_t key);
void send_kbd_modifiers(int client, uint32_t id);
void send_focus_modifiers(void);
void send_seat_event(int client, uint16_t opcode, const uint32_t *words,
                     unsigned nwords);

/* clipboard / DnD */
void clipboard_offer_to(int ci, uint32_t device_id);
void selection_client_gone(int ci);
void dnd_offer_enter(struct dsurface *s);
void dnd_offer_leave(void);
void dnd_motion(void);
void dnd_finish_drag(void);
struct wobject *dnd_target_device(int ci);

/* wobject helpers */
struct wobject *wobject_find(int client, uint32_t id);
struct wobject *wobject_add(int client, uint32_t id,
                            enum wobject_type type, uint32_t link);
void wobject_remove(struct wobject *obj);
struct wobject *wobject_type_find(int client, enum wobject_type type);
struct wpool *wpool_find(int client, uint32_t id);

/* protocol helpers */
void wl_send_string(int client, uint32_t obj, uint16_t opcode,
                    const uint32_t *prefix, unsigned nprefix,
                    const char *text, const uint32_t *suffix,
                    unsigned nsuffix);
unsigned wl_pack_string(uint32_t *w, unsigned i, const char *s);
void wl_registry_globals(int ci, uint32_t registry);
void wl_send_output_events(int ci, uint32_t id);

#endif /* DISPLAYD_H */
