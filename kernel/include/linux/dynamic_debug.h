/* SPDX-License-Identifier: MIT */
#ifndef LKPI_LINUX_DYNAMIC_DEBUG_H
#define LKPI_LINUX_DYNAMIC_DEBUG_H
/* Linux's runtime-switchable debug printing. b1nix has no control file to
 * switch them from, so the call sites compile to nothing — which is what they
 * do on Linux too until someone enables them. */
#define DECLARE_DYNDBG_CLASSMAP(...)
#define DYNAMIC_DEBUG_BRANCH(descriptor) 0
#endif
