#ifndef _ELF_H
#define _ELF_H

/* Standard ELF types and constants for b1nix userspace. A focused-but-real
 * subset: the headers, symbols, relocations, dynamic and note entries, and the
 * auxiliary vector. Enough for ports that parse ELF objects or read
 * /proc/self/auxv (e.g. Mesa's CPU-feature detection). */

#include <stdint.h>

typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf32_Word;
typedef int32_t Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf32_Xword;
typedef int64_t Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;
typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;
typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

#define EI_NIDENT 16

/* ELF magic (e_ident prefix). */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFMAG  "\177ELF"
#define SELFMAG 4

/* e_ident[] index constants + their values (added for the Chromium port, M60-62;
 * needed by abseil's ELF symbolizer elf_mem_image.cc / vdso_support.cc). */
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6
#define EI_OSABI      7
#define EI_ABIVERSION 8
#define EI_PAD        9

#define ELFCLASSNONE 0
#define ELFCLASS32   1
#define ELFCLASS64   2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define EV_NONE    0
#define EV_CURRENT 1

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half e_type;
  Elf64_Half e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;
  Elf64_Off e_phoff;
  Elf64_Off e_shoff;
  Elf64_Word e_flags;
  Elf64_Half e_ehsize;
  Elf64_Half e_phentsize;
  Elf64_Half e_phnum;
  Elf64_Half e_shentsize;
  Elf64_Half e_shnum;
  Elf64_Half e_shstrndx;
} Elf64_Ehdr;

typedef struct {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
  Elf64_Word p_type;
  Elf64_Word p_flags;
  Elf64_Off p_offset;
  Elf64_Addr p_vaddr;
  Elf64_Addr p_paddr;
  Elf64_Xword p_filesz;
  Elf64_Xword p_memsz;
  Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
  Elf32_Word sh_name;
  Elf32_Word sh_type;
  Elf32_Word sh_flags;
  Elf32_Addr sh_addr;
  Elf32_Off sh_offset;
  Elf32_Word sh_size;
  Elf32_Word sh_link;
  Elf32_Word sh_info;
  Elf32_Word sh_addralign;
  Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
  Elf64_Word sh_name;
  Elf64_Word sh_type;
  Elf64_Xword sh_flags;
  Elf64_Addr sh_addr;
  Elf64_Off sh_offset;
  Elf64_Xword sh_size;
  Elf64_Word sh_link;
  Elf64_Word sh_info;
  Elf64_Xword sh_addralign;
  Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
  Elf32_Word st_name;
  Elf32_Addr st_value;
  Elf32_Word st_size;
  unsigned char st_info;
  unsigned char st_other;
  Elf32_Section st_shndx;
} Elf32_Sym;

typedef struct {
  Elf64_Word st_name;
  unsigned char st_info;
  unsigned char st_other;
  Elf64_Section st_shndx;
  Elf64_Addr st_value;
  Elf64_Xword st_size;
} Elf64_Sym;

/* Symbol versioning types + section-index constants (added for the Chromium
 * port, M60-62; standard System V gABI layout, needed by abseil's ELF
 * symbolizer elf_mem_image.h via ElfW(Verdef)/ElfW(Verdaux)/ElfW(Versym)). */
typedef Elf64_Half Elf64_Versym;
typedef Elf32_Half Elf32_Versym;

typedef struct {
  Elf64_Half    vd_version;
  Elf64_Half    vd_flags;
  Elf64_Half    vd_ndx;
  Elf64_Half    vd_cnt;
  Elf64_Word    vd_hash;
  Elf64_Word    vd_aux;
  Elf64_Word    vd_next;
} Elf64_Verdef;
typedef struct {
  Elf32_Half    vd_version;
  Elf32_Half    vd_flags;
  Elf32_Half    vd_ndx;
  Elf32_Half    vd_cnt;
  Elf32_Word    vd_hash;
  Elf32_Word    vd_aux;
  Elf32_Word    vd_next;
} Elf32_Verdef;

typedef struct {
  Elf64_Word    vda_name;
  Elf64_Word    vda_next;
} Elf64_Verdaux;
typedef struct {
  Elf32_Word    vda_name;
  Elf32_Word    vda_next;
} Elf32_Verdaux;

typedef struct {
  Elf64_Half    vn_version;
  Elf64_Half    vn_cnt;
  Elf64_Word    vn_file;
  Elf64_Word    vn_aux;
  Elf64_Word    vn_next;
} Elf64_Verneed;
typedef struct {
  Elf32_Half    vn_version;
  Elf32_Half    vn_cnt;
  Elf32_Word    vn_file;
  Elf32_Word    vn_aux;
  Elf32_Word    vn_next;
} Elf32_Verneed;

typedef struct {
  Elf64_Word    vna_hash;
  Elf64_Half    vna_flags;
  Elf64_Half    vna_other;
  Elf64_Word    vna_name;
  Elf64_Word    vna_next;
} Elf64_Vernaux;
typedef struct {
  Elf32_Word    vna_hash;
  Elf32_Half    vna_flags;
  Elf32_Half    vna_other;
  Elf32_Word    vna_name;
  Elf32_Word    vna_next;
} Elf32_Vernaux;

/* Special section indices. */
#define SHN_UNDEF     0
#define SHN_LORESERVE 0xff00
#define SHN_LOPROC    0xff00
#define SHN_HIPROC    0xff1f
#define SHN_ABS       0xfff1
#define SHN_COMMON    0xfff2
#define SHN_HIRESERVE 0xffff

typedef struct {
  Elf32_Addr r_offset;
  Elf32_Word r_info;
} Elf32_Rel;
typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
} Elf64_Rel;
typedef struct {
  Elf32_Addr r_offset;
  Elf32_Word r_info;
  Elf32_Sword r_addend;
} Elf32_Rela;
typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
  Elf64_Sxword r_addend;
} Elf64_Rela;

typedef struct {
  Elf32_Sword d_tag;
  union {
    Elf32_Word d_val;
    Elf32_Addr d_ptr;
  } d_un;
} Elf32_Dyn;
typedef struct {
  Elf64_Sxword d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr d_ptr;
  } d_un;
} Elf64_Dyn;

typedef struct {
  Elf32_Word n_namesz;
  Elf32_Word n_descsz;
  Elf32_Word n_type;
} Elf32_Nhdr;
typedef struct {
  Elf64_Word n_namesz;
  Elf64_Word n_descsz;
  Elf64_Word n_type;
} Elf64_Nhdr;

/* GNU note types (n_type in a "GNU"-named note). Chromium's elf_reader reads the
 * build-id note; the rest are the standard glibc set. */
#define NT_GNU_ABI_TAG          1
#define NT_GNU_HWCAP            2
#define NT_GNU_BUILD_ID         3
/* Core-file note types (ptrace register sets, used by crashpad's ptracer). */
#define NT_PRSTATUS    1
#define NT_PRFPREG     2
#define NT_PRPSINFO    3
#define NT_TASKSTRUCT  4
#define NT_AUXV        6
#define NT_PRXFPREG    0x46e62b7f
#define NT_X86_XSTATE  0x202
#define NT_ARM_VFP     0x400
#define NT_ARM_TLS     0x401
#define NT_GNU_GOLD_VERSION     4
#define NT_GNU_PROPERTY_TYPE_0  5

/* Auxiliary vector (the kernel hands this to _start; also at /proc/self/auxv). */
typedef struct {
  uint32_t a_type;
  union {
    uint32_t a_val;
  } a_un;
} Elf32_auxv_t;
typedef struct {
  uint64_t a_type;
  union {
    uint64_t a_val;
  } a_un;
} Elf64_auxv_t;

/* e_type */
#define ET_NONE 0
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define ET_CORE 4

/* e_machine (subset) */
#define EM_386 3
#define EM_X86_64 62
#define EM_AARCH64 183

/* p_type */
#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_PHDR 6
#define PT_TLS 7
#define PT_GNU_STACK 0x6474e551

/* d_tag — dynamic-section entry tags (added for the Chromium port, M60-62; values
 * are the standard System V / GNU ELF ABI ones, needed by abseil's ELF/VDSO
 * symbolizer). */
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_INIT     12
#define DT_FINI     13
#define DT_SONAME   14
#define DT_RPATH    15
#define DT_SYMBOLIC 16
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_DEBUG    21
#define DT_TEXTREL  22
#define DT_JMPREL   23
#define DT_BIND_NOW 24
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH  29
#define DT_FLAGS    30
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERSYM   0x6ffffff0
#define DT_RELACOUNT 0x6ffffff9
#define DT_RELCOUNT  0x6ffffffa
#define DT_FLAGS_1  0x6ffffffb
#define DT_VERDEF   0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEED  0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

/* p_flags */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* sh_type (subset) */
#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_DYNSYM 11

/* Symbol binding/type (st_info) */
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2

/* Relocation info accessors */
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)

/* Dynamic section tags (d_tag) — the subset the runtime loader consumes. */
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_RELENT 19
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_GNU_HASH 0x6ffffef5

/* x86-64 relocation types (subset applied by the runtime loader). */
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_IRELATIVE 37

/* Auxiliary vector types (subset) */
#define AT_NULL 0
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26

#endif /* _ELF_H */
