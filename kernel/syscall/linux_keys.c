/*
 * The key retention service: add_key(2), request_key(2), keyctl(2).
 *
 * Keys of type "user" and "logon" carry a payload; "keyring" keys hold links to
 * other keys. Every task reaches its thread, process and session keyrings and
 * its user's user and user-session keyrings by the special ids -1 to -5, and a
 * key is "possessed" when it can be found by searching from those. Permissions
 * follow Linux's 32-bit mask (possessor, user, group, other), and there is no
 * root bypass: CAP_SYS_ADMIN matters only where Linux says it does.
 *
 * Not here: request_key's upcall to /sbin/request-key (a request that finds
 * nothing answers ENOKEY), the big_key/asymmetric/encrypted types, and
 * persistent keyrings beyond the user keyring they alias.
 */
#include <b1nix/errno.h>
#include <b1nix/mm.h>
#include <b1nix/sched.h>
#include <b1nix/spinlock.h>
#include <b1nix/syscall.h>
#include <b1nix/types.h>
#include <b1nix/uidgid.h>
#include <stdio.h>
#include <string.h>

#include "linux_modern.h"

#define KEY_MAX           4096
#define KEY_DESC_MAX      4096
#define KEY_PAYLOAD_MAX   32767
#define KEYRING_MAX_LINKS 256

#define KEY_SPEC_THREAD_KEYRING       -1
#define KEY_SPEC_PROCESS_KEYRING      -2
#define KEY_SPEC_SESSION_KEYRING      -3
#define KEY_SPEC_USER_KEYRING         -4
#define KEY_SPEC_USER_SESSION_KEYRING -5
#define KEY_SPEC_GROUP_KEYRING        -6
#define KEY_SPEC_REQKEY_AUTH_KEY      -7

#define KEY_POS_VIEW    0x01000000
#define KEY_POS_READ    0x02000000
#define KEY_POS_WRITE   0x04000000
#define KEY_POS_SEARCH  0x08000000
#define KEY_POS_LINK    0x10000000
#define KEY_POS_SETATTR 0x20000000
#define KEY_POS_ALL     0x3f000000
#define KEY_USR_VIEW    0x00010000
#define KEY_USR_READ    0x00020000
#define KEY_USR_WRITE   0x00040000
#define KEY_USR_SEARCH  0x00080000
#define KEY_USR_LINK    0x00100000
#define KEY_USR_SETATTR 0x00200000
#define KEY_USR_ALL     0x003f0000
#define KEY_NEED_VIEW    0x01
#define KEY_NEED_READ    0x02
#define KEY_NEED_WRITE   0x04
#define KEY_NEED_SEARCH  0x08
#define KEY_NEED_LINK    0x10
#define KEY_NEED_SETATTR 0x20

enum key_type { KT_NONE, KT_KEYRING, KT_USER, KT_LOGON };

struct key {
  i32 serial;
  u8 type;
  u8 revoked;
  u8 invalidated;
  u32 uid;
  u32 gid;
  u32 perm;
  u64 expiry_ticks; /* 0: never */
  char *desc;
  u8 *payload;
  u32 plen;
  i32 *links;       /* keyring: members */
  u32 nlinks;
  u32 refs;         /* links from keyrings plus special-keyring bindings */
};

static struct key g_keys[KEY_MAX];
static i32 g_next_serial = 0x10000000;
static spinlock_t g_key_lock = SPINLOCK_INIT;

/* Special keyrings, bound lazily. The thread and process ones are keyed by the
 * task table row (the process one by the thread-group leader's row), the user
 * ones by uid. */
#define KEY_UIDS 64
static i32 *g_thread_kr;
static i32 *g_process_kr;
static i32 *g_session_kr;
static usize g_task_rows;
static struct { u32 uid; i32 user; i32 user_session; } g_user_kr[KEY_UIDS];

static struct key *key_by_serial(i32 serial) {
  if (serial <= 0)
    return 0;
  for (usize i = 0; i < KEY_MAX; i++)
    if (g_keys[i].type != KT_NONE && g_keys[i].serial == serial)
      return &g_keys[i];
  return 0;
}

static int key_expired(struct key *k) {
  return k->expiry_ticks && scheduler_get_ticks() >= k->expiry_ticks;
}

static void key_free(struct key *k) {
  if (k->desc)
    kfree(k->desc);
  if (k->payload) {
    memset(k->payload, 0, k->plen); /* secrets do not linger in the heap */
    kfree(k->payload);
  }
  if (k->links)
    kfree(k->links);
  memset(k, 0, sizeof(*k));
}

static void key_put(struct key *k) {
  if (k && k->refs && --k->refs == 0) {
    if (k->type == KT_KEYRING)
      for (u32 i = 0; i < k->nlinks; i++)
        key_put(key_by_serial(k->links[i]));
    key_free(k);
  }
}

static struct key *key_alloc(enum key_type type, const char *desc, u32 uid,
                             u32 gid, u32 perm) {
  for (usize i = 0; i < KEY_MAX; i++) {
    if (g_keys[i].type != KT_NONE)
      continue;
    struct key *k = &g_keys[i];
    usize dl = strlen(desc);

    memset(k, 0, sizeof(*k));
    k->desc = kmalloc(dl + 1);
    if (!k->desc)
      return 0;
    memcpy(k->desc, desc, dl + 1);
    k->serial = g_next_serial++;
    if (g_next_serial <= 0)
      g_next_serial = 0x10000000;
    k->type = (u8)type;
    k->uid = uid;
    k->gid = gid;
    k->perm = perm;
    return k;
  }
  return 0;
}

static int keyring_link(struct key *kr, struct key *k) {
  for (u32 i = 0; i < kr->nlinks; i++)
    if (kr->links[i] == k->serial)
      return 0;
  if (kr->nlinks >= KEYRING_MAX_LINKS)
    return -EDQUOT;
  i32 *nl = kmalloc((kr->nlinks + 1) * sizeof(i32));
  if (!nl)
    return -ENOMEM;
  if (kr->nlinks)
    memcpy(nl, kr->links, kr->nlinks * sizeof(i32));
  nl[kr->nlinks] = k->serial;
  if (kr->links)
    kfree(kr->links);
  kr->links = nl;
  kr->nlinks++;
  k->refs++;
  return 0;
}

static int keyring_unlink(struct key *kr, struct key *k) {
  for (u32 i = 0; i < kr->nlinks; i++) {
    if (kr->links[i] != k->serial)
      continue;
    kr->links[i] = kr->links[--kr->nlinks];
    key_put(k);
    return 0;
  }
  return -ENOENT;
}

/* ── the task's keyrings ─────────────────────────────────────────── */

static int key_tables(void) {
  if (g_thread_kr)
    return 0;
  usize n = scheduler_max_task_slots();
  g_thread_kr = kzalloc(n * sizeof(i32));
  g_process_kr = kzalloc(n * sizeof(i32));
  g_session_kr = kzalloc(n * sizeof(i32));
  if (!g_thread_kr || !g_process_kr || !g_session_kr)
    return -ENOMEM;
  g_task_rows = n;
  return 0;
}

static u32 cur_uid(void) {
  struct cred *c = scheduler_get_current_cred();
  return c ? c->fsuid : 0;
}

static u32 cur_gid(void) {
  struct cred *c = scheduler_get_current_cred();
  return c ? c->fsgid : 0;
}

static usize cur_row(void) { return scheduler_task_index(current_task); }

static usize cur_process_row(void) {
  struct task *leader = scheduler_task_by_pid(task_tgid(current_task));
  return leader ? scheduler_task_index(leader) : cur_row();
}

static i32 new_keyring(const char *name, u32 uid, u32 gid) {
  struct key *k = key_alloc(KT_KEYRING, name, uid, gid,
                            KEY_POS_ALL | KEY_USR_VIEW | KEY_USR_READ |
                                KEY_USR_SEARCH | KEY_USR_LINK);

  if (!k)
    return -ENOMEM;
  k->refs = 1; /* the binding */
  return k->serial;
}

static int user_slot(u32 uid) {
  int free_slot = -1;

  for (int i = 0; i < KEY_UIDS; i++) {
    if (g_user_kr[i].user && g_user_kr[i].uid == uid)
      return i;
    if (!g_user_kr[i].user && free_slot < 0)
      free_slot = i;
  }
  return free_slot;
}

/* Resolve a special id to the keyring it names (creating it when asked), or
 * pass a plain serial through. */
static i32 key_resolve(i32 id, int create) {
  if (id > 0)
    return id;
  if (key_tables())
    return -ENOMEM;
  usize row;
  switch (id) {
  case KEY_SPEC_THREAD_KEYRING:
    row = cur_row();
    if (row >= g_task_rows)
      return -ENOKEY;
    if (!key_by_serial(g_thread_kr[row]) && create)
      g_thread_kr[row] = new_keyring("_tid", cur_uid(), cur_gid());
    return key_by_serial(g_thread_kr[row]) ? g_thread_kr[row] : -ENOKEY;
  case KEY_SPEC_PROCESS_KEYRING:
    row = cur_process_row();
    if (row >= g_task_rows)
      return -ENOKEY;
    if (!key_by_serial(g_process_kr[row]) && create)
      g_process_kr[row] = new_keyring("_pid", cur_uid(), cur_gid());
    return key_by_serial(g_process_kr[row]) ? g_process_kr[row] : -ENOKEY;
  case KEY_SPEC_SESSION_KEYRING:
    row = cur_process_row();
    if (row >= g_task_rows)
      return -ENOKEY;
    if (key_by_serial(g_session_kr[row]))
      return g_session_kr[row];
    /* No session keyring: the user-session keyring stands in, as on Linux. */
    if (!create)
      return key_resolve(KEY_SPEC_USER_SESSION_KEYRING, 1);
    g_session_kr[row] = new_keyring("_ses", cur_uid(), cur_gid());
    return g_session_kr[row] > 0 ? g_session_kr[row] : -ENOMEM;
  case KEY_SPEC_USER_KEYRING:
  case KEY_SPEC_USER_SESSION_KEYRING: {
    int s = user_slot(cur_uid());

    if (s < 0)
      return -EDQUOT;
    if (!g_user_kr[s].user) {
      char name[32];

      g_user_kr[s].uid = cur_uid();
      snprintf(name, sizeof(name), "_uid.%u", cur_uid());
      g_user_kr[s].user = new_keyring(name, cur_uid(), cur_gid());
      snprintf(name, sizeof(name), "_uid_ses.%u", cur_uid());
      g_user_kr[s].user_session = new_keyring(name, cur_uid(), cur_gid());
      /* The user-session keyring links the user keyring. */
      struct key *us = key_by_serial(g_user_kr[s].user_session);
      struct key *u = key_by_serial(g_user_kr[s].user);
      if (us && u)
        keyring_link(us, u);
    }
    return id == KEY_SPEC_USER_KEYRING ? g_user_kr[s].user
                                       : g_user_kr[s].user_session;
  }
  case KEY_SPEC_GROUP_KEYRING:
    return -EINVAL;
  case KEY_SPEC_REQKEY_AUTH_KEY:
    return -ENOKEY;
  default:
    return -EINVAL;
  }
}

/* Is `target` reachable from keyring `from` through searchable keyrings? */
static int key_reachable(i32 from, i32 target, int depth) {
  struct key *kr = key_by_serial(from);

  if (!kr || depth > 6)
    return 0;
  if (from == target)
    return 1;
  if (kr->type != KT_KEYRING || kr->revoked || key_expired(kr))
    return 0;
  for (u32 i = 0; i < kr->nlinks; i++)
    if (key_reachable(kr->links[i], target, depth + 1))
      return 1;
  return 0;
}

static int key_possessed(i32 serial) {
  static const i32 roots[] = {KEY_SPEC_THREAD_KEYRING, KEY_SPEC_PROCESS_KEYRING,
                              KEY_SPEC_SESSION_KEYRING};
  for (usize i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    i32 r = key_resolve(roots[i], 0);

    if (r > 0 && key_reachable(r, serial, 0))
      return 1;
  }
  return 0;
}

static int key_permitted(struct key *k, u32 need) {
  u32 perm = 0;

  if (key_possessed(k->serial))
    perm |= (k->perm >> 24) & 0x3f;
  if (k->uid == cur_uid())
    perm |= (k->perm >> 16) & 0x3f;
  else if (k->gid == cur_gid())
    perm |= (k->perm >> 8) & 0x3f;
  else
    perm |= k->perm & 0x3f;
  return (perm & need) == need;
}

/* Look up a key for an operation: resolve, find, check liveness and access. */
static struct key *key_lookup(i32 id, int create, u32 need, isize *err) {
  i32 serial = key_resolve(id, create);
  struct key *k;

  *err = 0;
  if (serial < 0) {
    *err = serial;
    return 0;
  }
  k = key_by_serial(serial);
  if (!k) {
    *err = -ENOKEY;
    return 0;
  }
  if (k->revoked) {
    *err = -EKEYREVOKED;
    return 0;
  }
  if (k->invalidated || key_expired(k)) {
    *err = -EKEYEXPIRED;
    return 0;
  }
  if (need && !key_permitted(k, need)) {
    *err = -EACCES;
    return 0;
  }
  return k;
}

static int type_from_user(u64 utype, enum key_type *out) {
  char t[32];

  if (syscall_copyinstr(t, sizeof(t), (const char *)(usize)utype) < 0)
    return -EFAULT;
  if (!strcmp(t, "keyring"))
    *out = KT_KEYRING;
  else if (!strcmp(t, "user"))
    *out = KT_USER;
  else if (!strcmp(t, "logon"))
    *out = KT_LOGON;
  else if (t[0] == '.')
    return -EPERM; /* internal types cannot be created from userspace */
  else
    return -ENODEV;
  return 0;
}

static const char *type_name(u8 t) {
  return t == KT_KEYRING ? "keyring" : t == KT_USER ? "user" : "logon";
}

static struct key *keyring_find(struct key *kr, u8 type, const char *desc) {
  for (u32 i = 0; i < kr->nlinks; i++) {
    struct key *k = key_by_serial(kr->links[i]);

    if (k && k->type == type && !strcmp(k->desc, desc))
      return k;
  }
  return 0;
}

static int set_payload(struct key *k, const u8 *p, u32 len) {
  u8 *np = 0;

  if (len) {
    np = kmalloc(len);
    if (!np)
      return -ENOMEM;
    memcpy(np, p, len);
  }
  if (k->payload) {
    memset(k->payload, 0, k->plen);
    kfree(k->payload);
  }
  k->payload = np;
  k->plen = len;
  return 0;
}

/* ── add_key / request_key ───────────────────────────────────────── */

static isize sys_add_key(u64 utype, u64 udesc, u64 upay, u64 plen, u64 ring) {
  enum key_type type;
  char desc[256];
  u8 *payload = 0;
  isize err;

  int rc = type_from_user(utype, &type);
  if (rc)
    return rc;
  if (!udesc)
    return -EINVAL;
  if (syscall_copyinstr(desc, sizeof(desc), (const char *)(usize)udesc) < 0)
    return -EFAULT;
  if (!desc[0])
    return -EINVAL;
  if (type == KT_LOGON && !strchr(desc, ':'))
    return -EINVAL; /* logon descriptions are "service:name" */
  if (plen > KEY_PAYLOAD_MAX || (type == KT_KEYRING && plen))
    return -EINVAL;
  if (type != KT_KEYRING && plen == 0 && type == KT_LOGON)
    return -EINVAL;
  if (plen) {
    payload = kmalloc((usize)plen);
    if (!payload)
      return -ENOMEM;
    if (syscall_copyin(payload, (const void *)(usize)upay, (usize)plen)) {
      kfree(payload);
      return -EFAULT;
    }
  }

  u64 f;
  spin_lock_irqsave(&g_key_lock, &f);
  struct key *kr = key_lookup((i32)ring, 1, KEY_NEED_WRITE, &err);
  isize ret;
  if (!kr) {
    ret = err;
  } else if (kr->type != KT_KEYRING) {
    ret = -ENOTDIR;
  } else {
    struct key *k = keyring_find(kr, (u8)type, desc);

    if (k && type != KT_KEYRING) {
      /* Same type and description in that keyring: update it. */
      ret = key_permitted(k, KEY_NEED_WRITE)
                ? (set_payload(k, payload, (u32)plen) ?: k->serial)
                : -EACCES;
    } else if (k) {
      ret = k->serial;
    } else {
      k = key_alloc(type, desc, cur_uid(), cur_gid(),
                    KEY_POS_ALL | KEY_USR_VIEW |
                        (type == KT_LOGON ? 0 : KEY_USR_READ) | KEY_USR_WRITE |
                        KEY_USR_SEARCH | KEY_USR_LINK | KEY_USR_SETATTR);
      if (!k) {
        ret = -ENOMEM;
      } else if ((ret = set_payload(k, payload, (u32)plen)) == 0 &&
                 (ret = keyring_link(kr, k)) == 0) {
        ret = k->serial;
      } else {
        key_free(k);
      }
    }
  }
  spin_unlock_irqrestore(&g_key_lock, f);
  if (payload) {
    memset(payload, 0, (usize)plen);
    kfree(payload);
  }
  return ret;
}

static struct key *key_search(i32 root, u8 type, const char *desc, int depth) {
  struct key *kr = key_by_serial(root);

  if (!kr || kr->type != KT_KEYRING || depth > 6 || kr->revoked ||
      !key_permitted(kr, KEY_NEED_SEARCH))
    return 0;
  for (u32 i = 0; i < kr->nlinks; i++) {
    struct key *k = key_by_serial(kr->links[i]);

    if (k && k->type == type && !strcmp(k->desc, desc) && !k->revoked &&
        !key_expired(k) && key_permitted(k, KEY_NEED_SEARCH))
      return k;
  }
  for (u32 i = 0; i < kr->nlinks; i++) {
    struct key *k = key_by_serial(kr->links[i]);

    if (k && k->type == KT_KEYRING) {
      struct key *hit = key_search(k->serial, type, desc, depth + 1);

      if (hit)
        return hit;
    }
  }
  return 0;
}

static isize do_request(u8 type, const char *desc, i32 dest) {
  static const i32 roots[] = {KEY_SPEC_THREAD_KEYRING, KEY_SPEC_PROCESS_KEYRING,
                              KEY_SPEC_SESSION_KEYRING};
  struct key *hit = 0;

  for (usize i = 0; i < 3 && !hit; i++) {
    i32 r = key_resolve(roots[i], 0);

    if (r > 0)
      hit = key_search(r, type, desc, 0);
  }
  if (!hit)
    return -ENOKEY;
  if (dest) {
    isize err;
    struct key *d = key_lookup(dest, 1, KEY_NEED_WRITE, &err);

    if (!d)
      return err;
    int rc = keyring_link(d, hit);
    if (rc)
      return rc;
  }
  return hit->serial;
}

static isize sys_request_key(u64 utype, u64 udesc, u64 ucallout, u64 dest) {
  enum key_type type;
  char desc[256];

  (void)ucallout; /* no /sbin/request-key upcall: a miss is ENOKEY */
  int rc = type_from_user(utype, &type);
  if (rc)
    return rc;
  if (syscall_copyinstr(desc, sizeof(desc), (const char *)(usize)udesc) < 0)
    return -EFAULT;
  u64 f;
  spin_lock_irqsave(&g_key_lock, &f);
  isize r = do_request((u8)type, desc, (i32)dest);
  spin_unlock_irqrestore(&g_key_lock, f);
  return r;
}

/* ── keyctl ──────────────────────────────────────────────────────── */

#define KEYCTL_GET_KEYRING_ID       0
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_UPDATE               2
#define KEYCTL_REVOKE               3
#define KEYCTL_CHOWN                4
#define KEYCTL_SETPERM              5
#define KEYCTL_DESCRIBE             6
#define KEYCTL_CLEAR                7
#define KEYCTL_LINK                 8
#define KEYCTL_UNLINK               9
#define KEYCTL_SEARCH               10
#define KEYCTL_READ                 11
#define KEYCTL_INSTANTIATE          12
#define KEYCTL_NEGATE               13
#define KEYCTL_SET_REQKEY_KEYRING   14
#define KEYCTL_SET_TIMEOUT          15
#define KEYCTL_ASSUME_AUTHORITY     16
#define KEYCTL_GET_SECURITY         17
#define KEYCTL_SESSION_TO_PARENT    18
#define KEYCTL_REJECT               19
#define KEYCTL_INVALIDATE           21
#define KEYCTL_GET_PERSISTENT       22
#define KEYCTL_CAPABILITIES         31

static isize copy_text_out(const char *text, u64 ubuf, u64 buflen) {
  usize n = strlen(text) + 1;

  if (ubuf && buflen >= n &&
      syscall_copyout((void *)(usize)ubuf, text, n))
    return -EFAULT;
  return (isize)n;
}

static isize keyctl_locked(u64 op, u64 a1, u64 a2, u64 a3, u64 a4) {
  isize err;
  struct key *k;

  switch (op) {
  case KEYCTL_GET_KEYRING_ID: {
    i32 s = key_resolve((i32)a1, a2 != 0);

    if (s < 0)
      return s;
    return key_by_serial(s) ? s : -ENOKEY;
  }
  case KEYCTL_JOIN_SESSION_KEYRING: {
    char name[256] = "_ses";
    usize row = cur_process_row();

    if (a1 && syscall_copyinstr(name, sizeof(name), (const char *)(usize)a1) < 0)
      return -EFAULT;
    if (key_tables() || row >= g_task_rows)
      return -ENOMEM;
    i32 s = new_keyring(name, cur_uid(), cur_gid());
    if (s < 0)
      return s;
    struct key *old = key_by_serial(g_session_kr[row]);
    if (old)
      key_put(old);
    g_session_kr[row] = s;
    return s;
  }
  case KEYCTL_UPDATE: {
    k = key_lookup((i32)a1, 0, KEY_NEED_WRITE, &err);
    if (!k)
      return err;
    if (k->type == KT_KEYRING)
      return -EOPNOTSUPP;
    if (a3 > KEY_PAYLOAD_MAX)
      return -EINVAL;
    u8 tmp[256];
    u8 *p = a3 <= sizeof(tmp) ? tmp : kmalloc((usize)a3);
    if (!p)
      return -ENOMEM;
    int rc = a3 && syscall_copyin(p, (const void *)(usize)a2, (usize)a3) ? -EFAULT : 0;
    if (!rc)
      rc = set_payload(k, p, (u32)a3);
    memset(p, 0, (usize)a3);
    if (p != tmp)
      kfree(p);
    return rc;
  }
  case KEYCTL_REVOKE:
  case KEYCTL_INVALIDATE:
    k = key_lookup((i32)a1, 0,
                   op == KEYCTL_REVOKE ? (KEY_NEED_WRITE | KEY_NEED_SETATTR)
                                       : KEY_NEED_SEARCH, &err);
    if (!k)
      return err;
    if (op == KEYCTL_REVOKE)
      k->revoked = 1;
    else
      k->invalidated = 1;
    return 0;
  case KEYCTL_CHOWN: {
    struct cred *c = scheduler_get_current_cred();

    k = key_lookup((i32)a1, 0, KEY_NEED_SETATTR, &err);
    if (!k)
      return err;
    if ((i32)a2 != -1 && (u32)a2 != k->uid &&
        !(c && cred_has_cap(c, CAP_SYS_ADMIN)))
      return -EACCES;
    if ((i32)a3 != -1 && (u32)a3 != k->gid && k->uid != cur_uid() &&
        !(c && cred_has_cap(c, CAP_SYS_ADMIN)))
      return -EACCES;
    if ((i32)a2 != -1)
      k->uid = (u32)a2;
    if ((i32)a3 != -1)
      k->gid = (u32)a3;
    return 0;
  }
  case KEYCTL_SETPERM:
    k = key_lookup((i32)a1, 0, KEY_NEED_SETATTR, &err);
    if (!k)
      return err;
    if ((u32)a2 & ~(u32)0x3f3f3f3f)
      return -EINVAL;
    if (k->uid != cur_uid())
      return -EACCES;
    k->perm = (u32)a2;
    return 0;
  case KEYCTL_DESCRIBE: {
    char text[512];

    k = key_lookup((i32)a1, 0, KEY_NEED_VIEW, &err);
    if (!k)
      return err;
    snprintf(text, sizeof(text), "%s;%u;%u;%08x;%s", type_name(k->type),
             k->uid, k->gid, k->perm, k->desc);
    return copy_text_out(text, a2, a3);
  }
  case KEYCTL_CLEAR:
    k = key_lookup((i32)a1, 0, KEY_NEED_WRITE, &err);
    if (!k)
      return err;
    if (k->type != KT_KEYRING)
      return -ENOTDIR;
    while (k->nlinks)
      keyring_unlink(k, key_by_serial(k->links[0]) ?: k);
    return 0;
  case KEYCTL_LINK:
  case KEYCTL_UNLINK: {
    struct key *kr;

    k = key_lookup((i32)a1, 0, op == KEYCTL_LINK ? KEY_NEED_LINK : 0, &err);
    if (!k && op == KEYCTL_LINK)
      return err;
    if (!k && !(k = key_by_serial(key_resolve((i32)a1, 0))))
      return -ENOKEY;
    kr = key_lookup((i32)a2, op == KEYCTL_LINK, KEY_NEED_WRITE, &err);
    if (!kr)
      return err;
    if (kr->type != KT_KEYRING)
      return -ENOTDIR;
    if (op == KEYCTL_UNLINK)
      return keyring_unlink(kr, k);
    /* A keyring may not come to contain itself. */
    if (k->type == KT_KEYRING && key_reachable(k->serial, kr->serial, 0))
      return -EDEADLK;
    return keyring_link(kr, k);
  }
  case KEYCTL_SEARCH: {
    enum key_type type;
    char desc[256];
    struct key *kr = key_lookup((i32)a1, 0, KEY_NEED_SEARCH, &err);

    if (!kr)
      return err;
    if (kr->type != KT_KEYRING)
      return -ENOTDIR;
    int rc = type_from_user(a2, &type);
    if (rc)
      return rc;
    if (syscall_copyinstr(desc, sizeof(desc), (const char *)(usize)a3) < 0)
      return -EFAULT;
    k = key_search(kr->serial, (u8)type, desc, 0);
    if (!k)
      return -ENOKEY;
    if (a4) {
      struct key *d = key_lookup((i32)a4, 1, KEY_NEED_WRITE, &err);

      if (!d)
        return err;
      rc = keyring_link(d, k);
      if (rc)
        return rc;
    }
    return k->serial;
  }
  case KEYCTL_READ:
    k = key_lookup((i32)a1, 0, 0, &err);
    if (!k)
      return err;
    /* READ is granted by read permission, or by search permission to a key
     * this task possesses -- Linux's rule. A logon key is never readable. */
    if (k->type == KT_LOGON)
      return -EOPNOTSUPP;
    if (!key_permitted(k, KEY_NEED_READ) &&
        !(key_possessed(k->serial) && key_permitted(k, KEY_NEED_SEARCH)))
      return -EACCES;
    if (k->type == KT_KEYRING) {
      u64 need = (u64)k->nlinks * sizeof(i32);

      if (a2 && a3 >= need && need &&
          syscall_copyout((void *)(usize)a2, k->links, (usize)need))
        return -EFAULT;
      return (isize)need;
    }
    if (a2 && a3 && k->plen &&
        syscall_copyout((void *)(usize)a2, k->payload,
                        a3 < k->plen ? (usize)a3 : k->plen))
      return -EFAULT;
    return (isize)k->plen;
  case KEYCTL_SET_TIMEOUT:
    k = key_lookup((i32)a1, 0, KEY_NEED_SETATTR, &err);
    if (!k)
      return err;
    k->expiry_ticks = a2 ? scheduler_get_ticks() +
                               a2 * (u64)sched_tick_hz() : 0;
    return 0;
  case KEYCTL_GET_SECURITY:
    k = key_lookup((i32)a1, 0, KEY_NEED_VIEW, &err);
    if (!k)
      return err;
    return copy_text_out("", a2, a3); /* no LSM labels keys */
  case KEYCTL_SET_REQKEY_KEYRING:
    /* The keyring request_key links into by default; only the default is
     * modelled, which this answers as the previous setting. */
    if ((i64)a1 < -1 || (i64)a1 > 7)
      return -EINVAL;
    return 0;
  case KEYCTL_GET_PERSISTENT:
    if ((i32)a1 != -1 && (u32)a1 != cur_uid()) {
      struct cred *c = scheduler_get_current_cred();

      if (!(c && cred_has_cap(c, CAP_SETUID)))
        return -EPERM;
    }
    return key_resolve(KEY_SPEC_USER_KEYRING, 1);
  case KEYCTL_INSTANTIATE:
  case KEYCTL_NEGATE:
  case KEYCTL_REJECT:
  case KEYCTL_ASSUME_AUTHORITY:
    /* Only an upcall holds the authorisation these need, and there is none. */
    return -EPERM;
  case KEYCTL_SESSION_TO_PARENT:
    return -EPERM;
  case KEYCTL_CAPABILITIES: {
    u8 caps[2] = {0x01 /* KEYCTL_CAPS0_CAPABILITIES */ |
                      0x02 /* PERSISTENT_KEYRINGS */ |
                      0x10 /* INVALIDATE */,
                  0};
    usize n = a2 < sizeof(caps) ? (usize)a2 : sizeof(caps);

    if (a1 && n && syscall_copyout((void *)(usize)a1, caps, n))
      return -EFAULT;
    return sizeof(caps);
  }
  default:
    return -EOPNOTSUPP;
  }
}

static isize sys_keyctl(u64 op, u64 a1, u64 a2, u64 a3, u64 a4) {
  u64 f;

  spin_lock_irqsave(&g_key_lock, &f);
  isize r = keyctl_locked(op, a1, a2, a3, a4);
  spin_unlock_irqrestore(&g_key_lock, f);
  return r;
}

/* A task leaving the table drops its thread (and, for a leader, process)
 * keyring binding; a forked child shares its parent's session keyring. */
void linux_keys_task_reset(usize row) {
  if (!g_thread_kr || row >= g_task_rows)
    return;
  u64 f;
  spin_lock_irqsave(&g_key_lock, &f);
  key_put(key_by_serial(g_thread_kr[row]));
  key_put(key_by_serial(g_process_kr[row]));
  key_put(key_by_serial(g_session_kr[row]));
  g_thread_kr[row] = g_process_kr[row] = g_session_kr[row] = 0;
  spin_unlock_irqrestore(&g_key_lock, f);
}

void linux_keys_fork_inherit(usize parent_row, usize child_row) {
  if (!g_thread_kr || parent_row >= g_task_rows || child_row >= g_task_rows)
    return;
  u64 f;
  spin_lock_irqsave(&g_key_lock, &f);
  struct key *s = key_by_serial(g_session_kr[parent_row]);
  g_thread_kr[child_row] = g_process_kr[child_row] = 0;
  g_session_kr[child_row] = s ? s->serial : 0;
  if (s)
    s->refs++;
  spin_unlock_irqrestore(&g_key_lock, f);
}

#if defined(__aarch64__)
#define LK_add_key     217
#define LK_request_key 218
#define LK_keyctl      219
#else
#define LK_add_key     248
#define LK_request_key 249
#define LK_keyctl      250
#endif

int linux_keys_syscall(u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
                       u64 *ret) {
  isize r;

  switch (nr) {
  case LK_add_key:
    r = sys_add_key(a0, a1, a2, a3, a4);
    break;
  case LK_request_key:
    r = sys_request_key(a0, a1, a2, a3);
    break;
  case LK_keyctl:
    r = sys_keyctl(a0, a1, a2, a3, a4);
    break;
  default:
    return 0;
  }
  *ret = (u64)r;
  return 1;
}
