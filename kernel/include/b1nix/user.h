#ifndef B1NIX_USER_H
#define B1NIX_USER_H

#include <b1nix/types.h>

/* Arguments and environment an exec may carry.
 *
 * Thirty-two of each, and the copy simply stopped there — no error, no
 * indication. A browser's child process is started with forty to sixty
 * arguments (--type=, --field-trial-handle=, feature lists) and an
 * environment of comparable size, so it came up missing the flags that tell
 * it what it is and which channel to use, and then behaved like a process
 * that could not find its parent. sys_execve already copies up to 256 of
 * each; this is the limit that was throwing them away. */
#define USER_MAX_ARGS 256
#define USER_MAX_ENVS 256
/* Total PT_LOAD segments across the executable and every DT_NEEDED object in the
 * eager-linked graph. Deep C++ chains reach many objects × ~4 segments each
 * (the Skia demo: 11 objects × 4 = 44), so 32 is too small — 64 leaves room. */
#define USER_MAX_IMAGE_SEGMENTS 64
/* dl_iterate_phdr module table: the executable + its DT_NEEDED shared objects
 * (the eager linker caps the object graph at DYN_MAX_OBJECTS = 16).
 * USER_DL_MODULE_NAME_MAX bounds the copied soname/path. */
#define USER_MAX_DL_MODULES 16
#define USER_DL_MODULE_NAME_MAX 96
/* Keep enough eagerly mapped stack for the musl loader's dependency graph and
 * relocation work; the larger stack window below still grows lazily. */
#define USER_STACK_SIZE (64 * PAGE_SIZE)
#define USER_SPACE_LIMIT 0x0000800000000000ULL
#define USER_STACK_TOP 0x0000800000000000ULL
/* Stack growth window reserved below USER_STACK_TOP. The loader describes it
 * with one VMA and procfs labels that VMA [stack], so both must agree. */
#define USER_STACK_MAX_SIZE (8ULL * 1024ULL * 1024ULL)

enum user_image_kind {
	USER_IMAGE_ELF64 = 2,
	USER_IMAGE_ELF32 = 3,
};

/* M40 — binary personality. b1nix native binaries use the b1nix syscall ABI
 * (numbers in <b1nix/syscall.h>); Linux binaries use the Linux x86_64 syscall
 * ABI (same CPU calling convention, different numbers). The ELF loader detects
 * a Linux binary (via EI_OSABI==ELFOSABI_LINUX or a GNU .note.ABI-tag with
 * OS=Linux) and tags the image PERSONALITY_LINUX; syscall_dispatch_impl then
 * translates Linux syscall numbers to the native handlers for such tasks. */
enum user_personality {
	PERSONALITY_B1NIX = 0,
	PERSONALITY_LINUX = 1,
};

struct vfs_node;

struct user_image_segment {
	u64 vaddr;
	u64 memsz;
	u64 filesz;
	u64 flags;
	void *data;
	/* Absolute byte offset of this segment in the backing ELF file (p_offset),
	 * used by the demand-paged loader to map segment pages back to file pages. */
	u64 file_offset;
	/* 1 = demand-paging candidate: read-only, page-aligned, filesz == memsz, and
	 * untouched by any in-kernel relocation. user_run_elf_image maps it
	 * file-backed lazy instead of pinning private frames, so the resident set is
	 * the touched working set and the rest stays reclaimable. */
	u8 demand_ok;
};

struct user_address_space {
	u64 pml4_frame;
	u64 stack_base;
	u64 stack_size;
	u64 stack_top;
	void *stack_image;
	usize stack_image_size;
};

struct task;

struct user_loaded_image {
	enum user_image_kind kind;
	/* The path exec was asked for — what AT_EXECFN reports, as on Linux. */
	const char *path;
	/* The file that was actually loaded: `path` with its final symlink chain
	 * followed. This is what /proc/<pid>/exe names, so a PID 1 started as
	 * /sbin/init reads back as the BusyBox multicall ELF it links to. */
	const char *real_path;
	u64 entry;
	struct user_address_space address_space;
	struct user_image_segment segments[USER_MAX_IMAGE_SEGMENTS];
	usize segment_count;
	int argc;
	const char **argv;
	const char **envp;
	int refcount;
	/* PT_TLS (thread-local storage) template for the main thread. tls_memsz==0
	 * means the binary has no TLS. tls_data holds a copy of the initialised
	 * portion (tdata, tls_filesz bytes); the rest up to tls_memsz is .tbss. */
	u64 tls_memsz;
	u64 tls_filesz;
	u64 tls_align;
	void *tls_data;
	/* M42: user VA of the kernel-owned signal-return trampoline page (RO+exec),
	 * mapped at exec. arch_build_signal_frame points the handler's return here
	 * instead of trusting a userspace-supplied sa_restorer. 0 = not mapped. */
	u64 sigreturn_trampoline;
	/* M40: binary personality (see enum user_personality). Set by the ELF64
	 * loader; PERSONALITY_LINUX activates Linux syscall-number translation. */
	enum user_personality personality;
	/* Demand-paged file backing (self-host RAM floor). exe_node is the ref-held
	 * VFS node of a disk-backed (read_cb) executable whose read-only segments are
	 * demand-paged from the page cache; 0 = fully eager (initramfs / ineligible). */
	struct vfs_node *exe_node;
	int demand_paged;
	/* M92: executable's program header table info for AT_PHDR/AT_PHNUM auxv.
	 * phdr_vaddr is the in-process VA where the ELF program headers are mapped;
	 * phnum is e_phnum. Set by the ELF loaders; used by user_build_initial_stack.
	 * For ET_EXEC (load_base=0) this is just e_phoff; for ET_DYN (PIE) it is
	 * load_base + e_phoff. 0 = not yet populated. */
	u64 phdr_vaddr;
	u16 phnum;
	/* dl_iterate_phdr support: one descriptor per loaded module (the executable
	 * plus every DT_NEEDED shared object), recorded during eager linking. The
	 * shared libgcc_s.so DWARF unwinder finds each module's PT_GNU_EH_FRAME
	 * (.eh_frame_hdr -> .eh_frame) via dl_iterate_phdr; this is what lets a C++
	 * exception thrown inside libstdc++.so.6 unwind back across the .so/exe
	 * boundary (otherwise the throw frame has no FDE -> std::terminate). base is the
	 * load bias (dlpi_addr), phdr_vaddr the in-process address of the program
	 * header table (dlpi_phdr), phnum its entry count. */
	struct user_dl_module {
		u64 base;
		u64 phdr_vaddr;
		u32 phnum;
		u64 eh_frame_va;  /* in-process .eh_frame address, for __register_frame */
		char name[USER_DL_MODULE_NAME_MAX];
	} dl_modules[USER_MAX_DL_MODULES];
	usize dl_module_count;
	/* Userspace ld.so support (ldso-migration plan). When a PT_INTERP names a
	 * real userspace dynamic linker (currently only musl's
	 * "/lib/ld-musl-x86_64.so.1"), the kernel loads the interpreter's own
	 * PT_LOAD segments unrelocated (it self-relocates via its own embedded
	 * R_X86_64_RELATIVE entries, exactly like Linux) and hands control to its
	 * entry point instead of running the in-kernel eager linker. interp_base
	 * != 0 marks this path; entry becomes the interpreter's entry, and
	 * app_entry preserves the executable's own entry point for AT_ENTRY. */
	u64 app_entry;
	u64 interp_base;
	/* M80: where the auxiliary vector ended up on the initial user stack, so
	 * /proc/<pid>/auxv can read it back out of the process's own address space
	 * (a crash reporter reads AT_PHDR/AT_ENTRY/AT_BASE from there to locate the
	 * executable and its interpreter). 0 = not populated. */
	u64 auxv_vaddr;
	u32 auxv_size;
	/* Path of the ELF interpreter this image runs under (empty when it has
	 * none). /proc/<pid>/maps names the interpreter's mappings with it, which is
	 * how a reader tells ld.so's text from the executable's. */
	char interp_path[64];
	/* M108: the credentials this image will run under once execve commits the
	 * file's set-user-ID / set-group-ID bits. The bits are only applied to the
	 * task AFTER the image has loaded (an exec that fails must not leave the
	 * caller privileged), but the auxiliary vector is built during the load —
	 * so without these the kernel would publish the PRE-exec euid/egid in
	 * AT_EUID/AT_EGID and AT_SECURE=0 for a setuid binary. musl's ld.so derives
	 * its "secure" mode (which is what suppresses LD_PRELOAD and
	 * LD_LIBRARY_PATH) from exactly those entries, so getting them wrong is a
	 * privilege escalation: an unprivileged caller could preload a library into
	 * /bin/su. cred_override != 0 means cred_euid/cred_egid are authoritative.
	 */
	u8 cred_override;
	u32 cred_euid;
	u32 cred_egid;
};

void userspace_init(void);
int user_spawn(const char *path, int argc, const char **argv);
/* Like user_spawn but with an explicit environment (envp, NULL-terminated). */
int user_spawn_env(const char *path, int argc, const char **argv,
                   const char **envp);
int user_execve_current(const char *path, const char **argv, const char **envp);
struct task;
void user_address_space_cleanup(struct task *t);
/* Full executable path of a task (the loaded image's path), for /proc/<pid>/exe.
 * Falls back to task->name. */
const char *user_task_exe_path(struct task *t);
/* Follow the final symlink chain of an exec path, so /proc/<pid>/exe names the
 * file that was loaded rather than the name used to reach it. */
void user_exec_realpath(const char *path, char *out, usize outsz);
void user_image_free(struct user_loaded_image *image);

#endif
