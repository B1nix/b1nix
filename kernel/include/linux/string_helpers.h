/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_STRING_HELPERS_H
#define LKPI_LINUX_STRING_HELPERS_H
#include <linux/types.h>
static inline const char *str_yes_no(bool v) { return v ? "yes" : "no"; }
static inline const char *str_on_off(bool v) { return v ? "on" : "off"; }
static inline const char *str_enabled_disabled(bool v)
{ return v ? "enabled" : "disabled"; }
static inline const char *str_enable_disable(bool v)
{ return v ? "enable" : "disable"; }
#endif
