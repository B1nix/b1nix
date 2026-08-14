/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LKPI_LINUX_PWM_H
#define LKPI_LINUX_PWM_H
#include <linux/err.h>
#include <linux/types.h>
/*
 * PWM backlight control, used on the SoC parts (Bay Trail, Cherry Trail) where
 * the panel's brightness line hangs off the platform controller rather than the
 * display engine. The Gen8/Gen9.5 parts this driver is cut to drive it natively,
 * so there is no PWM provider here and lookups fail — which routes the driver to
 * its native path rather than to a controller that would not answer.
 */
struct pwm_device;
struct pwm_state { u64 period; u64 duty_cycle; bool enabled; };
enum pwm_polarity { PWM_POLARITY_NORMAL, PWM_POLARITY_INVERSED };

static inline struct pwm_device *pwm_get(struct device *dev, const char *id)
{ (void)dev; (void)id; return ERR_PTR(-ENODEV); }
static inline void pwm_put(struct pwm_device *pwm) { (void)pwm; }
static inline int pwm_apply_might_sleep(struct pwm_device *pwm, const struct pwm_state *s)
{ (void)pwm; (void)s; return -ENODEV; }
static inline void pwm_init_state(const struct pwm_device *pwm, struct pwm_state *s)
{ (void)pwm; if (s) { s->period = 0; s->duty_cycle = 0; s->enabled = false; } }
static inline void pwm_get_state(const struct pwm_device *pwm, struct pwm_state *s)
{ pwm_init_state(pwm, s); }
static inline u64 pwm_get_relative_duty_cycle(const struct pwm_state *s, unsigned int scale)
{ return (s && s->period) ? (s->duty_cycle * scale) / s->period : 0; }
static inline int pwm_set_relative_duty_cycle(struct pwm_state *s, u64 duty, u64 scale)
{ if (!s || !scale) return -EINVAL; s->duty_cycle = (s->period * duty) / scale; return 0; }

/* The older spelling of pwm_apply_might_sleep. Same answer: no provider here. */
static inline int pwm_apply_state(struct pwm_device *pwm, const struct pwm_state *s)
{ return pwm_apply_might_sleep(pwm, s); }


static inline bool pwm_is_enabled(const struct pwm_device *pwm)
{ (void)pwm; return false; }

#endif
