#!/bin/sh
BB=/opt/busybox/bin/busybox
echo "BB-W6: start accounts"

# cryptpw: standard sha512-crypt of a known password with a fixed salt.
H=$($BB cryptpw -m sha512 -S w6salt secret)
case "$H" in
'$6$w6salt$'*) echo "BB-W6: ok cryptpw" ;;
esac

# addgroup: create a group, verify its /etc/group record.
$BB addgroup devs
$BB grep -q '^devs:' /etc/group && echo "BB-W6: ok addgroup"

# adduser: create a normal user with no password (-D), home dir, shell.
$BB adduser -D bob
$BB grep -q '^bob:' /etc/passwd && echo "BB-W6: ok adduser"
$BB grep -q '^bob:' /etc/shadow && echo "BB-W6: ok adduser-shadow"
[ -d /home/bob ] && echo "BB-W6: ok adduser-home"
BOB_UID=$($BB grep '^bob:' /etc/passwd | $BB cut -d: -f3)

# chpasswd: set bob's password (sha512). A $6$ hash must land in shadow.
echo "bob:hunter2" | $BB chpasswd -c sha512
$BB grep -q '^bob:\$6\$' /etc/shadow && echo "BB-W6: ok chpasswd"

# passwd-verify: recompute sha512-crypt('hunter2') with the stored salt and
# confirm it equals the stored hash — proves the password is verifiable.
STORED=$($BB grep '^bob:' /etc/shadow | $BB cut -d: -f2)
SALT=$(echo "$STORED" | $BB cut -d'$' -f3)
RECOMP=$($BB cryptpw -m sha512 -S "$SALT" hunter2)
[ "$STORED" = "$RECOMP" ] && echo "BB-W6: ok passwd-verify"

# su: drop from root to bob, run a command, confirm the uid switched.
SU_UID=$($BB su bob -c "$BB id -u")
[ "$SU_UID" = "$BOB_UID" ] && echo "BB-W6: ok su"

# passwd -l / -u: lock then unlock bob, observing the '!' shadow prefix.
$BB passwd -l bob >/dev/null 2>&1
$BB grep -q '^bob:!' /etc/shadow && echo "BB-W6: ok passwd-lock"
$BB passwd -u bob >/dev/null 2>&1
$BB grep -q '^bob:!' /etc/shadow || echo "BB-W6: ok passwd-unlock"

# login/getty: present & dispatchable. A full session needs a dedicated tty
# and PID 1 stays with B1NIX, so this asserts the applet links and selects.
$BB --list | $BB grep -q '^login$' && echo "BB-W6: ok login-applet"
$BB --list | $BB grep -q '^getty$' && echo "BB-W6: ok getty-applet"

# deluser/delgroup: tear the user and group back down, verify removal.
$BB deluser bob
$BB grep -q '^bob:' /etc/passwd || echo "BB-W6: ok deluser"
$BB grep -q '^bob:' /etc/shadow || echo "BB-W6: ok deluser-shadow"
$BB delgroup devs
$BB grep -q '^devs:' /etc/group || echo "BB-W6: ok delgroup"

echo "BB-W6: done"
