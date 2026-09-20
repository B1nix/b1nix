/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

void led_init(void);

enum {
    LED_STATE_BOOTING = 0,
    LED_STATE_IDLE,
    LED_STATE_RUNNING,
    LED_STATE_SUCCESS,
    LED_STATE_ERROR
};

void led_set_state(int state);
