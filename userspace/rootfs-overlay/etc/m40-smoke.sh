#!/bin/sh
# M40: static Linux x86_64 ABI-compat ELF smoke. The binary itself prints
# every M40-LINUX: ok/fail marker; this wrapper only brackets it with
# start/done so the smoke harness can tell "never ran" from "ran, some
# checks failed" (mirrors bash-smoke.sh's own self-bracketing).
echo "M40-LINUX: start"
if [ -x /bin/m40-linux-hello ]; then
  /bin/m40-linux-hello
  [ $? -eq 0 ] && echo "M40-LINUX: ok run-static"
fi
if [ -x /bin/m40-linux-abi ]; then
  /bin/m40-linux-abi
fi
echo "M40-LINUX: done"
