#ifndef _B1NIX_ASM_PRCTL_H
#define _B1NIX_ASM_PRCTL_H

/* Stub for LLVM exegesis — b1nix does not use Linux prctl, but the LLVM
   cross-build compiles exegesis unconditionally and its X86 Target.cpp
   includes <asm/prctl.h>. Provide the defines it needs. */
#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1011
#endif
#ifndef ARCH_SET_GS
#define ARCH_SET_GS 0x1012
#endif
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif

#endif
