/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_INPUT_H
#define LKPI_LINUX_INPUT_H
/* Input event codes, for the hotkey and lid reporting a display driver does.
 * b1nix has its own input stack (M47) with its own device model, so nothing
 * imported registers here; only the codes are needed. */
#define KEY_BRIGHTNESSUP   225
#define KEY_BRIGHTNESSDOWN 224
#define SW_LID             0x00
#endif
