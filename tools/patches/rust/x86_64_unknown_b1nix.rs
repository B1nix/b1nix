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
