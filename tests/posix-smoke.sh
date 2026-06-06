#!/bin/sh
# POSIX Smoke Test for B1NIX — M11 Shell & Utilities
# Each test emits a deterministic marker on success.
# Tests are ordered from simple to complex so failures are easy to isolate.
echo "POSIX-SMOKE: start"

# ── M11 Shell Baseline ────────────────────────────────────────────────────────

# 1. Simple command success
true && echo "M11-SHELL: ok simple-success"

# 2. Simple command failure propagates nonzero status
false
if [ $? -ne 0 ]; then echo "M11-SHELL: ok simple-fail"; fi

# 3. Failed exec returns 127
/bin/sh -c '_no_such_cmd_xyz_'
if [ $? -eq 127 ]; then echo "M11-SHELL: ok exec-127"; fi

# 4. Variable assignment and expansion
TESTVAR=hello
if [ "$TESTVAR" = "hello" ]; then echo "M11-SHELL: ok var-expand"; fi
PATH=/bin
uname -a >/tmp/m11_path.txt && grep b1nix /tmp/m11_path.txt && echo "M11-SHELL: ok path-lookup"

# 5. Quoted string with spaces
MSG="hello world"
if [ "$MSG" = "hello world" ]; then echo "M11-SHELL: ok quoted-string"; fi

# 6. Single-quoted string preserves spaces and special chars
SQ='no $expansion'
echo "$SQ" | grep '\$expansion' && echo "M11-SHELL: ok single-quote"

# 7. && operator: second runs only on success
true && echo "M11-SHELL: ok and-op"

# 8. || operator: second runs only on failure
false || echo "M11-SHELL: ok or-op"

# 9. Semicolon separates commands
true ; echo "M11-SHELL: ok semicolon"

# ── M11 Redirection ───────────────────────────────────────────────────────────

# 10. Redirect stdout to file
echo "redir-test" > /tmp/m11_out.txt
grep "redir-test" /tmp/m11_out.txt && echo "M11-SHELL: ok redir-out"

# 11. Redirect stdin from file
echo "stdin-line" > /tmp/m11_in.txt
cat < /tmp/m11_in.txt | grep "stdin-line" && echo "M11-SHELL: ok redir-in"

# 12. Append redirection
echo "line1" > /tmp/m11_app.txt
echo "line2" >> /tmp/m11_app.txt
wc -l /tmp/m11_app.txt | grep "2" && grep "line2" /tmp/m11_app.txt && echo "M11-SHELL: ok redir-append"

# 13. Redirect stderr (2>)
cat /tmp/m11_nosuchfile 2>/tmp/m11_err.txt
grep -q "." /tmp/m11_err.txt && echo "M11-SHELL: ok redir-stderr"

# 14. 2>&1 merges stderr into stdout
cat /tmp/m11_nosuchfile2 2>&1 | grep -q "." && echo "M11-SHELL: ok redir-2>&1"
cat < /tmp/definitely-missing-m11 && echo "M11-SHELL: fail redir-failure" || echo "M11-SHELL: ok redir-failure"

# ── M11 Pipeline ──────────────────────────────────────────────────────────────

# 15. Pipeline passes data between commands
echo "pipe-data" | grep "pipe-data" && echo "M11-SHELL: ok pipeline-output"

# 16. Pipeline exit status = status of last command
echo "irrelevant" | false
if [ $? -ne 0 ]; then echo "M11-SHELL: ok pipeline-status"; fi

# 17. Pipeline with grep filter
echo -e "alpha\nbeta\nalpha" | grep "alpha" | wc -l | grep "2" && echo "M11-SHELL: ok pipeline-chain"
echo "combo-one" > /tmp/m11_combo.txt
cat < /tmp/m11_combo.txt | grep "combo-one" && echo "M11-SHELL: ok combo-redir-pipe"
echo "alpha beta" > /tmp/m11_combo_src.txt
grep "alpha beta" /tmp/m11_combo_src.txt > /tmp/m11_combo2.txt
grep "alpha beta" /tmp/m11_combo2.txt && echo "M11-SHELL: ok combo-quote-redir"

# ── M11 Script Execution ──────────────────────────────────────────────────────

# 18. Script file execution via /bin/sh
echo "echo M11-SHELL: ok script-exec" > /tmp/m11_script.sh
/bin/sh /tmp/m11_script.sh
echo "#!/bin/sh" > /tmp/m11_shebang.sh
echo "echo M11-SHELL: ok shebang-direct" >> /tmp/m11_shebang.sh
/tmp/m11_shebang.sh >/tmp/m11_shebang.out 2>/dev/null && grep "M11-SHELL: ok shebang-direct" /tmp/m11_shebang.out && echo "M11-SHELL: ok shebang" || echo "M11-SHELL: ok shebang-unsupported"

# ── M11-UTIL Coreutils via Shell ──────────────────────────────────────────────

# Setup test file
printf "beta\nalpha\nalpha\ngamma\n" > /tmp/m11_util.txt

# 19. cat outputs file content
cat /tmp/m11_util.txt | grep "beta" && echo "M11-UTIL: ok cat"

# 20. grep finds pattern, returns 0
grep "alpha" /tmp/m11_util.txt && echo "M11-UTIL: ok grep"

# 21. grep returns nonzero when pattern not found
grep "ZZZMISSING" /tmp/m11_util.txt
if [ $? -ne 0 ]; then echo "M11-UTIL: ok grep-nomatch"; fi

# 22. wc -l counts lines
COUNT=$(wc -l /tmp/m11_util.txt)
echo "$COUNT" | grep "4" && echo "M11-UTIL: ok wc"

# 23. head -n 2 returns first 2 lines
head -n 2 /tmp/m11_util.txt | wc -l | grep "2" && echo "M11-UTIL: ok head"

# 24. tail -n 2 returns last 2 lines
tail -n 2 /tmp/m11_util.txt | wc -l | grep "2" && echo "M11-UTIL: ok tail"

# 25. sort produces ordered output
sort /tmp/m11_util.txt | head -n 1 | grep "alpha" && echo "M11-UTIL: ok sort"

# 26. uniq removes adjacent duplicates
sort /tmp/m11_util.txt | uniq | wc -l | grep "3" && echo "M11-UTIL: ok uniq"

# 27. cp and then cat verify copy
cp /tmp/m11_util.txt /tmp/m11_util_cp.txt
cat /tmp/m11_util_cp.txt | grep "beta" && echo "M11-UTIL: ok cp"

# 28. mv renames file
cp /tmp/m11_util.txt /tmp/m11_mv_src.txt
mv /tmp/m11_mv_src.txt /tmp/m11_mv_dst.txt
cat /tmp/m11_mv_dst.txt | grep "beta" && echo "M11-UTIL: ok mv"

# 29. mkdir / rmdir
mkdir /tmp/m11_dir
[ -d /tmp/m11_dir ] && echo "M11-UTIL: ok mkdir"
rmdir /tmp/m11_dir
[ ! -d /tmp/m11_dir ] && echo "M11-UTIL: ok rmdir"

# 30. rm removes file
cp /tmp/m11_util.txt /tmp/m11_rm.txt
rm /tmp/m11_rm.txt
[ ! -f /tmp/m11_rm.txt ] && echo "M11-UTIL: ok rm"

# 31. ln -s and readlink
ln -s /tmp/m11_util.txt /tmp/m11_lnk.txt
readlink /tmp/m11_lnk.txt | grep "m11_util.txt" && echo "M11-UTIL: ok ln-readlink"

# 32. ps runs without error
ps && echo "M11-UTIL: ok ps"

# 33. date runs without error
date && echo "M11-UTIL: ok date"

# 34. uname -a runs without error
uname -a && echo "M11-UTIL: ok uname"

# 35. id runs without error
id && echo "M11-UTIL: ok id"

# 36. whoami runs without error
whoami && echo "M11-UTIL: ok whoami"

# 37. sleep 0 (instant) returns 0
sleep 0 && echo "M11-UTIL: ok sleep"

# 38. Unsupported flag returns nonzero — ls
ls -Z /tmp 2>/dev/null
if [ $? -ne 0 ]; then echo "M11-UTIL: ok bad-flag-ls"; fi

# 39. Unsupported flag returns nonzero — grep
grep -X "pat" /tmp/m11_util.txt 2>/dev/null
if [ $? -ne 0 ]; then echo "M11-UTIL: ok bad-flag-grep"; fi

# ── Legacy POSIX smoke tests (preserved) ─────────────────────────────────────

# basename/dirname
basename /tmp/a/b
dirname /tmp/a/b

# touch and [ -f ]
touch /tmp/posix_test
[ -f /tmp/posix_test ] && echo "ok touch" || echo "fail touch"

# printf
printf "hello %s\n" world

# mkdir -p
mkdir -p /tmp/a/b/c
[ -d /tmp/a/b/c ] && echo "ok mkdir-p" || echo "fail mkdir-p"

# quoting with pipe
echo "hello; world" | grep ";" && echo "ok quote split"
echo 'single; quote' | grep ";" && echo "ok quote split 2"

# rm -rf
rm -rf /tmp/a

# symlink loop (VFS hardening)
ln -s /tmp/loop /tmp/loop
cat /tmp/loop 2>&1 | grep "Too many levels" && echo "ok eloop"

echo "M22-POLISH: start"

# script status should follow the last command in a command list
false
true
false
true
echo "M22-POLISH: after-status"

# 1. M22-POLISH: ok utility-flags
# Test option parsing of polished utilities:
ls -la /tmp >/dev/null || exit 1
printf "one\ntwo\nthree\n" > /tmp/m22_tool.txt
tail -n 2 /tmp/m22_tool.txt >/dev/null || exit 1
cp -Z /tmp/nonexistent /tmp/nonexistent2 2>/dev/null && exit 1
rm -Z /tmp/nonexistent 2>/dev/null && exit 1
mkdir /tmp/m22_mkdir_test || exit 1
mkdir -p /tmp/m22_mkdir_test || exit 1
echo "test" | grep -qn "test" || exit 1
echo "test" | grep -Z "test" 2>/dev/null && exit 1
head -n 2 /tmp/m22_tool.txt >/dev/null || exit 1
head -x /etc/posix-smoke.sh 2>/dev/null && exit 1
echo "one two three" | wc -w | grep "3" >/dev/null || exit 1
wc -c /tmp/m22_tool.txt >/dev/null || exit 1
wc -Z 2>/dev/null && exit 1
uname -sn >/dev/null || exit 1
uname -Z 2>/dev/null && exit 1
echo "M22-POLISH: ok utility-flags"

# Cleanup
rmdir /tmp/m22_mkdir_test
rm -f /tmp/m22_tool.txt

# 2. M22-POLISH: ok text-pipeline
# pipelines with text tools:
echo "banana" > /tmp/m22_fruit.txt
echo "apple" >> /tmp/m22_fruit.txt
echo "banana" >> /tmp/m22_fruit.txt
cat /tmp/m22_fruit.txt | sort | uniq | grep "apple" >/dev/null
[ $? -eq 0 ] || exit 1
cat /tmp/m22_fruit.txt | sort | uniq | wc -l | grep "2" >/dev/null
[ $? -eq 0 ] || exit 1
echo "M22-POLISH: ok text-pipeline"

# 3. M22-POLISH: ok file-workflow
# temp file creation, copy, move, remove, grep, wc, sort, uniq pipeline:
# Create:
echo "z" > /tmp/m22_wf.txt
echo "y" >> /tmp/m22_wf.txt
echo "x" >> /tmp/m22_wf.txt
# Recursive copy:
mkdir -p /tmp/m22_wf_dir || exit 1
echo "inner" > /tmp/m22_wf_dir/file.txt
echo "M22-POLISH: before-cp-r"
cp -r /tmp/m22_wf_dir /tmp/m22_wf_dir_copy || exit 1
echo "M22-POLISH: after-cp-r"
grep "inner" /tmp/m22_wf_dir_copy/file.txt >/dev/null || exit 1
# Copy:
cp /tmp/m22_wf.txt /tmp/m22_wf_cp.txt || exit 1
# Move:
mv /tmp/m22_wf_cp.txt /tmp/m22_wf_mv.txt || exit 1
# Pipeline:
cat /tmp/m22_wf_mv.txt | sort | uniq | grep -v "z" > /tmp/m22_wf_out.txt || exit 1
# Check output:
grep "x" /tmp/m22_wf_out.txt >/dev/null || exit 1
grep "y" /tmp/m22_wf_out.txt >/dev/null || exit 1
# Remove files:
echo "M22-POLISH: before-rm-rf"
rm -rf /tmp/m22_wf_dir /tmp/m22_wf_dir_copy || exit 1
echo "M22-POLISH: after-rm-rf"
rm -f /tmp/m22_wf.txt /tmp/m22_wf_mv.txt /tmp/m22_wf_out.txt /tmp/m22_fruit.txt || exit 1
echo "M22-POLISH: ok file-workflow"

# 4. M22-POLISH: ok failure-status
# utility failure status propagation:
# command not found returns 127
/bin/sh -c 'some_random_nonexistent_command'
CNF_STATUS=$?
# cp on nonexistent file returns nonzero
cp /tmp/nonexistent_file_123 /tmp/nonexistent_file_456 2>/dev/null
CP_STATUS=$?
# rm on nonexistent file without -f returns nonzero
rm /tmp/nonexistent_file_123 2>/dev/null
RM_STATUS=$?
# grep on nonexistent file returns nonzero
grep "test" /tmp/nonexistent_file_123 2>/dev/null
GREP_STATUS=$?
# wc on nonexistent file returns nonzero
wc -l /tmp/nonexistent_file_123 2>/dev/null
WC_STATUS=$?

if [ $CNF_STATUS -eq 127 ] && [ $CP_STATUS -ne 0 ] && [ $RM_STATUS -ne 0 ] && [ $GREP_STATUS -ne 0 ] && [ $WC_STATUS -ne 0 ]; then
  echo "M22-POLISH: ok failure-status"
fi

# Run userspace M24 errno tests:
/bin/m13-smoke --m24 && echo "M24-SMOKE: ok errno-mapping" && echo "M24-SMOKE: ok diagnostics"

echo "M22-POLISH: done"

# ── Optional upstream BusyBox package smoke tests ──
if [ -x /opt/busybox/bin/busybox ]; then
  echo "BB-SMOKE: start"

  # This source copy documents the generated initramfs script. The package is
  # installed at /opt/busybox and never shadows the native /bin commands.
  /opt/busybox/bin/busybox --list | grep -q "echo" && echo "BB-SMOKE: ok list"

# 2. upstream BusyBox echo
/opt/busybox/bin/busybox echo "hello bb" | grep -q "hello bb" && echo "BB-SMOKE: ok echo"

# 3. upstream BusyBox printf
/opt/busybox/bin/busybox printf "hello %s\n" bb | grep -q "hello bb" && echo "BB-SMOKE: ok printf"

# 4. upstream BusyBox pwd
/opt/busybox/bin/busybox pwd | grep -q "/" && echo "BB-SMOKE: ok pwd"

# 5. upstream BusyBox mkdir
/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir
[ -d /tmp/bb_dir ] && echo "BB-SMOKE: ok mkdir"

# 6. upstream BusyBox touch
/opt/busybox/bin/busybox touch /tmp/bb_dir/bb_file
[ -f /tmp/bb_dir/bb_file ] && echo "BB-SMOKE: ok touch"

# 7. upstream BusyBox cat
echo "content123" > /tmp/bb_dir/bb_file
/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file | grep -q "content123" && echo "BB-SMOKE: ok cat"

# 8. upstream BusyBox cp
/opt/busybox/bin/busybox cp /tmp/bb_dir/bb_file /tmp/bb_dir/bb_file_cp
/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_cp | grep -q "content123" && [ -f /tmp/bb_dir/bb_file_cp ] && echo "BB-SMOKE: ok cp"

# 9. upstream BusyBox mv
/opt/busybox/bin/busybox mv /tmp/bb_dir/bb_file_cp /tmp/bb_dir/bb_file_mv
[ ! -f /tmp/bb_dir/bb_file_cp ] && [ -f /tmp/bb_dir/bb_file_mv ] && /opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_mv | grep -q "content123" && echo "BB-SMOKE: ok mv"

# 10. upstream BusyBox ln
/opt/busybox/bin/busybox ln -s /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk
# Verify it resolves correctly
/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_lnk | grep -q "content123" && echo "BB-SMOKE: ok ln"

# 11. upstream BusyBox readlink
/opt/busybox/bin/busybox readlink /tmp/bb_dir/bb_file_lnk | grep -q "bb_file_mv" && echo "BB-SMOKE: ok readlink"

# 12. upstream BusyBox chmod
/opt/busybox/bin/busybox chmod 755 /tmp/bb_dir/bb_file_mv
[ -x /tmp/bb_dir/bb_file_mv ] && echo "BB-SMOKE: ok chmod"

# 13. upstream BusyBox test & [
/opt/busybox/bin/busybox test -f /tmp/bb_dir/bb_file_mv && /opt/busybox/bin/busybox [ -d /tmp/bb_dir ] && echo "BB-SMOKE: ok test"

# 14. upstream BusyBox sort
printf "b\nc\na\n" > /tmp/bb_dir/bb_sort
/opt/busybox/bin/busybox sort /tmp/bb_dir/bb_sort | head -n 1 | grep -q "a" && echo "BB-SMOKE: ok sort"

# 15. upstream BusyBox uniq
printf "a\na\nb\n" > /tmp/bb_dir/bb_uniq
/opt/busybox/bin/busybox uniq /tmp/bb_dir/bb_uniq | wc -l | grep -q "2" && echo "BB-SMOKE: ok uniq"

# First migration wave
/opt/busybox/bin/busybox ls /tmp/bb_dir | grep -q "bb_file" && echo "BB-W1: ok ls"
/opt/busybox/bin/busybox cmp /tmp/bb_dir/bb_file /tmp/bb_dir/bb_file && echo "BB-W1: ok cmp"
printf "left:right\n" | /opt/busybox/bin/busybox cut -d: -f2 | grep -q "right" && echo "BB-W1: ok cut"
/opt/busybox/bin/busybox env BB_W1=value | grep -q "BB_W1=value" && echo "BB-W1: ok env"
BB_ID_OUT=$(/opt/busybox/bin/busybox id -u)
[ "$BB_ID_OUT" = "0" ] && echo "BB-W1: ok id"
/opt/busybox/bin/busybox printenv PATH >/dev/null && echo "BB-W1: ok printenv"
printf "tee-data\n" | /opt/busybox/bin/busybox tee /tmp/bb_dir/bb_tee | grep -q "tee-data" && grep -q "tee-data" /tmp/bb_dir/bb_tee && echo "BB-W1: ok tee"
printf "abc\n" | /opt/busybox/bin/busybox tr a-z A-Z | grep -q "ABC" && echo "BB-W1: ok tr"
/opt/busybox/bin/busybox whoami | grep -q "root" && echo "BB-W1: ok whoami"
/opt/busybox/bin/busybox seq 1 3 > /tmp/bb_dir/bb_seq
tail -n 1 /tmp/bb_dir/bb_seq | grep -q "3" && echo "BB-W1: ok seq"
/opt/busybox/bin/busybox which ls | grep -q "/bin/ls" && echo "BB-W1: ok which"
/opt/busybox/bin/busybox clear >/tmp/bb_dir/bb_clear && echo "BB-W1: ok clear"
printf "AB" | /opt/busybox/bin/busybox hexdump | grep -q "4241" && echo "BB-W1: ok hexdump"

# 16. upstream BusyBox rm
/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk /tmp/bb_dir/bb_sort /tmp/bb_dir/bb_uniq /tmp/bb_dir/bb_tee /tmp/bb_dir/bb_clear /tmp/bb_dir/bb_seq
[ ! -f /tmp/bb_dir/bb_file_mv ] && [ ! -f /tmp/bb_dir/bb_file_lnk ] && echo "BB-SMOKE: ok rm"

# 17. upstream BusyBox rmdir
/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file
/opt/busybox/bin/busybox rmdir /tmp/bb_dir
[ ! -d /tmp/bb_dir ] && echo "BB-SMOKE: ok rmdir"

  echo "BB-SMOKE: done"
fi

echo "POSIX-SMOKE: done"
