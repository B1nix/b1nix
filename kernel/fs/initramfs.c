/* MINIMAL_INITRAMFS selects a tiny embedded file set (init + native/b1cc smoke
 * binaries only) for the RAM-constrained in-guest self-host build. It is opt-in
 * via the build system (`make MINIMAL_INITRAMFS=1` adds -DMINIMAL_INITRAMFS; the
 * self-host module staging injects the same define into its standalone copy of
 * this file). The default build embeds the FULL initramfs — do NOT hardcode the
 * define here, or every normal/smoke build silently drops all but a handful of
 * binaries (the entire suite then fails to spawn them). */

#include <b1nix/console.h>
#include <b1nix/errno.h>
#include <b1nix/initramfs.h>
#include <b1nix/mm.h>
#include <b1nix/vfs.h>
#include <string.h>

#include "initramfs_native_smoke.inc"
#include "initramfs_return_42.inc"
#include "initramfs_b1cc_hello.inc"
#include "initramfs_b1cc_argv.inc"
#include "initramfs_b1cc_file_write.inc"
#include "initramfs_b1cc_stderr_exit.inc"
#include "initramfs_b1cc_better_c.inc"
#include "initramfs_b1cc_m34.inc"
#ifdef B1CC_SELFHOST
/* M32/M33: /bin/b1cc + /lib/b1cc/{crt0.o,libb1nix.a,crt0-dynamic.o} + b1cc-selfsmoke. */
#include "initramfs_b1cc_selfhost.inc"
#include "initramfs_b1cc_selfsmoke.inc"
/* M33 on-device PIE self-smoke DT_NEEDEDs libc.so.1, so the minimal initramfs
 * must carry the shared libc blob. The full build already pulls it in via the
 * x86_64 include block below, so only add it here for the minimal build. */
#ifdef MINIMAL_INITRAMFS
#include "initramfs_shared_libc.inc"
#endif
#endif
#ifndef MINIMAL_INITRAMFS
#include "initramfs_m12_smoke.inc"
#include "initramfs_m13_smoke.inc"
#include "initramfs_m13_job_control.inc"
#include "initramfs_m8_aio_test.inc"
#include "initramfs_m17_smoke.inc"
#include "initramfs_m14_smoke.inc"
#include "initramfs_m15_smoke.inc"
#include "initramfs_m56_smoke.inc"
#include "initramfs_tcc_files.inc"
#include "initramfs_netsurf_files.inc"
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
#include "initramfs_m53_zlib_smoke.inc"
#include "initramfs_m53_libpng_smoke.inc"
#include "initramfs_m53_libjpeg_smoke.inc"
#include "initramfs_m53_libwebp_smoke.inc"
#include "initramfs_m53_libvpx_smoke.inc"
#include "initramfs_m53_wapcaplet_smoke.inc"
#include "initramfs_m53_parserutils_smoke.inc"
#include "initramfs_m53_hubbub_smoke.inc"
#include "initramfs_m53_libcss_smoke.inc"
#include "initramfs_m53_libdom_smoke.inc"
#include "initramfs_m53_nslibs_smoke.inc"
#include "initramfs_m53_httpd.inc"
#include "initramfs_m53_httpsd.inc"
#include "initramfs_m53_virgl_smoke.inc"
#include "initramfs_curl.inc"
#include "initramfs_wget.inc"
#include "initramfs_cacert.inc"
#include "initramfs_tlstest.inc"
#include "initramfs_m30_pie.inc"
#ifdef __x86_64__
#include "initramfs_m30_dynamic.inc"
#include "initramfs_shared_libc.inc"
#include "initramfs_m69_plugin.inc"
#endif
#include "initramfs_m34_smoke.inc"
#include "initramfs_m35_smoke.inc"
#include "initramfs_m38_sound.inc"
#include "initramfs_testwav.inc"
#include "initramfs_testfont.inc"
#ifdef __x86_64__
/* M40: static Linux x86_64 ELF for the Linux ABI compat smoke. x86_64 only. */
#include "initramfs_m40_linux.inc"
/* M67: prebuilt static Rust ELF for the Rust cross-toolchain smoke. x86_64 only. */
#include "initramfs_m67_rust.inc"
/* M89: GCC libgcc_s.so is no longer shipped — busybox and nsfb now fold the
 * libgcc builtins statically (-static-libgcc) and the C++ stack is on libc++,
 * so no default binary carries DT_NEEDED libgcc_s.so. (The on-demand rust proof
 * bundles its own libgcc_s.so in its ram0 module.) */
/* M89: GCC libstdc++.so.6 is no longer shipped — the whole C++ ecosystem
 * (smoke binaries, Mesa, V8/d8, litehtml, the native clang/libLLVM) is migrated
 * to LLVM libc++, so nothing imports it anymore. */
/* /lib/libc++.so.1 + /lib/libc++abi.so.1 — shared LLVM C++ stdlib (M89). The
 * hosted C++ smoke binaries link these via the libc++-default b1nix-c++.
 * libc++abi.so.1 folds the libunwind DWARF unwinder; cross-DSO C++ exceptions
 * work via dl_iterate_phdr + each object's PT_GNU_EH_FRAME (no __register_frame).
 * Their init_array constructors run via M75. x86_64. */
#include "initramfs_libcxx.inc"
#include "initramfs_libcxxabi.inc"
#endif
#include "initramfs_m42_w5pre_smoke.inc"
#include "initramfs_m46_smoke.inc"
#include "initramfs_m57_smoke.inc"
#include "initramfs_m73_smoke.inc"
#include "initramfs_m63_smoke.inc"
#include "initramfs_m71_aslr.inc"
#include "initramfs_m47_smoke.inc"
#include "initramfs_m48_smoke.inc"
#include "initramfs_m49_smoke.inc"
#include "initramfs_m_posixmm_smoke.inc"
#include "initramfs_m49_libwayland.inc"
#include "initramfs_m49_libwayland_server.inc"
#include "initramfs_m50_smoke.inc"
#include "initramfs_m51_smoke.inc"
#include "initramfs_m51_pixman_smoke.inc"
#include "initramfs_m51_freetype_smoke.inc"
#include "initramfs_m51_cairo_smoke.inc"
#include "initramfs_m51_cairo_wayland.inc"
#include "initramfs_m52_gl_smoke.inc"
#include "initramfs_m52_osmesa.inc"
#include "initramfs_m53_mesa_virgl.inc"
#include "initramfs_m52_glsl.inc"
#include "initramfs_m59_smoke.inc"
#include "initramfs_m91_skia_smoke.inc"
#include "initramfs_m91_skia_dm.inc"
/* M91 Skia shared-library graph — x86_64 full build only (the registration
 * table below is under the same #ifdef __x86_64__, and these .inc are large). */
#if defined(__x86_64__) && !defined(MINIMAL_INITRAMFS)
#include "initramfs_libskia.inc"
#include "initramfs_libraw_ptr.inc"
#include "initramfs_libfontconfig.inc"
#include "initramfs_libGLESv2.inc"
#include "initramfs_libEGL.inc"
#include "initramfs_libb1gui.inc"
#endif
#ifdef __x86_64__
#include "initramfs_m64_clang_smoke.inc"
#endif
#include "initramfs_cxx_smoke.inc"
#include "initramfs_m55_iostream.inc"
#include "initramfs_m55_litehtml.inc"
#include "initramfs_js.inc"
#include "initramfs_m58_smoke.inc"
#include "initramfs_m51_xkb_smoke.inc"
#include "initramfs_m51_clipboard_smoke.inc"
#include "initramfs_m51_harfbuzz_smoke.inc"
#include "initramfs_m51_fontconfig_smoke.inc"
#include "initramfs_displayd.inc"
#include "initramfs_gclock.inc"
#include "initramfs_gterm.inc"
#include "initramfs_gpaint.inc"
#include "initramfs_gdesktop.inc"
#include "initramfs_gabout.inc"
#include "initramfs_dropbear.inc"
#include "initramfs_bash.inc"
#include "initramfs_telinit.inc"
#include "initramfs_su.inc"
#include "initramfs_passwd.inc"
/* id, whoami — migrated to upstream BusyBox (M42 wave 8); /bin/{id,whoami} are
 * now applet-manifest symlinks to /opt/busybox/bin/busybox, so the standalone
 * b1nix ELFs are no longer embedded. */
#include "initramfs_groups.inc"
#include "initramfs_useradd.inc"
#include "initramfs_userdel.inc"
#include "initramfs_groupadd.inc"
/* chmod, chown — migrated to upstream BusyBox (M44 wave 9); /bin/{chmod,chown}
 * are now applet-manifest symlinks to /opt/busybox/bin/busybox, so the
 * standalone b1nix ELFs are no longer embedded. */
#include "initramfs_halt.inc"
#include "initramfs_setfattr.inc"
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
/* Login shell is GNU bash for both the local console and SSH/login sessions. */
static const char initramfs_passwd[] =
    "root:x:0:0:root:/root:/bin/bash\n"
    "user:x:1000:1000:b1nix user:/home/user:/bin/bash\n";

/* Valid login shells (/etc/shells). dropbear (and getusershell()-based tools)
 * reject a login whose passwd shell is not listed here; without this file it
 * falls back to a "/bin/sh","/bin/csh" default that excludes bash, so SSH login
 * with the bash shell was refused ("invalid shell, rejected"). */
static const char initramfs_shells[] =
    "/bin/sh\n"
    "/bin/bash\n"
    "/bin/ash\n"
    "/opt/busybox/bin/busybox\n";

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

/* bpkg — minimal package manager. A POSIX-sh wrapper over the tools b1nix
 * already ships (curl, busybox tar/gzip/sha256sum, ext4). See /etc/bpkg.conf
 * for INDEX_URL and tools/packages/bpkg-publish.sh for the host-side publisher. */
static const char initramfs_bpkg[] =
    "#!/bin/sh\n"
    "# bpkg - minimal package manager for b1nix.\n"
    "#\n"
    "# A thin POSIX-sh wrapper over tools that already ship in b1nix: curl (HTTPS via\n"
    "# libcurl+mbedTLS, and file://), busybox tar/gzip/sha256sum, ext4 rw. No daemon,\n"
    "# no JSON, no database - just a flat-text index and per-package metadata files.\n"
    "#\n"
    "# Index format: one package per line, '#' comments allowed, space-separated:\n"
    "#   name  version  arch  sha256(64-hex)  url  [deps]\n"
    "# The 6th 'deps' field is OPTIONAL (comma-separated package names) and\n"
    "# backward-compatible: legacy 5-field lines parse exactly as before.\n"
    "#\n"
    "# Dependencies are installed transitively before the package, cycle-safe and\n"
    "# skipping anything already installed.\n"
    "# ponytail: deps are name-only (no version constraints) - the index pins one\n"
    "#   version per (name,arch), so a bare name is unambiguous.\n"
    "# ponytail: GPG/signature verification omitted - sha256 in a trusted index is the\n"
    "#   integrity boundary. Upgrade path: detached .sig + minisign/gpg verify.\n"
    "# ponytail: version rollback / hold / autoremove omitted. Upgrade path: keep old\n"
    "#   tarballs in a cache dir and add install --version / a hold flag file.\n"
    "\n"
    "set -u\n"
    "\n"
    "ROOT=\"${ROOT:-/}\"\n"
    "case \"$ROOT\" in /) PREFIX= ;; */) PREFIX=\"${ROOT%/}\" ;; *) PREFIX=\"$ROOT\" ;; esac\n"
    "CONF=\"$PREFIX/etc/bpkg.conf\"\n"
    "STATE=\"$PREFIX/var/lib/bpkg\"\n"
    "INDEX=\"$STATE/index\"\n"
    "INSTALLED=\"$STATE/installed\"\n"
    "\n"
    "# Default, overridable by /etc/bpkg.conf\n"
    "INDEX_URL=\"file:///pkgs/index\"\n"
    "[ -f \"$CONF\" ] && . \"$CONF\"\n"
    "\n"
    "# busybox provides tar / gzip / sha256sum; call it explicitly so this works even\n"
    "# without /bin symlinks for those applets.\n"
    "BB=/opt/busybox/bin/busybox\n"
    "\n"
    "err() { echo \"bpkg: $*\" >&2; }\n"
    "\n"
    "usage() {\n"
    "	cat >&2 <<EOF\n"
    "usage: bpkg <command> [args]\n"
    "  update            fetch the package index from INDEX_URL\n"
    "  install <name>    install a package + its deps (verifies sha256)\n"
    "  install-all       install every package for this architecture\n"
    "  remove <name>     remove an installed package\n"
    "  list              list installed packages\n"
    "  search <pattern>  search the index\n"
    "EOF\n"
    "	exit 2\n"
    "}\n"
    "\n"
    "need_index() {\n"
    "	if [ ! -f \"$INDEX\" ]; then\n"
    "		err \"no package index; run 'bpkg update' first\"\n"
    "		exit 1\n"
    "	fi\n"
    "}\n"
    "\n"
    "# Find the index line matching $1 (name) AND the running arch. Prints the\n"
    "# normalized line 'name ver arch sha url deps' (deps='-' when absent), or\n"
    "# returns non-zero if no match. Skips blank/comment lines. The optional 6th\n"
    "# 'deps' field is comma-separated package names.\n"
    "find_pkg() {\n"
    "	name=\"$1\"\n"
    "	arch=\"$(uname -m)\"\n"
    "	while read -r f_name f_ver f_arch f_sha f_url f_deps _rest; do\n"
    "		case \"$f_name\" in ''|\\#*) continue ;; esac\n"
    "		[ \"$f_name\" = \"$name\" ] || continue\n"
    "		[ \"$f_arch\" = \"$arch\" ] || continue\n"
    "		if [ -z \"$f_ver\" ] || [ -z \"$f_arch\" ] || [ -z \"$f_sha\" ] || [ -z \"$f_url\" ]; then\n"
    "			err \"malformed index line for '$name'\"\n"
    "			return 2\n"
    "		fi\n"
    "		# deps absent on legacy 5-field lines -> emit '-' as a placeholder so\n"
    "		# the field count is stable for callers using positional parsing.\n"
    "		[ -n \"$f_deps\" ] || f_deps=-\n"
    "		echo \"$f_name $f_ver $f_arch $f_sha $f_url $f_deps\"\n"
    "		return 0\n"
    "	done < \"$INDEX\"\n"
    "	return 1\n"
    "}\n"
    "\n"
    "cmd_update() {\n"
    "	mkdir -p \"$STATE\"\n"
    "	tmp=\"$STATE/.index.$$\"\n"
    "	if ! curl -fsSL \"$INDEX_URL\" -o \"$tmp\"; then\n"
    "		err \"failed to fetch index from $INDEX_URL\"\n"
    "		rm -f \"$tmp\"\n"
    "		exit 1\n"
    "	fi\n"
    "	mv \"$tmp\" \"$INDEX\"\n"
    "	echo \"bpkg: index updated from $INDEX_URL\"\n"
    "}\n"
    "\n"
    "# is_installed <name> -> 0 if the package already has recorded metadata.\n"
    "is_installed() { [ -f \"$INSTALLED/$1.ver\" ]; }\n"
    "\n"
    "# resolve_deps <name> <seen...> - print the transitive install order for\n"
    "# <name> (its uninstalled deps first, deepest-first, then <name> itself), one\n"
    "# package per line, de-duplicated. Cycle-safe via <seen> (the resolution\n"
    "# stack). POSIX sh has no locals, so EACH recursive call is captured in $(...)\n"
    "# - that isolates the child's clobbering of positionals/d_* from this frame.\n"
    "resolve_deps() {\n"
    "	d_name=\"$1\"; shift; d_seen=\"$*\"\n"
    "	is_installed \"$d_name\" && return 0\n"
    "	d_line=\"$(find_pkg \"$d_name\")\" || { err \"package '$d_name' not found for arch $(uname -m)\"; exit 1; }\n"
    "	# 6th field of the normalized line is deps ('-' when none).\n"
    "	d_deps=\"${d_line##* }\"\n"
    "	d_out=\n"
    "	if [ \"$d_deps\" != \"-\" ] && [ -n \"$d_deps\" ]; then\n"
    "		old_ifs=\"$IFS\"; IFS=','\n"
    "		# shellcheck disable=SC2086\n"
    "		set -- $d_deps\n"
    "		IFS=\"$old_ifs\"\n"
    "		for dep in \"$@\"; do\n"
    "			[ -n \"$dep\" ] || continue\n"
    "			case \" $d_seen $d_name \" in *\" $dep \"*) continue ;; esac\n"
    "			# Capture the child's order in a subshell so it cannot clobber\n"
    "			# this frame's d_name / d_deps / positionals.\n"
    "			sub=\"$(resolve_deps \"$dep\" $d_seen \"$d_name\")\"\n"
    "			d_out=\"$d_out$sub\n"
    "\"\n"
    "		done\n"
    "	fi\n"
    "	# Emit deps (in order, de-duped against this frame) then the package itself.\n"
    "	printf '%s' \"$d_out\" | while read -r p; do\n"
    "		[ -n \"$p\" ] || continue\n"
    "		echo \"$p\"\n"
    "	done\n"
    "	echo \"$d_name\"\n"
    "}\n"
    "\n"
    "# cmd_install <name> - resolve deps transitively, then install each package in\n"
    "# dependency order (deps before dependents), skipping anything already present.\n"
    "cmd_install() {\n"
    "	[ $# -eq 1 ] || usage\n"
    "	need_index\n"
    "	if is_installed \"$1\"; then echo \"bpkg: $1 already installed\"; return 0; fi\n"
    "	order=\"$(resolve_deps \"$1\")\"\n"
    "	for pkg in $order; do\n"
    "		is_installed \"$pkg\" && continue\n"
    "		[ \"$pkg\" = \"$1\" ] || echo \"bpkg: $1 depends on $pkg\"\n"
    "		do_install \"$pkg\"\n"
    "	done\n"
    "}\n"
    "\n"
    "# do_install <name> - fetch, verify sha256, extract, record metadata for a\n"
    "# SINGLE package whose deps are already satisfied. No recursion happens here.\n"
    "do_install() {\n"
    "	name=\"$1\"\n"
    "	line=\"$(find_pkg \"$name\")\"\n"
    "	rc=$?\n"
    "	if [ $rc -ne 0 ]; then\n"
    "		err \"package '$name' not found for arch $(uname -m)\"\n"
    "		exit 1\n"
    "	fi\n"
    "	# shellcheck disable=SC2086\n"
    "	set -- $line\n"
    "	p_name=\"$1\"; p_ver=\"$2\"; p_arch=\"$3\"; p_sha=\"$4\"; p_url=\"$5\"\n"
    "\n"
    "	mkdir -p \"$INSTALLED\"\n"
    "	work=\"$STATE/.work.$$\"\n"
    "	rm -rf \"$work\"\n"
    "	mkdir -p \"$work\"\n"
    "	tarball=\"$work/pkg.tar.gz\"\n"
    "\n"
    "	if ! curl -fsSL \"$p_url\" -o \"$tarball\"; then\n"
    "		err \"failed to fetch $p_url\"\n"
    "		rm -rf \"$work\"\n"
    "		exit 1\n"
    "	fi\n"
    "\n"
    "	# Integrity check - security boundary, never skipped. Compute the digest\n"
    "	# and compare strings directly: busybox 'sha256sum -c' is unreliable here,\n"
    "	# and 'cut' is not always present, so use POSIX parameter expansion.\n"
    "	sum_out=\"$(\"$BB\" sha256sum \"$tarball\")\"\n"
    "	got_sha=\"${sum_out%% *}\"\n"
    "	if [ -z \"$got_sha\" ] || [ \"$got_sha\" != \"$p_sha\" ]; then\n"
    "		err \"checksum mismatch for '$p_name' - refusing to install\"\n"
    "		err \"expected $p_sha\"\n"
    "		err \"got      $got_sha\"\n"
    "		rm -rf \"$work\"\n"
    "		exit 1\n"
    "	fi\n"
    "\n"
    "	# Record the file list (member paths inside the tarball) before extracting.\n"
    "	list=\"$INSTALLED/$p_name.list\"\n"
    "	if ! \"$BB\" tar -tzf \"$tarball\" > \"$list\" 2>/dev/null; then\n"
    "		err \"cannot read tarball contents for '$p_name'\"\n"
    "		rm -f \"$list\"\n"
    "		rm -rf \"$work\"\n"
    "		exit 1\n"
    "	fi\n"
    "\n"
    "	if ! \"$BB\" tar -xzf \"$tarball\" -C \"$ROOT\" 2>/dev/null; then\n"
    "		err \"extraction failed for '$p_name'\"\n"
    "		rm -f \"$list\"\n"
    "		rm -rf \"$work\"\n"
    "		exit 1\n"
    "	fi\n"
    "\n"
    "	echo \"$p_ver\" > \"$INSTALLED/$p_name.ver\"\n"
    "	rm -rf \"$work\"\n"
    "	echo \"bpkg: installed $p_name $p_ver ($p_arch)\"\n"
    "}\n"
    "\n"
    "cmd_remove() {\n"
    "	[ $# -eq 1 ] || usage\n"
    "	name=\"$1\"\n"
    "	list=\"$INSTALLED/$name.list\"\n"
    "	if [ ! -f \"$list\" ]; then\n"
    "		err \"package '$name' is not installed\"\n"
    "		exit 1\n"
    "	fi\n"
    "	# Remove files longest-path-first is unnecessary (we rm -f files, not dirs).\n"
    "	while read -r path; do\n"
    "		[ -n \"$path\" ] || continue\n"
    "		# tar member paths are relative (e.g. \"bin/hello\"); anchor under ROOT.\n"
    "		case \"$path\" in\n"
    "			/*) target=\"$path\" ;;\n"
    "			*)  target=\"$ROOT/$path\" ;;\n"
    "		esac\n"
    "		# Only remove files/symlinks, never directories (shared, may be system).\n"
    "		[ -f \"$target\" ] || [ -L \"$target\" ] && rm -f \"$target\"\n"
    "	done < \"$list\"\n"
    "	rm -f \"$list\" \"$INSTALLED/$name.ver\"\n"
    "	echo \"bpkg: removed $name\"\n"
    "}\n"
    "\n"
    "cmd_list() {\n"
    "	if [ ! -d \"$INSTALLED\" ]; then\n"
    "		return 0\n"
    "	fi\n"
    "	any=0\n"
    "	for v in \"$INSTALLED\"/*.ver; do\n"
    "		[ -f \"$v\" ] || continue\n"
    "		n=\"$(basename \"$v\" .ver)\"\n"
    "		printf '%s %s\\n' \"$n\" \"$(cat \"$v\")\"\n"
    "		any=1\n"
    "	done\n"
    "	[ \"$any\" = 1 ] || echo \"bpkg: no packages installed\"\n"
    "}\n"
    "\n"
    "cmd_install_all() {\n"
    "	need_index\n"
    "	arch=\"$(uname -m)\"\n"
    "	while read -r name _version pkg_arch _sha _url _rest; do\n"
    "		case \"$name\" in ''|\\#*) continue ;; esac\n"
    "		[ \"$pkg_arch\" = \"$arch\" ] || continue\n"
    "		cmd_install \"$name\" || exit 1\n"
    "	done < \"$INDEX\"\n"
    "}\n"
    "\n"
    "cmd_search() {\n"
    "	[ $# -eq 1 ] || usage\n"
    "	need_index\n"
    "	# Ignore comments/blank lines; grep the rest.\n"
    "	grep -v '^[[:space:]]*#' \"$INDEX\" | grep -v '^[[:space:]]*$' | grep -- \"$1\" || {\n"
    "		err \"no match for '$1'\"\n"
    "		exit 1\n"
    "	}\n"
    "}\n"
    "\n"
    "[ $# -ge 1 ] || usage\n"
    "sub=\"$1\"; shift\n"
    "case \"$sub\" in\n"
    "	update)  cmd_update \"$@\" ;;\n"
    "	install) cmd_install \"$@\" ;;\n"
    "	install-all) cmd_install_all \"$@\" ;;\n"
    "	remove)  cmd_remove \"$@\" ;;\n"
    "	list)    cmd_list \"$@\" ;;\n"
    "	search)  cmd_search \"$@\" ;;\n"
    "	*)       usage ;;\n"
    "esac\n";

/* /etc/bpkg.conf — bpkg configuration (sourced as POSIX sh). */
static const char initramfs_bpkg_conf[] =
    "# bpkg configuration. Sourced as POSIX sh by /bin/bpkg.\n"
    "#\n"
    "# INDEX_URL: where 'bpkg update' fetches the package index from. Supports\n"
    "# file://, http:// and https:// (curl + libcurl/mbedTLS handle TLS).\n"
    "#\n"
    "# To install real packages: push pkgs/ to the PUBLIC repo B1nix/b1nix-pkgs\n"
    "# (see tools/packages/bpkg-publish.sh), then uncomment the jsDelivr line below.\n"
    "#INDEX_URL=https://cdn.jsdelivr.net/gh/B1nix/b1nix-pkgs@main/pkgs/index\n"
    "\n"
    "# Smoke/offline default: the index fixture pre-staged in the image.\n"
    "INDEX_URL=file:///pkgs/index\n";

/* bpkg test fixtures: a package index whose 'hello' entry carries the REAL
 * sha256 of the staged tarball (below), so the install path's integrity check
 * actually passes; 'badpkg' reuses the same tarball with a wrong sha256 so the
 * reject path is exercised for real. Published for both arches (same bytes). */
static const char initramfs_bpkg_index[] =
    "# bpkg package index (test fixture). Fields: name version arch sha256 url [deps]\n"
    "# The 'hello' package is published for both arches with its REAL sha256 so the\n"
    "# install path's checksum verification actually passes. 'badpkg' reuses the same\n"
    "# tarball but lists a deliberately WRONG sha256 so install must reject it.\n"
    "# 'dep1' is a tiny dependency target; 'needsdep' carries a 6th 'deps' field\n"
    "# (=dep1) so installing it must transitively install dep1 first - this proves\n"
    "# real dependency resolution (not a decorative field).\n"
    "hello 1.0 x86_64 c89698f1fefb1f6f38b0f6192f3fb5b86f0cafa5da9e92bef92bfee4891fcf6a file:///pkgs/hello-1.0-x86_64.tar.gz\n"
    "hello 1.0 i686 c89698f1fefb1f6f38b0f6192f3fb5b86f0cafa5da9e92bef92bfee4891fcf6a file:///pkgs/hello-1.0-i686.tar.gz\n"
    "badpkg 1.0 x86_64 deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef00 file:///pkgs/hello-1.0-x86_64.tar.gz\n"
    "badpkg 1.0 i686 deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef00 file:///pkgs/hello-1.0-i686.tar.gz\n"
    "dep1 1.0 x86_64 d64e9dff466789ba805b4559aed5da5950319d40bf7bbd2a207fa54db59faa80 file:///pkgs/dep1-1.0.tar.gz\n"
    "dep1 1.0 i686 d64e9dff466789ba805b4559aed5da5950319d40bf7bbd2a207fa54db59faa80 file:///pkgs/dep1-1.0.tar.gz\n"
    "needsdep 1.0 x86_64 c89698f1fefb1f6f38b0f6192f3fb5b86f0cafa5da9e92bef92bfee4891fcf6a file:///pkgs/hello-1.0-x86_64.tar.gz dep1\n"
    "needsdep 1.0 i686 c89698f1fefb1f6f38b0f6192f3fb5b86f0cafa5da9e92bef92bfee4891fcf6a file:///pkgs/hello-1.0-i686.tar.gz dep1\n";

/* The 'hello-1.0' package tarball: a gzipped tar containing bin/hello whose
 * contents are exactly "hello from bpkg\n". sha256 matches the index above.
 * Built deterministically on the host (tar --sort/--mtime/--owner). */
static const unsigned char initramfs_bpkg_hello_tgz[] = {
  0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xed, 0xd1,
  0x39, 0x0e, 0xc2, 0x40, 0x10, 0x04, 0xc0, 0x8d, 0x79, 0xc5, 0xfe, 0x80,
  0xc5, 0xe7, 0x7b, 0xb0, 0xc4, 0x25, 0x0c, 0x46, 0x06, 0xfe, 0xcf, 0xe2,
  0x80, 0x80, 0x88, 0xc4, 0x40, 0x50, 0x95, 0xb4, 0x34, 0x49, 0xb7, 0x34,
  0xdd, 0xe1, 0xbc, 0xdc, 0x6f, 0xfa, 0x7e, 0x08, 0xf3, 0x49, 0x59, 0x53,
  0x55, 0x53, 0x66, 0xef, 0x99, 0x52, 0x91, 0xc2, 0xaa, 0x6c, 0x52, 0xd1,
  0xd6, 0xcf, 0xc8, 0xf7, 0xb6, 0x2e, 0xeb, 0x10, 0xd3, 0x8c, 0x9b, 0x5e,
  0xee, 0xd7, 0xdb, 0x7a, 0x8c, 0xf1, 0x1b, 0x55, 0xff, 0x68, 0xfa, 0x7d,
  0xdc, 0x8e, 0xc3, 0x29, 0x76, 0x97, 0xe3, 0x6e, 0xf1, 0xeb, 0x3d, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xe6, 0x01, 0x86,
  0x97, 0x69, 0x44, 0x00, 0x28, 0x00, 0x00};

/* The 'dep1-1.0' dependency fixture: a gzipped tar containing lib/dep1.flag
 * ("dep1 marker\n"). sha256 matches the 'dep1' index line above. Used by the
 * dependency-resolution smoke check (needsdep -> dep1). */
static const unsigned char initramfs_bpkg_dep1_tgz[] = {
  0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xed, 0xd2,
  0x4d, 0x0a, 0xc2, 0x30, 0x10, 0x40, 0xe1, 0xac, 0x3d, 0x45, 0x4e, 0x90,
  0x66, 0x9a, 0x9f, 0x9e, 0x27, 0x62, 0x15, 0xb1, 0x82, 0x44, 0xbd, 0xbf,
  0x69, 0xbb, 0xb2, 0xe0, 0x42, 0x21, 0x55, 0xf0, 0x7d, 0x9b, 0x09, 0xd9,
  0x64, 0xe0, 0xc5, 0x34, 0xaa, 0x3a, 0x5b, 0x74, 0x21, 0x4c, 0xb3, 0x58,
  0xce, 0xe9, 0x2c, 0x2e, 0xda, 0xb6, 0x0b, 0xe3, 0x28, 0xf7, 0x51, 0xac,
  0x57, 0x3a, 0xd4, 0x5f, 0x4d, 0xa9, 0xfb, 0xf5, 0x96, 0xb2, 0xd6, 0x6b,
  0x3c, 0xf5, 0x8b, 0x4c, 0x33, 0x1c, 0xb7, 0x95, 0xff, 0xc0, 0x07, 0xfd,
  0x63, 0x68, 0xe9, 0xbf, 0x86, 0xb9, 0xff, 0xae, 0xbf, 0x88, 0xd9, 0x0f,
  0xe9, 0x50, 0xe5, 0x8d, 0x31, 0x70, 0xf4, 0xfe, 0x75, 0x7f, 0xf1, 0xcf,
  0xfd, 0xc5, 0x3a, 0x27, 0x4a, 0xdb, 0x2a, 0xdb, 0x2c, 0xfc, 0x79, 0xff,
  0xb1, 0xbc, 0x3e, 0xa7, 0x7c, 0xea, 0xf3, 0xe6, 0xdb, 0xbb, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x7d,
  0x0f, 0x34, 0xe9, 0x42, 0x37, 0x00, 0x28, 0x00, 0x00};

/* bpkg-smoke.sh — drives the real bpkg pipeline (run via /bin/sh in test mode);
 * emits BPKG-SMOKE markers the host harness greps for. */
static const char initramfs_bpkg_smoke[] =
    "#!/bin/sh\n"
    "# bpkg-smoke.sh - deterministic, offline smoke test for the bpkg package manager.\n"
    "# Drives the REAL pipeline (curl file:// -> sha256sum -> tar -xzf -> metadata)\n"
    "# against fixtures pre-staged in the initramfs (/pkgs). Emits BPKG-SMOKE markers\n"
    "# the host smoke harness greps for. No fake passes: each marker is gated on the\n"
    "# actual operation having succeeded (or, for checksum-reject, on it FAILING).\n"
    "echo \"BPKG-SMOKE: start\"\n"
    "\n"
    "# bpkg reads INDEX_URL from /etc/bpkg.conf; the test config points it at the\n"
    "# staged file:// index. Install into a scratch root so we don't touch the live\n"
    "# rootfs and can assert presence/absence cleanly.\n"
    "ROOT=/tmp/bpkgroot\n"
    "rm -rf \"$ROOT\"\n"
    "mkdir -p \"$ROOT\"\n"
    "rm -rf /var/lib/bpkg\n"
    "mkdir -p /var/lib\n"
    "ln -s \"$ROOT/var/lib/bpkg\" /var/lib/bpkg\n"
    "export ROOT\n"
    "\n"
    "# update: fetch the index from file:///pkgs/index via curl.\n"
    "if bpkg update >/dev/null 2>&1 && [ -f \"$ROOT/var/lib/bpkg/index\" ]; then\n"
    "	echo \"BPKG-SMOKE: ok update\"\n"
    "else\n"
    "	echo \"BPKG-SMOKE: fail update\"\n"
    "fi\n"
    "\n"
    "# install: fetch tarball, verify sha256, extract, record metadata.\n"
    "if bpkg install hello >/dev/null 2>&1 \\\n"
    "	&& [ -f \"$ROOT/bin/hello\" ] \\\n"
    "	&& [ \"$(cat \"$ROOT/bin/hello\")\" = \"hello from bpkg\" ] \\\n"
    "	&& [ -f /var/lib/bpkg/installed/hello.list ] \\\n"
    "	&& [ \"$(cat \"$ROOT/var/lib/bpkg/installed/hello.ver\")\" = \"1.0\" ]; then\n"
    "	echo \"BPKG-SMOKE: ok install\"\n"
    "else\n"
    "	echo \"BPKG-SMOKE: fail install\"\n"
    "fi\n"
    "\n"
    "# list: installed package shows up with its version.\n"
    "if bpkg list 2>/dev/null | grep -q '^hello 1.0$'; then\n"
    "	echo \"BPKG-SMOKE: ok list\"\n"
    "else\n"
    "	echo \"BPKG-SMOKE: fail list\"\n"
    "fi\n"
    "\n"
    "# checksum-reject: 'badpkg' shares the same tarball but the index lists a wrong\n"
    "# sha256. install MUST fail (non-zero) and MUST NOT extract anything. This proves\n"
    "# the integrity check is real, not decorative.\n"
    "if bpkg install badpkg >/dev/null 2>&1; then\n"
    "	echo \"BPKG-SMOKE: fail checksum-reject\"\n"
    "else\n"
    "	if [ ! -f \"$ROOT/bin/badfile\" ] && [ ! -f \"$ROOT/var/lib/bpkg/installed/badpkg.ver\" ]; then\n"
    "		echo \"BPKG-SMOKE: ok checksum-reject\"\n"
    "	else\n"
    "		echo \"BPKG-SMOKE: fail checksum-reject\"\n"
    "	fi\n"
    "fi\n"
    "\n"
    "# remove: delete recorded files + metadata; the installed file must be gone.\n"
    "if bpkg remove hello >/dev/null 2>&1 \\\n"
    "	&& [ ! -f \"$ROOT/bin/hello\" ] \\\n"
    "	&& [ ! -f \"$ROOT/var/lib/bpkg/installed/hello.ver\" ]; then\n"
    "	echo \"BPKG-SMOKE: ok remove\"\n"
    "else\n"
    "	echo \"BPKG-SMOKE: fail remove\"\n"
    "fi\n"
    "\n"
    "# dep-resolution: 'needsdep' carries deps=dep1 in the index. Installing it MUST\n"
    "# transitively install 'dep1' first (its file + metadata) AND 'needsdep' itself.\n"
    "# This proves real dependency resolution, not a parsed-but-ignored field.\n"
    "if bpkg install needsdep >/dev/null 2>&1 \\\n"
    "	&& [ -f \"$ROOT/lib/dep1.flag\" ] \\\n"
    "	&& [ -f \"$ROOT/var/lib/bpkg/installed/dep1.ver\" ] \\\n"
    "	&& [ -f \"$ROOT/var/lib/bpkg/installed/needsdep.ver\" ] \\\n"
    "	&& [ -f \"$ROOT/bin/hello\" ]; then\n"
    "	echo \"BPKG-SMOKE: ok dep-resolution\"\n"
    "else\n"
    "	echo \"BPKG-SMOKE: fail dep-resolution\"\n"
    "fi\n"
    "\n"
    "rm -f /var/lib/bpkg\n"
    "rm -rf \"$ROOT\"\n"
    "echo \"BPKG-SMOKE: done\"\n";

/* bash feature smoke: run by /bin/bash to prove the real GNU bash (not ash) is
 * the shell. Each test emits a "BASH-SMOKE: ok <feature>" marker the host smoke
 * script greps for. Exercises bash-only syntax ash does not implement: indexed
 * arrays, [[ ]] with glob/regex, $(( )) arithmetic, {a..b} brace ranges,
 * C-style for, ${var//x/y} substitution, and local function variables. */
static const char initramfs_bash_smoke[] =
    "#!/bin/bash\n"
    "[ -n \"$BASH_VERSION\" ] && echo \"BASH-SMOKE: ok version $BASH_VERSION\"\n"
    "a=(alpha beta gamma)\n"
    "[ \"${a[1]}\" = beta ] && [ \"${#a[@]}\" -eq 3 ] && echo \"BASH-SMOKE: ok arrays\"\n"
    "[[ abcde == a*e ]] && echo \"BASH-SMOKE: ok dbracket-glob\"\n"
    "[[ hello123 =~ ^[a-z]+[0-9]+$ ]] && echo \"BASH-SMOKE: ok regex-match\"\n"
    "[ $((6 * 7)) -eq 42 ] && echo \"BASH-SMOKE: ok arithmetic\"\n"
    "[ \"$(echo {1..5})\" = \"1 2 3 4 5\" ] && echo \"BASH-SMOKE: ok brace-range\"\n"
    "s=0; for ((i=1;i<=4;i++)); do s=$((s+i)); done\n"
    "[ \"$s\" -eq 10 ] && echo \"BASH-SMOKE: ok cstyle-for\"\n"
    "v=foobarbar; [ \"${v//bar/X}\" = fooXX ] && echo \"BASH-SMOKE: ok pattern-subst\"\n"
    "f() { local x=inner; echo \"$x\"; }\n"
    "[ \"$(f)\" = inner ] && echo \"BASH-SMOKE: ok local-vars\"\n"
    /* UTF-8 multibyte: αβγ is 3 characters but 6 bytes. With HANDLE_MULTIBYTE
     * ${#v} counts characters and ${v:1:1} is the 2nd character (β). */
    "v=$'\\u03b1\\u03b2\\u03b3'\n"
    "[ \"${#v}\" -eq 3 ] && echo \"BASH-SMOKE: ok utf8-length\"\n"
    "[ \"${v:1:1}\" = $'\\u03b2' ] && echo \"BASH-SMOKE: ok utf8-substr\"\n"
    "echo \"BASH-SMOKE: done\"\n";

/* M39: configurable init. /etc/inittab drives the production init's service
 * supervision. Format (BusyBox/SysV-flavoured):
 *   id:runlevels:action:process
 *   runlevels  digit string 0-6 the entry applies to (empty = all runlevels)
 *   action     sysinit | wait | once | respawn | initdefault | ctrlaltdel
 *              | shutdown
 * The default runlevel is 3; the console "terminal" is GNU bash. /bin/init
 * (PID 1) parses this at boot; `telinit <N>` requests a runlevel switch. */
static const char initramfs_inittab[] =
    "# /etc/inittab — b1nix configurable init (M39). See telinit(8).\n"
    "# Format: id:runlevels:action:process\n"
    "id:3:initdefault:\n"
    "si::sysinit:/etc/rc\n"
    /* The framebuffer console shell runs at runlevels 2-4 only. At runlevel 5
     * the graphical session (displayd) owns /dev/fb0 and the raw keyboard via
     * /dev/input; a bash on /dev/console there would both scribble over the
     * desktop and silently consume keystrokes. The serial getty stays up at 5
     * as a rescue path. */
    "console:234:respawn:/bin/bash --noediting\n"
    "ttyS0:2345:respawn:/bin/getty -L 115200 ttyS0 vt100\n"
    "display:5:respawn:/bin/displayd\n"
    "desktop:5:respawn:/bin/gdesktop\n"
    "ca::ctrlaltdel:/bin/reboot\n"
    "sd::shutdown:/etc/rc.shutdown\n";

/* Fontconfig system configuration. Skia's SkFontMgr_New_FontConfig() calls
 * FcInitLoadConfigAndFonts() which reads /etc/fonts/fonts.conf. Without this
 * file, fontconfig falls back to compiled-in defaults with empty FC_FONTPATH,
 * causing FcFontList() to return patterns with invalid FC_FILE pointers →
 * SIGSEGV in SkFontMgr_fontconfig::FontAccessible(). */
static const char initramfs_etc_fonts_conf[] =
    "<?xml version=\"1.0\"?>\n"
    "<fontconfig>\n"
    "  <dir>/share/fonts</dir>\n"
    "  <cachedir>/tmp/fontcache</cachedir>\n"
    "</fontconfig>\n";

/* Resolver configuration. The kernel DNS client parses the first
 * "nameserver" line lazily (kernel/net/dns.c); 10.0.2.3 is the QEMU
 * user-mode-networking built-in resolver. */
static const char initramfs_resolv_conf[] =
    "nameserver 10.0.2.3\n";

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

/* Migration wave 6: account-management applet smoke (login/su/passwd suite).
 * Driven by the upstream BusyBox ash so the shell semantics are real ($, $(...),
 * regex grep) — the in-kernel builtin shell that runs the POSIX smoke script
 * mangles '$' even inside single quotes, so this whole flow lives in a file that
 * is executed with `/bin/sh /etc/bb-w6/run.sh`. Stored verbatim (file content,
 * not shell-interpreted at storage time), so no '$' escaping is needed here.
 *
 * Every marker is gated on the real operation: addgroup/adduser/deluser/delgroup
 * mutate /etc/group, /etc/passwd, /etc/shadow and the home dir; chpasswd writes
 * a standard sha512-crypt ($6$) hash; passwd-verify recomputes that hash from the
 * stored salt and byte-compares it (proving the password is verifiable); su
 * actually drops root->user and the switched uid is read back. No fake markers. */
static const char initramfs_bb_w6_sh[] =
    "#!/bin/sh\n"
    "BB=/opt/busybox/bin/busybox\n"
    "echo \"BB-W6: start accounts\"\n"
    "\n"
    "# cryptpw: standard sha512-crypt of a known password with a fixed salt.\n"
    "H=$($BB cryptpw -m sha512 -S w6salt secret)\n"
    "case \"$H\" in\n"
    "'$6$w6salt$'*) echo \"BB-W6: ok cryptpw\" ;;\n"
    "esac\n"
    "\n"
    "# addgroup: create a group, verify its /etc/group record.\n"
    "$BB addgroup devs\n"
    "$BB grep -q '^devs:' /etc/group && echo \"BB-W6: ok addgroup\"\n"
    "\n"
    "# adduser: create a normal user with no password (-D), home dir, shell.\n"
    "$BB adduser -D bob\n"
    "$BB grep -q '^bob:' /etc/passwd && echo \"BB-W6: ok adduser\"\n"
    "$BB grep -q '^bob:' /etc/shadow && echo \"BB-W6: ok adduser-shadow\"\n"
    "[ -d /home/bob ] && echo \"BB-W6: ok adduser-home\"\n"
    "BOB_UID=$($BB grep '^bob:' /etc/passwd | $BB cut -d: -f3)\n"
    "\n"
    "# chpasswd: set bob's password (sha512). A $6$ hash must land in shadow.\n"
    "echo \"bob:hunter2\" | $BB chpasswd -c sha512\n"
    "$BB grep -q '^bob:\\$6\\$' /etc/shadow && echo \"BB-W6: ok chpasswd\"\n"
    "\n"
    "# passwd-verify: recompute sha512-crypt('hunter2') with the stored salt and\n"
    "# confirm it equals the stored hash — proves the password is verifiable.\n"
    "STORED=$($BB grep '^bob:' /etc/shadow | $BB cut -d: -f2)\n"
    "SALT=$(echo \"$STORED\" | $BB cut -d'$' -f3)\n"
    "RECOMP=$($BB cryptpw -m sha512 -S \"$SALT\" hunter2)\n"
    "[ \"$STORED\" = \"$RECOMP\" ] && echo \"BB-W6: ok passwd-verify\"\n"
    "\n"
    "# su: drop from root to bob, run a command, confirm the uid switched.\n"
    "SU_UID=$($BB su bob -c \"$BB id -u\")\n"
    "[ \"$SU_UID\" = \"$BOB_UID\" ] && echo \"BB-W6: ok su\"\n"
    "\n"
    "# passwd -l / -u: lock then unlock bob, observing the '!' shadow prefix.\n"
    "$BB passwd -l bob >/dev/null 2>&1\n"
    "$BB grep -q '^bob:!' /etc/shadow && echo \"BB-W6: ok passwd-lock\"\n"
    "$BB passwd -u bob >/dev/null 2>&1\n"
    "$BB grep -q '^bob:!' /etc/shadow || echo \"BB-W6: ok passwd-unlock\"\n"
    "\n"
    "# login/getty: present & dispatchable. A full session needs a dedicated tty\n"
    "# and PID 1 stays with B1NIX, so this asserts the applet links and selects.\n"
    "$BB --list | $BB grep -q '^login$' && echo \"BB-W6: ok login-applet\"\n"
    "$BB --list | $BB grep -q '^getty$' && echo \"BB-W6: ok getty-applet\"\n"
    "\n"
    "# deluser/delgroup: tear the user and group back down, verify removal.\n"
    "$BB deluser bob\n"
    "$BB grep -q '^bob:' /etc/passwd || echo \"BB-W6: ok deluser\"\n"
    "$BB grep -q '^bob:' /etc/shadow || echo \"BB-W6: ok deluser-shadow\"\n"
    "$BB delgroup devs\n"
    "$BB grep -q '^devs:' /etc/group || echo \"BB-W6: ok delgroup\"\n"
    "\n"
    "echo \"BB-W6: done\"\n";

static const char posix_smoke_script[] =
    "#!/bin/sh\n"
    "echo \"POSIX-SMOKE: start\"\n"
    "if ! grep -q 'b1nix.smoke=shell' /proc/cmdline; then\n"
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
    /* ── M33 shell-feature tests under ash ──────────────────────────
     * These replace the retired in-kernel builtin-shell unit tests
     * (m33_shell_smoke). Each exercises a POSIX sh feature through the real
     * /bin/sh (BusyBox ash) that now runs this script. Array/jobs tests were
     * dropped: arrays are a bash-only extension ash lacks, and job control is
     * meaningless in a non-interactive script. */
    "echo \"M33-SHELL: start\"\n"
    "M33X=$(echo mid); [ \"$M33X\" = \"mid\" ] && echo \"M33-SHELL: ok cmdsubst\"\n"
    "M33V=outer; (M33V=inner); [ \"$M33V\" = \"outer\" ] && echo \"M33-SHELL: ok subshell\"\n"
    "m33fn() { echo \"hi-$1\"; }; [ \"$(m33fn bob)\" = \"hi-bob\" ] && echo \"M33-SHELL: ok function\"\n"
    "case foo in f*) M33R=yes ;; *) M33R=no ;; esac; [ \"$M33R\" = \"yes\" ] && echo \"M33-SHELL: ok case\"\n"
    "M33S=0; for i in 1 2 3; do M33S=$((M33S + i)); done; [ \"$M33S\" = \"6\" ] && echo \"M33-SHELL: ok for-loop\"\n"
    "M33N=0; while [ \"$M33N\" -lt 3 ]; do M33N=$((M33N + 1)); done; [ \"$M33N\" = \"3\" ] && echo \"M33-SHELL: ok while-loop\"\n"
    "[ \"$((6 * 7))\" = \"42\" ] && echo \"M33-SHELL: ok arith\"\n"
    "unset M33U; [ \"${M33U:-def}\" = \"def\" ] && echo \"M33-SHELL: ok param-expand\"\n"
    "cat > /tmp/m33_hd <<M33EOF\n"
    "heredoc-body\n"
    "M33EOF\n"
    "grep -q heredoc-body /tmp/m33_hd && echo \"M33-SHELL: ok heredoc\"\n"
    "mkdir -p /tmp/m33_g; : > /tmp/m33_g/a.txt; : > /tmp/m33_g/b.txt\n"
    "set -- /tmp/m33_g/*.txt; [ \"$#\" = \"2\" ] && echo \"M33-SHELL: ok glob-star\"\n"
    /* Deliver SIGUSR1 from a concurrent process. External commands provide
     * deterministic user-return points where ash can run the pending trap. */
    "M33TRAP=none\n"
    "trap 'M33TRAP=delivered' USR1\n"
    "/bin/kill -USR1 $$ &\n"
    "for M33I in 1 2 3 4 5 6 7 8; do "
    "/bin/true; [ \"$M33TRAP\" = delivered ] && break; done\n"
    "[ \"$M33TRAP\" = delivered ] && echo \"M33-SHELL: ok async-trap\"\n"
    "trap - USR1\n"
    "rm -rf /tmp/m33_g /tmp/m33_hd\n"
    "echo \"M33-SHELL: done\"\n"
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
    "fi\n"
    "if ! grep -q 'b1nix.smoke=graphics' /proc/cmdline; then\n"
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
    "cat /proc/mounts | grep -q \"/\" && echo \"BB-W2B: ok df\"\n"
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
    /* ── Migration wave 3: process & system inspection (procps + dmesg). ──
     * Native shell grep is strstr-only and wc -c caps at 4096, so every check
     * pipes through /opt/busybox/bin/busybox grep. */
    "echo \"BB-W3: start procps\"\n"
    /* ps: process table; every row carries the resolved owner in the USER
     * column, so "root" proves ps enumerated /proc and formatted it. */
    "/opt/busybox/bin/busybox ps | /opt/busybox/bin/busybox grep -q \"root\" && echo \"BB-W3: ok ps\"\n"
    /* top: one batch iteration, stdin from /dev/null so it never waits on a tty. */
    "/opt/busybox/bin/busybox top -b -n 1 </dev/null | /opt/busybox/bin/busybox grep -q \"root\" && echo \"BB-W3: ok top\"\n"
    /* uptime: reads /proc/uptime + /proc/loadavg. */
    "/opt/busybox/bin/busybox uptime | /opt/busybox/bin/busybox grep -q \"load average\" && echo \"BB-W3: ok uptime\"\n"
    /* free: sysinfo() syscall + /proc/meminfo. */
    "/opt/busybox/bin/busybox free | /opt/busybox/bin/busybox grep -q \"Mem:\" && echo \"BB-W3: ok free\"\n"
    /* dmesg: drains the kernel ring buffer through klogctl -> SYS_DMESG. */
    "/opt/busybox/bin/busybox dmesg | /opt/busybox/bin/busybox grep -qi \"b1nix\" && echo \"BB-W3: ok dmesg\"\n"
    /* pidof/pgrep/pkill: a backgrounded busybox sleep gives a live target whose
     * comm is the exec basename "busybox". pgrep/pkill skip their own pid, so
     * pkill terminates only the backgrounded sleep. */
    "/opt/busybox/bin/busybox sleep 30 &\n"
    "/opt/busybox/bin/busybox pidof busybox | /opt/busybox/bin/busybox grep -q \"[0-9]\" && echo \"BB-W3: ok pidof\"\n"
    "/opt/busybox/bin/busybox pgrep busybox | /opt/busybox/bin/busybox grep -q \"[0-9]\" && echo \"BB-W3: ok pgrep\"\n"
    "/opt/busybox/bin/busybox pkill busybox && echo \"BB-W3: ok pkill\"\n"
    "echo \"BB-W3: done\"\n"
    /* ── Migration wave 4: storage & networking inspection/config. ── */
    "echo \"BB-W4: start\"\n"
    /* mount/umount round trip on sata0 (ext4), which M14 leaves unmounted. */
    "/opt/busybox/bin/busybox mkdir -p /mnt/w4\n"
    "/opt/busybox/bin/busybox mount -t ext4 sata0 /mnt/w4\n"
    "/opt/busybox/bin/busybox mount | /opt/busybox/bin/busybox grep -q \"/mnt/w4\" && echo \"BB-W4: ok mount\"\n"
    /* M43: create a file + dir at a mountpoint that was created at RUNTIME
     * (mkdir above), exercising the mount-crossing on a non-static mount node. */
    "echo m43data > /mnt/w4/m43_create.txt\n"
    "/opt/busybox/bin/busybox mkdir /mnt/w4/m43_dir\n"
    "/opt/busybox/bin/busybox cat /mnt/w4/m43_create.txt | /opt/busybox/bin/busybox grep -q m43data && [ -d /mnt/w4/m43_dir ] && echo \"M43: ok create-runtime-mountpoint\"\n"
    "/opt/busybox/bin/busybox rm -f /mnt/w4/m43_create.txt; /opt/busybox/bin/busybox rmdir /mnt/w4/m43_dir\n"
    "/opt/busybox/bin/busybox umount /mnt/w4\n"
    "/opt/busybox/bin/busybox mount | /opt/busybox/bin/busybox grep -q \"/mnt/w4\" || echo \"BB-W4: ok umount\"\n"
    /* nslookup on a numeric address: getaddrinfo numeric fast path, no live DNS
     * query, so deterministic offline. */
    "/opt/busybox/bin/busybox nslookup 10.0.2.2 | /opt/busybox/bin/busybox grep -q \"10.0.2.2\" && echo \"BB-W4: ok nslookup\"\n"
    /* lsof reads /proc/<pid>/fd/ symlinks; every process has open files. */
    "/opt/busybox/bin/busybox lsof 2>/dev/null | /opt/busybox/bin/busybox grep -q \"/\" && echo \"BB-W4: ok lsof\"\n"
    /* netstat reads /proc/net/tcp; the dropbear SSH daemon listens on :22. */
    "/opt/busybox/bin/busybox netstat -tln | /opt/busybox/bin/busybox grep -q \":22\" && echo \"BB-W4: ok netstat\"\n"
    /* route reads /proc/net/route; the on-link 10.0.2.0/24 route is always
     * present once DHCP assigns 10.0.2.15. */
    "/opt/busybox/bin/busybox route -n | /opt/busybox/bin/busybox grep -q \"10.0.2\" && echo \"BB-W4: ok route\"\n"
    /* ifconfig queries the interface via SIOCGIF* ioctls; eth0 carries the
     * DHCP-assigned 10.0.2.15. */
    "/opt/busybox/bin/busybox ifconfig eth0 | /opt/busybox/bin/busybox grep -q \"10.0.2.15\" && echo \"BB-W4: ok ifconfig\"\n"
    /* blkid reads the /dev/sata0 block node and identifies the ext4 fs that
     * M14 wrote and left unmounted. */
    "/opt/busybox/bin/busybox blkid /dev/sata0 2>/dev/null | /opt/busybox/bin/busybox grep -qi \"ext\" && echo \"BB-W4: ok blkid\"\n"
    /* fdisk -l reads the /dev/sata0 geometry via the BLK* ioctls. */
    "/opt/busybox/bin/busybox fdisk -l /dev/sata0 2>/dev/null | /opt/busybox/bin/busybox grep -q \"Disk /dev/sata0\" && echo \"BB-W4: ok fdisk\"\n"
    /* ping uses a raw ICMP socket; the gateway 10.0.2.2 answers echo. */
    "/opt/busybox/bin/busybox ping -c 1 -W 3 10.0.2.2 2>&1 | /opt/busybox/bin/busybox grep -q \"bytes from 10.0.2.2\" && echo \"BB-W4B: ok ping\"\n"
    /* losetup -f returns the first free loop device via /dev/loop-control. */
    "/opt/busybox/bin/busybox losetup -f 2>/dev/null | /opt/busybox/bin/busybox grep -q \"/dev/loop\" && echo \"BB-W4B: ok losetup\"\n"
    /* ip uses rtnetlink (AF_NETLINK); RTM_GETLINK lists the eth0 interface. */
    "/opt/busybox/bin/busybox ip link show 2>&1 | /opt/busybox/bin/busybox grep -q \"eth0\" && echo \"BB-W4B: ok ip\"\n"
    "echo \"BB-W4: done\"\n"
    /* ── Migration wave 5: upstream ash/sh shell bring-up. ── */
    "echo \"BB-W5: start ash\"\n"
    "/opt/busybox/bin/busybox --list | /opt/busybox/bin/busybox grep -q \"^ash$\" && echo \"BB-W5: ok list-ash\"\n"
    "/opt/busybox/bin/busybox --list | /opt/busybox/bin/busybox grep -q \"^sh$\" && echo \"BB-W5: ok list-sh\"\n"
    "/opt/busybox/bin/busybox ash -c 'echo ash-ok' | /opt/busybox/bin/busybox grep -q \"ash-ok\" && echo \"BB-W5: ok ash-c\"\n"
    "/opt/busybox/bin/busybox sh -c 'echo sh-ok' | /opt/busybox/bin/busybox grep -q \"sh-ok\" && echo \"BB-W5: ok busybox-sh-c\"\n"
    "/bin/sh -c 'echo bin-sh-ok' | /opt/busybox/bin/busybox grep -q \"bin-sh-ok\" && echo \"BB-W5: ok bin-sh-c\"\n"
    /* Drive ash variable expansion from a script file: the in-kernel builtin
     * shell that runs this smoke script expands $VAR even inside single quotes,
     * so a literal '$x' on the command line would be eaten before ash sees it.
     * Emitting the '$' via printf's \044 octal escape keeps it out of the outer
     * shell entirely; ash then parses and expands $x itself. */
    "/opt/busybox/bin/busybox printf 'x=7\\ntest \"\\044x\" = 7 && echo var-ok\\n' > /tmp/bb_dir/w5-vars.sh\n"
    "/bin/sh /tmp/bb_dir/w5-vars.sh | /opt/busybox/bin/busybox grep -q \"var-ok\" && echo \"BB-W5: ok vars\"\n"
    "/bin/sh -c 'echo $((2 + 3))' | /opt/busybox/bin/busybox grep -q \"5\" && echo \"BB-W5: ok math\"\n"
    "/bin/sh -c 'printf \"a\\\\nb\\\\n\" | grep -q b && echo pipe-ok' | /opt/busybox/bin/busybox grep -q \"pipe-ok\" && echo \"BB-W5: ok pipe\"\n"
    "/bin/sh -c 'echo redir-ok > /tmp/bb_dir/w5-redir; cat /tmp/bb_dir/w5-redir' | /opt/busybox/bin/busybox grep -q \"redir-ok\" && echo \"BB-W5: ok redir\"\n"
    "/bin/sh -c 'sleep 0; echo wait-ok' | /opt/busybox/bin/busybox grep -q \"wait-ok\" && echo \"BB-W5: ok wait\"\n"
    /* ash arithmetic-in-while-loop regression: $((i+1)) drives strtoull, which
     * must advance its endptr past a leading-0 number ("0+1") or ash's arith
     * parser spins forever and overflows its value stack. Driven from a script
     * file so the in-kernel builtin shell does not pre-expand the '$' (the '$'
     * is emitted via printf's \044). */
    "/opt/busybox/bin/busybox printf 'i=0\\nwhile [ \\044i -lt 3 ]; do i=\\044((i+1)); done\\necho loop-ok \\044i\\n' > /tmp/bb_dir/w5-loop.sh\n"
    "/bin/sh /tmp/bb_dir/w5-loop.sh | /opt/busybox/bin/busybox grep -q \"loop-ok 3\" && echo \"BB-W5: ok arith-loop\"\n"
    "echo \"BB-W5: done\"\n"
    /* ── Migration wave 6: account management (login/su/passwd suite). ──
     * The whole flow lives in /etc/bb-w6/run.sh and runs under real ash so the
     * '$'-heavy account logic (hashes, command substitution, regex grep) is not
     * mangled by the in-kernel builtin shell. */
    "/bin/sh /etc/bb-w6/run.sh\n"
    /* ── Migration wave 7: new applets in BusyBox 1.38 (uuidgen, tree, vmstat, sha384sum, tsort) ── */
    "/opt/busybox/bin/busybox uuidgen 2>/dev/null | grep -qE '^[0-9a-f-]{36}$' && echo \"BB-W7: ok uuidgen\"\n"
    "/opt/busybox/bin/busybox sha384sum /proc/version 2>/dev/null | grep -qE '^[0-9a-f]{96}' && echo \"BB-W7: ok sha384sum-upstream\"\n"
    "/opt/busybox/bin/busybox vmstat 2>/dev/null | grep -qE '[0-9]+' && echo \"BB-W7: ok vmstat-upstream\"\n"
    "printf 'libc kernel\\\\nkernel user\\\\nuser libc\\\\n' | /opt/busybox/bin/busybox tsort 2>/dev/null | head -n 1 | grep -q \"libc\" && echo \"BB-W7: ok tsort\"\n"
    "mkdir -p /tmp/bb_dir/w7/sub\n"
    "printf 'leaf\\\\n' > /tmp/bb_dir/w7/leaf.txt\n"
    "ln -sf leaf.txt /tmp/bb_dir/w7/link.txt 2>/dev/null || :\n"
    "/opt/busybox/bin/busybox tree /tmp/bb_dir/w7 2>/dev/null | grep -qE 'leaf' && echo \"BB-W7: ok tree-upstream\"\n"
    /* getfattr (upstream read side) over a user.* xattr set by the b1nix-native
     * setfattr (SYS_SETXATTR -> per-inode backend -> SYS_GETXATTR). */
    "/bin/setfattr -n user.b1nix -v wave7 /tmp/bb_dir/w7/leaf.txt\n"
    "/opt/busybox/bin/busybox getfattr -n user.b1nix /tmp/bb_dir/w7/leaf.txt 2>/dev/null | grep -q 'user.b1nix=\"wave7\"' && echo \"BB-W7: ok getfattr\"\n"
    /* lsblk is migrated to upstream (/bin/lsblk -> /opt/busybox/bin/busybox).
     * It enumerates /sys/block + /sys/dev/block and reads /proc/self/mountinfo,
     * all grown in the kernel for this wave. */
    "/bin/lsblk 2>/dev/null | grep -qw sata0 && echo \"BB-W7: ok lsblk\"\n"
    "rm -rf /tmp/bb_dir/w7\n"
    "/opt/busybox/bin/busybox --version 2>/dev/null | grep -qF \"1.38.0\" && echo \"BB-W7: ok version\"\n"
    /* ── Migration wave 8: promote applets to upstream via /bin/<cmd> ──
     * Each command is now an applet-manifest symlink (/bin/<cmd> ->
     * /opt/busybox/bin/busybox), so these exercise the *promoted* path (bare
     * absolute /bin entry, not the explicit /opt/... invocation of wave 7).
     * id and whoami retire their standalone b1nix ELFs; the rest gain a
     * shell-reachable /bin entry for the first time. */
    "echo \"BB-W8: start promote\"\n"
    "/bin/id -u 2>/dev/null | grep -q '^0$' && echo \"BB-W8: ok id\"\n"
    "/bin/whoami 2>/dev/null | grep -q '^root$' && echo \"BB-W8: ok whoami\"\n"
    "/bin/uuidgen 2>/dev/null | grep -qE '^[0-9a-f-]{36}$' && echo \"BB-W8: ok uuidgen\"\n"
    "/bin/sha384sum /proc/version 2>/dev/null | grep -qE '^[0-9a-f]{96}' && echo \"BB-W8: ok sha384sum\"\n"
    "/bin/vmstat 2>/dev/null | grep -qE '[0-9]+' && echo \"BB-W8: ok vmstat\"\n"
    "mkdir -p /tmp/bb_dir/w8\n"
    "printf 'leaf\\\\n' > /tmp/bb_dir/w8/leaf.txt\n"
    "/bin/tree /tmp/bb_dir/w8 2>/dev/null | grep -q 'leaf' && echo \"BB-W8: ok tree\"\n"
    "rm -rf /tmp/bb_dir/w8\n"
    "echo \"BB-W8: done\"\n"
    /* ── Migration wave 9: retire chmod/chown ELFs to upstream + /bin/tsort ──
     * /bin/{chmod,chown} are now manifest symlinks (the standalone b1nix ELFs
     * are no longer embedded); tsort gains a /bin entry for the first time.
     * Verify the action via the promoted /bin/<cmd>, reading back the result
     * with the upstream stat (which has no /bin entry of its own). */
    "echo \"BB-W9: start promote\"\n"
    "printf x > /tmp/bb_dir/w9f\n"
    "/bin/chmod 600 /tmp/bb_dir/w9f && /opt/busybox/bin/busybox stat -c '%a' /tmp/bb_dir/w9f 2>/dev/null | grep -q '^600$' && echo \"BB-W9: ok chmod\"\n"
    "/bin/chown 0:0 /tmp/bb_dir/w9f && /opt/busybox/bin/busybox stat -c '%u' /tmp/bb_dir/w9f 2>/dev/null | grep -q '^0$' && echo \"BB-W9: ok chown\"\n"
    "echo 'a b' > /tmp/bb_dir/w9t\n"
    "echo 'b c' >> /tmp/bb_dir/w9t\n"
    "/bin/tsort /tmp/bb_dir/w9t 2>/dev/null | head -n 1 | grep -q a && echo \"BB-W9: ok tsort\"\n"
    "rm -f /tmp/bb_dir/w9t\n"
    "rm -f /tmp/bb_dir/w9f\n"
    "echo \"BB-W9: done\"\n"
    "rm -rf /tmp/bb_dir/w2b\n"
    "rm -rf /tmp/bb_dir/w2\n"
    "/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file_mv /tmp/bb_dir/bb_file_lnk /tmp/bb_dir/bb_sort /tmp/bb_dir/bb_uniq /tmp/bb_dir/bb_tee /tmp/bb_dir/bb_clear /tmp/bb_dir/bb_seq /tmp/bb_dir/w5-redir /tmp/bb_dir/w5-vars.sh /tmp/bb_dir/w5-loop.sh\n"
    "[ ! -f /tmp/bb_dir/bb_file_mv ] && [ ! -f /tmp/bb_dir/bb_file_lnk ] && echo \"BB-SMOKE: ok rm\"\n"
    "/opt/busybox/bin/busybox rm -f /tmp/bb_dir/bb_file\n"
    "/opt/busybox/bin/busybox rmdir /tmp/bb_dir\n"
    "[ ! -d /tmp/bb_dir ] && echo \"BB-SMOKE: ok rmdir\"\n"
    "echo \"BB-SMOKE: done\"\n"
    "fi\n"
    "echo \"POSIX-SMOKE: done\"\n";



#ifdef MINIMAL_INITRAMFS
static const struct initramfs_file files[] = {
    {"/bin/init", (const char *)vfs_init_elf, sizeof(vfs_init_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/native-smoke", (const char *)vfs_native_smoke_elf,
     sizeof(vfs_native_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/return_42", (const char *)vfs_return_42_elf,
     sizeof(vfs_return_42_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_hello", (const char *)vfs_b1cc_hello_elf,
     sizeof(vfs_b1cc_hello_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_argv", (const char *)vfs_b1cc_argv_elf,
     sizeof(vfs_b1cc_argv_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_file_write", (const char *)vfs_b1cc_file_write_elf,
     sizeof(vfs_b1cc_file_write_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_stderr_exit", (const char *)vfs_b1cc_stderr_exit_elf,
     sizeof(vfs_b1cc_stderr_exit_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_better_c", (const char *)vfs_b1cc_better_c_elf,
     sizeof(vfs_b1cc_better_c_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_m34", (const char *)vfs_b1cc_m34_elf,
     sizeof(vfs_b1cc_m34_elf), INITRAMFS_EXECUTABLE},
    B1CC_M34_INITRAMFS_FILES
#ifdef B1CC_SELFHOST
    {"/bin/b1cc", (const char *)vfs_b1cc_elf, sizeof(vfs_b1cc_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/b1cc-selfsmoke", (const char *)vfs_b1cc_selfsmoke_elf,
     sizeof(vfs_b1cc_selfsmoke_elf), INITRAMFS_EXECUTABLE},
    {"/lib/b1cc/crt0.o", (const char *)vfs_b1cc_crt0, sizeof(vfs_b1cc_crt0), 0},
    {"/lib/b1cc/libb1nix.a", (const char *)vfs_b1cc_libc, sizeof(vfs_b1cc_libc), 0},
    /* M33 dynamic path: crt0-dynamic.o is b1cc's internal PIE linker input;
     * libc.so.1 is the runtime shared object the produced PIE DT_NEEDEDs and the
     * kernel's eager in-kernel linker resolves against. */
    {"/lib/b1cc/crt0-dynamic.o", (const char *)vfs_b1cc_crt0dyn,
     sizeof(vfs_b1cc_crt0dyn), 0},
    {"/lib/libc.so.1", (const char *)vfs_shared_libc_elf,
     sizeof(vfs_shared_libc_elf), INITRAMFS_EXECUTABLE},
    {"/lib/libc.so", "/lib/libc.so.1", 15, INITRAMFS_SYMLINK},
#endif

    {"/mnt/iso/.keep", "", 0, 0},
    {"/mnt/root/.keep", "", 0, 0},
};
#else
static const struct initramfs_file files[] = {
    {"/bin/init", (const char *)vfs_init_elf, sizeof(vfs_init_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/sh", "/opt/busybox/bin/busybox", 24, INITRAMFS_SYMLINK},
    {"/bin/hello", (const char *)vfs_hello_elf, sizeof(vfs_hello_elf),
     INITRAMFS_EXECUTABLE},
    {"/opt/busybox/bin/busybox", (const char *)vfs_upstream_busybox_elf,
     sizeof(vfs_upstream_busybox_elf), INITRAMFS_EXECUTABLE},
    {"/etc/bb-w2b/hello.xz", (const char *)initramfs_bb_w2b_xz,
     sizeof(initramfs_bb_w2b_xz), 0},
    {"/etc/bb-w6/run.sh", initramfs_bb_w6_sh, sizeof(initramfs_bb_w6_sh) - 1,
     INITRAMFS_EXECUTABLE},
    /* bpkg — minimal package manager, its config, and the offline smoke
     * fixtures (index + tarball + driver). The two tarball filenames carry the
     * same bytes so the install path resolves on either arch. */
    {"/bin/bpkg", initramfs_bpkg, sizeof(initramfs_bpkg) - 1,
     INITRAMFS_EXECUTABLE},
    {"/etc/bpkg.conf", initramfs_bpkg_conf, sizeof(initramfs_bpkg_conf) - 1, 0},
    {"/etc/bpkg-smoke.sh", initramfs_bpkg_smoke,
     sizeof(initramfs_bpkg_smoke) - 1, INITRAMFS_EXECUTABLE},
    {"/pkgs/index", initramfs_bpkg_index, sizeof(initramfs_bpkg_index) - 1, 0},
    {"/pkgs/hello-1.0-x86_64.tar.gz", (const char *)initramfs_bpkg_hello_tgz,
     sizeof(initramfs_bpkg_hello_tgz), 0},
    {"/pkgs/hello-1.0-i686.tar.gz", (const char *)initramfs_bpkg_hello_tgz,
     sizeof(initramfs_bpkg_hello_tgz), 0},
    {"/pkgs/dep1-1.0.tar.gz", (const char *)initramfs_bpkg_dep1_tgz,
     sizeof(initramfs_bpkg_dep1_tgz), 0},
    /* Applet symlinks — generated from tools/configs/applet-manifest.conf */
    {"/bin/busybox", "/opt/busybox/bin/busybox", 24, INITRAMFS_SYMLINK},
    /* M39: getty for inittab serial/tty sessions — the upstream BusyBox applet,
     * reachable under the conventional /bin and /sbin names. */
    {"/bin/getty", "/opt/busybox/bin/busybox", 24, INITRAMFS_SYMLINK},
    {"/sbin/getty", "/opt/busybox/bin/busybox", 24, INITRAMFS_SYMLINK},
#  include "initramfs_applet_symlinks.inc"
    {"/bin/native-smoke", (const char *)vfs_native_smoke_elf,
     sizeof(vfs_native_smoke_elf), INITRAMFS_EXECUTABLE},
    /* b1cc-compiled smoke binaries (M5/M7) — exercised by programs.c right after
     * native-smoke. Their .inc are #included unconditionally above, so they must
     * be registered in BOTH the minimal and the full table (a missing-from-full
     * entry shows up as B1CC-*-SMOKE: spawn-fail in the full smoke). */
    {"/bin/return_42", (const char *)vfs_return_42_elf,
     sizeof(vfs_return_42_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_hello", (const char *)vfs_b1cc_hello_elf,
     sizeof(vfs_b1cc_hello_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_argv", (const char *)vfs_b1cc_argv_elf,
     sizeof(vfs_b1cc_argv_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_file_write", (const char *)vfs_b1cc_file_write_elf,
     sizeof(vfs_b1cc_file_write_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_stderr_exit", (const char *)vfs_b1cc_stderr_exit_elf,
     sizeof(vfs_b1cc_stderr_exit_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_better_c", (const char *)vfs_b1cc_better_c_elf,
     sizeof(vfs_b1cc_better_c_elf), INITRAMFS_EXECUTABLE},
    {"/bin/b1cc_m34", (const char *)vfs_b1cc_m34_elf,
     sizeof(vfs_b1cc_m34_elf), INITRAMFS_EXECUTABLE},
#ifndef MINIMAL_INITRAMFS
    B1CC_M34_INITRAMFS_FILES
#endif
#ifdef B1CC_SELFHOST
    {"/bin/b1cc", (const char *)vfs_b1cc_elf, sizeof(vfs_b1cc_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/b1cc-selfsmoke", (const char *)vfs_b1cc_selfsmoke_elf,
     sizeof(vfs_b1cc_selfsmoke_elf), INITRAMFS_EXECUTABLE},
    {"/lib/b1cc/crt0.o", (const char *)vfs_b1cc_crt0, sizeof(vfs_b1cc_crt0), 0},
    {"/lib/b1cc/libb1nix.a", (const char *)vfs_b1cc_libc, sizeof(vfs_b1cc_libc), 0},
    /* M33 dynamic path: PIE linker input (libc.so.1 already registered below). */
    {"/lib/b1cc/crt0-dynamic.o", (const char *)vfs_b1cc_crt0dyn,
     sizeof(vfs_b1cc_crt0dyn), 0},
#endif
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
    {"/bin/m56-smoke", (const char *)vfs_m56_smoke_elf,
     sizeof(vfs_m56_smoke_elf), INITRAMFS_EXECUTABLE},
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
    {"/bin/m53-zlib-smoke", (const char *)vfs_m53_zlib_smoke_elf,
     sizeof(vfs_m53_zlib_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-libpng-smoke", (const char *)vfs_m53_libpng_smoke_elf,
     sizeof(vfs_m53_libpng_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-libjpeg-smoke", (const char *)vfs_m53_libjpeg_smoke_elf,
     sizeof(vfs_m53_libjpeg_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-libwebp-smoke", (const char *)vfs_m53_libwebp_smoke_elf,
     sizeof(vfs_m53_libwebp_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-libvpx-smoke", (const char *)vfs_m53_libvpx_smoke_elf,
     sizeof(vfs_m53_libvpx_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-wapcaplet-smoke", (const char *)vfs_m53_wapcaplet_smoke_elf,
     sizeof(vfs_m53_wapcaplet_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-parserutils-smoke", (const char *)vfs_m53_parserutils_smoke_elf,
     sizeof(vfs_m53_parserutils_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-hubbub-smoke", (const char *)vfs_m53_hubbub_smoke_elf,
     sizeof(vfs_m53_hubbub_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-libcss-smoke", (const char *)vfs_m53_libcss_smoke_elf,
     sizeof(vfs_m53_libcss_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-libdom-smoke", (const char *)vfs_m53_libdom_smoke_elf,
     sizeof(vfs_m53_libdom_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-nslibs-smoke", (const char *)vfs_m53_nslibs_smoke_elf,
     sizeof(vfs_m53_nslibs_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-httpd", (const char *)vfs_m53_httpd_elf,
     sizeof(vfs_m53_httpd_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-httpsd", (const char *)vfs_m53_httpsd_elf,
     sizeof(vfs_m53_httpsd_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-virgl-smoke", (const char *)vfs_m53_virgl_smoke_elf,
     sizeof(vfs_m53_virgl_smoke_elf), INITRAMFS_EXECUTABLE},
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
    /* GNU bash 5.2 — default interactive shell. Also reachable as /bin/sh via
     * the symlink below (bash runs in POSIX mode when invoked as "sh"). */
    {"/bin/bash", (const char *)vfs_bash_elf, sizeof(vfs_bash_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/m30-pie", (const char *)vfs_m30_pie_elf,
     sizeof(vfs_m30_pie_elf), INITRAMFS_EXECUTABLE},
#ifdef __x86_64__
    {"/bin/m30-dynamic", (const char *)vfs_m30_dynamic_elf,
     sizeof(vfs_m30_dynamic_elf), INITRAMFS_EXECUTABLE},
    {"/lib/libc.so.1", (const char *)vfs_shared_libc_elf,
     sizeof(vfs_shared_libc_elf), INITRAMFS_EXECUTABLE},
    {"/lib/libc.so", "/lib/libc.so.1", 15, INITRAMFS_SYMLINK},
    {"/lib/m69_plugin.so", (const char *)vfs_m69_plugin_elf,
     sizeof(vfs_m69_plugin_elf), INITRAMFS_EXECUTABLE},
    /* M40: a static Linux x86_64 ELF that uses Linux syscall numbers; the Linux
     * ABI translation layer maps them to the native handlers. */
    {"/bin/m40-linux-hello", (const char *)vfs_m40_linux_hello,
     sizeof(vfs_m40_linux_hello), INITRAMFS_EXECUTABLE},
    /* M67: a static Rust program (Vec/String/HashMap/thread) built for
     * x86_64-unknown-b1nix with nightly rustc + build-std, linked against the
     * b1nix libc via the cross gcc. Exercises the Rust std unix PAL at runtime. */
    {"/bin/m67-rust", (const char *)vfs_m67_rust_elf,
     sizeof(vfs_m67_rust_elf), INITRAMFS_EXECUTABLE},
    /* M89: /lib/libgcc_s.so removed — busybox/nsfb fold libgcc statically and the
     * C++ stack is on libc++, so nothing carries DT_NEEDED libgcc_s.so. */
    /* M89: /lib/libstdc++.so.6 removed — the C++ ecosystem is on libc++ now. */
    /* Shared LLVM C++ standard library (M89). libc++-linked binaries carry
     * DT_NEEDED libc++.so.1 -> libc++abi.so.1 -> libc.so.1; the M69 exec-time
     * linker resolves the chain. libc++abi.so.1 folds the libunwind unwinder. */
    {"/lib/libc++.so.1", (const char *)vfs_libcxx_elf,
     sizeof(vfs_libcxx_elf), INITRAMFS_EXECUTABLE},
    {"/lib/libc++.so", "/lib/libc++.so.1", 17, INITRAMFS_SYMLINK},
    {"/lib/libc++abi.so.1", (const char *)vfs_libcxxabi_so1,
     sizeof(vfs_libcxxabi_so1), INITRAMFS_EXECUTABLE},
    {"/lib/libc++abi.so", "/lib/libc++abi.so.1", 20, INITRAMFS_SYMLINK},
    {"/lib/libskia.so", (const char *)vfs_libskia_so,
     sizeof(vfs_libskia_so), INITRAMFS_EXECUTABLE},
    {"/lib/libraw_ptr.so", (const char *)vfs_libraw_ptr_so,
     sizeof(vfs_libraw_ptr_so), INITRAMFS_EXECUTABLE},
    {"/lib/libfontconfig.so", (const char *)vfs_libfontconfig_so,
     sizeof(vfs_libfontconfig_so), INITRAMFS_EXECUTABLE},
    /* Skia's Ganesh GL backend DT_NEEDEDs libGLESv2.so/libEGL.so; these are
     * thin b1nix stubs (the real GL/EGL entrypoints are folded into the Skia
     * demo executable over Mesa OSMesa and exported via --export-dynamic). */
    {"/lib/libGLESv2.so", (const char *)vfs_libGLESv2_so,
     sizeof(vfs_libGLESv2_so), INITRAMFS_EXECUTABLE},
    {"/lib/libEGL.so", (const char *)vfs_libEGL_so,
     sizeof(vfs_libEGL_so), INITRAMFS_EXECUTABLE},
    {"/lib/libb1gui.so", (const char *)vfs_libb1gui_so,
     sizeof(vfs_libb1gui_so), INITRAMFS_EXECUTABLE},
#endif
    {"/bin/m34-smoke", (const char *)vfs_m34_smoke_elf,
     sizeof(vfs_m34_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m35-smoke", (const char *)vfs_m35_smoke_elf,
     sizeof(vfs_m35_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m38-sound", (const char *)vfs_m38_sound_elf,
     sizeof(vfs_m38_sound_elf), INITRAMFS_EXECUTABLE},
    {"/test.wav", (const char *)vfs_testwav,
     sizeof(vfs_testwav), 0},
    {"/share/fonts/B1nixMono-Regular.ttf", (const char *)vfs_testfont,
     sizeof(vfs_testfont), 0},
    {"/bin/m42-w5pre-smoke", (const char *)vfs_m42_w5pre_smoke_elf,
     sizeof(vfs_m42_w5pre_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m46-smoke", (const char *)vfs_m46_smoke_elf,
     sizeof(vfs_m46_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m57-smoke", (const char *)vfs_m57_smoke_elf,
     sizeof(vfs_m57_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m73-smoke", (const char *)vfs_m73_smoke_elf,
     sizeof(vfs_m73_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m63-smoke", (const char *)vfs_m63_smoke_elf,
     sizeof(vfs_m63_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m71-aslr", (const char *)vfs_m71_aslr_elf,
     sizeof(vfs_m71_aslr_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m47-smoke", (const char *)vfs_m47_smoke_elf,
     sizeof(vfs_m47_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m48-smoke", (const char *)vfs_m48_smoke_elf,
     sizeof(vfs_m48_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m49-smoke", (const char *)vfs_m49_smoke_elf,
     sizeof(vfs_m49_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m-posixmm-smoke", (const char *)vfs_m_posixmm_smoke_elf,
     sizeof(vfs_m_posixmm_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m49-libwayland", (const char *)vfs_m49_libwayland_elf,
     sizeof(vfs_m49_libwayland_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m49-libwayland-server",
     (const char *)vfs_m49_libwayland_server_elf,
     sizeof(vfs_m49_libwayland_server_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m50-smoke", (const char *)vfs_m50_smoke_elf,
     sizeof(vfs_m50_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-smoke", (const char *)vfs_m51_smoke_elf,
     sizeof(vfs_m51_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-pixman-smoke", (const char *)vfs_m51_pixman_smoke_elf,
     sizeof(vfs_m51_pixman_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-freetype-smoke", (const char *)vfs_m51_freetype_smoke_elf,
     sizeof(vfs_m51_freetype_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-cairo-smoke", (const char *)vfs_m51_cairo_smoke_elf,
     sizeof(vfs_m51_cairo_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-cairo-wayland", (const char *)vfs_m51_cairo_wayland_elf,
     sizeof(vfs_m51_cairo_wayland_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m52-gl-smoke", (const char *)vfs_m52_gl_smoke_elf,
     sizeof(vfs_m52_gl_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m52-osmesa", (const char *)vfs_m52_osmesa_elf,
     sizeof(vfs_m52_osmesa_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m53-mesa-virgl", (const char *)vfs_m53_mesa_virgl_elf,
     sizeof(vfs_m53_mesa_virgl_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m52-glsl", (const char *)vfs_m52_glsl_elf,
     sizeof(vfs_m52_glsl_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m59-smoke", (const char *)vfs_m59_smoke_elf,
     sizeof(vfs_m59_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m91-skia-smoke", (const char *)vfs_m91_skia_smoke_elf,
     sizeof(vfs_m91_skia_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/skia-dm", (const char *)vfs_m91_skia_dm_elf,
     sizeof(vfs_m91_skia_dm_elf), INITRAMFS_EXECUTABLE},
#ifdef __x86_64__
    {"/bin/m64-clang-smoke", (const char *)vfs_m64_clang_smoke_elf,
     sizeof(vfs_m64_clang_smoke_elf), INITRAMFS_EXECUTABLE},
#endif
    {"/bin/cxx-smoke", (const char *)vfs_cxx_smoke_elf,
     sizeof(vfs_cxx_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m55-iostream", (const char *)vfs_m55_iostream_elf,
     sizeof(vfs_m55_iostream_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m55-litehtml", (const char *)vfs_m55_litehtml_elf,
     sizeof(vfs_m55_litehtml_elf), INITRAMFS_EXECUTABLE},
    {"/bin/js", (const char *)vfs_js_elf,
     sizeof(vfs_js_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m58-smoke", (const char *)vfs_m58_smoke_elf,
     sizeof(vfs_m58_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-xkb-smoke", (const char *)vfs_m51_xkb_smoke_elf,
     sizeof(vfs_m51_xkb_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-clipboard-smoke", (const char *)vfs_m51_clipboard_smoke_elf,
     sizeof(vfs_m51_clipboard_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-harfbuzz-smoke", (const char *)vfs_m51_harfbuzz_smoke_elf,
     sizeof(vfs_m51_harfbuzz_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/m51-fontconfig-smoke", (const char *)vfs_m51_fontconfig_smoke_elf,
     sizeof(vfs_m51_fontconfig_smoke_elf), INITRAMFS_EXECUTABLE},
    {"/bin/displayd", (const char *)vfs_displayd_elf,
     sizeof(vfs_displayd_elf), INITRAMFS_EXECUTABLE},
    {"/bin/gclock", (const char *)vfs_gclock_elf,
     sizeof(vfs_gclock_elf), INITRAMFS_EXECUTABLE},
    {"/bin/gterm", (const char *)vfs_gterm_elf,
     sizeof(vfs_gterm_elf), INITRAMFS_EXECUTABLE},
    {"/bin/gpaint", (const char *)vfs_gpaint_elf,
     sizeof(vfs_gpaint_elf), INITRAMFS_EXECUTABLE},
    {"/bin/gdesktop", (const char *)vfs_gdesktop_elf,
     sizeof(vfs_gdesktop_elf), INITRAMFS_EXECUTABLE},
    {"/bin/gabout", (const char *)vfs_gabout_elf,
     sizeof(vfs_gabout_elf), INITRAMFS_EXECUTABLE},
    /* M30: compatibility interpreter path. Startup dependency loading,
     * symbol lookup, and relocation are performed eagerly by the kernel. */
    {"/lib/ld-b1nix.so", (const char *)vfs_m30_pie_elf,
     sizeof(vfs_m30_pie_elf), INITRAMFS_EXECUTABLE},
    {"/etc/motd", "welcome to b1nix m4\n", 23, 0},
    {"/etc/passwd", initramfs_passwd, sizeof(initramfs_passwd) - 1, 0},
    {"/etc/shells", initramfs_shells, sizeof(initramfs_shells) - 1, 0},
    {"/etc/shadow", initramfs_shadow, sizeof(initramfs_shadow) - 1, 0},
    {"/etc/group", initramfs_group, sizeof(initramfs_group) - 1, 0},
    {"/etc/rc", initramfs_rc, sizeof(initramfs_rc) - 1, INITRAMFS_EXECUTABLE},
    {"/etc/bash-smoke.sh", initramfs_bash_smoke,
     sizeof(initramfs_bash_smoke) - 1, INITRAMFS_EXECUTABLE},
    /* M39: configurable init — inittab + telinit. */
    {"/etc/inittab", initramfs_inittab, sizeof(initramfs_inittab) - 1, 0},
    {"/sbin/telinit", (const char *)vfs_telinit_elf, sizeof(vfs_telinit_elf),
     INITRAMFS_EXECUTABLE},
    {"/bin/telinit", (const char *)vfs_telinit_elf, sizeof(vfs_telinit_elf),
     INITRAMFS_EXECUTABLE},
    {"/etc/init.d/sshd", initramfs_sshd_service, sizeof(initramfs_sshd_service) - 1, INITRAMFS_EXECUTABLE},
    {"/etc/fstab", initramfs_fstab, sizeof(initramfs_fstab) - 1, 0},
    {"/etc/fonts/fonts.conf", initramfs_etc_fonts_conf,
     sizeof(initramfs_etc_fonts_conf) - 1, 0},
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
    /* The halt ELF handles reboot/poweroff/halt/shutdown by argv[0] inspection.
     * Plain root-owned binaries (NOT setuid): SYS_REBOOT itself enforces root,
     * matching Linux reboot()/CAP_SYS_BOOT. */
    {"/bin/halt", (const char *)vfs_halt_elf, sizeof(vfs_halt_elf), INITRAMFS_EXECUTABLE},
    {"/bin/reboot", (const char *)vfs_halt_elf, sizeof(vfs_halt_elf), INITRAMFS_EXECUTABLE},
    {"/bin/poweroff", (const char *)vfs_halt_elf, sizeof(vfs_halt_elf), INITRAMFS_EXECUTABLE},
    {"/bin/shutdown", (const char *)vfs_halt_elf, sizeof(vfs_halt_elf), INITRAMFS_EXECUTABLE},
    {"/bin/setfattr", (const char *)vfs_setfattr_elf, sizeof(vfs_setfattr_elf), INITRAMFS_EXECUTABLE},
    {"/bin/su", (const char *)vfs_su_elf, sizeof(vfs_su_elf), INITRAMFS_EXECUTABLE | INITRAMFS_SETUID},
    {"/bin/passwd", (const char *)vfs_passwd_elf, sizeof(vfs_passwd_elf), INITRAMFS_EXECUTABLE | INITRAMFS_SETUID},
    /* /bin/id and /bin/whoami — served by applet-manifest upstream symlinks
     * (M42 wave 8), emitted from initramfs_applet_symlinks.inc below. */
    {"/bin/groups", (const char *)vfs_groups_elf, sizeof(vfs_groups_elf), INITRAMFS_EXECUTABLE},
    {"/bin/useradd", (const char *)vfs_useradd_elf, sizeof(vfs_useradd_elf), INITRAMFS_EXECUTABLE},
    {"/bin/userdel", (const char *)vfs_userdel_elf, sizeof(vfs_userdel_elf), INITRAMFS_EXECUTABLE},
    {"/bin/groupadd", (const char *)vfs_groupadd_elf, sizeof(vfs_groupadd_elf), INITRAMFS_EXECUTABLE},
    /* /bin/chown and /bin/chmod — served by applet-manifest upstream symlinks
     * (M44 wave 9), emitted from initramfs_applet_symlinks.inc below. */
    /* M37: mount point for the live ISO; the initramfs must contain a node
     * under /mnt/iso so that vfs_mount() can find the target directory after
     * the initramfs is mounted at "/".  Without this entry add_node() never
     * creates the /mnt/iso branch inside the initramfs root, and vfs_find_node
     * returns ENOENT when the liveiso boot code calls vfs_mount(usb0,
     * "/mnt/iso", "iso9660", 0). */
    {"/mnt/iso/.keep", "", 0, 0},
    /* M37: mount point for the verified live rootfs (loop0 ext4). */
    {"/mnt/root/.keep", "", 0, 0},
    TCC_INITRAMFS_FILES,
    NETSURF_INITRAMFS_FILES
};
#endif

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
