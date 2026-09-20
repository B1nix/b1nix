/* B1NIX Mojo Core C API
 * Implements minimal Mojo IPC core over M56/M57 primitives (epoll, eventfd,
 * memfd, socketpair, SCM_RIGHTS).
 */
#ifndef B1NIX_MOJO_MOJO_H
#define B1NIX_MOJO_MOJO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t MojoHandle;
#define MOJO_HANDLE_INVALID ((MojoHandle)0)

typedef uint32_t MojoResult;
#define MOJO_RESULT_OK ((MojoResult)0)
#define MOJO_RESULT_CANCELLED ((MojoResult)1)
#define MOJO_RESULT_UNKNOWN ((MojoResult)2)
#define MOJO_RESULT_INVALID_ARGUMENT ((MojoResult)3)
#define MOJO_RESULT_DEADLINE_EXCEEDED ((MojoResult)4)
#define MOJO_RESULT_NOT_FOUND ((MojoResult)5)
#define MOJO_RESULT_ALREADY_EXISTS ((MojoResult)6)
#define MOJO_RESULT_PERMISSION_DENIED ((MojoResult)7)
#define MOJO_RESULT_RESOURCE_EXHAUSTED ((MojoResult)8)
#define MOJO_RESULT_FAILED_PRECONDITION ((MojoResult)9)
#define MOJO_RESULT_ABORTED ((MojoResult)10)
#define MOJO_RESULT_OUT_OF_RANGE ((MojoResult)11)
#define MOJO_RESULT_UNIMPLEMENTED ((MojoResult)12)
#define MOJO_RESULT_INTERNAL ((MojoResult)13)
#define MOJO_RESULT_UNAVAILABLE ((MojoResult)14)
#define MOJO_RESULT_DATA_LOSS ((MojoResult)15)
#define MOJO_RESULT_BUSY ((MojoResult)16)
#define MOJO_RESULT_SHOULD_WAIT ((MojoResult)17)

typedef uint32_t MojoHandleSignals;
#define MOJO_HANDLE_SIGNAL_NONE ((MojoHandleSignals)0)
#define MOJO_HANDLE_SIGNAL_READABLE ((MojoHandleSignals)(1 << 0))
#define MOJO_HANDLE_SIGNAL_WRITABLE ((MojoHandleSignals)(1 << 1))
#define MOJO_HANDLE_SIGNAL_PEER_CLOSED ((MojoHandleSignals)(1 << 2))

typedef uint32_t MojoCreateMessagePipeFlags;
#define MOJO_CREATE_MESSAGE_PIPE_FLAG_NONE ((MojoCreateMessagePipeFlags)0)

struct MojoCreateMessagePipeOptions {
  uint32_t struct_size;
  MojoCreateMessagePipeFlags flags;
};

typedef uint32_t MojoWriteMessageFlags;
#define MOJO_WRITE_MESSAGE_FLAG_NONE ((MojoWriteMessageFlags)0)

typedef uint32_t MojoReadMessageFlags;
#define MOJO_READ_MESSAGE_FLAG_NONE ((MojoReadMessageFlags)0)

typedef uint32_t MojoCreateSharedBufferFlags;
#define MOJO_CREATE_SHARED_BUFFER_FLAG_NONE ((MojoCreateSharedBufferFlags)0)

struct MojoCreateSharedBufferOptions {
  uint32_t struct_size;
  MojoCreateSharedBufferFlags flags;
};

typedef uint32_t MojoMapBufferFlags;
#define MOJO_MAP_BUFFER_FLAG_NONE ((MojoMapBufferFlags)0)

struct MojoMapBufferOptions {
  uint32_t struct_size;
  MojoMapBufferFlags flags;
};

typedef uint32_t MojoDuplicateBufferHandleFlags;
#define MOJO_DUPLICATE_BUFFER_HANDLE_FLAG_NONE ((MojoDuplicateBufferHandleFlags)0)

struct MojoDuplicateBufferHandleOptions {
  uint32_t struct_size;
  MojoDuplicateBufferHandleFlags flags;
};

typedef uint32_t MojoCreateWatcherFlags;
#define MOJO_CREATE_WATCHER_FLAG_NONE ((MojoCreateWatcherFlags)0)

struct MojoCreateWatcherOptions {
  uint32_t struct_size;
  MojoCreateWatcherFlags flags;
};

typedef uint32_t MojoWatchFlags;
#define MOJO_WATCH_FLAG_NONE ((MojoWatchFlags)0)

struct MojoWatchOptions {
  uint32_t struct_size;
  MojoWatchFlags flags;
};

typedef void (*MojoWatcherCallback)(uintptr_t context, MojoResult result,
                                     MojoHandleSignals state, uint32_t flags);

/* Basic Handle Lifecycle */
MojoResult MojoClose(MojoHandle handle);

/* Message Pipe API */
MojoResult MojoCreateMessagePipe(
    const struct MojoCreateMessagePipeOptions *options,
    MojoHandle *message_pipe_handle0, MojoHandle *message_pipe_handle1);

MojoResult MojoWriteMessage(MojoHandle handle, const void *bytes,
                             uint32_t num_bytes, const MojoHandle *handles,
                             uint32_t num_handles, MojoWriteMessageFlags flags);

MojoResult MojoReadMessage(MojoHandle handle, void *bytes, uint32_t *num_bytes,
                            MojoHandle *handles, uint32_t *num_handles,
                            MojoReadMessageFlags flags);

/* Shared Buffer API */
MojoResult MojoCreateSharedBuffer(
    uint64_t num_bytes, const struct MojoCreateSharedBufferOptions *options,
    MojoHandle *shared_buffer_handle);

MojoResult MojoDuplicateBufferHandle(
    MojoHandle buffer_handle,
    const struct MojoDuplicateBufferHandleOptions *options,
    MojoHandle *new_buffer_handle);

MojoResult MojoMapBuffer(MojoHandle buffer_handle, uint64_t offset,
                          uint64_t num_bytes,
                          const struct MojoMapBufferOptions *options,
                          void **buffer);

MojoResult MojoUnmapBuffer(void *buffer);

/* Watcher / Event Loop API */
MojoResult MojoCreateWatcher(MojoWatcherCallback callback,
                              const struct MojoCreateWatcherOptions *options,
                              MojoHandle *watcher_handle);

MojoResult MojoWatch(MojoHandle watcher_handle, MojoHandle handle,
                      MojoHandleSignals signals, uintptr_t context,
                      const struct MojoWatchOptions *options);

MojoResult MojoCancelWatch(MojoHandle watcher_handle, uintptr_t context);

MojoResult MojoArmWatcher(MojoHandle watcher_handle,
                           uint32_t *num_ready_contexts,
                           uintptr_t *ready_contexts,
                           MojoResult *ready_results,
                           MojoHandleSignals *ready_signals);

/* Platform Handle Interop */
MojoResult MojoWrapPlatformHandle(int fd, MojoHandle *handle);
MojoResult MojoUnwrapPlatformHandle(MojoHandle handle, int *fd);

#ifdef __cplusplus
}
#endif

#endif /* B1NIX_MOJO_MOJO_H */
