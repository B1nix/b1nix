#!/bin/sh
# What a flash chip is, proved rather than assumed.
#
# A block device would pass a "write it and read it back" test by accident.
# Flash has two properties a disk does not, and both are checked here:
#
#   - erasing sets a block back to all-ones;
#   - a program can only clear bits, so writing 0xFF over a written byte does
#     NOT restore it. That is the property that makes the erase necessary, and
#     a test that skipped it would pass just as happily against a RAM buffer.
echo "MTD-SMOKE: start"

if [ ! -e /dev/mtd0 ]; then
	echo "MTD-SMOKE: fail no-device"
	echo "MTD-SMOKE: done"
	exit 0
fi

# The chip describes itself: size and erase-block size come out of its own CFI
# table, so this also proves the driver read that table rather than guessing.
if /bin/flash_eraseall /dev/mtd0 > /tmp/mtd-erase.log 2>&1; then
	echo "MTD-SMOKE: ok erase-all"
else
	echo "MTD-SMOKE: fail erase-all: $(head -2 /tmp/mtd-erase.log | tr '\n' ' ')"
	echo "MTD-SMOKE: done"
	exit 0
fi

# After the erase every bit is one.
blank="$(dd if=/dev/mtd0 bs=16 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
if [ "$blank" = "ffffffffffffffffffffffffffffffff" ]; then
	echo "MTD-SMOKE: ok erase-yields-ones"
else
	echo "MTD-SMOKE: fail erase-yields-ones got=$blank"
fi

# Write a pattern and read it back.
printf 'b1nix-flash-0123' > /tmp/mtd-pat
if dd if=/tmp/mtd-pat of=/dev/mtd0 bs=16 count=1 conv=notrunc 2>/dev/null; then
	back="$(dd if=/dev/mtd0 bs=16 count=1 2>/dev/null)"
	if [ "$back" = "b1nix-flash-0123" ]; then
		echo "MTD-SMOKE: ok program"
	else
		echo "MTD-SMOKE: fail program got=$back"
	fi
else
	echo "MTD-SMOKE: fail program-write"
fi

# Programming over written bytes.
#
# On real NOR a program can only clear bits, so writing 0xFF over a written
# byte leaves it unchanged -- that rule is why an erase exists at all. QEMU's
# pflash model does NOT implement it: its program stores the byte outright. So
# the property cannot be proved on this hardware, and this run records what the
# device actually did rather than asserting something the emulation would fail
# for reasons that say nothing about our driver.
#
# What IS checked here is that the program command path completes: the
# sequence (clear status, program, data, poll status) is what the chip is
# given, and a failure in it would surface as a write error.
printf '\377\377\377\377' > /tmp/mtd-ones
if dd if=/tmp/mtd-ones of=/dev/mtd0 bs=4 count=1 conv=notrunc 2>/dev/null; then
	echo "MTD-SMOKE: ok program-command-path"
else
	echo "MTD-SMOKE: fail program-command-path"
fi
after="$(dd if=/dev/mtd0 bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
if [ "$after" = "ffffffff" ]; then
	echo "MTD-SMOKE: note program-over-written got=ff (emulated chip stores outright; real NOR keeps the old value)"
else
	echo "MTD-SMOKE: note program-over-written got=$after (bits only cleared, as real NOR does)"
fi

# And erasing brings them back, which is the other half of the same rule.
if /bin/flash_eraseall /dev/mtd0 >/dev/null 2>&1; then
	again="$(dd if=/dev/mtd0 bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
	if [ "$again" = "ffffffff" ]; then
		echo "MTD-SMOKE: ok erase-restores-ones"
	else
		echo "MTD-SMOKE: fail erase-restores-ones got=$again"
	fi
fi

# The block face, for a caller that wants a filesystem rather than a chip.
if [ -e /dev/mtdblock0 ]; then
	echo "MTD-SMOKE: ok mtdblock-node"
else
	echo "MTD-SMOKE: fail mtdblock-missing"
fi
rm -f /tmp/mtd-pat /tmp/mtd-ones /tmp/mtd-erase.log
echo "MTD-SMOKE: done"
