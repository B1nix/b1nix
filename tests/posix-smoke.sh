#!/bin/sh
# POSIX Smoke Test for B1NIX
echo "POSIX-SMOKE: start"

# 1. Test basename/dirname
basename /tmp/a/b
dirname /tmp/a/b

# 2. Test touch and [ -f ]
touch /tmp/posix_test
[ -f /tmp/posix_test ] && echo "ok touch" || echo "fail touch"

# 3. Test printf
printf "hello %s\n" world

# 4. Test shell operators && and ||
true && echo "ok and" || echo "fail and"
false || echo "ok or" && echo "ok or 2"

# 5. Test mkdir -p
mkdir -p /tmp/a/b/c
[ -d /tmp/a/b/c ] && echo "ok mkdir-p" || echo "fail mkdir-p"

# 6. Test shell quoting and splitting
echo "hello; world" | grep ";" && echo "ok quote split"
echo 'single; quote' | grep ";" && echo "ok quote split 2"

# 7. Test rm -rf
rm -rf /tmp/a

# 8. Test symlink loop (VFS hardening)
ln -s /tmp/loop /tmp/loop
cat /tmp/loop 2>&1 | grep "Too many levels" && echo "ok ELOOP"

echo "POSIX-SMOKE: done"
