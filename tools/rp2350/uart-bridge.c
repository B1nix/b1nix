// SPDX-License-Identifier: MIT
/*
 * Copyright 2021 Álvaro Fernández Rojas <noltari@gmail.com>
 * Enhanced for b1nix RP2350 / RP2040 HIL Test Controller & Multi-State LED Indication
 */

#include <hardware/irq.h>
#include <hardware/structs/sio.h>
#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <pico/multicore.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <string.h>
#include <tusb.h>

#if !defined(MIN)
#define MIN(a, b) ((a > b) ? b : a)
#endif /* MIN */

#ifndef PICO_DEFAULT_LED_PIN
#define LED_PIN 25
#else
#define LED_PIN PICO_DEFAULT_LED_PIN
#endif

/* Hardware Pinout for Raspberry Pi 4 HIL */
#define RESET_PIN           22  /* GP22: Target Hardware Reset (Active LOW) */
#define RESET_PIN_SIDE       2  /* GP2: Convenient side header pin on Waveshare USB-A / Zero */
#define POWER_SENSE_PIN     21  /* GP21: Target 3.3V Power Sense */
#define POWER_SENSE_SIDE     3  /* GP3: Convenient side header pin on Waveshare USB-A / Zero */


#define BUFFER_SIZE 2560

#define DEF_BIT_RATE 115200
#define DEF_STOP_BITS 1
#define DEF_PARITY 0
#define DEF_DATA_BITS 8

typedef struct {
	uart_inst_t *const inst;
	uint irq;
	void *irq_fn;
	uint8_t tx_pin;
	uint8_t rx_pin;
} uart_id_t;

typedef struct {
	cdc_line_coding_t usb_lc;
	cdc_line_coding_t uart_lc;
	mutex_t lc_mtx;
	uint8_t uart_buffer[BUFFER_SIZE];
	uint32_t uart_pos;
	mutex_t uart_mtx;
	uint8_t usb_buffer[BUFFER_SIZE];
	uint32_t usb_pos;
	mutex_t usb_mtx;
} uart_data_t;

void uart0_irq_fn(void);
void uart1_irq_fn(void);

/* Default Pinmux: UART0 on GP0/GP1 (RPi4 primary console), UART1 on GP4/GP5 */
const uart_id_t UART_ID[CFG_TUD_CDC] = {
	{
		.inst = uart0,
		.irq = UART0_IRQ,
		.irq_fn = &uart0_irq_fn,
		.tx_pin = 0,    /* GP0 -> RPi4 Pin 10 (GPIO 15 / RXD) */
		.rx_pin = 1,    /* GP1 -> RPi4 Pin 8  (GPIO 14 / TXD) */
	}, {
		.inst = uart1,
		.irq = UART1_IRQ,
		.irq_fn = &uart1_irq_fn,
		.tx_pin = 4,    /* GP4 (Auxiliary UART1 TX) */
		.rx_pin = 5,    /* GP5 (Auxiliary UART1 RX) */
	}
};

uart_data_t UART_DATA[CFG_TUD_CDC];

/* Activity & Status Tracking */
static volatile uint32_t g_traffic_counter = 0;
static volatile uint32_t g_last_traffic_counter = 0;
static volatile uint32_t g_traffic_active_until_ms = 0;
static volatile bool g_usb_connected = false;
static volatile bool g_reset_pulse_active = false;
static volatile uint32_t g_reset_until_ms = 0;

static const char CMD_RESET[] = "@@RESET@@";
static char g_cmd_buf[16];
static size_t g_cmd_idx = 0;

static inline uint databits_usb2uart(uint8_t data_bits)
{
	switch (data_bits) {
		case 5:  return 5;
		case 6:  return 6;
		case 7:  return 7;
		default: return 8;
	}
}

static inline uart_parity_t parity_usb2uart(uint8_t usb_parity)
{
	switch (usb_parity) {
		case 1:  return UART_PARITY_ODD;
		case 2:  return UART_PARITY_EVEN;
		default: return UART_PARITY_NONE;
	}
}

static inline uint stopbits_usb2uart(uint8_t stop_bits)
{
	switch (stop_bits) {
		case 2:  return 2;
		default: return 1;
	}
}

static inline void set_reset_line(bool assert_reset)
{
	if (assert_reset) {
		/* Pull LOW to GND during active reset */
		gpio_set_dir(RESET_PIN, GPIO_OUT);
		gpio_put(RESET_PIN, 0);
		gpio_set_dir(RESET_PIN_SIDE, GPIO_OUT);
		gpio_put(RESET_PIN_SIDE, 0);
	} else {
		/* High-Z (Open-Drain): let target pull itself up and run freely */
		gpio_set_dir(RESET_PIN, GPIO_IN);
		gpio_disable_pulls(RESET_PIN);
		gpio_set_dir(RESET_PIN_SIDE, GPIO_IN);
		gpio_disable_pulls(RESET_PIN_SIDE);
	}
}

void trigger_hardware_reset(void)
{
	set_reset_line(true);
	g_reset_pulse_active = true;
	g_reset_until_ms = to_ms_since_boot(get_absolute_time()) + 100;
}

void update_hardware_reset_state(uint32_t now_ms)
{
	if (g_reset_pulse_active && now_ms >= g_reset_until_ms) {
		set_reset_line(false);
		g_reset_pulse_active = false;
	}
}

void update_uart_cfg(uint8_t itf)
{
	const uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	mutex_enter_blocking(&ud->lc_mtx);

	if (ud->usb_lc.bit_rate != ud->uart_lc.bit_rate) {
		uart_set_baudrate(ui->inst, ud->usb_lc.bit_rate);
		ud->uart_lc.bit_rate = ud->usb_lc.bit_rate;
	}

	if ((ud->usb_lc.stop_bits != ud->uart_lc.stop_bits) ||
	    (ud->usb_lc.parity != ud->uart_lc.parity) ||
	    (ud->usb_lc.data_bits != ud->uart_lc.data_bits)) {
		uart_set_format(ui->inst,
				databits_usb2uart(ud->usb_lc.data_bits),
				stopbits_usb2uart(ud->usb_lc.stop_bits),
				parity_usb2uart(ud->usb_lc.parity));
		ud->uart_lc.data_bits = ud->usb_lc.data_bits;
		ud->uart_lc.parity = ud->usb_lc.parity;
		ud->uart_lc.stop_bits = ud->usb_lc.stop_bits;
	}

	mutex_exit(&ud->lc_mtx);
}

void usb_read_bytes(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];
	uint32_t len = tud_cdc_n_available(itf);

	if (len && mutex_try_enter(&ud->usb_mtx, NULL)) {
		len = MIN(len, BUFFER_SIZE - ud->usb_pos);
		if (len) {
			uint32_t count = tud_cdc_n_read(itf, &ud->usb_buffer[ud->usb_pos], len);
			
			/* Check for in-band @@RESET@@ command on interface 0 */
			if (itf == 0 && count > 0) {
				for (uint32_t i = 0; i < count; i++) {
					char ch = (char)ud->usb_buffer[ud->usb_pos + i];
					g_cmd_buf[g_cmd_idx++] = ch;
					if (g_cmd_idx >= sizeof(g_cmd_buf)) {
						memmove(g_cmd_buf, g_cmd_buf + 1, sizeof(g_cmd_buf) - 1);
						g_cmd_idx = sizeof(g_cmd_buf) - 1;
					}
					if (strstr(g_cmd_buf, CMD_RESET) != NULL) {
						trigger_hardware_reset();
						g_cmd_idx = 0;
						memset(g_cmd_buf, 0, sizeof(g_cmd_buf));
					}
				}
			}

			ud->usb_pos += count;
			g_traffic_counter += count;
		}
		mutex_exit(&ud->usb_mtx);
	}
}

void usb_write_bytes(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	if (ud->uart_pos && mutex_try_enter(&ud->uart_mtx, NULL)) {
		uint32_t count = tud_cdc_n_write(itf, ud->uart_buffer, ud->uart_pos);
		if (count < ud->uart_pos)
			memmove(ud->uart_buffer, &ud->uart_buffer[count], ud->uart_pos - count);
		ud->uart_pos -= count;

		mutex_exit(&ud->uart_mtx);

		if (count)
			tud_cdc_n_write_flush(itf);
	}
}

void usb_cdc_process(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	mutex_enter_blocking(&ud->lc_mtx);
	tud_cdc_n_get_line_coding(itf, &ud->usb_lc);
	mutex_exit(&ud->lc_mtx);

	usb_read_bytes(itf);
	usb_write_bytes(itf);
}

#include "led.h"

/* ==============================================================================
 * Multi-State LED Indication:
 * - LED_STATE_BOOTING : Power-on
 * - LED_STATE_IDLE    : Standby (Amber / Breathing)
 * - LED_STATE_SUCCESS : Terminal Open & Connected (Green / Solid ON)
 * - LED_STATE_RUNNING : Active UART RX/TX Data (Blue / Fast Blink)
 * - LED_STATE_ERROR   : Target Hardware Reset Active (Red)
 * ============================================================================== */
void update_led_state(uint32_t now_ms)
{
	static int s_last_state = -1;
	int current_state;

	/* 1. Check for active UART traffic */
	if (g_traffic_counter != g_last_traffic_counter) {
		g_last_traffic_counter = g_traffic_counter;
		g_traffic_active_until_ms = now_ms + 100;
	}

	/* State A: Hardware Reset Active */
	if (g_reset_pulse_active) {
		current_state = LED_STATE_ERROR;
	}
	/* State B: Active UART Data Streaming */
	else if (now_ms < g_traffic_active_until_ms) {
		current_state = LED_STATE_RUNNING;
	}
	/* State C: USB Host Terminal Open */
	else if (g_usb_connected) {
		current_state = LED_STATE_SUCCESS;
	}
	/* State D: Idle / Standby */
	else {
		current_state = LED_STATE_IDLE;
	}

	if (current_state != s_last_state) {
		s_last_state = current_state;
		led_set_state(current_state);
	}
}

void core1_entry(void)
{
	tusb_init();

	while (1) {
		int itf;
		bool any_con = false;

		tud_task();

		for (itf = 0; itf < CFG_TUD_CDC; itf++) {
			if (tud_cdc_n_connected(itf)) {
				any_con = true;
				usb_cdc_process(itf);
			}
		}

		g_usb_connected = any_con;
		uint32_t now_ms = to_ms_since_boot(get_absolute_time());
		update_hardware_reset_state(now_ms);
		update_led_state(now_ms);
	}
}

static inline void uart_read_bytes(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];
	const uart_id_t *ui = &UART_ID[itf];

	if (uart_is_readable(ui->inst)) {
		mutex_enter_blocking(&ud->uart_mtx);

		while (uart_is_readable(ui->inst) && (ud->uart_pos < BUFFER_SIZE)) {
			ud->uart_buffer[ud->uart_pos] = uart_getc(ui->inst);
			ud->uart_pos++;
			g_traffic_counter++;
		}

		mutex_exit(&ud->uart_mtx);
	}
}

void uart0_irq_fn(void)
{
	uart_read_bytes(0);
}

void uart1_irq_fn(void)
{
	uart_read_bytes(1);
}

void uart_write_bytes(uint8_t itf)
{
	uart_data_t *ud = &UART_DATA[itf];

	if (ud->usb_pos && mutex_try_enter(&ud->usb_mtx, NULL)) {
		const uart_id_t *ui = &UART_ID[itf];
		uint32_t count = 0;

		while (uart_is_writable(ui->inst) && count < ud->usb_pos) {
			uart_putc_raw(ui->inst, ud->usb_buffer[count]);
			count++;
			g_traffic_counter++;
		}

		if (count < ud->usb_pos)
			memmove(ud->usb_buffer, &ud->usb_buffer[count], ud->usb_pos - count);
		ud->usb_pos -= count;

		mutex_exit(&ud->usb_mtx);
	}
}

void init_uart_data(uint8_t itf)
{
	const uart_id_t *ui = &UART_ID[itf];
	uart_data_t *ud = &UART_DATA[itf];

	/* Pinmux */
	gpio_set_function(ui->tx_pin, GPIO_FUNC_UART);
	gpio_set_function(ui->rx_pin, GPIO_FUNC_UART);
	gpio_pull_up(ui->rx_pin);

	/* USB CDC LC */
	ud->usb_lc.bit_rate = DEF_BIT_RATE;
	ud->usb_lc.data_bits = DEF_DATA_BITS;
	ud->usb_lc.parity = DEF_PARITY;
	ud->usb_lc.stop_bits = DEF_STOP_BITS;

	/* UART LC */
	ud->uart_lc.bit_rate = DEF_BIT_RATE;
	ud->uart_lc.data_bits = DEF_DATA_BITS;
	ud->uart_lc.parity = DEF_PARITY;
	ud->uart_lc.stop_bits = DEF_STOP_BITS;

	/* Buffer */
	ud->uart_pos = 0;
	ud->usb_pos = 0;

	/* Mutex */
	mutex_init(&ud->lc_mtx);
	mutex_init(&ud->uart_mtx);
	mutex_init(&ud->usb_mtx);

	/* UART start */
	uart_init(ui->inst, ud->usb_lc.bit_rate);
	uart_set_hw_flow(ui->inst, false, false);
	uart_set_format(ui->inst, databits_usb2uart(ud->usb_lc.data_bits),
			stopbits_usb2uart(ud->usb_lc.stop_bits),
			parity_usb2uart(ud->usb_lc.parity));
	uart_set_fifo_enabled(ui->inst, true);

	/* UART RX Interrupt */
	irq_set_exclusive_handler(ui->irq, ui->irq_fn);
	irq_set_enabled(ui->irq, true);
	uart_set_irq_enables(ui->inst, true, false);
}

int main(void)
{
	int itf;

	usbd_serial_init();

	/* Initialize board-specific LED system (NeoPixel / RGB PWM / Single LED) */
	led_init();
	led_set_state(LED_STATE_BOOTING);

	/* Configure Target Hardware Reset Pins as Open-Drain (Default: High-Z / Input) */
	gpio_init(RESET_PIN);
	gpio_init(RESET_PIN_SIDE);
	set_reset_line(false);

	/* Configure Target Power Sense Pins (GP21 & GP3: Input with Pull-Down) */
	gpio_init(POWER_SENSE_PIN);
	gpio_set_dir(POWER_SENSE_PIN, GPIO_IN);
	gpio_pull_down(POWER_SENSE_PIN);
	gpio_init(POWER_SENSE_SIDE);
	gpio_set_dir(POWER_SENSE_SIDE, GPIO_IN);
	gpio_pull_down(POWER_SENSE_SIDE);

	for (itf = 0; itf < CFG_TUD_CDC; itf++)
		init_uart_data(itf);

	multicore_launch_core1(core1_entry);

	while (1) {
		for (itf = 0; itf < CFG_TUD_CDC; itf++) {
			update_uart_cfg(itf);
			uart_write_bytes(itf);
		}
	}

	return 0;
}
