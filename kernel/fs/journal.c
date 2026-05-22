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
  jdev->s_start = sb->s_start;
  if (jdev->s_start < sb->s_first || jdev->s_start >= sb->s_maxlen) {
    jdev->s_start = sb->s_first;
  }

  kfree(sb_buf);
  return jdev;
}

static u32 journal_next_block(struct journal_dev *jdev, u32 *pos) {
  u32 cur = *pos;
  (*pos)++;
  if (*pos >= jdev->maxlen) {
    *pos = jdev->first;
  }
  return cur;
}

int journal_recover(struct journal_dev *jdev) {
  if (!jdev || !jdev->used)
    return -1;

  console_write("jbd: starting recovery from seq=");
  console_write_dec(jdev->next_seq);
  console_write(", start_block=");
  console_write_dec(jdev->s_start);
  console_write("\n");

  u32 current = jdev->s_start;
  int replayed = 0;
  int scanned_txs = 0;
  int committed_txs = 0;
  u32 seq = jdev->next_seq;

  while (1) {
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

    if (hdr->h_sequence != seq) {
      kfree(hdr_buf);
      break;
    }

    if (hdr->h_blocktype == JBD_DESCRIPTOR_BLOCK) {
      struct jbd_block_tag *tag =
          (struct jbd_block_tag *)(hdr_buf + sizeof(struct jbd_header));
      u32 pos = current;
      journal_next_block(jdev, &pos); // skip descriptor block
      u32 tx_data_start = pos;
      int done = 0;
      int count = 0;

      while (!done) {
        if (tag->t_flags & JBD_TAG_LAST_TAG)
          done = 1;
        journal_next_block(jdev, &pos); // skip data block
        count++;
        tag++;
      }

      u32 commit_pos = journal_next_block(jdev, &pos); // commit block pos

      /* Read commit block and verify */
      u8 *commit_buf = kmalloc(jdev->block_size);
      int has_commit = 0;
      if (jdev->ops.read_journal_block(jdev, commit_pos, commit_buf) == 0) {
        struct jbd_header *chdr = (struct jbd_header *)commit_buf;
        if (chdr->h_magic == JBD_MAGIC &&
            chdr->h_blocktype == JBD_COMMIT_BLOCK &&
            chdr->h_sequence == seq) {
          has_commit = 1;
        }
      }
      kfree(commit_buf);

      scanned_txs++;
      if (has_commit) {
        committed_txs++;

        struct jbd_block_tag *rtag =
            (struct jbd_block_tag *)(hdr_buf + sizeof(struct jbd_header));
        u32 rdata_pos = tx_data_start;
        int rdone = 0;
        while (!rdone) {
          u32 blocknr = rtag->t_blocknr;
          if (rtag->t_flags & JBD_TAG_LAST_TAG)
            rdone = 1;

          u8 *data_buf = kmalloc(jdev->block_size);
          if (jdev->ops.read_journal_block(jdev, rdata_pos, data_buf) == 0) {
            jdev->ops.write_fs_block(jdev, blocknr, data_buf);
            replayed++;
          }
          kfree(data_buf);
          journal_next_block(jdev, &rdata_pos);
          rtag++;
        }

        current = pos;
        seq++;
      } else {
        console_write("jbd: skip uncommitted tx seq=");
        console_write_dec(seq);
        console_write("\n");
        kfree(hdr_buf);
        break;
      }
    } else {
      kfree(hdr_buf);
      break;
    }
    kfree(hdr_buf);
  }

  if (committed_txs > 0) {
    jdev->next_seq = seq;
    jdev->s_start = current;

    u8 *sb_buf = kmalloc(jdev->block_size);
    if (jdev->ops.read_journal_block(jdev, 0, sb_buf) == 0) {
      struct jbd_superblock *sb = (struct jbd_superblock *)sb_buf;
      sb->s_sequence = jdev->next_seq;
      sb->s_start = jdev->s_start;
      jdev->ops.write_journal_block(jdev, 0, sb_buf);
    }
    kfree(sb_buf);
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
  u32 pos = jdev->s_start;

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
  u32 desc_pos = journal_next_block(jdev, &pos);
  jdev->ops.write_journal_block(jdev, desc_pos, desc_buf);
  kfree(desc_buf);

  /* 2. Write data blocks into the journal. */
  for (u32 i = 0; i < count; i++) {
    u32 data_pos = journal_next_block(jdev, &pos);
    jdev->ops.write_journal_block(jdev, data_pos, handle->data_blocks[i]);
  }

  /* 3. Write the commit block. */
  u8 *commit_buf = kzalloc(jdev->block_size);
  struct jbd_header *chdr = (struct jbd_header *)commit_buf;
  chdr->h_magic = JBD_MAGIC;
  chdr->h_blocktype = JBD_COMMIT_BLOCK;
  chdr->h_sequence = jdev->next_seq;
  u32 commit_pos = journal_next_block(jdev, &pos);
  jdev->ops.write_journal_block(jdev, commit_pos, commit_buf);
  kfree(commit_buf);

  u32 next_tx_start = pos;

  /* 4. Write data blocks into the filesystem (immediate checkpoint). */
  for (u32 i = 0; i < count; i++) {
    jdev->ops.write_fs_block(jdev, handle->fs_blocks[i],
                             handle->data_blocks[i]);
    kfree(handle->data_blocks[i]);
    handle->data_blocks[i] = 0;
  }

  /* 5. Update the superblock. */
  jdev->next_seq++;
  jdev->s_start = next_tx_start;

  u8 *sb_buf = kmalloc(jdev->block_size);
  if (jdev->ops.read_journal_block(jdev, 0, sb_buf) == 0) {
    struct jbd_superblock *sb = (struct jbd_superblock *)sb_buf;
    sb->s_sequence = jdev->next_seq;
    sb->s_start = jdev->s_start;
    jdev->ops.write_journal_block(jdev, 0, sb_buf);
  }
  kfree(sb_buf);

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
