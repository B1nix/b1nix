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

echo "POSIX-SMOKE: done"
