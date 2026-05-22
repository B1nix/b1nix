#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/journal.h>
#include <b1nix/mm.h>
#include <string.h>

/* JBD format magic values, compatible with ext3/ext4. */
#define JBD_MAGIC 0xC03B3998
#define JBD_DESCRIPTOR_BLOCK 1
#define JBD_COMMIT_BLOCK 2
#define JBD_SUPERBLOCK_V1 3
#define JBD_SUPERBLOCK_V2 4
#define JBD_REVOKE_BLOCK 5

struct jbd_header {
  u32 h_magic;
  u32 h_blocktype;
  u32 h_sequence;
};

struct jbd_block_tag {
  u32 t_blocknr;
  u32 t_flags;
};
#define JBD_TAG_LAST_TAG 8

struct jbd_superblock {
  struct jbd_header s_header;
  u32 s_blocksize;
  u32 s_maxlen;
  u32 s_first;
  u32 s_sequence;
  u32 s_start;
  u32 s_errno;
};

#define MAX_JOURNALS 4
#define MAX_JOURNAL_HANDLES 16

static struct journal_dev jdevs[MAX_JOURNALS];
static struct journal_handle handles[MAX_JOURNAL_HANDLES];

struct journal_dev *journal_mount(void *fs_priv, u32 block_size,
                                  struct journal_ops *ops) {
  struct journal_dev *jdev = 0;
  for (int i = 0; i < MAX_JOURNALS; i++) {
    if (!jdevs[i].used) {
      jdev = &jdevs[i];
            break;
        }
    }
  if (!jdev)
    return 0;
    
  jdev->used = 1;
  jdev->fs_priv = fs_priv;
  jdev->block_size = block_size;
  jdev->ops = *ops;
    
  u8 *sb_buf = kmalloc(block_size);
  if (jdev->ops.read_journal_block(jdev, 0, sb_buf) < 0) {
    kfree(sb_buf);
    jdev->used = 0;
    return 0;
}

  struct jbd_superblock *sb = (struct jbd_superblock *)sb_buf;
  if (sb->s_header.h_magic != JBD_MAGIC) {
    kfree(sb_buf);
    jdev->used = 0;
        return 0;
    }
    
  jdev->maxlen = sb->s_maxlen;
  jdev->nblocks = sb->s_maxlen;
  jdev->first = sb->s_first;
  jdev->next_seq = sb->s_sequence;
    
  kfree(sb_buf);
  return jdev;
    }
    
int journal_recover(struct journal_dev *jdev) {
  if (!jdev || !jdev->used)
    return -1;
    
  console_write("jbd: starting recovery from seq=");
  console_write_dec(jdev->next_seq);
  console_write("\n");
    
  if (jdev->next_seq <= 1)
    return 0;
    
  u32 current = jdev->first;
  int replayed = 0;
  int scanned_txs = 0;
  int committed_txs = 0;
    
  for (u32 seq = 1; seq < jdev->next_seq; seq++) {
    u8 *hdr_buf = kmalloc(jdev->block_size);
    if (jdev->ops.read_journal_block(jdev, current, hdr_buf) < 0) {
      kfree(hdr_buf);
      break;
    }
    
    struct jbd_header *hdr = (struct jbd_header *)hdr_buf;
    if (hdr->h_magic != JBD_MAGIC) {
      kfree(hdr_buf);
      break;
    }

    if (hdr->h_blocktype == JBD_DESCRIPTOR_BLOCK) {
      struct jbd_block_tag *tag =
          (struct jbd_block_tag *)(hdr_buf + sizeof(struct jbd_header));
      u32 data_block = current + 1;
      u32 tx_data_start = data_block;
      u32 tx_seq = hdr->h_sequence;
      int done = 0;

      while (!done) {
        (void)tag->t_blocknr;
        if (tag->t_flags & JBD_TAG_LAST_TAG)
          done = 1;
        data_block++;
        tag++;
      }

      /* Descriptor + N data blocks must be followed by a matching commit
       * block before we are allowed to replay the transaction. */
      u8 *commit_buf = kmalloc(jdev->block_size);
      int has_commit = 0;
      if (jdev->ops.read_journal_block(jdev, data_block, commit_buf) == 0) {
        struct jbd_header *chdr = (struct jbd_header *)commit_buf;
        if (chdr->h_magic == JBD_MAGIC &&
            chdr->h_blocktype == JBD_COMMIT_BLOCK &&
            chdr->h_sequence == tx_seq) {
          has_commit = 1;
        }
      }
      kfree(commit_buf);

      scanned_txs++;
      if (has_commit) {
        committed_txs++;
        struct jbd_block_tag *rtag =
            (struct jbd_block_tag *)(hdr_buf + sizeof(struct jbd_header));
        u32 rdata = tx_data_start;
        int rdone = 0;
        while (!rdone) {
          u32 blocknr = rtag->t_blocknr;
          if (rtag->t_flags & JBD_TAG_LAST_TAG)
            rdone = 1;

          u8 *data_buf = kmalloc(jdev->block_size);
          if (jdev->ops.read_journal_block(jdev, rdata, data_buf) == 0) {
            jdev->ops.write_fs_block(jdev, blocknr, data_buf);
            replayed++;
          }
          kfree(data_buf);
          rdata++;
          rtag++;
        }
      } else {
        console_write("jbd: skip uncommitted tx seq=");
        console_write_dec(tx_seq);
        console_write("\n");
      }

      current = data_block + 1;
    } else if (hdr->h_blocktype == JBD_COMMIT_BLOCK ||
               hdr->h_blocktype == JBD_REVOKE_BLOCK) {
      current++;
    } else {
      kfree(hdr_buf);
      break;
    }
    kfree(hdr_buf);
    }
    
  console_write("jbd: replayed ");
  console_write_dec(replayed);
  console_write(" blocks from ");
  console_write_dec(committed_txs);
  console_write("/");
  console_write_dec(scanned_txs);
  console_write(" committed txs\n");
    return 0;
}

struct journal_handle *journal_start_transaction(struct journal_dev *jdev) {
  for (int i = 0; i < MAX_JOURNAL_HANDLES; i++) {
    if (!handles[i].active) {
      handles[i].active = 1;
      handles[i].jdev = jdev;
      handles[i].transaction_id = jdev->next_seq;
      handles[i].modified_count = 0;
      for (u32 j = 0; j < MAX_JBD_BLOCKS_PER_TX; j++)
        handles[i].data_blocks[j] = 0;
      return &handles[i];
    }
  }
  return 0;
}
    
int journal_log_block(struct journal_handle *handle, u32 fs_block,
                      const void *data) {
  if (!handle || !handle->active)
    return -1;
  if (handle->modified_count >= MAX_JBD_BLOCKS_PER_TX)
    return -1;

  void *copy = kmalloc(handle->jdev->block_size);
  if (!copy)
    return -1;
  memcpy(copy, data, handle->jdev->block_size);

  handle->fs_blocks[handle->modified_count] = fs_block;
  handle->data_blocks[handle->modified_count] = copy;
  handle->modified_count++;
  return 0;
}
    
int journal_commit_transaction(struct journal_handle *handle) {
  if (!handle || !handle->active)
    return -1;
  if (handle->modified_count == 0) {
    handle->active = 0;
    return 0;
  }
    
  struct journal_dev *jdev = handle->jdev;
  u32 count = handle->modified_count;
  u32 log_start =
      jdev->first + (jdev->next_seq % (jdev->nblocks - jdev->first));

  /* 1. Write the descriptor block. */
  u8 *desc_buf = kzalloc(jdev->block_size);
  struct jbd_header *hdr = (struct jbd_header *)desc_buf;
  hdr->h_magic = JBD_MAGIC;
  hdr->h_blocktype = JBD_DESCRIPTOR_BLOCK;
  hdr->h_sequence = jdev->next_seq;

  struct jbd_block_tag *tags =
      (struct jbd_block_tag *)(desc_buf + sizeof(struct jbd_header));
  for (u32 i = 0; i < count; i++) {
    tags[i].t_blocknr = handle->fs_blocks[i];
    tags[i].t_flags = (i == count - 1) ? JBD_TAG_LAST_TAG : 0;
  }
  jdev->ops.write_journal_block(jdev, log_start, desc_buf);
  kfree(desc_buf);

  /* 2. Write data blocks into the journal. */
  for (u32 i = 0; i < count; i++) {
    jdev->ops.write_journal_block(jdev, log_start + 1 + i,
                                  handle->data_blocks[i]);
  }

  /* 3. Write the commit block. */
  u8 *commit_buf = kzalloc(jdev->block_size);
  struct jbd_header *chdr = (struct jbd_header *)commit_buf;
  chdr->h_magic = JBD_MAGIC;
  chdr->h_blocktype = JBD_COMMIT_BLOCK;
  chdr->h_sequence = jdev->next_seq;
  jdev->ops.write_journal_block(jdev, log_start + 1 + count, commit_buf);
        kfree(commit_buf);
    
  /* 4. Update the superblock sequence. */
  jdev->next_seq++;
  u8 *sb_buf = kmalloc(jdev->block_size);
  if (jdev->ops.read_journal_block(jdev, 0, sb_buf) == 0) {
    struct jbd_superblock *sb = (struct jbd_superblock *)sb_buf;
    sb->s_sequence = jdev->next_seq;
    jdev->ops.write_journal_block(jdev, 0, sb_buf);
  }
  kfree(sb_buf);
    
  /* 5. Write data blocks into the filesystem. */
  for (u32 i = 0; i < count; i++) {
    jdev->ops.write_fs_block(jdev, handle->fs_blocks[i],
                             handle->data_blocks[i]);
    kfree(handle->data_blocks[i]);
    handle->data_blocks[i] = 0;
}

  handle->active = 0;
  return 0;
}

void journal_abort_transaction(struct journal_handle *handle) {
  if (!handle || !handle->active)
    return;
  for (u32 i = 0; i < handle->modified_count; i++) {
    if (handle->data_blocks[i]) {
      kfree(handle->data_blocks[i]);
      handle->data_blocks[i] = 0;
    }
  }
  handle->modified_count = 0;
  handle->active = 0;
}
