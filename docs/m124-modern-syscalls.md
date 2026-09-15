# M124: system calls Linux added later

Implemented in `kernel/syscall/linux_modern.c`, `kernel/syscall/linux_keys.c`
and `kernel/fs/landlock.c`. Each has a probe in the Debian lane
(`tools/images/debian-stage.sh`, stage 12) that checks results and errno
values, not merely the absence of ENOSYS.

| Call | State | Behaviour |
|---|---|---|
| `openat2` | done | `RESOLVE_NO_XDEV`, `NO_MAGICLINKS`, `NO_SYMLINKS`, `BENEATH`, `IN_ROOT`, `CACHED`; `open_how` size and flag validation. Resolution walks components itself. |
| `pidfd_getfd` | done | ptrace access check; the new descriptor is close-on-exec. |
| `kcmp` | done | FILE, VM, FILES; FS/IO/SYSVSEM/SIGHAND by thread group (the sharing CLONE_THREAD implies). EPOLL_TFD answers EOPNOTSUPP. |
| `futex_wait`, `futex_wake`, `futex_waitv` | done | 32-bit futexes (the only size Linux implements), private flag, absolute timeouts on REALTIME/MONOTONIC; waitv returns the index woken. Bitset masks are not matched (as the existing FUTEX_WAIT_BITSET). |
| `process_madvise` | done | COLD, PAGEOUT, WILLNEED, COLLAPSE accepted as hints; CAP_SYS_NICE for another process. |
| `process_mrelease` | done | EINVAL unless the target is being killed. |
| `sched_setattr`, `sched_getattr` | done | SCHED_OTHER with nice; other policies and util clamps refused, E2BIG size negotiation. |
| `mbind`, `get_mempolicy`, `set_mempolicy`, `set_mempolicy_home_node` | done | Single-node semantics: masks naming other nodes are EINVAL, policy stored per task and inherited. |
| `pkey_alloc`, `pkey_free`, `pkey_mprotect` | partial | Linux's answers on a CPU without protection keys (ENOSPC, EINVAL, key -1 = mprotect). A CPU with PKU gets the same answers; keys are not modelled. |
| `cachestat` | done | Cached and dirty page counts over a range. |
| `remap_file_pages` | done | Linux's emulation: remaps the file again over a shared file mapping. |
| `statmount`, `listmount` | done | 64-bit never-reused mount ids (also `STATX_MNT_ID_UNIQUE`); SB_BASIC, MNT_BASIC, MNT_ROOT, MNT_POINT, FS_TYPE, MNT_OPTS, SB_SOURCE, NS_ID; recursive listing, reverse order. |
| `add_key`, `request_key`, `keyctl` | done | keyring/user/logon types, special keyrings -1..-5, possession and permission masks, read/describe/update/revoke/invalidate/link/unlink/search/clear/chown/setperm/set_timeout. No request-key upcall: a miss is ENOKEY. |
| Landlock | done | ABI 3 filesystem rights with layered rulesets, enforced on open, create, remove, rename/link (REFER), truncate and exec; canonical paths so symlinks cannot escape. Network rules (ABI 4) refused. |
| `quotactl`, `quotactl_fd` | partial | Target, type and permission validation as Linux; the answer is ENOSYS because no filesystem here implements quotas. |
| `memfd_secret` | planned | Needs pages removed from the kernel direct map, which has no attribute-changing API yet. |

The unmapped-syscall log line is checked through the KDE smoke run.
