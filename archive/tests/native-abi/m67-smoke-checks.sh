# ARCHIVED from tests/smoke.sh: the checks for /bin/m67-rust, a static
# x86_64-unknown-b1nix Rust program speaking the native syscall ABI.

# ── M67 Rust cross-toolchain (x86_64 only: it runs a Rust std program) ──
if [ "$ARCH" = "x86_64" ]; then
	section "M67 Rust Cross-Toolchain"
	check_output "$LOG" "M67-RUST: start" "M67 Rust std smoke starts"
	check_output "$LOG" "rust on b1nix squares=\[0, 1, 4, 9, 16\] sum=30" "Rust Vec/String/HashMap + iterator sum ran (println! reached the console)"
	check_output "$LOG" "thread returned 42" "Rust std::thread::spawn + join works (futex bridge to native SYS_FUTEX)"
	check_output "$LOG" "M67-RUST: ok run-std" "static Rust ELF ran and exited 0"
	check_output "$LOG" "M67-RUST: done" "M67 Rust smoke completes"
fi

