#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -e

echo "[*] Checking Fastboot device..."
fastboot devices

echo "[*] Restoring default slot to B (Normal Android / LineageOS)..."
fastboot --set-active=b

echo "[*] Rebooting into normal Android..."
fastboot reboot

echo "[+] Successfully restored default slot B!"
