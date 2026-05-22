#ifndef B1NIX_JOURNAL_H
#define B1NIX_JOURNAL_H

#include <b1nix/types.h>

struct journal_dev;

/* Callbacks provided by the filesystem backend. */
struct journal_ops {
  int (*read_journal_block)(struct journal_dev *jdev, u32 logical_block,
                            void *buffer);
  int (*write_journal_block)(struct journal_dev *jdev, u32 logical_block,
                             const void *buffer);
  int (*write_fs_block)(struct journal_dev *jdev, u32 fs_block,
                        const void *buffer);
};

struct journal_dev {
  int used;
    u32 block_size;
  u32 nblocks;
  u32 first;
  u32 next_seq;
  u32 s_start;
  u32 maxlen;
  void *fs_priv;
  struct journal_ops ops;
};

#define MAX_JBD_BLOCKS_PER_TX 32

struct journal_handle {
  int active;
  struct journal_dev *jdev;
  u32 transaction_id;
  u32 modified_count;
  u32 fs_blocks[MAX_JBD_BLOCKS_PER_TX];
  void *data_blocks[MAX_JBD_BLOCKS_PER_TX];
};

/* Journal mount and recovery API. */
struct journal_dev *journal_mount(void *fs_priv, u32 block_size,
                                  struct journal_ops *ops);
int journal_recover(struct journal_dev *jdev);

/* Transaction API. */
struct journal_handle *journal_start_transaction(struct journal_dev *jdev);
int journal_log_block(struct journal_handle *handle, u32 fs_block,
                      const void *data);
int journal_commit_transaction(struct journal_handle *handle);
void journal_abort_transaction(struct journal_handle *handle);

#endif
