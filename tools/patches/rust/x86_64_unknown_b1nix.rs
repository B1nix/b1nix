// x86_64-unknown-b1nix — b1nix native host target (M68).
//
// b1nix is a small Unix-like monolithic kernel with a POSIX-ish libc that is
// Linux-ABI-aliased. We reuse Rust std's `unix` PAL unchanged (os=linux,
// env=musl, llvm-target points at linux-gnu so every std::sys module picks the
// Linux flavor). `host_tools = true` so the bootstrap builds rustc/cargo here.
//
// b1nix now has real shared libraries: the cross-GCC produces ET_DYN objects and
// the kernel M30 startup linker + M69 dlopen load them. rustc is built as a
// dynamic, position-independent (PIE / ET_DYN) program that links libLLVM.so and
// librustc_driver.so; the kernel loader resolves their DT_NEEDED at exec.

use crate::spec::{
    Arch, Cc, LinkSelfContainedDefault, LinkerFlavor, Lld, PanicStrategy, RelocModel, RelroLevel,
    StackProbeType, Target,
    TargetMetadata, TlsModel, base,
};

pub(crate) fn target() -> Target {
    let mut base = base::linux_musl::opts();
    base.cpu = "x86-64".into();
    base.plt_by_default = false;
    base.max_atomic_width = Some(64);
    base.stack_probes = StackProbeType::Inline;

    // The b1nix cross gcc drives the link (it knows the crt startfiles — now the
    // shared crtbeginS/crtendS + libgcc_s — and the b1nix libc sysroot).
    base.linker = Some("x86_64-b1nix-gcc".into());
    base.linker_flavor = LinkerFlavor::Gnu(Cc::Yes, Lld::No);
    base.dynamic_linking = true;
    base.executables = true;
    base.crt_static_default = false;
    base.crt_static_respected = true;
    base.crt_static_allows_dylibs = true;
    base.relocation_model = RelocModel::Pic;
    base.position_independent_executables = true;
    base.static_position_independent_executables = false;
    base.has_rpath = true;
    // Let the b1nix cross-gcc add its own crt (crt0/crtbeginS/crtendS via specs);
    // do NOT use rust's self-contained musl crt (b1nix has no crti.o/crtn.o).
    base.link_self_contained = LinkSelfContainedDefault::False;
    base.relro_level = RelroLevel::Off;
    // b1nix has no dynamic-TLS runtime (__tls_get_addr / DTV); force initial-exec
    // (matches the cross-gcc default) so __thread uses %fs+GOT(TPOFF).
    base.tls_model = TlsModel::InitialExec;
    base.add_pre_link_args(LinkerFlavor::Gnu(Cc::Yes, Lld::No), &["-m64"]);
    // Dynamic libc: the cross-gcc sysroot now ships libc.so (shared). Two parts:
    //  * `-lc` here, EARLY, resolves to libc.so and provides the C mem*/str* funcs
    //    dynamically before the libc+libm COMBINED archive (-lm, later) can pull
    //    static copies. (libc has special driver search, so it is found this early;
    //    a bare `-lgcc_s` here is NOT — its -L lands later, hence not added here.)
    //  * `--no-as-needed` stays active for the rest of the line so the driver's own
    //    (later, correctly -L'd) `-lgcc_s` — which provides the unwinder _Unwind_*,
    //    __register_frame and __popcountdi2 that libLLVM.so imports — is kept as a
    //    NEEDED shared lib instead of being dropped under --gc-sections/--as-needed
    //    (the static libc.a chain that used to drag libgcc in is gone now).
    base.add_pre_link_args(
        LinkerFlavor::Gnu(Cc::Yes, Lld::No),
        &["-Wl,--push-state", "-Wl,--no-as-needed", "-lc", "-Wl,--pop-state"],
    );
    // The unwinder (_Unwind_*, __register_frame) and the __popcountdi2 builtin that
    // libLLVM.so imports must be provided AFTER it on the link line, and libgcc_s's
    // -L is not set up early enough for a pre-link `-lgcc_s` (rust drives with
    // -nodefaultlibs, so the gcc driver does not add libgcc_s itself either). Put it
    // in late_link_args, which ld processes after the user libraries (incl.
    // libLLVM.so) and after the toolchain -L paths — so it is both found and
    // positioned to satisfy libLLVM.so's references. --no-as-needed (set above) is
    // still active here, so it is kept as a NEEDED shared lib.
    crate::spec::add_link_args(
        &mut base.late_link_args,
        LinkerFlavor::Gnu(Cc::Yes, Lld::No),
        // --push-state/--pop-state isolates this from rust's own -Bstatic/-Bdynamic
        // bookkeeping; -Bdynamic forces libgcc_s.so to link as a shared lib even
        // inside the crt-static rustc binary link (where the default is -Bstatic,
        // which would otherwise "attempt static link of dynamic object").
        &["-Wl,--push-state", "-Wl,-Bdynamic", "-Wl,--no-as-needed", "-lgcc_s", "-Wl,--pop-state"],
    );

    // panic=abort for now — the unwinder exists (libgcc_s), but unwinding across
    // the b1nix exec boundary is not yet exercised; see RUST-PORT-PLAN.md.
    base.panic_strategy = PanicStrategy::Abort;

    Target {
        llvm_target: "x86_64-unknown-linux-gnu".into(),
        metadata: TargetMetadata {
            description: Some("b1nix x86_64 (POSIX-ish libc, dynamic PIE, native host)".into()),
            tier: Some(3),
            host_tools: Some(true),
            std: Some(true),
        },
        pointer_width: 64,
        data_layout:
            "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128".into(),
        arch: Arch::X86_64,
        options: base,
    }
}
