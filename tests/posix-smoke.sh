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

# 6. Test rm -rf
rm -rf /tmp/a

echo "POSIX-SMOKE: done"
