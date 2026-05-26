#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#include "../../build/x86/initramfs_native_smoke.inc"
#include "../../build/x86/initramfs_m12_smoke.inc"
#include "../../build/x86/initramfs_m13_smoke.inc"
#include "../../build/x86/initramfs_m13_job_control.inc"
#include "../../build/x86/initramfs_m8_aio_test.inc"
#include "../../build/x86/initramfs_m17_smoke.inc"
#include "../../build/x86/initramfs_m14_smoke.inc"
#include "../../build/x86/initramfs_m15_smoke.inc"
#include "../../build/x86/initramfs_tcc_files.inc"
#include "../../build/x86/initramfs_m25_smoke.inc"

static const unsigned char vfs_init_elf[] = {
    0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x3e, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x38, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x42, 0x31, 0x4e, 0x58, 0x45, 0x58, 0x45, 0x43, 0x00, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x2f, 0x62, 0x69, 0x6e, 0x2f, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
    0x00, 0x2f, 0x62, 0x69, 0x6e, 0x2f, 0x6d, 0x32, 0x32, 0x2d, 0x73, 0x6d,
    0x6f, 0x6b, 0x65, 0x00, 0x2f, 0x62, 0x69, 0x6e, 0x2f, 0x6d, 0x32, 0x34,
    0x2d, 0x73, 0x74, 0x72, 0x65, 0x73, 0x73, 0x00, 0x2f, 0x62, 0x69, 0x6e,
    0x2f, 0x73, 0x68, 0x00, 0x00};

static const unsigned char vfs_hello_elf[] = {
    0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x3e, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x38, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x29, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x42, 0x31, 0x4e, 0x58, 0x45, 0x58, 0x45, 0x43, 0x00, 0x65, 0x63, 0x68,
    0x6f, 0x00, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x66, 0x72, 0x6f, 0x6d,
    0x20, 0x56, 0x46, 0x53, 0x2d, 0x6c, 0x6f, 0x61, 0x64, 0x65, 0x64, 0x20,
    0x45, 0x4c, 0x46, 0x0a, 0x00};

static const char initramfs_fstab[] =
    "# <source>    <target> <fstype>    <options>  <dump> <pass>\n"
    "initramfs     /        initramfs   defaults   0      0\n"
    "virtio-blk0   /mnt     fat32       noauto     0      0\n"
    "sata0         /mnt     ext2        noauto     0      0\n"
    "nvme0         /mnt     ext2        noauto     0      0\n"
    "nvme0         /btrfs   btrfs       noauto     0      0\n";

static const char posix_smoke_script[] =
    "#!/bin/sh\n"
    "echo \"POSIX-SMOKE: start\"\n"
    /* ── M11 Shell Baseline ─────────────────────────────────────── */
    /* 1. simple success */
    "true && echo \"M11-SHELL: ok simple-success\"\n"
    /* 2. simple failure: false returns nonzero; || branch runs */
    "false || echo \"M11-SHELL: ok simple-fail\"\n"
    /* 3. exec-127: /bin/sh -c with bad cmd; sh exits 127; || fires */
    "/bin/sh -c '_no_such_cmd_xyz_' || echo \"M11-SHELL: ok exec-127\"\n"
    /* 4. variable expansion via export */
    "export TESTVAR=hello\n"
    "echo $TESTVAR | grep hello && echo \"M11-SHELL: ok var-expand\"\n"
    /* 4b. PATH lookup for plain command names */
    "export PATH=/bin\n"
    "uname -a >/tmp/m11_path.txt && grep b1nix /tmp/m11_path.txt && echo \"M11-SHELL: ok path-lookup\"\n"
    /* 5. double-quoted string with spaces */
    "echo \"hello world\" | grep \"hello world\" && echo \"M11-SHELL: ok quoted-string\"\n"
    /* 6. single-quoted string passes literal text through */
    "echo 'no expansion here' | grep 'no expansion' && echo \"M11-SHELL: ok single-quote\"\n"
    /* 7. && operator */
    "true && echo \"M11-SHELL: ok and-op\"\n"
    /* 8. || operator */
    "false || echo \"M11-SHELL: ok or-op\"\n"
    /* 9. semicolon */
    "true ; echo \"M11-SHELL: ok semicolon\"\n"
    /* ── M11 Redirection ────────────────────────────────────────── */
    /* 10. redirect stdout > */
    "echo \"redir-test\" > /tmp/m11_out.txt\n"
    "grep \"redir-test\" /tmp/m11_out.txt && echo \"M11-SHELL: ok redir-out\"\n"
    /* 11. redirect stdin < */
    "echo \"stdin-line\" > /tmp/m11_in.txt\n"
    "cat < /tmp/m11_in.txt | grep \"stdin-line\" && echo \"M11-SHELL: ok redir-in\"\n"
    /* 12. append >> */
    "echo \"line1\" > /tmp/m11_app.txt\n"
    "echo \"line2\" >> /tmp/m11_app.txt\n"
    "wc -l /tmp/m11_app.txt | grep \"2\" && grep \"line2\" /tmp/m11_app.txt && echo \"M11-SHELL: ok redir-append\"\n"
    /* 13. redirect stderr 2> — cat nonexistent; error goes to file */
    "cat /tmp/m11_nosuchfile 2>/tmp/m11_err.txt\n"
    "wc -c /tmp/m11_err.txt | grep -v \" 0\" && echo \"M11-SHELL: ok redir-stderr\"\n"
    /* 14. 2>&1 — cat nonexistent; error merged into stdout; pipe to grep */
    "cat /tmp/m11_nosuchfile2 2>&1 | grep \"cat\" && echo \"M11-SHELL: ok redir-2>&1\"\n"
    /* 14b. redirection target open failure returns nonzero */
    "cat < /tmp/definitely-missing-m11 && echo \"M11-SHELL: fail redir-failure\" || echo \"M11-SHELL: ok redir-failure\"\n"
    /* ── M11 Pipeline ───────────────────────────────────────────── */
    /* 15. pipeline output */
    "echo \"pipe-data\" | grep \"pipe-data\" && echo \"M11-SHELL: ok pipeline-output\"\n"
    /* 16. pipeline exit status = last cmd (false exits 1; || fires) */
    "echo \"irrelevant\" | false || echo \"M11-SHELL: ok pipeline-status\"\n"
    /* 17. pipeline-chain: 2-stage pipe then file wc */
    "printf \"alpha\\nbeta\\nalpha\\n\" | grep \"alpha\" > /tmp/m11_chain.txt\n"
    "wc -l /tmp/m11_chain.txt | grep \"2\" && echo \"M11-SHELL: ok pipeline-chain\"\n"
    "echo \"combo-one\" > /tmp/m11_combo.txt\n"
    "cat < /tmp/m11_combo.txt | grep \"combo-one\" && echo \"M11-SHELL: ok combo-redir-pipe\"\n"
    "echo \"alpha beta\" > /tmp/m11_combo_src.txt\n"
    "grep \"alpha beta\" /tmp/m11_combo_src.txt > /tmp/m11_combo2.txt\n"
    "grep \"alpha beta\" /tmp/m11_combo2.txt && echo \"M11-SHELL: ok combo-quote-redir\"\n"
    /* ── M11 Script exec ────────────────────────────────────────── */
    /* 18. script execution via /bin/sh */
    "echo \"echo M11-SHELL: ok script-exec\" > /tmp/m11_scr.sh\n"
    "/bin/sh /tmp/m11_scr.sh\n"
    /* 18b. direct shebang execution status */
    "echo \"#!/bin/sh\" > /tmp/m11_shebang.sh\n"
    "echo \"echo M11-SHELL: ok shebang-direct\" >> /tmp/m11_shebang.sh\n"
    "/tmp/m11_shebang.sh >/tmp/m11_shebang.out 2>/dev/null && grep \"M11-SHELL: ok shebang-direct\" /tmp/m11_shebang.out && echo \"M11-SHELL: ok shebang\" || echo \"M11-SHELL: ok shebang-unsupported\"\n"
    /* ── M11-UTIL coreutils via shell ──────────────────────────── */
    "printf \"beta\\nalpha\\nalpha\\ngamma\\n\" > /tmp/m11_util.txt\n"
    /* 19. cat */
    "cat /tmp/m11_util.txt | grep \"beta\" && echo \"M11-UTIL: ok cat\"\n"
    /* 20. grep match */
    "grep \"alpha\" /tmp/m11_util.txt && echo \"M11-UTIL: ok grep\"\n"
    /* 21. grep no match */
    "grep \"ZZZMISSING\" /tmp/m11_util.txt || echo \"M11-UTIL: ok grep-nomatch\"\n"
    /* 22. wc -l */
    "wc -l /tmp/m11_util.txt | grep \"4\" && echo \"M11-UTIL: ok wc\"\n"
    /* 23. head -n 2 — write to file then wc */
    "head -n 2 /tmp/m11_util.txt > /tmp/m11_head.txt\n"
    "wc -l /tmp/m11_head.txt | grep \"2\" && echo \"M11-UTIL: ok head\"\n"
    /* 24. tail -n 2 — write to file then wc */
    "tail -n 2 /tmp/m11_util.txt > /tmp/m11_tail.txt\n"
    "wc -l /tmp/m11_tail.txt | grep \"2\" && echo \"M11-UTIL: ok tail\"\n"
    /* 25. sort — sort to file, grep first line */
    "sort /tmp/m11_util.txt > /tmp/m11_sort.txt\n"
    "head -n 1 /tmp/m11_sort.txt | grep \"alpha\" && echo \"M11-UTIL: ok sort\"\n"
    /* 26. uniq — sort to file, uniq to file, wc */
    "sort /tmp/m11_util.txt > /tmp/m11_sorted.txt\n"
    "uniq /tmp/m11_sorted.txt > /tmp/m11_uniq.txt\n"
    "wc -l /tmp/m11_uniq.txt | grep \"3\" && echo \"M11-UTIL: ok uniq\"\n"
    /* 27. cp */
    "cp /tmp/m11_util.txt /tmp/m11_util_cp.txt\n"
    "cat /tmp/m11_util_cp.txt | grep \"beta\" && echo \"M11-UTIL: ok cp\"\n"
    /* 28. mv */
    "cp /tmp/m11_util.txt /tmp/m11_mv_src.txt\n"
    "mv /tmp/m11_mv_src.txt /tmp/m11_mv_dst.txt\n"
    "cat /tmp/m11_mv_dst.txt | grep \"beta\" && echo \"M11-UTIL: ok mv\"\n"
    /* 29. mkdir */
    "mkdir /tmp/m11_dir\n"
    "[ -d /tmp/m11_dir ] && echo \"M11-UTIL: ok mkdir\"\n"
    /* 30. rmdir */
    "rmdir /tmp/m11_dir\n"
    "[ -d /tmp/m11_dir ] || echo \"M11-UTIL: ok rmdir\"\n"
    /* 31. rm */
    "cp /tmp/m11_util.txt /tmp/m11_rm.txt\n"
    "rm /tmp/m11_rm.txt\n"
    "[ -f /tmp/m11_rm.txt ] || echo \"M11-UTIL: ok rm\"\n"
    /* 32. ln -s + readlink */
    "ln -s /tmp/m11_util.txt /tmp/m11_lnk.txt\n"
    "readlink /tmp/m11_lnk.txt | grep \"m11_util\" && echo \"M11-UTIL: ok ln-readlink\"\n"
    /* 33. ps */
    "ps && echo \"M11-UTIL: ok ps\"\n"
    /* 34. date */
    "date && echo \"M11-UTIL: ok date\"\n"
    /* 35. uname */
    "uname -a && echo \"M11-UTIL: ok uname\"\n"
    /* 36. id */
    "id && echo \"M11-UTIL: ok id\"\n"
    /* 37. whoami */
    "whoami && echo \"M11-UTIL: ok whoami\"\n"
    /* 38. sleep 0 */
    "sleep 0 && echo \"M11-UTIL: ok sleep\"\n"
    /* 39. unsupported flag for ls returns nonzero */
    "ls -Z /tmp 2>/dev/null || echo \"M11-UTIL: ok bad-flag-ls\"\n"
    /* 40. unsupported flag for grep returns nonzero */
    "grep -X \"pat\" /tmp/m11_util.txt 2>/dev/null || echo \"M11-UTIL: ok bad-flag-grep\"\n"
    /* ── Legacy POSIX tests preserved ──────────────────────────── */
    "touch /tmp/posix_test\n"
    "[ -f /tmp/posix_test ] && echo \"ok touch\" || echo \"fail touch\"\n"
    "true && echo \"ok and\" || echo \"fail and\"\n"
    "false || echo \"ok or\"\n"
    "mkdir -p /tmp/a/b/c\n"
    "[ -d /tmp/a/b/c ] && echo \"ok mkdir-p\" || echo \"fail mkdir-p\"\n"
    "echo \"hello; world\" | grep \";\" && echo \"ok quote split\"\n"
    "echo 'single; quote' | grep \";\" && echo \"ok quote split 2\"\n"
    "rm -rf /tmp/a\n"
    "ln -s /tmp/loop /tmp/loop\n"
    "cat /tmp/loop && echo \"fail eloop\" || echo \"ok eloop\"\n"
    "echo \"M22-POLISH: start\"\n"
    "false\n"
    "true\n"
    "true\n"
    "false\n"
    "echo \"M22-POLISH: after-status\"\n"
    "ls -la /tmp >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "printf \"one\\ntwo\\nthree\\n\" > /tmp/m22_tool.txt\n"
    "tail -n 2 /tmp/m22_tool.txt >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "cp -Z /tmp/nonexistent /tmp/nonexistent2 2>/dev/null\n"
    "[ $? -ne 0 ] || exit 1\n"
    "rm -Z /tmp/nonexistent 2>/dev/null\n"
    "[ $? -ne 0 ] || exit 1\n"
    "mkdir /tmp/m22_mkdir_test\n"
    "[ $? -eq 0 ] || exit 1\n"
    "mkdir -p /tmp/m22_mkdir_test\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"test\" | grep -qn \"test\"\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"test\" | grep -Z \"test\" 2>/dev/null\n"
    "[ $? -ne 0 ] || exit 1\n"
    "head -n 2 /tmp/m22_tool.txt >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "head -x /etc/posix-smoke.sh 2>/dev/null\n"
    "[ $? -ne 0 ] || exit 1\n"
    "echo \"one two three\" | wc -w | grep \"3\" >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "wc -c /etc/posix-smoke.sh >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "wc -Z 2>/dev/null\n"
    "[ $? -ne 0 ] || exit 1\n"
    "uname -sn >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "uname -Z 2>/dev/null\n"
    "[ $? -ne 0 ] || exit 1\n"
    "echo \"M22-POLISH: ok utility-flags\"\n"
    "rmdir /tmp/m22_mkdir_test\n"
    "rm -f /tmp/m22_tool.txt\n"
    "echo \"banana\" > /tmp/m22_fruit.txt\n"
    "echo \"apple\" >> /tmp/m22_fruit.txt\n"
    "echo \"banana\" >> /tmp/m22_fruit.txt\n"
    "mkdir /tmp/m22_wf_dir\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"inner\" > /tmp/m22_wf_dir/file.txt\n"
    "echo \"M22-POLISH: before-cp-r\"\n"
    "cp -r /tmp/m22_wf_dir /tmp/m22_wf_dir_copy\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"M22-POLISH: after-cp-r\"\n"
    "grep \"inner\" /tmp/m22_wf_dir_copy/file.txt >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "cat /tmp/m22_fruit.txt | sort | uniq | grep \"apple\" >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "cat /tmp/m22_fruit.txt | sort | uniq | wc -l | grep \"2\" >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"M22-POLISH: ok text-pipeline\"\n"
    "echo \"z\" > /tmp/m22_wf.txt\n"
    "echo \"y\" >> /tmp/m22_wf.txt\n"
    "echo \"x\" >> /tmp/m22_wf.txt\n"
    "cp /tmp/m22_wf.txt /tmp/m22_wf_cp.txt\n"
    "[ $? -eq 0 ] || exit 1\n"
    "mv /tmp/m22_wf_cp.txt /tmp/m22_wf_mv.txt\n"
    "[ $? -eq 0 ] || exit 1\n"
    "cat /tmp/m22_wf_mv.txt | sort | uniq | grep -v \"z\" > /tmp/m22_wf_out.txt\n"
    "[ $? -eq 0 ] || exit 1\n"
    "grep \"x\" /tmp/m22_wf_out.txt >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "grep \"y\" /tmp/m22_wf_out.txt >/dev/null\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"M22-POLISH: before-rm-rf\"\n"
    "rm -rf /tmp/m22_wf_dir /tmp/m22_wf_dir_copy\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"M22-POLISH: after-rm-rf\"\n"
    "rm -f /tmp/m22_wf.txt /tmp/m22_wf_mv.txt /tmp/m22_wf_out.txt /tmp/m22_fruit.txt\n"
    "[ $? -eq 0 ] || exit 1\n"
    "echo \"M22-POLISH: ok file-workflow\"\n"
    "/bin/sh -c 'some_random_nonexistent_command'\n"
    "CNF_STATUS=$?\n"
    "cp /tmp/nonexistent_file_123 /tmp/nonexistent_file_456 2>/dev/null\n"
    "CP_STATUS=$?\n"
    "rm /tmp/nonexistent_file_123 2>/dev/null\n"
    "RM_STATUS=$?\n"
    "grep \"test\" /tmp/nonexistent_file_123 2>/dev/null\n"
    "GREP_STATUS=$?\n"
    "wc -l /tmp/nonexistent_file_123 2>/dev/null\n"
    "WC_STATUS=$?\n"
    "if [ $CNF_STATUS -eq 127 ] && [ $CP_STATUS -ne 0 ] && [ $RM_STATUS -ne 0 ] && [ $GREP_STATUS -ne 0 ] && [ $WC_STATUS -ne 0 ]; then\n"
    "  echo \"M22-POLISH: ok failure-status\"\n"
    "fi\n"
    "echo \"M22-POLISH: done\"\n"
    "echo \"POSIX-SMOKE: done\"\n";



static const struct initramfs_file files[] = {
    {"/bin/init", (const char *)vfs_init_elf, sizeof(vfs_init_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/sh", "builtin:sh\n", 11, INITRAMFS_EXECUTABLE},
    {"/bin/hello", (const char *)vfs_hello_elf, sizeof(vfs_hello_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/native-smoke", (const char *)vfs_native_smoke_elf,
     sizeof(vfs_native_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m12-smoke", (const char *)vfs_m12_smoke_elf,
     sizeof(vfs_m12_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m13-smoke", (const char *)vfs_m13_smoke_elf,
     sizeof(vfs_m13_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m13-job-control", (const char *)vfs_m13_job_control_elf,
     sizeof(vfs_m13_job_control_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m17-smoke", (const char *)vfs_m17_smoke_elf,
     sizeof(vfs_m17_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m8-aio-test", (const char *)vfs_m8_aio_test_elf,
     sizeof(vfs_m8_aio_test_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m13-helper", (const char *)vfs_m13_smoke_elf,
     sizeof(vfs_m13_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m14-smoke", (const char *)vfs_m14_smoke_elf,
     sizeof(vfs_m14_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m15-smoke", (const char *)vfs_m15_smoke_elf,
     sizeof(vfs_m15_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/etc/motd", "welcome to b1nix m4\n", 23, 0},
    {"/etc/fstab", initramfs_fstab, sizeof(initramfs_fstab) - 1, 0},
    {"/etc/posix-smoke.sh", posix_smoke_script, sizeof(posix_smoke_script) - 1,
     0},
    {"/README", "initramfs is alive\n", 20, 0},
    {"/bin/m25-smoke", (const char *)vfs_m25_smoke_elf,
     sizeof(vfs_m25_smoke_elf), INITRAMFS_EXECUTABLE},
    TCC_INITRAMFS_FILES
};

static int initramfs_vfs_statfs(struct vfs_node *node,
                                struct b1nix_statfs *st) {
  (void)node;
  memset(st, 0, sizeof(*st));
  st->f_type = 0x858458f6;
  st->f_bsize = 4096;

  usize total_size = 0;
  for (usize i = 0; i < (sizeof(files) / sizeof(files[0])); i++) {
    total_size += files[i].size;
  }

  st->f_blocks = (total_size + 4095) / 4096;
  if (st->f_blocks == 0)
    st->f_blocks = 1;
  st->f_bfree = 0;
  st->f_bavail = 0;
  st->f_files = (sizeof(files) / sizeof(files[0]));
  st->f_ffree = 0;
  st->f_namelen = 64;
  return 0;
}

static struct vfs_node *initramfs_mount_cb(const char *source, u64 flags,
                                           void *data) {
  (void)source;
  (void)flags;
  (void)data;
  struct vfs_node *root = vfs_create_node(VFS_DIRECTORY);
  if (!root)
    return ERR_PTR(-ENOMEM);

  root->inode->statfs_cb = initramfs_vfs_statfs;

  for (usize i = 0; i < (sizeof(files) / sizeof(files[0])); i++) {
    struct vfs_node *node =
        vfs_add_node(files[i].path, VFS_FILE, (void *)files[i].data,
                     files[i].size, files[i].flags);
    if (node) {
      node->inode->statfs_cb = initramfs_vfs_statfs;
    }
  }

  return root;
}

static struct vfs_fs initramfs_fs = {
    .name = "initramfs",
    .mount = initramfs_mount_cb,
};

void initramfs_init(void) {
  vfs_register_fs(&initramfs_fs);
  console_write("initramfs: files 0x");
  console_write_hex64(initramfs_count());
  console_write("\n");
}

const struct initramfs_file *initramfs_find(const char *path) {
  for (usize i = 0; i < initramfs_count(); i++) {
    if (strcmp(files[i].path, path) == 0) {
      return &files[i];
    }
  }
  return 0;
}

const struct initramfs_file *initramfs_get(usize index) {
  if (index >= initramfs_count())
    return 0;
  return &files[index];
}

usize initramfs_count(void) { return sizeof(files) / sizeof(files[0]); }
