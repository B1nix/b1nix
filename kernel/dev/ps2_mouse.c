#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/console.h>
#include <b1nix/input.h>
#include <b1nix/io.h>
#include <b1nix/sched.h>
#include <b1nix/ps2_mouse.h>

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT 0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02

static struct ps2_mouse_state mouse_state;
static u8 packet[3];
static int packet_index;
static int mouse_ready;
static volatile int mouse_event_pending;
static int mouse_worker_started;

static int ps2_wait_input_clear(void)
{
    for (u32 i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_IN_FULL) == 0) return 1;
    }
    return 0;
}

static int ps2_wait_output_full(void)
{
    for (u32 i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL) return 1;
    }
    return 0;
}

static void ps2_write_cmd(u8 cmd)
{
    if (!ps2_wait_input_clear()) return;
    outb(PS2_CMD_PORT, cmd);
}

static void ps2_write_data(u8 data)
{
    if (!ps2_wait_input_clear()) return;
    outb(PS2_DATA_PORT, data);
}

/* Read one controller byte, distinguishing a real 0x00 from a timeout
 * (returns -1 on timeout). The ACK scan needs this: folding both into 0
 * made the old loop give up the instant the ACK was a few microseconds
 * late — the actual cause of "enable failed" under QEMU/macOS. */
static int ps2_read_byte_to(void)
{
    if (!ps2_wait_output_full()) return -1;
    return (int)inb(PS2_DATA_PORT);
}

static void ps2_drain_output(void)
{
    for (u32 i = 0; i < 64; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)) return;
        (void)inb(PS2_DATA_PORT);
    }
}

static void ps2_mouse_write(u8 value)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(value);
}

/* Send a mouse command and wait for the 0xFA ACK. Tolerant of stale bytes
 * (skipped), late ACKs (scans several reads), and resend requests (0xFE,
 * retried). Returns 1 on ACK, 0 if no ACK after a few attempts. */
static int ps2_mouse_command(u8 command)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        ps2_mouse_write(command);
        for (int i = 0; i < 24; i++) {
            int value = ps2_read_byte_to();
            if (value < 0) break;        /* timed out waiting — retry */
            if (value == 0xFA) return 1; /* ACK */
            if (value == 0xFE) break;    /* resend — retry the command */
            /* anything else: a stale/streaming byte, keep scanning */
        }
    }
    return 0;
}

static void ps2_mouse_event_worker(void *arg)
{
    (void)arg;
    mouse_worker_started = 1;
    while (1) {
        /* IRQ (producer, possibly another CPU once the device-IRQ path runs
         * BKL-free) sets this; consume it atomically so a set racing with our
         * clear is never lost. mouse_state itself is only a cursor coordinate —
         * a torn read there is cosmetic, so it is left unlocked. */
        __atomic_exchange_n(&mouse_event_pending, 0, __ATOMIC_ACQUIRE);
        scheduler_sleep_ticks(1);
    }
}

void ps2_mouse_init(void)
{
    const struct boot_info *bi = bootinfo_get();
    if (!bi->has_framebuffer) {
        return;
    }

    packet_index = 0;
    mouse_state.x = (int)(bi->framebuffer.width / 2);
    mouse_state.y = (int)(bi->framebuffer.height / 2);
    mouse_state.buttons = 0;

    /* The init handshake polls the i8042 output buffer for each command's
     * 0xFA ACK. Interrupts must stay OFF across the whole sequence: the
     * keyboard IRQ path and the timer-tick i8042 poll both drain the SAME
     * output buffer, and since mouse_ready is still 0 they would route the
     * mouse's ACK into ps2_mouse_handle_byte, which drops it — leaving our
     * poll to time out as a spurious "enable failed". (This is exactly what
     * regressed once the tick-poll keyboard fallback was added.) */
    int irqs_were_on = interrupts_enabled();
    interrupts_disable();

    ps2_drain_output();

    ps2_write_cmd(0xA8); // Enable auxiliary device (mouse)
    ps2_drain_output();  // swallow any ACK/status the enable produced
    ps2_write_cmd(0x60); // Write Command Byte
    ps2_write_data(0x47); // enable kbd+mouse IRQ, translation, system flag
    ps2_drain_output();

    /* Reset the mouse (0xFF) to put it in a known state and confirm it's
     * actually there: a present device ACKs (0xFA) then self-tests (0xAA,
     * 0x00). This is far more reliable on QEMU than jumping straight to
     * enable. */
    int present = ps2_mouse_command(0xFF);
    if (present) {
        /* Consume the BAT result (0xAA) + device id (0x00). */
        for (int i = 0; i < 4; i++) {
            int v = ps2_read_byte_to();
            if (v < 0 || v == 0xAA)
                break;
        }
        ps2_drain_output();
    }

    int defaults = ps2_mouse_command(0xF6); /* set defaults */
    int enabled = ps2_mouse_command(0xF4);  /* enable data reporting */

    if (irqs_were_on)
        interrupts_enable();

    /* QEMU often enables data reporting even when a single ACK read slips;
     * treat the device as usable if reset OR either enable step succeeded.
     * The packet-sync check in ps2_mouse_handle_byte filters any noise on a
     * machine that genuinely has no mouse. */
    if (!present && !defaults && !enabled) {
        console_write("ps2_mouse: enable failed\n");
        return;
    }

    x86_pic_unmask(12);
    (void)kthread_create("ps2-mouse-ev", ps2_mouse_event_worker, 0);
    mouse_ready = 1;
    console_write("ps2_mouse: initialized on irq12\n");
}

extern void ps2_kbd_handle_byte(u8 scancode);

void ps2_mouse_handle_byte(u8 data)
{
    if (!mouse_ready) {
        return;
    }

    if (packet_index == 0 && (data & 0x08) == 0) {
        return;
    }

    /* Defensive bound: the i8042 decode state is non-atomic across two IRQ
     * entry points plus the timer-tick poll; an unexpected re-entry could push
     * packet_index past the 3-byte buffer. Reset rather than overrun (R4-11). */
    if (packet_index >= 3)
        packet_index = 0;

    packet[packet_index++] = data;
    if (packet_index < 3) return;
    packet_index = 0;

    int dx = (int)((int8_t)packet[1]);
    int dy = (int)((int8_t)packet[2]);
    int old_x = mouse_state.x;
    int old_y = mouse_state.y;
    u8 old_buttons = mouse_state.buttons;
    mouse_state.buttons = (u8)(packet[0] & 0x07);

    mouse_state.x += dx;
    mouse_state.y -= dy;

    const struct boot_info *bi = bootinfo_get();
    int max_x = (int)bi->framebuffer.width - 1;
    int max_y = (int)bi->framebuffer.height - 1;
    if (mouse_state.x < 0) mouse_state.x = 0;
    if (mouse_state.y < 0) mouse_state.y = 0;
    if (mouse_state.x > max_x) mouse_state.x = max_x;
    if (mouse_state.y > max_y) mouse_state.y = max_y;

    if (mouse_state.x != old_x || mouse_state.y != old_y || mouse_state.buttons != old_buttons) {
        __atomic_store_n(&mouse_event_pending, 1, __ATOMIC_RELEASE);
    }

    /* M47: mirror the decoded packet to /dev/input/event1 — relative motion,
     * button edges, plus the kernel-clamped absolute cursor position. */
    if (dx)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_X, dx);
    if (dy)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_REL, B1NIX_REL_Y, -dy);
    u8 changed = (u8)(mouse_state.buttons ^ old_buttons);
    if (changed & 0x01)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_KEY, B1NIX_BTN_LEFT,
                         (mouse_state.buttons & 0x01) ? 1 : 0);
    if (changed & 0x02)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_KEY, B1NIX_BTN_RIGHT,
                         (mouse_state.buttons & 0x02) ? 1 : 0);
    if (changed & 0x04)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_KEY, B1NIX_BTN_MIDDLE,
                         (mouse_state.buttons & 0x04) ? 1 : 0);
    if (mouse_state.x != old_x)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_ABS, B1NIX_ABS_X, mouse_state.x);
    if (mouse_state.y != old_y)
        input_event_push(INPUT_DEV_MOUSE, B1NIX_EV_ABS, B1NIX_ABS_Y, mouse_state.y);
    input_event_sync(INPUT_DEV_MOUSE);
}

void ps2_mouse_interrupt_handler(void)
{
    while (1) {
        u8 status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUT_FULL)) {
            break;
        }
        u8 data = inb(PS2_DATA_PORT);
        if (status & 0x20) {
            ps2_mouse_handle_byte(data);
        } else {
            ps2_kbd_handle_byte(data);
        }
    }
}

void ps2_mouse_get_state(struct ps2_mouse_state *out)
{
    if (!out) return;
    out->x = mouse_state.x;
    out->y = mouse_state.y;
    out->buttons = mouse_state.buttons;
}
