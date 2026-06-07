#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#include "initramfs_native_smoke.inc"
#include "initramfs_m12_smoke.inc"
#include "initramfs_m13_smoke.inc"
#include "initramfs_m13_job_control.inc"
#include "initramfs_m8_aio_test.inc"
#include "initramfs_m17_smoke.inc"
#include "initramfs_m14_smoke.inc"
#include "initramfs_m15_smoke.inc"
#include "initramfs_tcc_files.inc"
#include "initramfs_m25_smoke.inc"
#include "initramfs_m26_smoke.inc"
#include "initramfs_m24b_smoke.inc"
#include "initramfs_m27_smoke.inc"
#include "initramfs_m29_smoke.inc"
#include "initramfs_m31_smoke.inc"
#include "initramfs_m31_setuid.inc"
#include "initramfs_m32_smoke.inc"
#include "initramfs_m32_nettool.inc"
#include "initramfs_m32_pcre2_smoke.inc"
#include "initramfs_curl.inc"
#include "initramfs_wget.inc"
#include "initramfs_cacert.inc"
#include "initramfs_tlstest.inc"
#include "initramfs_m30_pie.inc"
#include "initramfs_m34_smoke.inc"
#include "initramfs_m35_smoke.inc"
#include "initramfs_dropbear.inc"
#include "initramfs_su.inc"
#include "initramfs_passwd.inc"
#include "initramfs_id.inc"
#include "initramfs_whoami.inc"
#include "initramfs_groups.inc"
#include "initramfs_useradd.inc"
#include "initramfs_userdel.inc"
#include "initramfs_groupadd.inc"
#include "initramfs_chown.inc"
#include "initramfs_chmod.inc"
#if B1NIX_UPSTREAM_BUSYBOX
#include "initramfs_busybox.inc"
#endif



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

/* User database. name:passwd:uid:gid:gecos:home:shell. The pw_passwd
 * field is "x", meaning the real hash lives in /etc/shadow. */
static const char initramfs_passwd[] =
    "root:x:0:0:root:/root:/bin/sh\n"
    "user:x:1000:1000:b1nix user:/home/user:/bin/sh\n";

/* M31: shadow database. Format: name:hash:lastchange:min:max:warn:inactive:expire:reserved
 * Empty fields after the hash are POSIX-compliant placeholders. The hash
 * is b1nix's $b1$<salt>$<base64> format (kernel/lib/crypt.c). Passwords
 * here are 'root' and 'user' — change via /bin/passwd. */
static const char initramfs_shadow[] =
    "root:$b1$rootsalt$YLR0bb6kf/9n3oGVFfPVbAVfAqs.k/9jnNVshpAFNbj6hBY2OZmMg6Jav8muEuSt8vkIU8mahAr9KwerDzvv6Q:0:0:99999:7:::\n"
    "user:$b1$usersalt$BaxHIimflG4IPjGvD7HDPKcnI1nRIILqEKYNIyHy6iDPeyxWpyqT4p5Hir8Iauy.ZiTCnjIUPj1KKlgdLNBHXQ:0:0:99999:7:::\n";

static const char initramfs_group[] =
    "root:x:0:\n"
    "daemon:x:1:\n"
    "tty:x:5:\n"
    "disk:x:6:\n"
    "net:x:7:\n"
    "wheel:x:10:root\n"
    "users:x:1000:user\n";

static const char initramfs_sshd_service[] =
    "#!/bin/sh\n"
    "# System-wide SSH daemon service control script\n"
    "\n"
    "export PATH=\"/bin\"\n"
    "export PIDFILE=\"/var/run/sshd.pid\"\n"
    "export LOGFILE=\"/var/log/sshd.log\"\n"
    "\n"
    "case $1 in\n"
    "  start)\n"
    "    [ -f $PIDFILE ] && export pid=$(cat $PIDFILE) && kill -0 $pid 2>/dev/null && echo \"sshd is already running (PID: $pid)\" && exit 0\n"
    "    [ -f $PIDFILE ] && rm -f $PIDFILE\n"
    "\n"
    /* M32c host-key persistence: the host key is stored in /etc/ssh/hk_ed25519,
     * which resides on the mounted persistent rootfs (if mounted). */
    "    export HOSTKEY=/etc/ssh/hk_ed25519\n"
    "    mkdir -p /etc/ssh\n"
    "    [ -f $HOSTKEY ] || echo \"sshd: generating host key...\"\n"
    "    [ -f $HOSTKEY ] || /bin/dropbearkey -t ed25519 -f $HOSTKEY\n"
    "\n"
    "    mkdir -p /var/run /var/log\n"
    "\n"
    /* M32c bind policy: loopback-only is the SAFE default so the daemon is
     * never exposed by accident. b1nix.ssh-external opts in to all interfaces
     * (0.0.0.0); b1nix.ssh-loopback is the explicit, back-compatible way to
     * keep the loopback default. NOTE: keep shell '#' comments out of this
     * case arm — comment text containing ')' collides with case pattern
     * syntax in the in-kernel shell and silently aborts the arm. */
    "    export BIND_ADDR=\"127.0.0.1:\"\n"
    "    [ -f /proc/cmdline ] && grep -q \"b1nix.ssh-external\" /proc/cmdline && export BIND_ADDR=\"\"\n"
    "    [ -f /proc/cmdline ] && grep -q \"b1nix.ssh-loopback\" /proc/cmdline && export BIND_ADDR=\"127.0.0.1:\"\n"
    "\n"
    /* M32c hardening: sane connection lifecycle defaults are always on; the
     * root/password restrictions are opt-in so the automated smoke (root +
     * password over loopback) keeps working while an exposed deployment can
     * tighten policy via b1nix.ssh-no-root (-w) / b1nix.ssh-pubkey-only (-s). */
    "    export HARDEN=\"-I 300 -K 60 -T 6\"\n"
    "    [ -f /proc/cmdline ] && grep -q \"b1nix.ssh-no-root\" /proc/cmdline && export HARDEN=\"$HARDEN -w\"\n"
    "    [ -f /proc/cmdline ] && grep -q \"b1nix.ssh-pubkey-only\" /proc/cmdline && export HARDEN=\"$HARDEN -s\"\n"
    "\n"
    "    echo \"sshd: starting daemon (bind: ${BIND_ADDR}22)...\"\n"
    "    /bin/dropbear -r $HOSTKEY -p ${BIND_ADDR}22 $HARDEN -F >$LOGFILE 2>&1 &\n"
    "    echo $! > $PIDFILE\n"
    "    echo \"sshd: started (PID: $(cat $PIDFILE))\"\n"
    "    ;;\n"
    "  stop)\n"
    "    [ -f $PIDFILE ] || echo \"sshd is not running\"\n"
    "    [ -f $PIDFILE ] || exit 0\n"
    "    export pid=$(cat $PIDFILE)\n"
    "    echo \"sshd: stopping daemon (PID: $pid)...\"\n"
    "    kill $pid 2>/dev/null\n"
    "    sleep 1\n"
    "    kill -0 $pid 2>/dev/null && sleep 1\n"
    "    kill -0 $pid 2>/dev/null && kill -9 $pid 2>/dev/null\n"
    "    rm -f $PIDFILE\n"
    "    echo \"sshd: stopped\"\n"
    "    ;;\n"
    "  restart)\n"
    "    /bin/sh $0 stop\n"
    "    sleep 1\n"
    "    /bin/sh $0 start\n"
    "    ;;\n"
    "  status)\n"
    "    [ -f $PIDFILE ] && export pid=$(cat $PIDFILE) && kill -0 $pid 2>/dev/null && echo \"sshd is running (PID: $pid)\" && exit 0\n"
    "    echo \"sshd: stopped\"\n"
    "    exit 1\n"
    "    ;;\n"
    "  *)\n"
    "    echo \"Usage: $0 {start|stop|restart|status}\"\n"
    "    exit 1\n"
    "    ;;\n"
    "esac\n";

/* Boot rc script: init runs this once at startup, before the login shell.
 * The first-boot block is setup for the persistent root image; it is
 * idempotent via the .b1nix-setup marker. */
static const char initramfs_rc[] =
    "#!/bin/sh\n"
    "# b1nix boot rc script - runs once at startup, before the login shell.\n"
    "echo \"M27-INIT: rc-script start\"\n"
    "[ -f /etc/motd ] && cat /etc/motd\n"
    /* Home directories for the accounts in /etc/passwd so a login shell (local
     * or over SSH) has a valid working directory instead of warning on chdir. */
    "mkdir -p /root /home/user /tmp\n"
    "[ ! -f /.b1nix-setup ] && mkdir -p /root /home/user /tmp "
    "&& echo ready > /.b1nix-setup "
    "&& echo \"M27-INIT: first-boot rootfs initialised\"\n"
    /* Start the SSH daemon service. The init.d script generates the Ed25519
     * host key on first start (now working on both x86 and x86_64) and
     * backgrounds dropbear, so this returns promptly and leaves a pid file for
     * service management. */
    "# Start SSH daemon service\n"
    "[ -f /etc/init.d/sshd ] && /bin/sh /etc/init.d/sshd start\n"
    "echo \"M27-INIT: ok rc-script\"\n";

/* Resolver configuration. The kernel DNS client parses the first
 * "nameserver" line lazily (kernel/net/dns.c); 10.0.2.3 is the QEMU
 * user-mode-networking built-in resolver. */
static const char initramfs_resolv_conf[] =
    "nameserver 10.0.2.3\n";

#if B1NIX_UPSTREAM_BUSYBOX
/* Migration wave 2b: a tiny .xz fixture so the upstream BusyBox smoke can
 * exercise xz decompression (xzcat/unxz). BusyBox ships an xz *decompressor*
 * only — there is no xz compressor applet — so the compressed input cannot be
 * produced inside b1nix and is embedded here instead. Plaintext: "b1nix-xz-OK\n"
 * Compressed with `xz -0 --check=crc32`: a 256 KiB dictionary (NOT -9's 64 MiB,
 * which would OOM the 128 MiB test VM) and a CRC32 integrity check. */
static const unsigned char initramfs_bb_w2b_xz[] = {
    0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x01, 0x69, 0x22, 0xde, 0x36,
    0x03, 0xc0, 0x10, 0x0c, 0x21, 0x01, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0xe3, 0x11, 0x88, 0x01, 0x00, 0x0b, 0x62, 0x31, 0x6e, 0x69, 0x78,
    0x2d, 0x78, 0x7a, 0x2d, 0x4f, 0x4b, 0x0a, 0x00, 0x6d, 0xc4, 0xbc, 0x36,
    0x00, 0x01, 0x24, 0x0c, 0xa6, 0x18, 0xd8, 0xd8, 0x90, 0x42, 0x99, 0x0d,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x59, 0x5a};
#endif

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
    /* 15b. M33 concurrent pipeline: stream this multi-KB script (>512B, the pipe
     * buffer) through a pipe. The producer (cat) and consumer (wc) now run
     * concurrently, so the producer never blocks on a full pipe before the
     * consumer starts. If the old sequential path were still in use this line
     * would deadlock and the boot would never reach POSIX-SMOKE: done. */
    "cat /etc/posix-smoke.sh | cat > /tmp/m33_pipe.out && echo \"M33-SHELL: ok pipe-large\"\n"
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
#if B1NIX_UPSTREAM_BUSYBOX
    "# ── Upstream BusyBox package smoke tests ──\n"
    "echo \"BB-SMOKE: start\"\n"
    "/opt/busybox/bin/busybox --list | grep -q \"echo\" && echo \"BB-SMOKE: ok list\"\n"
    "/opt/busybox/bin/busybox echo \"hello bb\" | grep -q \"hello bb\" && echo \"BB-SMOKE: ok echo\"\n"
    "/opt/busybox/bin/busybox printf \"hello %s\\\\n\" bb | grep -q \"hello bb\" && echo \"BB-SMOKE: ok printf\"\n"
    "/opt/busybox/bin/busybox pwd | grep -q \"/\" && echo \"BB-SMOKE: ok pwd\"\n"
    "/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir\n"
    "[ -d /tmp/bb_dir ] && echo \"BB-SMOKE: ok mkdir\"\n"
    "/opt/busybox/bin/busybox touch /tmp/bb_dir/bb_file\n"
    "[ -f /tmp/bb_dir/bb_file ] && echo \"BB-SMOKE: ok touch\"\n"
    "echo \"content123\" > /tmp/bb_dir/bb_file\n"
    "/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file | grep -q \"content123\" && echo \"BB-SMOKE: ok cat\"\n"
    "/opt/busybox/bin/busybox cp /tmp/bb_dir/bb_file /tmp/bb_dir/bb_file_cp\n"
    "/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_cp | grep -q \"content123\" && [ -f /tmp/bb_dir/bb_file_cp ] && echo \"BB-SMOKE: ok cp\"\n"
    "/opt/busybox/bin/busybox mv /tmp/bb_dir/bb_file_cp /tmp/bb_dir/bb_file_mv\n"
    "[ ! -f /tmp/bb_dir/bb_file_cp ] && [ -f /tmp/bb_dir/bb_file_mv ] && /opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_mv | grep -q \"content123\" && echo \"BB-SMOKE: ok mv\"\n"
    "/opt/busybox/bin/busybox ln -s /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk\n"
    "/opt/busybox/bin/busybox cat /tmp/bb_dir/bb_file_lnk | grep -q \"content123\" && echo \"BB-SMOKE: ok ln\"\n"
    "/opt/busybox/bin/busybox readlink /tmp/bb_dir/bb_file_lnk | grep -q \"bb_file_mv\" && echo \"BB-SMOKE: ok readlink\"\n"
    "/opt/busybox/bin/busybox chmod 755 /tmp/bb_dir/bb_file_mv\n"
    "[ -x /tmp/bb_dir/bb_file_mv ] && echo \"BB-SMOKE: ok chmod\"\n"
    "/opt/busybox/bin/busybox test -f /tmp/bb_dir/bb_file_mv && /opt/busybox/bin/busybox [ -d /tmp/bb_dir ] && echo \"BB-SMOKE: ok test\"\n"
    "printf \"b\\\\nc\\\\na\\\\n\" > /tmp/bb_dir/bb_sort\n"
    "/opt/busybox/bin/busybox sort /tmp/bb_dir/bb_sort | head -n 1 | grep -q \"a\" && echo \"BB-SMOKE: ok sort\"\n"
    "printf \"a\\\\na\\\\nb\\\\n\" > /tmp/bb_dir/bb_uniq\n"
    "/opt/busybox/bin/busybox uniq /tmp/bb_dir/bb_uniq | wc -l | grep -q \"2\" && echo \"BB-SMOKE: ok uniq\"\n"
    "/opt/busybox/bin/busybox ls /tmp/bb_dir | grep -q \"bb_file\" && echo \"BB-W1: ok ls\"\n"
    "/opt/busybox/bin/busybox cmp /tmp/bb_dir/bb_file /tmp/bb_dir/bb_file && echo \"BB-W1: ok cmp\"\n"
    "printf \"left:right\\\\n\" | /opt/busybox/bin/busybox cut -d: -f2 | grep -q \"right\" && echo \"BB-W1: ok cut\"\n"
    "/opt/busybox/bin/busybox env BB_W1=value | grep -q \"BB_W1=value\" && echo \"BB-W1: ok env\"\n"
    "BB_ID_OUT=$(/opt/busybox/bin/busybox id -u)\n"
    "[ \"$BB_ID_OUT\" = \"0\" ] && echo \"BB-W1: ok id\"\n"
    "/opt/busybox/bin/busybox printenv PATH >/dev/null && echo \"BB-W1: ok printenv\"\n"
    "printf \"tee-data\\\\n\" | /opt/busybox/bin/busybox tee /tmp/bb_dir/bb_tee | grep -q \"tee-data\" && grep -q \"tee-data\" /tmp/bb_dir/bb_tee && echo \"BB-W1: ok tee\"\n"
    "printf \"abc\\\\n\" | /opt/busybox/bin/busybox tr a-z A-Z | grep -q \"ABC\" && echo \"BB-W1: ok tr\"\n"
    "/opt/busybox/bin/busybox whoami | grep -q \"root\" && echo \"BB-W1: ok whoami\"\n"
    "/opt/busybox/bin/busybox seq 1 3 > /tmp/bb_dir/bb_seq\n"
    "tail -n 1 /tmp/bb_dir/bb_seq | grep -q \"3\" && echo \"BB-W1: ok seq\"\n"
    "/opt/busybox/bin/busybox which ls | grep -q \"/bin/ls\" && echo \"BB-W1: ok which\"\n"
    "/opt/busybox/bin/busybox clear >/tmp/bb_dir/bb_clear && echo \"BB-W1: ok clear\"\n"
    "printf \"AB\" | /opt/busybox/bin/busybox hexdump | grep -q \"4241\" && echo \"BB-W1: ok hexdump\"\n"
    "mkdir -p /tmp/bb_dir/w2/sub\n"
    "printf \"alpha1\\\\nbeta2\\\\n\" > /tmp/bb_dir/w2/sub/data.txt\n"
    "/opt/busybox/bin/busybox stat -c %s /tmp/bb_dir/w2/sub/data.txt | grep -q \"13\" && echo \"BB-W2: ok stat\"\n"
    "/opt/busybox/bin/busybox realpath /tmp/bb_dir/w2/sub/data.txt | grep -q \"/tmp/bb_dir/w2/sub/data.txt\" && echo \"BB-W2: ok realpath\"\n"
    "BB_TMP=$(/opt/busybox/bin/busybox mktemp -d /tmp/bb_dir/w2/tmp.XXXXXX)\n"
    "[ -d \"$BB_TMP\" ] && echo \"BB-W2: ok mktemp\"\n"
    "/opt/busybox/bin/busybox find /tmp/bb_dir/w2 -name \"*.txt\" | grep -q \"data.txt\" && echo \"BB-W2: ok find\"\n"
    "/opt/busybox/bin/busybox grep -E \"alpha[0-9]+\" /tmp/bb_dir/w2/sub/data.txt | grep -q \"alpha1\" && echo \"BB-W2: ok grep\"\n"
    "/opt/busybox/bin/busybox grep -Ei \"[A-Z]+[0-9]\" /tmp/bb_dir/w2/sub/data.txt | grep -q \"alpha1\" && echo \"BB-W2: ok grep-icase\"\n"
    "printf \"name-42\\\\n\" | /opt/busybox/bin/busybox sed \"s/\\\\([a-z]*\\\\)-\\\\([0-9]*\\\\)/\\\\2:\\\\1/\" | grep -q \"42:name\" && echo \"BB-W2: ok sed\"\n"
    "printf \"left:right\\\\n\" | /opt/busybox/bin/busybox awk -F: \"{ print NF }\" | grep -q \"2\" && echo \"BB-W2: ok awk\"\n"
    "printf \"one two\\\\n\" | /opt/busybox/bin/busybox xargs /opt/busybox/bin/busybox echo | grep -q \"one two\" && echo \"BB-W2: ok xargs\"\n"
    "cp /tmp/bb_dir/w2/sub/data.txt /tmp/bb_dir/w2/sub/same.txt\n"
    "/opt/busybox/bin/busybox diff /tmp/bb_dir/w2/sub/data.txt /tmp/bb_dir/w2/sub/same.txt\n"
    "BB_DIFF_SAME=$?\n"
    "printf \"changed\\\\n\" >> /tmp/bb_dir/w2/sub/same.txt\n"
    "/opt/busybox/bin/busybox diff /tmp/bb_dir/w2/sub/data.txt /tmp/bb_dir/w2/sub/same.txt >/dev/null\n"
    "BB_DIFF_CHANGED=$?\n"
    "[ $BB_DIFF_SAME -eq 0 ] && [ $BB_DIFF_CHANGED -ne 0 ] && echo \"BB-W2: ok diff\"\n"
    "printf \"abc\" > /tmp/bb_dir/w2/abc\n"
    "/opt/busybox/bin/busybox cksum /tmp/bb_dir/w2/abc | grep -q \"1219131554 3\" && echo \"BB-W2: ok cksum\"\n"
    "/opt/busybox/bin/busybox md5sum /tmp/bb_dir/w2/abc | grep -q \"900150983cd24fb0d6963f7d28e17f72\" && echo \"BB-W2: ok md5sum\"\n"
    "/opt/busybox/bin/busybox sha256sum /tmp/bb_dir/w2/abc | grep -q \"ba7816bf8f01cfea414140de5dae2223\" && echo \"BB-W2: ok sha256sum\"\n"
    /* ── Migration wave 2b: file/archive utilities ── */
    "/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir/w2b\n"
    "/opt/busybox/bin/busybox seq 1 4000 > /tmp/bb_dir/w2b/big.txt\n"
    /* dd: byte-exact copy with bs/count */
    "/opt/busybox/bin/busybox dd if=/tmp/bb_dir/w2/abc of=/tmp/bb_dir/w2b/abc.dd bs=1 count=3 2>/dev/null\n"
    "/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2/abc /tmp/bb_dir/w2b/abc.dd && echo \"BB-W2B: ok dd\"\n"
    /* du: reports a numeric block count for the file. Use busybox grep — the
     * native shell grep is a literal substring matcher and cannot evaluate the
     * [0-9] character class. */
    "/opt/busybox/bin/busybox du -k /tmp/bb_dir/w2b/big.txt | /opt/busybox/bin/busybox grep -q \"[0-9]\" && echo \"BB-W2B: ok du\"\n"
    /* df: lists the root mount via /proc/mounts */
    "/opt/busybox/bin/busybox df / | grep -q \"/\" && echo \"BB-W2B: ok df\"\n"
    /* tar: create + extract, byte-verify the round trip */
    "/opt/busybox/bin/busybox tar -cf /tmp/bb_dir/w2b/t.tar -C /tmp/bb_dir/w2b big.txt\n"
    "/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir/w2b/ex\n"
    "/opt/busybox/bin/busybox tar -xf /tmp/bb_dir/w2b/t.tar -C /tmp/bb_dir/w2b/ex\n"
    "/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/ex/big.txt && echo \"BB-W2B: ok tar\"\n"
    /* tar + gzip seamless extract (-xz): gzip the plain tar with the gzip
     * applet, then let tar auto-decompress it on extract (internal inflate,
     * no external compressor process). */
    "/opt/busybox/bin/busybox gzip -c /tmp/bb_dir/w2b/t.tar > /tmp/bb_dir/w2b/tz.tar.gz\n"
    "/opt/busybox/bin/busybox mkdir -p /tmp/bb_dir/w2b/exz\n"
    "/opt/busybox/bin/busybox tar -xzf /tmp/bb_dir/w2b/tz.tar.gz -C /tmp/bb_dir/w2b/exz\n"
    "/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/exz/big.txt && echo \"BB-W2B: ok tar-gzip\"\n"
    /* gzip round trip */
    "/opt/busybox/bin/busybox gzip -c /tmp/bb_dir/w2b/big.txt > /tmp/bb_dir/w2b/big.gz\n"
    "/opt/busybox/bin/busybox gunzip -c /tmp/bb_dir/w2b/big.gz > /tmp/bb_dir/w2b/big.gunzip\n"
    "/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/big.gunzip && echo \"BB-W2B: ok gzip\"\n"
    /* bzip2 round trip */
    "/opt/busybox/bin/busybox bzip2 -c /tmp/bb_dir/w2b/big.txt > /tmp/bb_dir/w2b/big.bz2\n"
    "/opt/busybox/bin/busybox bunzip2 -c /tmp/bb_dir/w2b/big.bz2 > /tmp/bb_dir/w2b/big.bunzip2\n"
    "/opt/busybox/bin/busybox cmp /tmp/bb_dir/w2b/big.txt /tmp/bb_dir/w2b/big.bunzip2 && echo \"BB-W2B: ok bzip2\"\n"
    /* xz decompression of an embedded fixture (BusyBox has no xz compressor) */
    "/opt/busybox/bin/busybox xzcat /etc/bb-w2b/hello.xz | grep -q \"b1nix-xz-OK\" && echo \"BB-W2B: ok xzcat\"\n"
    "/opt/busybox/bin/busybox unxz -c /etc/bb-w2b/hello.xz | grep -q \"b1nix-xz-OK\" && echo \"BB-W2B: ok unxz\"\n"
    /* malformed input: gunzip on non-gzip data must fail (nonzero status) */
    "echo \"not a gzip stream\" > /tmp/bb_dir/w2b/bad.gz\n"
    "/opt/busybox/bin/busybox gunzip -c /tmp/bb_dir/w2b/bad.gz > /dev/null 2>&1\n"
    "BB_GZ_BAD=$?\n"
    "[ $BB_GZ_BAD -ne 0 ] && echo \"BB-W2B: ok gunzip-malformed\"\n"
    "rm -rf /tmp/bb_dir/w2b\n"
    "rm -rf /tmp/bb_dir/w2\n"
    "/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk /tmp/bb_dir/bb_sort /tmp/bb_dir/bb_uniq /tmp/bb_dir/bb_tee /tmp/bb_dir/bb_clear /tmp/bb_dir/bb_seq\n"
    "[ ! -f /tmp/bb_dir/bb_file_mv ] && [ ! -f /tmp/bb_dir/bb_file_lnk ] && echo \"BB-SMOKE: ok rm\"\n"
    "/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file\n"
    "/opt/busybox/bin/busybox rmdir /tmp/bb_dir\n"
    "[ ! -d /tmp/bb_dir ] && echo \"BB-SMOKE: ok rmdir\"\n"
    "echo \"BB-SMOKE: done\"\n"
#endif
    "echo \"POSIX-SMOKE: done\"\n";



static const struct initramfs_file files[] = {
    {"/bin/init", (const char *)vfs_init_elf, sizeof(vfs_init_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/sh", "builtin:sh\n", 11, INITRAMFS_EXECUTABLE},
    {"/bin/hello", (const char *)vfs_hello_elf, sizeof(vfs_hello_elf),
     INITRAMFS_EXECUTABLE},
#if B1NIX_UPSTREAM_BUSYBOX
    {"/opt/busybox/bin/busybox", (const char *)vfs_upstream_busybox_elf,
     sizeof(vfs_upstream_busybox_elf), INITRAMFS_EXECUTABLE},
    {"/etc/bb-w2b/hello.xz", (const char *)initramfs_bb_w2b_xz,
     sizeof(initramfs_bb_w2b_xz), 0},
#endif
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
    {"/bin/m24b-smoke", (const char *)vfs_m24b_smoke_elf,
     sizeof(vfs_m24b_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m27-smoke", (const char *)vfs_m27_smoke_elf,
     sizeof(vfs_m27_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m29-smoke", (const char *)vfs_m29_smoke_elf,
     sizeof(vfs_m29_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m31-smoke", (const char *)vfs_m31_smoke_elf,
     sizeof(vfs_m31_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m31-setuid", (const char *)vfs_m31_setuid_elf,
     sizeof(vfs_m31_setuid_elf),
     INITRAMFS_EXECUTABLE | INITRAMFS_SETUID},
    {"/bin/m32-smoke", (const char *)vfs_m32_smoke_elf,
     sizeof(vfs_m32_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m32-nettool", (const char *)vfs_m32_nettool_elf,
     sizeof(vfs_m32_nettool_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m32-pcre2-smoke", (const char *)vfs_m32_pcre2_smoke_elf,
     sizeof(vfs_m32_pcre2_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/curl", (const char *)vfs_curl_elf, sizeof(vfs_curl_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/wget", (const char *)vfs_wget_elf, sizeof(vfs_wget_elf),
     INITRAMFS_EXECUTABLE},
    /* Dropbear multi-call binary: the applet is selected by argv[0]'s
     * basename, so the same image is registered under each name. */
    {"/bin/dropbear", (const char *)vfs_dropbear_elf, sizeof(vfs_dropbear_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/dbclient", (const char *)vfs_dropbear_elf,
     sizeof(vfs_dropbear_elf), INITRAMFS_EXECUTABLE},
    {"/bin/ssh", (const char *)vfs_dropbear_elf,
     sizeof(vfs_dropbear_elf), INITRAMFS_EXECUTABLE},
    {"/bin/dropbearkey", (const char *)vfs_dropbear_elf,
     sizeof(vfs_dropbear_elf), INITRAMFS_EXECUTABLE},
    {"/bin/dropbearconvert", (const char *)vfs_dropbear_elf,
     sizeof(vfs_dropbear_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m30-pie", (const char *)vfs_m30_pie_elf,
     sizeof(vfs_m30_pie_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m34-smoke", (const char *)vfs_m34_smoke_elf,
     sizeof(vfs_m34_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m35-smoke", (const char *)vfs_m35_smoke_elf,
     sizeof(vfs_m35_smoke_elf), INITRAMFS_EXECUTABLE},
    /* M30: the dynamic linker file is shipped as the PIE binary itself —
     * the in-kernel loader does relocation work, so /lib/ld-b1nix.so
     * exists as a name on disk that PT_INTERP can reference even though
     * b1nix doesn't hand control off to a separate userspace ld.so. */
    {"/lib/ld-b1nix.so", (const char *)vfs_m30_pie_elf,
     sizeof(vfs_m30_pie_elf), INITRAMFS_EXECUTABLE},
    {"/etc/motd", "welcome to b1nix m4\n", 23, 0},
    {"/etc/passwd", initramfs_passwd, sizeof(initramfs_passwd) - 1, 0},
    {"/etc/shadow", initramfs_shadow, sizeof(initramfs_shadow) - 1, 0},
    {"/etc/group", initramfs_group, sizeof(initramfs_group) - 1, 0},
    {"/etc/rc", initramfs_rc, sizeof(initramfs_rc) - 1, INITRAMFS_EXECUTABLE},
    {"/etc/init.d/sshd", initramfs_sshd_service, sizeof(initramfs_sshd_service) - 1, INITRAMFS_EXECUTABLE},
    {"/etc/fstab", initramfs_fstab, sizeof(initramfs_fstab) - 1, 0},
    {"/etc/resolv.conf", initramfs_resolv_conf,
     sizeof(initramfs_resolv_conf) - 1, 0},
    {"/etc/ssl/certs/ca-certificates.crt", (const char *)vfs_cacert_pem,
     sizeof(vfs_cacert_pem), 0},
    {"/etc/tls-test/ca.pem", (const char *)vfs_tls_ca_pem,
     sizeof(vfs_tls_ca_pem), 0},
    {"/etc/tls-test/server-cert.pem", (const char *)vfs_tls_server_cert_pem,
     sizeof(vfs_tls_server_cert_pem), 0},
    {"/etc/tls-test/server-key.pem", (const char *)vfs_tls_server_key_pem,
     sizeof(vfs_tls_server_key_pem), 0},
    {"/etc/posix-smoke.sh", posix_smoke_script, sizeof(posix_smoke_script) - 1,
     0},
    {"/README", "initramfs is alive\n", 20, 0},
    {"/bin/m25-smoke", (const char *)vfs_m25_smoke_elf,
     sizeof(vfs_m25_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m26-smoke", (const char *)vfs_m26_smoke_elf,
     sizeof(vfs_m26_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/su", (const char *)vfs_su_elf, sizeof(vfs_su_elf), INITRAMFS_EXECUTABLE | INITRAMFS_SETUID},
    {"/bin/passwd", (const char *)vfs_passwd_elf, sizeof(vfs_passwd_elf), INITRAMFS_EXECUTABLE | INITRAMFS_SETUID},
    {"/bin/id", (const char *)vfs_id_elf, sizeof(vfs_id_elf), INITRAMFS_EXECUTABLE},
    {"/bin/whoami", (const char *)vfs_whoami_elf, sizeof(vfs_whoami_elf), INITRAMFS_EXECUTABLE},
    {"/bin/groups", (const char *)vfs_groups_elf, sizeof(vfs_groups_elf), INITRAMFS_EXECUTABLE},
    {"/bin/useradd", (const char *)vfs_useradd_elf, sizeof(vfs_useradd_elf), INITRAMFS_EXECUTABLE},
    {"/bin/userdel", (const char *)vfs_userdel_elf, sizeof(vfs_userdel_elf), INITRAMFS_EXECUTABLE},
    {"/bin/groupadd", (const char *)vfs_groupadd_elf, sizeof(vfs_groupadd_elf), INITRAMFS_EXECUTABLE},
    {"/bin/chown", (const char *)vfs_chown_elf, sizeof(vfs_chown_elf), INITRAMFS_EXECUTABLE},
    {"/bin/chmod", (const char *)vfs_chmod_elf, sizeof(vfs_chmod_elf), INITRAMFS_EXECUTABLE},
    /* M37: mount point for the live ISO; the initramfs must contain a node
     * under /mnt/iso so that vfs_mount() can find the target directory after
     * the initramfs is mounted at "/".  Without this entry add_node() never
     * creates the /mnt/iso branch inside the initramfs root, and vfs_find_node
     * returns ENOENT when the liveiso boot code calls vfs_mount(usb0,
     * "/mnt/iso", "iso9660", 0). */
    {"/mnt/iso/.keep", "", 0, 0},
    /* M37: mount point for the verified live rootfs (loop0 ext4). */
    {"/mnt/root/.keep", "", 0, 0},
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

  root->inode->mode = 0755;
  root->inode->uid = 0;
  root->inode->gid = 0;
  root->inode->statfs_cb = initramfs_vfs_statfs;
  vfs_set_currently_mounting_root(root);

  for (usize i = 0; i < (sizeof(files) / sizeof(files[0])); i++) {
    enum vfs_node_type type = (files[i].flags & INITRAMFS_SYMLINK) ? VFS_SYMLINK : VFS_FILE;
    struct vfs_node *node =
        vfs_add_node(files[i].path, type, (void *)files[i].data,
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
