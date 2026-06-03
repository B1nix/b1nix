#include <b1nix/arch.h>
#include <b1nix/bootinfo.h>
#include <b1nix/compositor.h>
#include <b1nix/console.h>
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

static u8 ps2_read_data(void)
{
    if (!ps2_wait_output_full()) return 0;
    return inb(PS2_DATA_PORT);
}

static void ps2_mouse_write(u8 value)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(value);
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
        if (__atomic_exchange_n(&mouse_event_pending, 0, __ATOMIC_ACQUIRE)) {
            compositor_wake();
        }
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

    // Drain any pending data in the PS/2 controller output buffer
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL) {
        (void)inb(PS2_DATA_PORT);
    }

    ps2_write_cmd(0xA8); // Enable auxiliary device (mouse)
    ps2_write_cmd(0x60); // Write Command Byte
    ps2_write_data(0x47); // 0x47: enable keyboard, mouse, translation, and system flag

    ps2_mouse_write(0xF6);
    (void)ps2_read_data();
    ps2_mouse_write(0xF4);
    u8 ack = ps2_read_data();
    if (ack != 0xFA) {
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
