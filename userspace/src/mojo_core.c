/* B1NIX Mojo Core Implementation
 * Implements minimal Mojo IPC core over M56/M57 primitives (epoll, eventfd,
 * memfd, socketpair, SCM_RIGHTS).
 */
#include <mojo/mojo.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

typedef enum {
  MOJO_HANDLE_TYPE_NONE = 0,
  MOJO_HANDLE_TYPE_MESSAGE_PIPE,
  MOJO_HANDLE_TYPE_SHARED_BUFFER,
  MOJO_HANDLE_TYPE_WATCHER,
  MOJO_HANDLE_TYPE_PLATFORM_FD,
} MojoHandleType;

struct MojoHandleEntry {
  MojoHandle handle;
  MojoHandleType type;
  int fd;
  uint64_t size;
};

#define MAX_MOJO_HANDLES 1024
static struct MojoHandleEntry g_handles[MAX_MOJO_HANDLES];
static MojoHandle g_next_handle = 1;
static pthread_mutex_t g_mojo_lock = PTHREAD_MUTEX_INITIALIZER;

/* Track mapped buffer regions for MojoUnmapBuffer */
struct MojoMappingEntry {
  void *addr;
  size_t length;
};
#define MAX_MOJO_MAPPINGS 256
static struct MojoMappingEntry g_mappings[MAX_MOJO_MAPPINGS];

static MojoHandle alloc_handle_entry(MojoHandleType type, int fd, uint64_t size) {
  pthread_mutex_lock(&g_mojo_lock);
  for (int i = 0; i < MAX_MOJO_HANDLES; i++) {
    if (g_handles[i].type == MOJO_HANDLE_TYPE_NONE) {
      MojoHandle h = g_next_handle++;
      if (g_next_handle == 0) g_next_handle = 1;
      g_handles[i].handle = h;
      g_handles[i].type = type;
      g_handles[i].fd = fd;
      g_handles[i].size = size;
      pthread_mutex_unlock(&g_mojo_lock);
      return h;
    }
  }
  pthread_mutex_unlock(&g_mojo_lock);
  return MOJO_HANDLE_INVALID;
}

static int get_handle_entry(MojoHandle handle, struct MojoHandleEntry *out) {
  if (handle == MOJO_HANDLE_INVALID) return -1;
  pthread_mutex_lock(&g_mojo_lock);
  for (int i = 0; i < MAX_MOJO_HANDLES; i++) {
    if (g_handles[i].handle == handle && g_handles[i].type != MOJO_HANDLE_TYPE_NONE) {
      if (out) *out = g_handles[i];
      pthread_mutex_unlock(&g_mojo_lock);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_mojo_lock);
  return -1;
}

static int remove_handle_entry(MojoHandle handle, struct MojoHandleEntry *out) {
  if (handle == MOJO_HANDLE_INVALID) return -1;
  pthread_mutex_lock(&g_mojo_lock);
  for (int i = 0; i < MAX_MOJO_HANDLES; i++) {
    if (g_handles[i].handle == handle && g_handles[i].type != MOJO_HANDLE_TYPE_NONE) {
      if (out) *out = g_handles[i];
      memset(&g_handles[i], 0, sizeof(struct MojoHandleEntry));
      pthread_mutex_unlock(&g_mojo_lock);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_mojo_lock);
  return -1;
}

MojoResult MojoClose(MojoHandle handle) {
  struct MojoHandleEntry entry;
  if (remove_handle_entry(handle, &entry) < 0) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }
  if (entry.fd >= 0) {
    close(entry.fd);
  }
  return MOJO_RESULT_OK;
}

MojoResult MojoCreateMessagePipe(
    const struct MojoCreateMessagePipeOptions *options,
    MojoHandle *message_pipe_handle0, MojoHandle *message_pipe_handle1) {
  (void)options;
  if (!message_pipe_handle0 || !message_pipe_handle1) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }
  fcntl(sv[0], F_SETFL, O_NONBLOCK);
  fcntl(sv[0], F_SETFD, FD_CLOEXEC);
  fcntl(sv[1], F_SETFL, O_NONBLOCK);
  fcntl(sv[1], F_SETFD, FD_CLOEXEC);

  MojoHandle h0 = alloc_handle_entry(MOJO_HANDLE_TYPE_MESSAGE_PIPE, sv[0], 0);
  MojoHandle h1 = alloc_handle_entry(MOJO_HANDLE_TYPE_MESSAGE_PIPE, sv[1], 0);

  if (h0 == MOJO_HANDLE_INVALID || h1 == MOJO_HANDLE_INVALID) {
    if (h0 != MOJO_HANDLE_INVALID) MojoClose(h0);
    if (h1 != MOJO_HANDLE_INVALID) MojoClose(h1);
    close(sv[0]);
    close(sv[1]);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *message_pipe_handle0 = h0;
  *message_pipe_handle1 = h1;
  return MOJO_RESULT_OK;
}

#define MOJO_FRAME_MAGIC 0x4D4F4A4FU /* "MOJO" */

struct MojoFrameHeader {
  uint32_t magic;
  uint32_t payload_size;
  uint32_t handle_count;
  uint32_t flags;
};

MojoResult MojoWriteMessage(MojoHandle handle, const void *bytes,
                             uint32_t num_bytes, const MojoHandle *handles,
                             uint32_t num_handles, MojoWriteMessageFlags flags) {
  (void)flags;
  struct MojoHandleEntry entry;
  if (get_handle_entry(handle, &entry) < 0 ||
      (entry.type != MOJO_HANDLE_TYPE_MESSAGE_PIPE &&
       entry.type != MOJO_HANDLE_TYPE_PLATFORM_FD)) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  if (num_handles > 16) {
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  int fds_to_pass[16];
  struct MojoHandleEntry attached_entries[16];

  for (uint32_t i = 0; i < num_handles; i++) {
    if (get_handle_entry(handles[i], &attached_entries[i]) < 0) {
      return MOJO_RESULT_INVALID_ARGUMENT;
    }
    fds_to_pass[i] = attached_entries[i].fd;
  }

  struct MojoFrameHeader hdr;
  hdr.magic = MOJO_FRAME_MAGIC;
  hdr.payload_size = num_bytes;
  hdr.handle_count = num_handles;
  hdr.flags = 0;

  struct iovec iov[2];
  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = (void *)bytes;
  iov[1].iov_len = num_bytes;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = (num_bytes > 0) ? 2 : 1;

  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(16 * sizeof(int))];
  } cmsg_un;

  if (num_handles > 0) {
    memset(&cmsg_un, 0, sizeof(cmsg_un));
    msg.msg_control = cmsg_un.buf;
    msg.msg_controllen = CMSG_SPACE(num_handles * sizeof(int));
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(num_handles * sizeof(int));
    memcpy(CMSG_DATA(c), fds_to_pass, num_handles * sizeof(int));
  }

  ssize_t ret = sendmsg(entry.fd, &msg, 0);
  if (ret < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MOJO_RESULT_SHOULD_WAIT;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
      return MOJO_RESULT_FAILED_PRECONDITION;
    }
    return MOJO_RESULT_UNKNOWN;
  }

  /* Success: ownership of attached handles transferred; close handle entries (without closing underlying FDs) */
  for (uint32_t i = 0; i < num_handles; i++) {
    struct MojoHandleEntry dummy;
    remove_handle_entry(handles[i], &dummy);
  }

  return MOJO_RESULT_OK;
}

MojoResult MojoReadMessage(MojoHandle handle, void *bytes, uint32_t *num_bytes,
                            MojoHandle *handles, uint32_t *num_handles,
                            MojoReadMessageFlags flags) {
  (void)flags;
  struct MojoHandleEntry entry;
  if (get_handle_entry(handle, &entry) < 0 ||
      (entry.type != MOJO_HANDLE_TYPE_MESSAGE_PIPE &&
       entry.type != MOJO_HANDLE_TYPE_PLATFORM_FD)) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  /* Peek header to verify sizes */
  struct MojoFrameHeader hdr;
  memset(&hdr, 0, sizeof(hdr));

  struct iovec iov_peek;
  iov_peek.iov_base = &hdr;
  iov_peek.iov_len = sizeof(hdr);

  struct msghdr msg_peek;
  memset(&msg_peek, 0, sizeof(msg_peek));
  msg_peek.msg_iov = &iov_peek;
  msg_peek.msg_iovlen = 1;

  ssize_t peek_res = recvmsg(entry.fd, &msg_peek, MSG_PEEK);
  if (peek_res < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return MOJO_RESULT_SHOULD_WAIT;
    }
    if (errno == ECONNRESET) {
      return MOJO_RESULT_FAILED_PRECONDITION;
    }
    return MOJO_RESULT_UNKNOWN;
  }
  if (peek_res == 0) {
    return MOJO_RESULT_FAILED_PRECONDITION; /* Peer closed */
  }

  if (peek_res < (ssize_t)sizeof(hdr) || hdr.magic != MOJO_FRAME_MAGIC) {
    return MOJO_RESULT_DATA_LOSS;
  }

  uint32_t req_bytes = hdr.payload_size;
  uint32_t req_handles = hdr.handle_count;

  uint32_t in_bytes = num_bytes ? *num_bytes : 0;
  uint32_t in_handles = num_handles ? *num_handles : 0;

  if (in_bytes < req_bytes || in_handles < req_handles || !bytes) {
    if (num_bytes) *num_bytes = req_bytes;
    if (num_handles) *num_handles = req_handles;
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  /* Read actual message */
  struct iovec iov[2];
  iov[0].iov_base = &hdr;
  iov[0].iov_len = sizeof(hdr);
  iov[1].iov_base = bytes;
  iov[1].iov_len = req_bytes;

  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = (req_bytes > 0) ? 2 : 1;

  int rx_fds[16];
  union {
    struct cmsghdr align;
    char buf[CMSG_SPACE(16 * sizeof(int))];
  } cmsg_un;

  memset(&cmsg_un, 0, sizeof(cmsg_un));
  msg.msg_control = cmsg_un.buf;
  msg.msg_controllen = sizeof(cmsg_un.buf);

  ssize_t ret = recvmsg(entry.fd, &msg, 0);
  if (ret < 0) {
    return MOJO_RESULT_UNKNOWN;
  }

  uint32_t got_handles_count = 0;
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  if (c && c->cmsg_type == SCM_RIGHTS) {
    size_t data_len = c->cmsg_len - CMSG_LEN(0);
    got_handles_count = (uint32_t)(data_len / sizeof(int));
    memcpy(rx_fds, CMSG_DATA(c), data_len);
  }

  for (uint32_t i = 0; i < got_handles_count && i < in_handles; i++) {
    MojoHandle new_h = alloc_handle_entry(MOJO_HANDLE_TYPE_MESSAGE_PIPE, rx_fds[i], 0);
    handles[i] = new_h;
  }

  if (num_bytes) *num_bytes = req_bytes;
  if (num_handles) *num_handles = got_handles_count;

  return MOJO_RESULT_OK;
}

MojoResult MojoCreateSharedBuffer(
    uint64_t num_bytes, const struct MojoCreateSharedBufferOptions *options,
    MojoHandle *shared_buffer_handle) {
  (void)options;
  if (!shared_buffer_handle || num_bytes == 0) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  int fd = memfd_create("mojo_shmbuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    /* Fallback if memfd unavailable */
    char path[] = "/tmp/mojo_shm_XXXXXX";
    fd = mkstemp(path);
    if (fd < 0) return MOJO_RESULT_RESOURCE_EXHAUSTED;
    unlink(path);
  }

  if (ftruncate(fd, (off_t)num_bytes) < 0) {
    close(fd);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  MojoHandle h = alloc_handle_entry(MOJO_HANDLE_TYPE_SHARED_BUFFER, fd, num_bytes);
  if (h == MOJO_HANDLE_INVALID) {
    close(fd);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *shared_buffer_handle = h;
  return MOJO_RESULT_OK;
}

MojoResult MojoDuplicateBufferHandle(
    MojoHandle buffer_handle,
    const struct MojoDuplicateBufferHandleOptions *options,
    MojoHandle *new_buffer_handle) {
  (void)options;
  struct MojoHandleEntry entry;
  if (get_handle_entry(buffer_handle, &entry) < 0 ||
      entry.type != MOJO_HANDLE_TYPE_SHARED_BUFFER) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  int dup_fd = fcntl(entry.fd, F_DUPFD_CLOEXEC, 0);
  if (dup_fd < 0) {
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  MojoHandle new_h = alloc_handle_entry(MOJO_HANDLE_TYPE_SHARED_BUFFER, dup_fd, entry.size);
  if (new_h == MOJO_HANDLE_INVALID) {
    close(dup_fd);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *new_buffer_handle = new_h;
  return MOJO_RESULT_OK;
}

MojoResult MojoMapBuffer(MojoHandle buffer_handle, uint64_t offset,
                          uint64_t num_bytes,
                          const struct MojoMapBufferOptions *options,
                          void **buffer) {
  (void)options;
  if (!buffer) return MOJO_RESULT_INVALID_ARGUMENT;

  struct MojoHandleEntry entry;
  if (get_handle_entry(buffer_handle, &entry) < 0 ||
      (entry.type != MOJO_HANDLE_TYPE_SHARED_BUFFER &&
       entry.type != MOJO_HANDLE_TYPE_PLATFORM_FD)) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  void *addr = mmap(NULL, (size_t)num_bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                    entry.fd, (off_t)offset);
  if (addr == MAP_FAILED) {
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  pthread_mutex_lock(&g_mojo_lock);
  for (int i = 0; i < MAX_MOJO_MAPPINGS; i++) {
    if (g_mappings[i].addr == NULL) {
      g_mappings[i].addr = addr;
      g_mappings[i].length = (size_t)num_bytes;
      break;
    }
  }
  pthread_mutex_unlock(&g_mojo_lock);

  *buffer = addr;
  return MOJO_RESULT_OK;
}

MojoResult MojoUnmapBuffer(void *buffer) {
  if (!buffer) return MOJO_RESULT_INVALID_ARGUMENT;

  size_t len = 0;
  pthread_mutex_lock(&g_mojo_lock);
  for (int i = 0; i < MAX_MOJO_MAPPINGS; i++) {
    if (g_mappings[i].addr == buffer) {
      len = g_mappings[i].length;
      g_mappings[i].addr = NULL;
      g_mappings[i].length = 0;
      break;
    }
  }
  pthread_mutex_unlock(&g_mojo_lock);

  if (len == 0) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  if (munmap(buffer, len) < 0) {
    return MOJO_RESULT_UNKNOWN;
  }
  return MOJO_RESULT_OK;
}

MojoResult MojoCreateWatcher(MojoWatcherCallback callback,
                              const struct MojoCreateWatcherOptions *options,
                              MojoHandle *watcher_handle) {
  (void)callback;
  (void)options;
  if (!watcher_handle) return MOJO_RESULT_INVALID_ARGUMENT;

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd < 0) return MOJO_RESULT_RESOURCE_EXHAUSTED;

  MojoHandle h = alloc_handle_entry(MOJO_HANDLE_TYPE_WATCHER, epfd, 0);
  if (h == MOJO_HANDLE_INVALID) {
    close(epfd);
    return MOJO_RESULT_RESOURCE_EXHAUSTED;
  }

  *watcher_handle = h;
  return MOJO_RESULT_OK;
}

MojoResult MojoWatch(MojoHandle watcher_handle, MojoHandle handle,
                      MojoHandleSignals signals, uintptr_t context,
                      const struct MojoWatchOptions *options) {
  (void)options;
  struct MojoHandleEntry watcher_entry, handle_entry;
  if (get_handle_entry(watcher_handle, &watcher_entry) < 0 ||
      watcher_entry.type != MOJO_HANDLE_TYPE_WATCHER ||
      get_handle_entry(handle, &handle_entry) < 0) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  if (signals & MOJO_HANDLE_SIGNAL_READABLE) ev.events |= EPOLLIN;
  if (signals & MOJO_HANDLE_SIGNAL_WRITABLE) ev.events |= EPOLLOUT;
  if (signals & MOJO_HANDLE_SIGNAL_PEER_CLOSED) ev.events |= (EPOLLHUP | EPOLLRDHUP);
  ev.data.ptr = (void *)context;

  if (epoll_ctl(watcher_entry.fd, EPOLL_CTL_ADD, handle_entry.fd, &ev) < 0) {
    if (errno == EEXIST) {
      if (epoll_ctl(watcher_entry.fd, EPOLL_CTL_MOD, handle_entry.fd, &ev) < 0) {
        return MOJO_RESULT_UNKNOWN;
      }
    } else {
      return MOJO_RESULT_UNKNOWN;
    }
  }

  return MOJO_RESULT_OK;
}

MojoResult MojoCancelWatch(MojoHandle watcher_handle, uintptr_t context) {
  (void)context;
  struct MojoHandleEntry watcher_entry;
  if (get_handle_entry(watcher_handle, &watcher_entry) < 0 ||
      watcher_entry.type != MOJO_HANDLE_TYPE_WATCHER) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  /* Note: epoll_ctl DEL requires fd. We accept cancelling or cleanup in epoll */
  return MOJO_RESULT_OK;
}

MojoResult MojoArmWatcher(MojoHandle watcher_handle,
                           uint32_t *num_ready_contexts,
                           uintptr_t *ready_contexts,
                           MojoResult *ready_results,
                           MojoHandleSignals *ready_signals) {
  struct MojoHandleEntry watcher_entry;
  if (get_handle_entry(watcher_handle, &watcher_entry) < 0 ||
      watcher_entry.type != MOJO_HANDLE_TYPE_WATCHER ||
      !num_ready_contexts) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }

  uint32_t max_events = *num_ready_contexts;
  if (max_events == 0) return MOJO_RESULT_INVALID_ARGUMENT;

  struct epoll_event events[16];
  if (max_events > 16) max_events = 16;

  int n = epoll_wait(watcher_entry.fd, events, max_events, 0);
  if (n < 0) {
    return MOJO_RESULT_UNKNOWN;
  }

  for (int i = 0; i < n; i++) {
    if (ready_contexts) ready_contexts[i] = (uintptr_t)events[i].data.ptr;
    if (ready_results) ready_results[i] = MOJO_RESULT_OK;
    if (ready_signals) {
      MojoHandleSignals sigs = 0;
      if (events[i].events & EPOLLIN) sigs |= MOJO_HANDLE_SIGNAL_READABLE;
      if (events[i].events & EPOLLOUT) sigs |= MOJO_HANDLE_SIGNAL_WRITABLE;
      if (events[i].events & (EPOLLHUP | EPOLLRDHUP)) sigs |= MOJO_HANDLE_SIGNAL_PEER_CLOSED;
      ready_signals[i] = sigs;
    }
  }

  *num_ready_contexts = n;
  return MOJO_RESULT_OK;
}

MojoResult MojoWrapPlatformHandle(int fd, MojoHandle *handle) {
  if (fd < 0 || !handle) return MOJO_RESULT_INVALID_ARGUMENT;
  MojoHandle h = alloc_handle_entry(MOJO_HANDLE_TYPE_PLATFORM_FD, fd, 0);
  if (h == MOJO_HANDLE_INVALID) return MOJO_RESULT_RESOURCE_EXHAUSTED;
  *handle = h;
  return MOJO_RESULT_OK;
}

MojoResult MojoUnwrapPlatformHandle(MojoHandle handle, int *fd) {
  if (!fd) return MOJO_RESULT_INVALID_ARGUMENT;
  struct MojoHandleEntry entry;
  if (remove_handle_entry(handle, &entry) < 0) {
    return MOJO_RESULT_INVALID_ARGUMENT;
  }
  *fd = entry.fd;
  return MOJO_RESULT_OK;
}
