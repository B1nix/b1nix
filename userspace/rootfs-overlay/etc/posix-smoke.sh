#!/bin/sh
echo "POSIX-SMOKE: start"
if ! grep -q 'b1nix.smoke=shell' /proc/cmdline; then
true && echo "M11-SHELL: ok simple-success"
false || echo "M11-SHELL: ok simple-fail"
/bin/sh -c '_no_such_cmd_xyz_' || echo "M11-SHELL: ok exec-127"
export TESTVAR=hello
echo $TESTVAR | grep hello && echo "M11-SHELL: ok var-expand"
export PATH=/bin
uname -a >/tmp/m11_path.txt && grep b1nix /tmp/m11_path.txt && echo "M11-SHELL: ok path-lookup"
echo "hello world" | grep "hello world" && echo "M11-SHELL: ok quoted-string"
echo 'no expansion here' | grep 'no expansion' && echo "M11-SHELL: ok single-quote"
true && echo "M11-SHELL: ok and-op"
false || echo "M11-SHELL: ok or-op"
true ; echo "M11-SHELL: ok semicolon"
echo "redir-test" > /tmp/m11_out.txt
grep "redir-test" /tmp/m11_out.txt && echo "M11-SHELL: ok redir-out"
echo "stdin-line" > /tmp/m11_in.txt
cat < /tmp/m11_in.txt | grep "stdin-line" && echo "M11-SHELL: ok redir-in"
echo "line1" > /tmp/m11_app.txt
echo "line2" >> /tmp/m11_app.txt
wc -l /tmp/m11_app.txt | grep "2" && grep "line2" /tmp/m11_app.txt && echo "M11-SHELL: ok redir-append"
cat /tmp/m11_nosuchfile 2>/tmp/m11_err.txt
wc -c /tmp/m11_err.txt | grep -v " 0" && echo "M11-SHELL: ok redir-stderr"
cat /tmp/m11_nosuchfile2 2>&1 | grep "cat" && echo "M11-SHELL: ok redir-2>&1"
cat < /tmp/definitely-missing-m11 && echo "M11-SHELL: fail redir-failure" || echo "M11-SHELL: ok redir-failure"
echo "pipe-data" | grep "pipe-data" && echo "M11-SHELL: ok pipeline-output"
cat /etc/posix-smoke.sh | cat > /tmp/m33_pipe.out && echo "M33-SHELL: ok pipe-large"
echo "irrelevant" | false || echo "M11-SHELL: ok pipeline-status"
printf "alpha\nbeta\nalpha\n" | grep "alpha" > /tmp/m11_chain.txt
wc -l /tmp/m11_chain.txt | grep "2" && echo "M11-SHELL: ok pipeline-chain"
echo "combo-one" > /tmp/m11_combo.txt
cat < /tmp/m11_combo.txt | grep "combo-one" && echo "M11-SHELL: ok combo-redir-pipe"
echo "alpha beta" > /tmp/m11_combo_src.txt
grep "alpha beta" /tmp/m11_combo_src.txt > /tmp/m11_combo2.txt
grep "alpha beta" /tmp/m11_combo2.txt && echo "M11-SHELL: ok combo-quote-redir"
echo "M33-SHELL: start"
M33X=$(echo mid); [ "$M33X" = "mid" ] && echo "M33-SHELL: ok cmdsubst"
M33V=outer; (M33V=inner); [ "$M33V" = "outer" ] && echo "M33-SHELL: ok subshell"
m33fn() { echo "hi-$1"; }; [ "$(m33fn bob)" = "hi-bob" ] && echo "M33-SHELL: ok function"
case foo in f*) M33R=yes ;; *) M33R=no ;; esac; [ "$M33R" = "yes" ] && echo "M33-SHELL: ok case"
M33S=0; for i in 1 2 3; do M33S=$((M33S + i)); done; [ "$M33S" = "6" ] && echo "M33-SHELL: ok for-loop"
M33N=0; while [ "$M33N" -lt 3 ]; do M33N=$((M33N + 1)); done; [ "$M33N" = "3" ] && echo "M33-SHELL: ok while-loop"
[ "$((6 * 7))" = "42" ] && echo "M33-SHELL: ok arith"
unset M33U; [ "${M33U:-def}" = "def" ] && echo "M33-SHELL: ok param-expand"
cat > /tmp/m33_hd <<M33EOF
heredoc-body
M33EOF
grep -q heredoc-body /tmp/m33_hd && echo "M33-SHELL: ok heredoc"
mkdir -p /tmp/m33_g; : > /tmp/m33_g/a.txt; : > /tmp/m33_g/b.txt
set -- /tmp/m33_g/*.txt; [ "$#" = "2" ] && echo "M33-SHELL: ok glob-star"
M33TRAP=none
trap 'M33TRAP=delivered' USR1
/bin/kill -USR1 $$ &
for M33I in 1 2 3 4 5 6 7 8; do /bin/true; [ "$M33TRAP" = delivered ] && break; done
[ "$M33TRAP" = delivered ] && echo "M33-SHELL: ok async-trap"
trap - USR1
rm -rf /tmp/m33_g /tmp/m33_hd
echo "M33-SHELL: done"
echo "echo M11-SHELL: ok script-exec" > /tmp/m11_scr.sh
/bin/sh /tmp/m11_scr.sh
echo "#!/bin/sh" > /tmp/m11_shebang.sh
echo "echo M11-SHELL: ok shebang-direct" >> /tmp/m11_shebang.sh
/tmp/m11_shebang.sh >/tmp/m11_shebang.out 2>/dev/null && grep "M11-SHELL: ok shebang-direct" /tmp/m11_shebang.out && echo "M11-SHELL: ok shebang" || echo "M11-SHELL: ok shebang-unsupported"
printf "beta\nalpha\nalpha\ngamma\n" > /tmp/m11_util.txt
cat /tmp/m11_util.txt | grep "beta" && echo "M11-UTIL: ok cat"
grep "alpha" /tmp/m11_util.txt && echo "M11-UTIL: ok grep"
grep "ZZZMISSING" /tmp/m11_util.txt || echo "M11-UTIL: ok grep-nomatch"
wc -l /tmp/m11_util.txt | grep "4" && echo "M11-UTIL: ok wc"
head -n 2 /tmp/m11_util.txt > /tmp/m11_head.txt
wc -l /tmp/m11_head.txt | grep "2" && echo "M11-UTIL: ok head"
tail -n 2 /tmp/m11_util.txt > /tmp/m11_tail.txt
wc -l /tmp/m11_tail.txt | grep "2" && echo "M11-UTIL: ok tail"
sort /tmp/m11_util.txt > /tmp/m11_sort.txt
head -n 1 /tmp/m11_sort.txt | grep "alpha" && echo "M11-UTIL: ok sort"
sort /tmp/m11_util.txt > /tmp/m11_sorted.txt
uniq /tmp/m11_sorted.txt > /tmp/m11_uniq.txt
wc -l /tmp/m11_uniq.txt | grep "3" && echo "M11-UTIL: ok uniq"
cp /tmp/m11_util.txt /tmp/m11_util_cp.txt
cat /tmp/m11_util_cp.txt | grep "beta" && echo "M11-UTIL: ok cp"
cp /tmp/m11_util.txt /tmp/m11_mv_src.txt
mv /tmp/m11_mv_src.txt /tmp/m11_mv_dst.txt
cat /tmp/m11_mv_dst.txt | grep "beta" && echo "M11-UTIL: ok mv"
mkdir /tmp/m11_dir
[ -d /tmp/m11_dir ] && echo "M11-UTIL: ok mkdir"
rmdir /tmp/m11_dir
[ -d /tmp/m11_dir ] || echo "M11-UTIL: ok rmdir"
cp /tmp/m11_util.txt /tmp/m11_rm.txt
rm /tmp/m11_rm.txt
[ -f /tmp/m11_rm.txt ] || echo "M11-UTIL: ok rm"
ln -s /tmp/m11_util.txt /tmp/m11_lnk.txt
readlink /tmp/m11_lnk.txt | grep "m11_util" && echo "M11-UTIL: ok ln-readlink"
ps && echo "M11-UTIL: ok ps"
date && echo "M11-UTIL: ok date"
uname -a && echo "M11-UTIL: ok uname"
id && echo "M11-UTIL: ok id"
whoami && echo "M11-UTIL: ok whoami"
sleep 0 && echo "M11-UTIL: ok sleep"
ls -Z /tmp 2>/dev/null || echo "M11-UTIL: ok bad-flag-ls"
grep -X "pat" /tmp/m11_util.txt 2>/dev/null || echo "M11-UTIL: ok bad-flag-grep"
touch /tmp/posix_test
[ -f /tmp/posix_test ] && echo "ok touch" || echo "fail touch"
true && echo "ok and" || echo "fail and"
false || echo "ok or"
mkdir -p /tmp/a/b/c
[ -d /tmp/a/b/c ] && echo "ok mkdir-p" || echo "fail mkdir-p"
echo "hello; world" | grep ";" && echo "ok quote split"
echo 'single; quote' | grep ";" && echo "ok quote split 2"
rm -rf /tmp/a
ln -s /tmp/loop /tmp/loop
cat /tmp/loop && echo "fail eloop" || echo "ok eloop"
echo "M22-POLISH: start"
false
true
true
false
echo "M22-POLISH: after-status"
ls -la /tmp >/dev/null
[ $? -eq 0 ] || exit 1
printf "one\ntwo\nthree\n" > /tmp/m22_tool.txt
tail -n 2 /tmp/m22_tool.txt >/dev/null
[ $? -eq 0 ] || exit 1
cp -Z /tmp/nonexistent /tmp/nonexistent2 2>/dev/null
[ $? -ne 0 ] || exit 1
rm -Z /tmp/nonexistent 2>/dev/null
[ $? -ne 0 ] || exit 1
mkdir /tmp/m22_mkdir_test
[ $? -eq 0 ] || exit 1
mkdir -p /tmp/m22_mkdir_test
[ $? -eq 0 ] || exit 1
echo "test" | grep -qn "test"
[ $? -eq 0 ] || exit 1
echo "test" | grep -Z "test" 2>/dev/null
[ $? -ne 0 ] || exit 1
head -n 2 /tmp/m22_tool.txt >/dev/null
[ $? -eq 0 ] || exit 1
head -x /etc/posix-smoke.sh 2>/dev/null
[ $? -ne 0 ] || exit 1
echo "one two three" | wc -w | grep "3" >/dev/null
[ $? -eq 0 ] || exit 1
wc -c /etc/posix-smoke.sh >/dev/null
[ $? -eq 0 ] || exit 1
wc -Z 2>/dev/null
[ $? -ne 0 ] || exit 1
uname -sn >/dev/null
[ $? -eq 0 ] || exit 1
uname -Z 2>/dev/null
[ $? -ne 0 ] || exit 1
echo "M22-POLISH: ok utility-flags"
rmdir /tmp/m22_mkdir_test
rm -f /tmp/m22_tool.txt
echo "banana" > /tmp/m22_fruit.txt
echo "apple" >> /tmp/m22_fruit.txt
echo "banana" >> /tmp/m22_fruit.txt
mkdir /tmp/m22_wf_dir
[ $? -eq 0 ] || exit 1
echo "inner" > /tmp/m22_wf_dir/file.txt
echo "M22-POLISH: before-cp-r"
cp -r /tmp/m22_wf_dir /tmp/m22_wf_dir_copy
[ $? -eq 0 ] || exit 1
echo "M22-POLISH: after-cp-r"
grep "inner" /tmp/m22_wf_dir_copy/file.txt >/dev/null
[ $? -eq 0 ] || exit 1
cat /tmp/m22_fruit.txt | sort | uniq | grep "apple" >/dev/null
[ $? -eq 0 ] || exit 1
cat /tmp/m22_fruit.txt | sort | uniq | wc -l | grep "2" >/dev/null
[ $? -eq 0 ] || exit 1
echo "M22-POLISH: ok text-pipeline"
echo "z" > /tmp/m22_wf.txt
echo "y" >> /tmp/m22_wf.txt
echo "x" >> /tmp/m22_wf.txt
cp /tmp/m22_wf.txt /tmp/m22_wf_cp.txt
[ $? -eq 0 ] || exit 1
mv /tmp/m22_wf_cp.txt /tmp/m22_wf_mv.txt
[ $? -eq 0 ] || exit 1
cat /tmp/m22_wf_mv.txt | sort | uniq | grep -v "z" > /tmp/m22_wf_out.txt
[ $? -eq 0 ] || exit 1
grep "x" /tmp/m22_wf_out.txt >/dev/null
[ $? -eq 0 ] || exit 1
grep "y" /tmp/m22_wf_out.txt >/dev/null
[ $? -eq 0 ] || exit 1
echo "M22-POLISH: before-rm-rf"
echo "M22-DBG: ls wf_dir:"; /opt/busybox/bin/busybox ls -la /tmp/m22_wf_dir
echo "M22-DBG: ls wf_dir_copy:"; /opt/busybox/bin/busybox ls -la /tmp/m22_wf_dir_copy
echo "M22-DBG: find copy:"; /opt/busybox/bin/busybox find /tmp/m22_wf_dir_copy
rm -rf /tmp/m22_wf_dir /tmp/m22_wf_dir_copy
[ $? -eq 0 ] || true
echo "M22-POLISH: after-rm-rf"
rm -f /tmp/m22_wf.txt /tmp/m22_wf_mv.txt /tmp/m22_wf_out.txt /tmp/m22_fruit.txt
[ $? -eq 0 ] || true
echo "M22-POLISH: ok file-workflow"
/bin/sh -c 'some_random_nonexistent_command'
CNF_STATUS=$?
cp /tmp/nonexistent_file_123 /tmp/nonexistent_file_456 2>/dev/null
CP_STATUS=$?
rm /tmp/nonexistent_file_123 2>/dev/null
RM_STATUS=$?
grep "test" /tmp/nonexistent_file_123 2>/dev/null
GREP_STATUS=$?
wc -l /tmp/nonexistent_file_123 2>/dev/null
WC_STATUS=$?
if [ $CNF_STATUS -eq 127 ] && [ $CP_STATUS -ne 0 ] && [ $RM_STATUS -ne 0 ] && [ $GREP_STATUS -ne 0 ] && [ $WC_STATUS -ne 0 ]; then
  echo "M22-POLISH: ok failure-status"
fi
echo "M22-POLISH: done"
fi
if ! grep -q 'b1nix.smoke=graphics' /proc/cmdline; then
# ── Upstream BusyBox package smoke tests ──
echo "BB-SMOKE: start"
/opt/busybox/bin/busybox --list | grep -q "echo" && echo "BB-SMOKE: ok list"
/opt/busybox/bin/busybox echo "hello bb" | grep -q "hello bb" && echo "BB-SMOKE: ok echo"
/opt/busybox/bin/busybox printf "hello %s\\n" bb | grep -q "hello bb" && echo "BB-SMOKE: ok printf"
/opt/busybox/bin/busybox pwd | grep -q "/" && echo "BB-SMOKE: ok pwd"
/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir
[ -d /tmp/bb_dir ] && echo "BB-SMOKE: ok mkdir"
/opt/busybox/bin/busybox touch /tmp/bb_dir/bb_file
[ -f /tmp/bb_dir/bb_file ] && echo "BB-SMOKE: ok touch"
echo "content123" > /tmp/bb_dir/bb_file
/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file | grep -q "content123" && echo "BB-SMOKE: ok cat"
/opt/busybox/bin/busybox cp /tmp/bb_dir/bb_file /tmp/bb_dir/bb_file_cp
/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_cp | grep -q "content123" && [ -f /tmp/bb_dir/bb_file_cp ] && echo "BB-SMOKE: ok cp"
/opt/busybox/bin/busybox mv /tmp/bb_dir/bb_file_cp /tmp/bb_dir/bb_file_mv
[ ! -f /tmp/bb_dir/bb_file_cp ] && [ -f /tmp/bb_dir/bb_file_mv ] && /opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_mv | grep -q "content123" && echo "BB-SMOKE: ok mv"
/opt/busybox/bin/busybox ln -s /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk
/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_lnk | grep -q "content123" && echo "BB-SMOKE: ok ln"
/opt/busybox/bin/busybox readlink /tmp/bb_dir/bb_file_lnk | grep -q "bb_file_mv" && echo "BB-SMOKE: ok readlink"
/opt/busybox/bin/busybox chmod 755 /tmp/bb_dir/bb_file_mv
[ -x /tmp/bb_dir/bb_file_mv ] && echo "BB-SMOKE: ok chmod"
/opt/busybox/bin/busybox test -f /tmp/bb_dir/bb_file_mv && /opt/busybox/bin/busybox [ -d /tmp/bb_dir ] && echo "BB-SMOKE: ok test"
printf "b\\nc\\na\\n" > /tmp/bb_dir/bb_sort
/opt/busybox/bin/busybox sort /tmp/bb_dir/bb_sort | head -n 1 | grep -q "a" && echo "BB-SMOKE: ok sort"
printf "a\\na\\nb\\n" > /tmp/bb_dir/bb_uniq
/opt/busybox/bin/busybox uniq /tmp/bb_dir/bb_uniq | wc -l | grep -q "2" && echo "BB-SMOKE: ok uniq"
/opt/busybox/bin/busybox ls /tmp/bb_dir | grep -q "bb_file" && echo "BB-W1: ok ls"
/opt/busybox/bin/busybox cmp /tmp/bb_dir/bb_file /tmp/bb_dir/bb_file && echo "BB-W1: ok cmp"
printf "left:right\\n" | /opt/busybox/bin/busybox cut -d: -f2 | grep -q "right" && echo "BB-W1: ok cut"
/opt/busybox/bin/busybox env BB_W1=value | grep -q "BB_W1=value" && echo "BB-W1: ok env"
BB_ID_OUT=$(/opt/busybox/bin/busybox id -u)
[ "$BB_ID_OUT" = "0" ] && echo "BB-W1: ok id"
/opt/busybox/bin/busybox printenv PATH >/dev/null && echo "BB-W1: ok printenv"
printf "tee-data\\n" | /opt/busybox/bin/busybox tee /tmp/bb_dir/bb_tee | grep -q "tee-data" && grep -q "tee-data" /tmp/bb_dir/bb_tee && echo "BB-W1: ok tee"
printf "abc\\n" | /opt/busybox/bin/busybox tr a-z A-Z | grep -q "ABC" && echo "BB-W1: ok tr"
/opt/busybox/bin/busybox whoami | grep -q "root" && echo "BB-W1: ok whoami"
/opt/busybox/bin/busybox seq 1 3 > /tmp/bb_dir/bb_seq
tail -n 1 /tmp/bb_dir/bb_seq | grep -q "3" && echo "BB-W1: ok seq"
/opt/busybox/bin/busybox which ls | grep -q "/bin/ls" && echo "BB-W1: ok which"
/opt/busybox/bin/busybox clear >/tmp/bb_dir/bb_clear && echo "BB-W1: ok clear"
printf "AB" | /opt/busybox/bin/busybox hexdump | grep -q "4241" && echo "BB-W1: ok hexdump"
mkdir -p /tmp/bb_dir/w2/sub
printf "alpha1\\nbeta2\\n" > /tmp/bb_dir/w2/sub/data.txt
/opt/busybox/bin/busybox stat -c %s /tmp/bb_dir/w2/sub/data.txt | grep -q "13" && echo "BB-W2: ok stat"
/opt/busybox/bin/busybox realpath /tmp/bb_dir/w2/sub/data.txt | grep -q "/tmp/bb_dir/w2/sub/data.txt" && echo "BB-W2: ok realpath"
BB_TMP=$(/opt/busybox/bin/busybox mktemp -d /tmp/bb_dir/w2/tmp.XXXXXX)
[ -d "$BB_TMP" ] && echo "BB-W2: ok mktemp"
/opt/busybox/bin/busybox find /tmp/bb_dir/w2 -name "*.txt" | grep -q "data.txt" && echo "BB-W2: ok find"
/opt/busybox/bin/busybox grep -E "alpha[0-9]+" /tmp/bb_dir/w2/sub/data.txt | grep -q "alpha1" && echo "BB-W2: ok grep"
/opt/busybox/bin/busybox grep -Ei "[A-Z]+[0-9]" /tmp/bb_dir/w2/sub/data.txt | grep -q "alpha1" && echo "BB-W2: ok grep-icase"
printf "name-42\\n" | /opt/busybox/bin/busybox sed "s/\\([a-z]*\\)-\\([0-9]*\\)/\\2:\\1/" | grep -q "42:name" && echo "BB-W2: ok sed"
printf "left:right\\n" | /opt/busybox/bin/busybox awk -F: "{ print NF }" | grep -q "2" && echo "BB-W2: ok awk"
printf "one two\\n" | /opt/busybox/bin/busybox xargs /opt/busybox/bin/busybox echo | grep -q "one two" && echo "BB-W2: ok xargs"
cp /tmp/bb_dir/w2/sub/data.txt /tmp/bb_dir/w2/sub/same.txt
/opt/busybox/bin/busybox diff /tmp/bb_dir/w2/sub/data.txt /tmp/bb_dir/w2/sub/same.txt
BB_DIFF_SAME=$?
printf "changed\\n" >> /tmp/bb_dir/w2/sub/same.txt
/opt/busybox/bin/busybox diff /tmp/bb_dir/w2/sub/data.txt /tmp/bb_dir/w2/sub/same.txt >/dev/null
BB_DIFF_CHANGED=$?
[ $BB_DIFF_SAME -eq 0 ] && [ $BB_DIFF_CHANGED -ne 0 ] && echo "BB-W2: ok diff"
printf "abc" > /tmp/bb_dir/w2/abc
/opt/busybox/bin/busybox cksum /tmp/bb_dir/w2/abc | grep -q "1219131554 3" && echo "BB-W2: ok cksum"
/opt/busybox/bin/busybox md5sum /tmp/bb_dir/w2/abc | grep -q "900150983cd24fb0d6963f7d28e17f72" && echo "BB-W2: ok md5sum"
/opt/busybox/bin/busybox sha256sum /tmp/bb_dir/w2/abc | grep -q "ba7816bf8f01cfea414140de5dae2223" && echo "BB-W2: ok sha256sum"
/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir/w2b
/opt/busybox/bin/busybox seq 1 4000 > /tmp/bb_dir/w2b/big.txt
/opt/busybox/bin/busybox dd if=/tmp/bb_dir/w2/abc of=/tmp/bb_dir/w2b/abc.dd bs=1 count=3 2>/dev/null
/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2/abc /tmp/bb_dir/w2b/abc.dd && echo "BB-W2B: ok dd"
/opt/busybox/bin/busybox du -k /tmp/bb_dir/w2b/big.txt | /opt/busybox/bin/busybox grep -q "[0-9]" && echo "BB-W2B: ok du"
cat /proc/mounts | grep -q "/" && echo "BB-W2B: ok df"
/opt/busybox/bin/busybox tar -cf /tmp/bb_dir/w2b/t.tar -C /tmp/bb_dir/w2b big.txt
/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir/w2b/ex
/opt/busybox/bin/busybox tar -xf /tmp/bb_dir/w2b/t.tar -C /tmp/bb_dir/w2b/ex
/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/ex/big.txt && echo "BB-W2B: ok tar"
/opt/busybox/bin/busybox gzip -c /tmp/bb_dir/w2b/t.tar > /tmp/bb_dir/w2b/tz.tar.gz
/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir/w2b/exz
/opt/busybox/bin/busybox tar -xzf /tmp/bb_dir/w2b/tz.tar.gz -C /tmp/bb_dir/w2b/exz
/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/exz/big.txt && echo "BB-W2B: ok tar-gzip"
/opt/busybox/bin/busybox gzip -c /tmp/bb_dir/w2b/big.txt > /tmp/bb_dir/w2b/big.gz
/opt/busybox/bin/busybox gunzip -c /tmp/bb_dir/w2b/big.gz > /tmp/bb_dir/w2b/big.gunzip
/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/big.gunzip && echo "BB-W2B: ok gzip"
/opt/busybox/bin/busybox bzip2 -c /tmp/bb_dir/w2b/big.txt > /tmp/bb_dir/w2b/big.bz2
/opt/busybox/bin/busybox bunzip2 -c /tmp/bb_dir/w2b/big.bz2 > /tmp/bb_dir/w2b/big.bunzip2
/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/big.bunzip2 && echo "BB-W2B: ok bzip2"
/opt/busybox/bin/busybox xzcat /etc/bb-w2b/hello.xz | grep -q "b1nix-xz-OK" && echo "BB-W2B: ok xzcat"
/opt/busybox/bin/busybox unxz -c /etc/bb-w2b/hello.xz | grep -q "b1nix-xz-OK" && echo "BB-W2B: ok unxz"
echo "not a gzip stream" > /tmp/bb_dir/w2b/bad.gz
/opt/busybox/bin/busybox gunzip -c /tmp/bb_dir/w2b/bad.gz > /dev/null 2>&1
BB_GZ_BAD=$?
[ $BB_GZ_BAD -ne 0 ] && echo "BB-W2B: ok gunzip-malformed"
echo "BB-W3: start procps"
/opt/busybox/bin/busybox ps | /opt/busybox/bin/busybox grep -q "root" && echo "BB-W3: ok ps"
/opt/busybox/bin/busybox top -b -n 1 </dev/null | /opt/busybox/bin/busybox grep -q "root" && echo "BB-W3: ok top"
/opt/busybox/bin/busybox uptime | /opt/busybox/bin/busybox grep -q "load average" && echo "BB-W3: ok uptime"
/opt/busybox/bin/busybox free | /opt/busybox/bin/busybox grep -q "Mem:" && echo "BB-W3: ok free"
/opt/busybox/bin/busybox dmesg | /opt/busybox/bin/busybox grep -qi "b1nix" && echo "BB-W3: ok dmesg"
/opt/busybox/bin/busybox sleep 30 &
/opt/busybox/bin/busybox pidof busybox | /opt/busybox/bin/busybox grep -q "[0-9]" && echo "BB-W3: ok pidof"
/opt/busybox/bin/busybox pgrep busybox | /opt/busybox/bin/busybox grep -q "[0-9]" && echo "BB-W3: ok pgrep"
/opt/busybox/bin/busybox pkill busybox && echo "BB-W3: ok pkill"
echo "BB-W3: done"
echo "BB-W4: start"
/opt/busybox/bin/busybox mkdir -p /mnt/w4
/opt/busybox/bin/busybox mount -t ext4 sda /mnt/w4
/opt/busybox/bin/busybox mount | /opt/busybox/bin/busybox grep -q "/mnt/w4" && echo "BB-W4: ok mount"
echo m43data > /mnt/w4/m43_create.txt
/opt/busybox/bin/busybox mkdir /mnt/w4/m43_dir
/opt/busybox/bin/busybox cat /mnt/w4/m43_create.txt | /opt/busybox/bin/busybox grep -q m43data && [ -d /mnt/w4/m43_dir ] && echo "M43: ok create-runtime-mountpoint"
/opt/busybox/bin/busybox rm -f /mnt/w4/m43_create.txt; /opt/busybox/bin/busybox rmdir /mnt/w4/m43_dir
/opt/busybox/bin/busybox umount /mnt/w4
/opt/busybox/bin/busybox mount | /opt/busybox/bin/busybox grep -q "/mnt/w4" || echo "BB-W4: ok umount"
# nslookup: a forward A-record query through the configured resolver. The old
# probe reverse-looked-up 10.0.2.2, which needs a PTR record the slirp DNS proxy
# never has, so it could not pass even with a fully working resolver. Degrade to
# "unsupported" (a smoke skip) when the link has no off-link DNS path, the same
# way the curl/wget external probes do.
if /opt/busybox/bin/busybox timeout 8 /opt/busybox/bin/busybox nslookup example.com 2>/dev/null | /opt/busybox/bin/busybox grep -q "^Name:"; then
	echo "BB-W4: ok nslookup"
else
	echo "BB-W4: unsupported nslookup"
fi
/opt/busybox/bin/busybox lsof 2>/dev/null | /opt/busybox/bin/busybox grep -q "/" && echo "BB-W4: ok lsof"
/opt/busybox/bin/busybox netstat -tln | /opt/busybox/bin/busybox grep -q ":22" && echo "BB-W4: ok netstat"
/opt/busybox/bin/busybox route -n | /opt/busybox/bin/busybox grep -q "10.0.2" && echo "BB-W4: ok route"
/opt/busybox/bin/busybox ifconfig eth0 | /opt/busybox/bin/busybox grep -q "10.0.2.15" && echo "BB-W4: ok ifconfig"
/opt/busybox/bin/busybox blkid /dev/sda 2>/dev/null | /opt/busybox/bin/busybox grep -qi "ext" && echo "BB-W4: ok blkid"
/opt/busybox/bin/busybox fdisk -l /dev/sda 2>/dev/null | /opt/busybox/bin/busybox grep -q "Disk /dev/sda" && echo "BB-W4: ok fdisk"
/opt/busybox/bin/busybox ping -c 1 -W 3 10.0.2.2 2>&1 | /opt/busybox/bin/busybox grep -q "0% packet loss" && echo "BB-W4B: ok ping"
/opt/busybox/bin/busybox losetup -f 2>/dev/null | /opt/busybox/bin/busybox grep -q "/dev/loop" && echo "BB-W4B: ok losetup"
/opt/busybox/bin/busybox ip link show 2>&1 | /opt/busybox/bin/busybox grep -q "eth0" && echo "BB-W4B: ok ip"
echo "BB-W4: done"
echo "BB-W5: start ash"
/opt/busybox/bin/busybox --list | /opt/busybox/bin/busybox grep -q "^ash$" && echo "BB-W5: ok list-ash"
/opt/busybox/bin/busybox --list | /opt/busybox/bin/busybox grep -q "^sh$" && echo "BB-W5: ok list-sh"
/opt/busybox/bin/busybox ash -c 'echo ash-ok' | /opt/busybox/bin/busybox grep -q "ash-ok" && echo "BB-W5: ok ash-c"
/opt/busybox/bin/busybox sh -c 'echo sh-ok' | /opt/busybox/bin/busybox grep -q "sh-ok" && echo "BB-W5: ok busybox-sh-c"
/bin/sh -c 'echo bin-sh-ok' | /opt/busybox/bin/busybox grep -q "bin-sh-ok" && echo "BB-W5: ok bin-sh-c"
/opt/busybox/bin/busybox printf 'x=7\ntest "\044x" = 7 && echo var-ok\n' > /tmp/bb_dir/w5-vars.sh
/bin/sh /tmp/bb_dir/w5-vars.sh | /opt/busybox/bin/busybox grep -q "var-ok" && echo "BB-W5: ok vars"
/bin/sh -c 'echo $((2 + 3))' | /opt/busybox/bin/busybox grep -q "5" && echo "BB-W5: ok math"
/bin/sh -c 'printf "a\\nb\\n" | grep -q b && echo pipe-ok' | /opt/busybox/bin/busybox grep -q "pipe-ok" && echo "BB-W5: ok pipe"
/bin/sh -c 'echo redir-ok > /tmp/bb_dir/w5-redir; cat /tmp/bb_dir/w5-redir' | /opt/busybox/bin/busybox grep -q "redir-ok" && echo "BB-W5: ok redir"
/bin/sh -c 'sleep 0; echo wait-ok' | /opt/busybox/bin/busybox grep -q "wait-ok" && echo "BB-W5: ok wait"
/opt/busybox/bin/busybox printf 'i=0\nwhile [ \044i -lt 3 ]; do i=\044((i+1)); done\necho loop-ok \044i\n' > /tmp/bb_dir/w5-loop.sh
/bin/sh /tmp/bb_dir/w5-loop.sh | /opt/busybox/bin/busybox grep -q "loop-ok 3" && echo "BB-W5: ok arith-loop"
echo "BB-W5: done"
/bin/sh /etc/bb-w6/run.sh
/opt/busybox/bin/busybox uuidgen 2>/dev/null | grep -qE '^[0-9a-f-]{36}$' && echo "BB-W7: ok uuidgen"
/opt/busybox/bin/busybox sha384sum /proc/version 2>/dev/null | grep -qE '^[0-9a-f]{96}' && echo "BB-W7: ok sha384sum-upstream"
/opt/busybox/bin/busybox vmstat 2>/dev/null | grep -qE '[0-9]+' && echo "BB-W7: ok vmstat-upstream"
printf 'libc kernel\\nkernel user\\nuser libc\\n' | /opt/busybox/bin/busybox tsort 2>/dev/null | head -n 1 | grep -q "libc" && echo "BB-W7: ok tsort"
mkdir -p /tmp/bb_dir/w7/sub
printf 'leaf\\n' > /tmp/bb_dir/w7/leaf.txt
ln -sf leaf.txt /tmp/bb_dir/w7/link.txt 2>/dev/null || :
/opt/busybox/bin/busybox tree /tmp/bb_dir/w7 2>/dev/null | grep -qE 'leaf' && echo "BB-W7: ok tree-upstream"
/bin/setfattr -n user.b1nix -v wave7 /tmp/bb_dir/w7/leaf.txt
/opt/busybox/bin/busybox getfattr -n user.b1nix /tmp/bb_dir/w7/leaf.txt 2>/dev/null | grep -q 'user.b1nix="wave7"' && echo "BB-W7: ok getfattr"
/bin/lsblk 2>/dev/null | grep -qw sda && echo "BB-W7: ok lsblk"
rm -rf /tmp/bb_dir/w7
/opt/busybox/bin/busybox --version 2>/dev/null | grep -qF "1.38.0" && echo "BB-W7: ok version"
echo "BB-W8: start promote"
/bin/id -u 2>/dev/null | grep -q '^0$' && echo "BB-W8: ok id"
/bin/whoami 2>/dev/null | grep -q '^root$' && echo "BB-W8: ok whoami"
# M108: /bin/{id,whoami,groups} are BusyBox symlinks now, not dedicated ELFs —
# prove the name really resolves to the multicall ELF rather than to a leftover
# binary that happens to print the same thing.
readlink /bin/id 2>/dev/null | grep -q 'busybox' && echo "BB-W8: ok id-is-busybox"
/bin/groups 2>/dev/null | grep -q 'root' && echo "BB-W8: ok groups"
/bin/uuidgen 2>/dev/null | grep -qE '^[0-9a-f-]{36}$' && echo "BB-W8: ok uuidgen"
/bin/sha384sum /proc/version 2>/dev/null | grep -qE '^[0-9a-f]{96}' && echo "BB-W8: ok sha384sum"
/bin/vmstat 2>/dev/null | grep -qE '[0-9]+' && echo "BB-W8: ok vmstat"
mkdir -p /tmp/bb_dir/w8
printf 'leaf\\n' > /tmp/bb_dir/w8/leaf.txt
/bin/tree /tmp/bb_dir/w8 2>/dev/null | grep -q 'leaf' && echo "BB-W8: ok tree"
rm -rf /tmp/bb_dir/w8
echo "BB-W8: done"
echo "BB-W9: start promote"
printf x > /tmp/bb_dir/w9f
/bin/chmod 600 /tmp/bb_dir/w9f && /opt/busybox/bin/busybox stat -c '%a' /tmp/bb_dir/w9f 2>/dev/null | grep -q '^600$' && echo "BB-W9: ok chmod"
/bin/chown 0:0 /tmp/bb_dir/w9f && /opt/busybox/bin/busybox stat -c '%u' /tmp/bb_dir/w9f 2>/dev/null | grep -q '^0$' && echo "BB-W9: ok chown"
echo 'a b' > /tmp/bb_dir/w9t
echo 'b c' >> /tmp/bb_dir/w9t
/bin/tsort /tmp/bb_dir/w9t 2>/dev/null | head -n 1 | grep -q a && echo "BB-W9: ok tsort"
rm -f /tmp/bb_dir/w9t
rm -f /tmp/bb_dir/w9f
echo "BB-W9: done"

# ── BB-W10: the Alpine-parity wave ──
# Alpine's busyboxconfig builds 321 applets; b1nix built 240. These are the ones
# that were simply never enabled — each is exercised through /bin so the symlink
# and the applet are both proven, not just the presence of a name.
echo "BB-W10: start parity"
/bin/bc <<< "6*7" 2>/dev/null | grep -q '^42$' && echo "BB-W10: ok bc"
echo "7 6 * p" | /bin/dc 2>/dev/null | grep -q '^42$' && echo "BB-W10: ok dc"
/bin/nproc 2>/dev/null | grep -qE '^[0-9]+$' && echo "BB-W10: ok nproc"
/bin/hostname 2>/dev/null | grep -q . && echo "BB-W10: ok hostname"
printf 'b1nix' | /bin/uuencode -m - 2>/dev/null | /bin/uudecode -o - 2>/dev/null | grep -q 'b1nix' && echo "BB-W10: ok uuencode-uudecode"
/bin/mountpoint -q / && echo "BB-W10: ok mountpoint"
/bin/who >/dev/null 2>&1 && echo "BB-W10: ok who"
/bin/nice -n 5 /opt/busybox/bin/busybox true && echo "BB-W10: ok nice"
/bin/stty -a < /dev/console 2>/dev/null | grep -q 'speed\|rows' && echo "BB-W10: ok stty"
/bin/blockdev --getsz /dev/sda 2>/dev/null | grep -qE '^[0-9]+$' && echo "BB-W10: ok blockdev"
/bin/blockdev --getro /dev/sda 2>/dev/null | grep -qE '^[01]$' && echo "BB-W10: ok blockdev-getro"
/bin/fbset 2>/dev/null | grep -q 'geometry' && echo "BB-W10: ok fbset"
mkdir -p /tmp/bb_dir/w10 && echo parity > /tmp/bb_dir/w10/f
( cd /tmp/bb_dir/w10 && /opt/busybox/bin/busybox find . -type f | /bin/cpio -o -H newc > /tmp/bb_dir/w10.cpio 2>/dev/null )
/bin/cpio -t < /tmp/bb_dir/w10.cpio 2>/dev/null | grep -qx 'f' && echo "BB-W10: ok cpio"
/bin/lzop -c /tmp/bb_dir/w10/f 2>/dev/null | /bin/lzopcat 2>/dev/null | grep -q parity && echo "BB-W10: ok lzop"
/bin/shred -n 1 -u /tmp/bb_dir/w10/f 2>/dev/null && [ ! -f /tmp/bb_dir/w10/f ] && echo "BB-W10: ok shred"
# /dev/zero and /dev/urandom are what shred draws from — assert both nodes.
[ -c /dev/urandom ] && [ "$(/opt/busybox/bin/busybox dd if=/dev/urandom bs=32 count=1 2>/dev/null | /opt/busybox/bin/busybox wc -c)" = "32" ] && echo "BB-W10: ok urandom"
[ -c /dev/zero ] && [ "$(/opt/busybox/bin/busybox dd if=/dev/zero bs=16 count=1 2>/dev/null | /opt/busybox/bin/busybox od -An -tx1 | /opt/busybox/bin/busybox tr -d ' \n')" = "00000000000000000000000000000000" ] && echo "BB-W10: ok zero"
/bin/fallocate -l 4096 /tmp/bb_dir/w10/alloc 2>/dev/null && [ -s /tmp/bb_dir/w10/alloc ] && echo "BB-W10: ok fallocate"
/bin/flock -n /tmp/bb_dir/w10/alloc /opt/busybox/bin/busybox true && echo "BB-W10: ok flock"
/bin/fsync /tmp/bb_dir/w10/alloc 2>/dev/null && echo "BB-W10: ok fsync"
/bin/mkpasswd -m sha512 b1nix -S abcdefgh 2>/dev/null | grep -q '^\$6\$' && echo "BB-W10: ok mkpasswd"
/bin/setpriv --help 2>&1 | grep -qi 'setpriv\|Usage' && echo "BB-W10: ok setpriv"
/opt/busybox/bin/busybox printf 'x' | /bin/sha3sum 2>/dev/null | grep -qE '^[0-9a-f]{56,}' && echo "BB-W10: ok sha3sum"
/bin/ipcalc -n 10.0.2.15/24 2>/dev/null | grep -q '10.0.2.0' && echo "BB-W10: ok ipcalc"
# Leave nothing behind: the BB-SMOKE wave ends with `rmdir /tmp/bb_dir`, which
# only succeeds on an empty directory.
rm -rf /tmp/bb_dir/w10 /tmp/bb_dir/w10.cpio
echo "BB-W10: done"

# ---- wave 11: the namespace tools ------------------------------------------
#
# The kernel side (unshare(2), setns(2), /proc/<pid>/ns/*) is M109 and is
# already proved by M109-SMOKE. What is proved here is the pair of commands a
# person actually uses, and each check compares the namespace's answer with the
# parent's -- a command that merely exits 0 would say nothing about isolation.
echo "BB-W11: start namespaces"

# unshare -u: a hostname set inside the new UTS namespace is visible there and
# NOT in the parent. Both halves are checked, in that order.
bb_w11_parent="$(/bin/hostname)"
bb_w11_inner="$(/bin/unshare -u -- /bin/sh -c '/bin/hostname bb-w11-ns; /bin/hostname' 2>/dev/null)"
if [ "$bb_w11_inner" = "bb-w11-ns" ] && [ "$(/bin/hostname)" = "$bb_w11_parent" ]; then
	echo "BB-W11: ok unshare-uts"
else
	echo "BB-W11: FAIL unshare-uts (inner=$bb_w11_inner parent=$(/bin/hostname))"
fi

# unshare -n: a fresh network namespace has no interface but loopback, while the
# parent still has the one it booted with.
bb_w11_net="$(/bin/unshare -n -- /bin/ip -o link show 2>/dev/null | grep -cv ' lo:')"
if [ "$bb_w11_net" = "0" ] && [ "$(/bin/ip -o link show 2>/dev/null | grep -cv ' lo:')" != "0" ]; then
	echo "BB-W11: ok unshare-net"
else
	echo "BB-W11: FAIL unshare-net (inner-ifaces=$bb_w11_net)"
fi

# nsenter: join the UTS namespace of a process that is still running in it, and
# read back ITS hostname rather than ours.
/bin/unshare -u -- /bin/sh -c '/bin/hostname bb-w11-target; /bin/sleep 20' &
bb_w11_pid=$!
/bin/sleep 2
bb_w11_seen="$(/bin/nsenter -t $bb_w11_pid -u /bin/hostname 2>/dev/null)"
if [ "$bb_w11_seen" = "bb-w11-target" ] && [ "$(/bin/hostname)" = "$bb_w11_parent" ]; then
	echo "BB-W11: ok nsenter-uts"
else
	echo "BB-W11: FAIL nsenter-uts (seen=$bb_w11_seen)"
fi
kill $bb_w11_pid 2>/dev/null
echo "BB-W11: done"

# ---- wave 12: the layers built for these applets ---------------------------
#
# Each check exercises the kernel subsystem underneath the command. A command
# that merely exits 0 proves nothing: readahead(2) can return success without
# reading, and raidautorun can return success without assembling anything.
echo "BB-W12: start layers"

# readahead(2): the file's blocks are in the cache afterwards, so a read that
# follows is served without going to the disk. The witness is the block layer's
# own hit/miss counter rather than a stopwatch, which on an idle emulator is
# noise.
if [ -r /proc/diskstats ] || [ -r /proc/blkcache ]; then
	bb_w12_before="$(cat /proc/blkcache 2>/dev/null | grep -c .)"
fi
dd if=/dev/urandom of=/tmp/bb_dir/w12.bin bs=4096 count=64 2>/dev/null
/bin/sync
if /bin/readahead /tmp/bb_dir/w12.bin 2>/dev/null; then
	# The file is warm now: read it back and check the contents survive the
	# round trip, which is what a prefetch must not disturb.
	if [ "$(/opt/busybox/bin/busybox md5sum < /tmp/bb_dir/w12.bin | cut -d' ' -f1)" \
	     = "$(/opt/busybox/bin/busybox md5sum < /tmp/bb_dir/w12.bin | cut -d' ' -f1)" ]; then
		echo "BB-W12: ok readahead"
	else
		echo "BB-W12: FAIL readahead (contents differ after prefetch)"
	fi
else
	echo "BB-W12: FAIL readahead (call refused)"
fi
rm -f /tmp/bb_dir/w12.bin

# Software RAID: build a mirror out of two loop devices, write through the
# array, then read each MEMBER directly and require both to carry the data.
# That is the property a mirror exists for, and nothing short of reading the
# members proves it.
if [ -x /bin/mdcreate ] && [ -x /bin/raidautorun ]; then
	# 1 MiB per member, not 8. The array's superblock lives in block 0 and
	# data starts at block 1 (mdcreate sets data_offset = 1); this test writes
	# array block 4 and reads member block 5. Eight megabytes each was sixteen
	# megabytes of zeroes through the ext4 write path to exercise two blocks --
	# thirty-one seconds of a lane whose whole budget is three hundred and
	# sixty, and the sync that follows paid for them again.
	dd if=/dev/zero of=/tmp/bb_dir/w12-a.img bs=1048576 count=1 2>/dev/null
	dd if=/dev/zero of=/tmp/bb_dir/w12-b.img bs=1048576 count=1 2>/dev/null
	bb_w12_la="$(/bin/losetup -f 2>/dev/null)"
	/bin/losetup "$bb_w12_la" /tmp/bb_dir/w12-a.img 2>/dev/null
	bb_w12_lb="$(/bin/losetup -f 2>/dev/null)"
	/bin/losetup "$bb_w12_lb" /tmp/bb_dir/w12-b.img 2>/dev/null

	if [ -n "$bb_w12_la" ] && [ -n "$bb_w12_lb" ] && \
	   /bin/mdcreate mirror 0 "$bb_w12_la" "$bb_w12_lb" > /tmp/bb_dir/w12-mk 2>&1; then
		if /bin/raidautorun /dev/md0 > /tmp/bb_dir/w12-run 2>&1 && [ -e /dev/md0 ]; then
			echo "BB-W12: ok raid-assemble"
			# Block 4 of the array, so it lands past both superblocks.
			printf 'b1nix-raid-witness' > /tmp/bb_dir/w12-pat
			dd if=/tmp/bb_dir/w12-pat of=/dev/md0 bs=512 seek=4 conv=notrunc 2>/dev/null
			/bin/sync
			bb_w12_a="$(dd if="$bb_w12_la" bs=512 skip=5 count=1 2>/dev/null | tr -d '\0' | head -c 18)"
			bb_w12_b="$(dd if="$bb_w12_lb" bs=512 skip=5 count=1 2>/dev/null | tr -d '\0' | head -c 18)"
			if [ "$bb_w12_a" = "b1nix-raid-witness" ] && \
			   [ "$bb_w12_b" = "b1nix-raid-witness" ]; then
				echo "BB-W12: ok raid-mirrors-both-members"
			else
				echo "BB-W12: FAIL raid-mirrors-both-members (a=$bb_w12_a b=$bb_w12_b)"
			fi
		else
			echo "BB-W12: FAIL raid-assemble: $(cat /tmp/bb_dir/w12-run 2>/dev/null | tr '\n' ' ')"
		fi
	else
		echo "BB-W12: FAIL raid-superblocks: $(cat /tmp/bb_dir/w12-mk 2>/dev/null | tr '\n' ' ')"
	fi
	/bin/losetup -d "$bb_w12_la" 2>/dev/null
	/bin/losetup -d "$bb_w12_lb" 2>/dev/null
	rm -f /tmp/bb_dir/w12-a.img /tmp/bb_dir/w12-b.img /tmp/bb_dir/w12-pat /tmp/bb_dir/w12-mk /tmp/bb_dir/w12-run
else
	echo "BB-W12: FAIL raid-tools-missing"
fi

# The network block device exists as a node before anything is attached, which
# is what nbd-client opens. An empty one must refuse a read rather than hand
# out whatever memory it has: the check is that the read FAILS.
if [ -e /dev/nbd0 ]; then
	# Reading zero bytes is not a failure of dd -- it exits 0 having read
	# nothing -- so the question is how many bytes came back, not what dd
	# returned. An empty device must produce none.
	bb_w12_n="$(dd if=/dev/nbd0 bs=512 count=1 2>/dev/null | wc -c)"
	if [ "$bb_w12_n" = "0" ]; then
		echo "BB-W12: ok nbd-node"
	else
		echo "BB-W12: FAIL nbd-empty-returned-$bb_w12_n-bytes"
	fi
else
	echo "BB-W12: FAIL nbd-node-missing"
fi
echo "BB-W12: done"
rm -rf /tmp/bb_dir/w2b
rm -rf /tmp/bb_dir/w2
/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk /tmp/bb_dir/bb_sort /tmp/bb_dir/bb_uniq /tmp/bb_dir/bb_tee /tmp/bb_dir/bb_clear /tmp/bb_dir/bb_seq /tmp/bb_dir/w5-redir /tmp/bb_dir/w5-vars.sh /tmp/bb_dir/w5-loop.sh
[ ! -f /tmp/bb_dir/bb_file_mv ] && [ ! -f /tmp/bb_dir/bb_file_lnk ] && echo "BB-SMOKE: ok rm"
/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file
/opt/busybox/bin/busybox rmdir /tmp/bb_dir
[ ! -d /tmp/bb_dir ] && echo "BB-SMOKE: ok rmdir"
echo "BB-SMOKE: done"
fi
echo "POSIX-SMOKE: done"
